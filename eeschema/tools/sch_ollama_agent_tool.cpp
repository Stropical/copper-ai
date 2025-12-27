/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sch_ollama_agent_tool.h"
#include "sch_ollama_agent_dialog.h"
#include <sch_edit_frame.h>
#include <dialogs/dialog_text_entry.h>
#include <confirm.h>
#include <tools/sch_actions.h>
#include <math/vector2d.h>
#include <base_units.h>
#include <wx/log.h>
#include <sch_junction.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_text.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_pin.h>
#include <sch_field.h>
#include <sch_connection.h>
#include <stroke_params.h>
#include <math/box2.h>
#include <nlohmann/json.hpp>
#include <set>
#include <map>
#include <vector>

using json = nlohmann::json;
#include <sch_commit.h>
#include <lib_id.h>


SCH_OLLAMA_AGENT_TOOL::SCH_OLLAMA_AGENT_TOOL() :
    SCH_TOOL_BASE<SCH_EDIT_FRAME>( "eeschema.OllamaAgentTool" ),
    m_model( wxS( "qwen3:4b" ) )  // Default model
{
}


bool SCH_OLLAMA_AGENT_TOOL::Init()
{
    if( !SCH_TOOL_BASE<SCH_EDIT_FRAME>::Init() )
        return false;

    // Initialize agent - ollama client will be created lazily when needed
    // to avoid potential exceptions during tool initialization
    m_agent = std::make_unique<SCH_AGENT>( m_frame );

    return true;
}


int SCH_OLLAMA_AGENT_TOOL::ProcessRequest( const TOOL_EVENT& aEvent )
{
    wxString userRequest;

    if( aEvent.HasParameter() )
    {
        userRequest = aEvent.Parameter<wxString>();
    }
    else
    {
        // Get request from user via simple dialog
        WX_TEXT_ENTRY_DIALOG dlg( m_frame, _( "Ollama Agent Request" ),
                                  _( "Enter your request:" ), wxEmptyString );
        
        if( dlg.ShowModal() != wxID_OK )
            return 0;

        userRequest = dlg.GetValue();
    }

    if( userRequest.IsEmpty() )
        return 0;

    // Initialize ollama client lazily if needed
    if( !m_ollama )
    {
        try
        {
            m_ollama = std::make_unique<OLLAMA_CLIENT>();
        }
        catch( ... )
        {
            DisplayError( m_frame, _( "Failed to initialize Python agent client. Please check your network configuration." ) );
            return 0;
        }
    }

    // Append full schematic context before sending to Python pcb_agent.
    wxString context = GetFullSchematicContext();
    wxString prompt = userRequest;
    if( !context.IsEmpty() )
        prompt << wxS( "\n\n" ) << context;

    // Send request to Python agent (which handles prompt building, RAG, etc.)
    wxString response;
    if( !m_ollama->ChatCompletion( m_model, prompt, response ) )
    {
        DisplayError( m_frame, _( "Failed to communicate with Python agent server." ) );
        return 0;
    }

    // Parse and execute
    if( !ParseAndExecute( response ) )
    {
        DisplayInfoMessage( m_frame, _( "Agent response received but could not parse commands." ),
                           _( "Ollama Agent" ) );
    }

    return 0;
}


int SCH_OLLAMA_AGENT_TOOL::ShowAgentDialog( const TOOL_EVENT& aEvent )
{
    SCH_OLLAMA_AGENT_DIALOG dlg( m_frame, this );
    dlg.ShowModal();
    return 0;
}


OLLAMA_CLIENT* SCH_OLLAMA_AGENT_TOOL::GetOllama()
{
    if( !m_ollama )
    {
        try
        {
            m_ollama = std::make_unique<OLLAMA_CLIENT>();
        }
        catch( ... )
        {
            return nullptr;
        }
    }
    return m_ollama.get();
}


wxString SCH_OLLAMA_AGENT_TOOL::GetCurrentSchematicContent()
{
    if( !m_frame )
        return wxEmptyString;

    SCH_SCREEN* screen = m_frame->GetScreen();
    if( !screen )
        return wxEmptyString;

    SCH_SHEET_PATH& sheet = m_frame->GetCurrentSheet();

    wxString content;
    content << wxS( "CURRENT SCHEMATIC CONTENT:\n" );
    content << wxS( "Sheet: " ) << m_frame->GetFullScreenDesc() << wxS( "\n\n" );

    // Collect items by type for organized output
    std::vector<SCH_SYMBOL*> symbols;
    std::vector<SCH_JUNCTION*> junctions;
    std::vector<SCH_LINE*> wires;
    std::vector<SCH_LABEL*> labels;
    std::vector<SCH_TEXT*> texts;

    for( SCH_ITEM* item : screen->Items() )
    {
        switch( item->Type() )
        {
        case SCH_SYMBOL_T:
            symbols.push_back( static_cast<SCH_SYMBOL*>( item ) );
            break;
        case SCH_JUNCTION_T:
            junctions.push_back( static_cast<SCH_JUNCTION*>( item ) );
            break;
        case SCH_LINE_T:
        {
            SCH_LINE* line = static_cast<SCH_LINE*>( item );
            if( line->GetLayer() == LAYER_WIRE )
                wires.push_back( line );
            break;
        }
        case SCH_LABEL_T:
            labels.push_back( static_cast<SCH_LABEL*>( item ) );
            break;
        case SCH_TEXT_T:
            texts.push_back( static_cast<SCH_TEXT*>( item ) );
            break;
        default:
            break;
        }
    }

    // Format symbols/components with detailed information
    if( !symbols.empty() )
    {
        content << wxS( "Components:\n" );
        for( SCH_SYMBOL* symbol : symbols )
        {
            VECTOR2I pos = symbol->GetPosition();
            double x_mm = schIUScale.IUTomm( pos.x );
            double y_mm = schIUScale.IUTomm( pos.y );
            
            wxString ref = symbol->GetRef( &sheet, true );
            wxString libId = symbol->GetLibId().Format();
            int unit = symbol->GetUnit();
            int bodyStyle = symbol->GetBodyStyle();
            
            content << wxString::Format( wxS( "  - Component %s (%s) at (%.2f, %.2f) mm\n" ),
                                        ref, libId, x_mm, y_mm );
            
            if( unit > 1 || bodyStyle > 1 )
            {
                content << wxString::Format( wxS( "    Unit: %d, Body Style: %d\n" ), unit, bodyStyle );
            }
            
            // Get fields (value, footprint, etc.)
            SCH_FIELDS fields = symbol->GetFields();
            bool hasFields = false;
            for( const SCH_FIELD& field : fields )
            {
                if( field.GetText().IsEmpty() )
                    continue;
                    
                wxString fieldName = field.GetName();
                if( fieldName.IsEmpty() )
                    fieldName = wxS( "Value" ); // Default field name
                    
                if( !hasFields )
                {
                    content << wxS( "    Fields:\n" );
                    hasFields = true;
                }
                content << wxString::Format( wxS( "      %s: %s\n" ), fieldName, field.GetText() );
            }
            
            // Get pins with their positions and net connections
            std::vector<SCH_PIN*> pins = symbol->GetPins( &sheet );
            if( !pins.empty() )
            {
                content << wxS( "    Pins:\n" );
                for( SCH_PIN* pin : pins )
                {
                    VECTOR2I pinPos = pin->GetPosition();
                    double pinX_mm = schIUScale.IUTomm( pinPos.x );
                    double pinY_mm = schIUScale.IUTomm( pinPos.y );
                    wxString pinName = pin->GetShownName();
                    wxString pinNumber = pin->GetShownNumber();
                    
                    // Get net connection if available
                    wxString netName = wxS( "<unconnected>" );
                    if( SCH_CONNECTION* conn = pin->Connection( &sheet ) )
                    {
                        netName = conn->Name();
                        if( netName.IsEmpty() )
                            netName = wxS( "<unnamed net>" );
                    }
                    
                    content << wxString::Format( wxS( "      Pin %s (%s) at (%.2f, %.2f) mm -> Net: %s\n" ),
                                                pinName, pinNumber, pinX_mm, pinY_mm, netName );
                }
            }
            content << wxS( "\n" );
        }
        content << wxS( "\n" );
    }

    // Format junctions
    if( !junctions.empty() )
    {
        content << wxS( "Junctions:\n" );
        for( SCH_JUNCTION* junction : junctions )
        {
            VECTOR2I pos = junction->GetPosition();
            double x_mm = schIUScale.IUTomm( pos.x );
            double y_mm = schIUScale.IUTomm( pos.y );
            
            // Get net name if available
            wxString netName = wxS( "" );
            if( SCH_CONNECTION* conn = junction->Connection( &sheet ) )
            {
                netName = conn->Name();
            }
            
            if( !netName.IsEmpty() )
            {
                content << wxString::Format( wxS( "  - Junction at (%.2f, %.2f) mm on net: %s\n" ),
                                            x_mm, y_mm, netName );
            }
            else
            {
                content << wxString::Format( wxS( "  - Junction at (%.2f, %.2f) mm\n" ), x_mm, y_mm );
            }
        }
        content << wxS( "\n" );
    }

    // Format wires
    if( !wires.empty() )
    {
        content << wxS( "Wires:\n" );
        for( SCH_LINE* wire : wires )
        {
            VECTOR2I start = wire->GetStartPoint();
            VECTOR2I end = wire->GetEndPoint();
            double x1_mm = schIUScale.IUTomm( start.x );
            double y1_mm = schIUScale.IUTomm( start.y );
            double x2_mm = schIUScale.IUTomm( end.x );
            double y2_mm = schIUScale.IUTomm( end.y );
            
            // Get net name if available
            wxString netName = wxS( "" );
            if( SCH_CONNECTION* conn = wire->Connection( &sheet ) )
            {
                netName = conn->Name();
            }
            
            if( !netName.IsEmpty() )
            {
                content << wxString::Format( wxS( "  - Wire from (%.2f, %.2f) to (%.2f, %.2f) mm on net: %s\n" ),
                                            x1_mm, y1_mm, x2_mm, y2_mm, netName );
            }
            else
            {
                content << wxString::Format( wxS( "  - Wire from (%.2f, %.2f) to (%.2f, %.2f) mm\n" ),
                                            x1_mm, y1_mm, x2_mm, y2_mm );
            }
        }
        content << wxS( "\n" );
    }

    // Format labels
    if( !labels.empty() )
    {
        content << wxS( "Labels:\n" );
        for( SCH_LABEL* label : labels )
        {
            VECTOR2I pos = label->GetPosition();
            double x_mm = schIUScale.IUTomm( pos.x );
            double y_mm = schIUScale.IUTomm( pos.y );
            wxString labelText = label->GetText();
            
            // Get net name if available
            wxString netName = wxS( "" );
            if( SCH_CONNECTION* conn = label->Connection( &sheet ) )
            {
                netName = conn->Name();
            }
            
            if( !netName.IsEmpty() && netName != labelText )
            {
                content << wxString::Format( wxS( "  - Label \"%s\" at (%.2f, %.2f) mm (net: %s)\n" ),
                                            labelText, x_mm, y_mm, netName );
            }
            else
            {
                content << wxString::Format( wxS( "  - Label \"%s\" at (%.2f, %.2f) mm\n" ),
                                            labelText, x_mm, y_mm );
            }
        }
        content << wxS( "\n" );
    }

    // Format text
    if( !texts.empty() )
    {
        content << wxS( "Text:\n" );
        for( SCH_TEXT* text : texts )
        {
            VECTOR2I pos = text->GetPosition();
            double x_mm = schIUScale.IUTomm( pos.x );
            double y_mm = schIUScale.IUTomm( pos.y );
            wxString textContent = text->GetText();
            content << wxString::Format( wxS( "  - Text \"%s\" at (%.2f, %.2f) mm\n" ),
                                        textContent, x_mm, y_mm );
        }
        content << wxS( "\n" );
    }

    if( symbols.empty() && junctions.empty() && wires.empty() && labels.empty() && texts.empty() )
    {
        content << wxS( "  (Schematic is empty)\n" );
    }

    return content;
}

wxString SCH_OLLAMA_AGENT_TOOL::GetFullSchematicContext( size_t aMaxChars )
{
    if( !m_frame )
        return wxEmptyString;

    SCHEMATIC& schematic = m_frame->Schematic();
    SCH_SHEET_LIST sheets = schematic.Hierarchy();
    sheets.SortByPageNumbers();

    wxString out;
    out << wxS( "KICAD_SCHEMATIC_CONTEXT (all sheets)\n" );
    out << wxS( "Current sheet: " ) << m_frame->GetFullScreenDesc() << wxS( "\n" );
    out << wxS( "Sheet count: " ) << wxString::Format( wxS( "%d" ), (int) sheets.size() ) << wxS( "\n\n" );

    // Netlist-ish view built from per-pin connections.
    std::map<wxString, std::vector<wxString>> netToNodes;

    for( const SCH_SHEET_PATH& sheetPath : sheets )
    {
        SCH_SCREEN* screen = sheetPath.LastScreen();
        if( !screen )
            continue;

        out << wxS( "=== SHEET ===\n" );
        out << wxS( "Path: " ) << sheetPath.PathHumanReadable() << wxS( "\n" );
        out << wxS( "Page: " ) << sheetPath.GetPageNumber() << wxS( "\n" );
        if( sheetPath.Last() )
            out << wxS( "File: " ) << sheetPath.Last()->GetFileName() << wxS( "\n" );

        int componentCount = 0;
        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
            if( !symbol )
                continue;

            componentCount++;

            wxString ref = symbol->GetRef( &sheetPath, true );
            wxString libId = symbol->GetLibId().Format();

            wxString value;
            wxString footprint;
            wxString datasheet;

            SCH_FIELDS fields = symbol->GetFields();
            for( const SCH_FIELD& field : fields )
            {
                if( field.GetText().IsEmpty() )
                    continue;

                wxString name = field.GetName();
                if( name.IsEmpty() )
                    continue;

                if( name.CmpNoCase( wxS( "Value" ) ) == 0 )
                    value = field.GetText();
                else if( name.CmpNoCase( wxS( "Footprint" ) ) == 0 )
                    footprint = field.GetText();
                else if( name.CmpNoCase( wxS( "Datasheet" ) ) == 0 )
                    datasheet = field.GetText();
            }

            out << wxS( "- " ) << ref << wxS( " (" ) << libId << wxS( ")" );
            if( !value.IsEmpty() )
                out << wxS( " value=" ) << value;
            if( !footprint.IsEmpty() )
                out << wxS( " footprint=" ) << footprint;
            if( !datasheet.IsEmpty() )
                out << wxS( " datasheet=" ) << datasheet;
            out << wxS( "\n" );

            std::vector<SCH_PIN*> pins = symbol->GetPins( &sheetPath );
            for( SCH_PIN* pin : pins )
            {
                if( !pin )
                    continue;

                wxString pinNumber = pin->GetShownNumber();
                wxString pinName = pin->GetShownName();

                wxString netName = wxS( "<unconnected>" );
                if( SCH_CONNECTION* conn = pin->Connection( &sheetPath ) )
                {
                    netName = conn->Name();
                    if( netName.IsEmpty() )
                        netName = wxS( "<unnamed>" );
                }

                wxString node = ref + wxS( ":" ) + pinNumber;
                if( !pinName.IsEmpty() )
                    node << wxS( "(" ) << pinName << wxS( ")" );

                netToNodes[netName].push_back( node );
            }

            if( aMaxChars > 0 && (size_t) out.length() > aMaxChars )
            {
                out << wxS( "\n[TRUNCATED: schematic context exceeded size limit]\n" );
                return out;
            }
        }

        if( componentCount == 0 )
            out << wxS( "(no components)\n" );

        out << wxS( "\n" );
    }

    out << wxS( "=== NETS (from pin connections) ===\n" );
    for( const auto& kv : netToNodes )
    {
        const wxString& netName = kv.first;
        const std::vector<wxString>& nodes = kv.second;

        out << wxS( "* " ) << netName << wxS( ": " );
        for( size_t i = 0; i < nodes.size(); i++ )
        {
            out << nodes[i];
            if( i + 1 < nodes.size() )
                out << wxS( ", " );
        }
        out << wxS( "\n" );

        if( aMaxChars > 0 && (size_t) out.length() > aMaxChars )
        {
            out << wxS( "\n[TRUNCATED: schematic context exceeded size limit]\n" );
            return out;
        }
    }

    return out;
}




bool SCH_OLLAMA_AGENT_TOOL::ParseAndExecute( const wxString& aResponse )
{
    bool success = false;
    m_agent->BeginBatch();

    wxStringTokenizer tokenizer( aResponse, wxS( "\n" ) );
    std::set<std::string> unknownToolsLogged;
    
    while( tokenizer.HasMoreTokens() )
    {
        wxString line = tokenizer.GetNextToken().Trim();
        
        if( line.IsEmpty() || line.StartsWith( wxS( "#" ) ) )
            continue;

        wxString upperLine = line.Upper();

        if( upperLine.StartsWith( wxS( "TOOL" ) ) )
        {
            wxString rest = line.Mid( 4 );
            rest.Trim();
            rest.Trim( false );

            wxString toolName = rest.BeforeFirst( ' ' ).Trim();
            wxString payload;

            if( rest.Contains( wxS( " " ) ) )
            {
                payload = rest.AfterFirst( ' ' );
                payload.Trim();
                payload.Trim( false );
            }

            if( toolName.IsEmpty() )
                continue;

            wxString lowerTool = toolName;
            lowerTool.MakeLower();

            bool supportedTool =
                    lowerTool == wxS( "schematic.place_component" )
                    || lowerTool == wxS( "schematic.move_component" )
                    || lowerTool == wxS( "mock.selection_inspector" );

            if( !supportedTool )
            {
                std::string normalizedTool = lowerTool.ToStdString();

                if( unknownToolsLogged.insert( normalizedTool ).second )
                {
                    wxLogWarning( wxS( "[OllamaAgent] Unknown tool requested: %s" ),
                                  toolName.wx_str() );
                }

                continue;
            }

            if( m_toolCallHandler )
            {
                m_toolCallHandler->HandleToolCall( toolName, payload );
                success = true;
            }
            else if( ExecuteToolCommand( toolName, payload ) )
            {
                success = true;
            }

            continue;
        }

        // Parse JUNCTION command
        if( upperLine.StartsWith( wxS( "JUNCTION" ) ) )
        {
            double x, y;
            if( wxSscanf( line, wxS( "JUNCTION %lf %lf" ), &x, &y ) == 2 )
            {
                VECTOR2I pos( schIUScale.mmToIU( x ), schIUScale.mmToIU( y ) );
                m_agent->AddJunction( pos );
                success = true;
            }
        }
        // Parse WIRE command
        else if( upperLine.StartsWith( wxS( "WIRE" ) ) )
        {
            double x1, y1, x2, y2;
            if( wxSscanf( line, wxS( "WIRE %lf %lf %lf %lf" ), &x1, &y1, &x2, &y2 ) == 4 )
            {
                VECTOR2I start( schIUScale.mmToIU( x1 ), schIUScale.mmToIU( y1 ) );
                VECTOR2I end( schIUScale.mmToIU( x2 ), schIUScale.mmToIU( y2 ) );
                m_agent->AddWire( start, end );
                success = true;
            }
        }
        // Parse LABEL command
        else if( upperLine.StartsWith( wxS( "LABEL" ) ) )
        {
            double x, y;
            wxString text;
            if( wxSscanf( line, wxS( "LABEL %lf %lf" ), &x, &y ) == 2 )
            {
                // Extract text (may be quoted)
                int textStart = line.Find( wxS( "\"" ) );
                if( textStart != wxNOT_FOUND )
                {
                    int textEndInSub = line.Mid( textStart + 1 ).Find( wxS( "\"" ) );
                    if( textEndInSub != wxNOT_FOUND )
                    {
                        text = line.SubString( textStart + 1, textStart + textEndInSub );
                    }
                }
                else
                {
                    // No quotes, take rest of line
                    text = line.AfterFirst( ' ' ).AfterFirst( ' ' ).AfterFirst( ' ' );
                }

                if( !text.IsEmpty() )
                {
                    VECTOR2I pos( schIUScale.mmToIU( x ), schIUScale.mmToIU( y ) );
                    m_agent->AddLabel( pos, text );
                    success = true;
                }
            }
        }
        // Parse TEXT command
        else if( upperLine.StartsWith( wxS( "TEXT" ) ) )
        {
            double x, y;
            wxString text;
            if( wxSscanf( line, wxS( "TEXT %lf %lf" ), &x, &y ) == 2 )
            {
                int textStart = line.Find( wxS( "\"" ) );
                if( textStart != wxNOT_FOUND )
                {
                    int textEndInSub = line.Mid( textStart + 1 ).Find( wxS( "\"" ) );
                    if( textEndInSub != wxNOT_FOUND )
                    {
                        text = line.SubString( textStart + 1, textStart + textEndInSub );
                    }
                }
                else
                {
                    text = line.AfterFirst( ' ' ).AfterFirst( ' ' ).AfterFirst( ' ' );
                }

                if( !text.IsEmpty() )
                {
                    VECTOR2I pos( schIUScale.mmToIU( x ), schIUScale.mmToIU( y ) );
                    m_agent->AddText( pos, text );
                    success = true;
                }
            }
        }
    }

    m_agent->EndBatch( _( "Ollama agent operation" ) );
    return success;
}




bool SCH_OLLAMA_AGENT_TOOL::ExecuteToolCommand( const wxString& aToolName, const wxString& aPayload )
{
    if( aToolName.CmpNoCase( wxS( "mock.selection_inspector" ) ) == 0 )
    {
        wxLogMessage( wxS( "[OllamaAgent] mock tool '%s' invoked with payload: %s" ),
                      aToolName.wx_str(), aPayload.wx_str() );
        return true;
    }

    if( aToolName.CmpNoCase( wxS( "schematic.place_component" ) ) == 0 )
    {
        try
        {
            json payload = aPayload.IsEmpty() ? json::object() : json::parse( aPayload.ToStdString() );
            return HandlePlaceComponentTool( payload );
        }
        catch( const json::exception& e )
        {
            wxLogWarning( wxS( "[OllamaAgent] place_component payload parse error: %s" ),
                          wxString::FromUTF8( e.what() ) );
            return false;
        }
    }

    if( aToolName.CmpNoCase( wxS( "schematic.move_component" ) ) == 0 )
    {
        try
        {
            json payload = aPayload.IsEmpty() ? json::object() : json::parse( aPayload.ToStdString() );
            return HandleMoveComponentTool( payload );
        }
        catch( const json::exception& e )
        {
            wxLogWarning( wxS( "[OllamaAgent] move_component payload parse error: %s" ),
                          wxString::FromUTF8( e.what() ) );
            return false;
        }
    }

    if( aToolName.CmpNoCase( wxS( "schematic.add_wire" ) ) == 0 )
    {
        try
        {
            json payload = aPayload.IsEmpty() ? json::object() : json::parse( aPayload.ToStdString() );
            return HandleAddWireTool( payload );
        }
        catch( const json::exception& e )
        {
            wxLogWarning( wxS( "[OllamaAgent] add_wire payload parse error: %s" ),
                          wxString::FromUTF8( e.what() ) );
            return false;
        }
    }

    wxLogWarning( wxS( "[OllamaAgent] Unknown tool requested: %s" ), aToolName.wx_str() );
    return false;
}


bool SCH_OLLAMA_AGENT_TOOL::HandlePlaceComponentTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    auto getString = [&]( const char* aKey ) -> std::optional<wxString>
    {
        if( aPayload.contains( aKey ) && aPayload[aKey].is_string() )
            return wxString::FromUTF8( aPayload[aKey].get<std::string>() );

        return std::nullopt;
    };

    std::optional<wxString> symbolIdOpt = getString( "symbol" );

    if( !symbolIdOpt || symbolIdOpt->IsEmpty() )
    {
        DisplayError( m_frame, _( "place_component tool requires a \"symbol\" field such as \"Device:R\"." ) );
        return false;
    }

    wxString symbolId = *symbolIdOpt;
    symbolId.Trim( true ).Trim( false );

    LIB_ID libId;
    UTF8 utfSymbol( symbolId.ToStdString().c_str() );

    if( libId.Parse( utfSymbol ) >= 0 || !libId.IsValid() )
    {
        DisplayError( m_frame,
                      wxString::Format( _( "Unable to parse library identifier \"%s\". Use libnick:symbol_name." ),
                                        symbolId ) );
        return false;
    }

    LIB_SYMBOL* libSymbol = m_frame->GetLibSymbol( libId );

    if( !libSymbol )
    {
        DisplayError( m_frame,
                      wxString::Format( _( "Symbol \"%s\" not found in the current library tables." ),
                                        symbolId ) );
        return false;
    }

    double xMm = aPayload.value( "x", 0.0 );
    double yMm = aPayload.value( "y", 0.0 );
    int unit = aPayload.value( "unit", 1 );
    int bodyStyle = aPayload.value( "body_style", 1 );
    double rotation = aPayload.value( "rotation", 0.0 );

    VECTOR2I pos( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) );
    SCH_SHEET_PATH& sheet = m_frame->GetCurrentSheet();
    SCH_SCREEN* screen = sheet.LastScreen();

    if( !screen )
        return false;

    SCH_SYMBOL* newSymbol =
            new SCH_SYMBOL( *libSymbol, libId, &sheet, unit, bodyStyle, pos, &m_frame->Schematic() );

    newSymbol->SetPosition( pos );

    std::optional<wxString> referenceOpt = getString( "reference" );
    if( referenceOpt && !referenceOpt->IsEmpty() )
        newSymbol->SetRef( &sheet, *referenceOpt );

    int orientation = 0;

    if( rotation != 0.0 )
    {
        int snapped = static_cast<int>( std::round( rotation / 90.0 ) * 90 ) % 360;
        if( snapped < 0 )
            snapped += 360;

        switch( snapped )
        {
        case 0: orientation = SYM_ORIENT_0; break;
        case 90: orientation = SYM_ORIENT_90; break;
        case 180: orientation = SYM_ORIENT_180; break;
        case 270: orientation = SYM_ORIENT_270; break;
        default: orientation = SYM_ORIENT_0; break;
        }

        newSymbol->SetOrientation( orientation );
    }

    if( m_frame->eeconfig()->m_AutoplaceFields.enable )
        newSymbol->AutoplaceFields( screen, AUTOPLACE_AUTO );

    SCH_COMMIT commit( m_frame );
    commit.Added( newSymbol, screen );
    commit.Push( _( "Place component" ) );

    if( m_frame->GetToolManager() )
    {
        m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, newSymbol );
    }

    return true;
}


bool SCH_OLLAMA_AGENT_TOOL::HandleMoveComponentTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    auto getString = [&]( const char* aKey ) -> std::optional<wxString>
    {
        if( aPayload.contains( aKey ) && aPayload[aKey].is_string() )
            return wxString::FromUTF8( aPayload[aKey].get<std::string>() );

        return std::nullopt;
    };

    std::optional<wxString> referenceOpt = getString( "reference" );

    if( !referenceOpt || referenceOpt->IsEmpty() )
    {
        DisplayError( m_frame, _( "move_component tool requires a \"reference\" field." ) );
        return false;
    }

    wxString reference = *referenceOpt;
    reference.Trim( true ).Trim( false );

    // Find the symbol by reference
    SCH_SYMBOL* symbol = nullptr;
    SCH_SHEET_PATH symbolSheet;
    SCH_SHEET_LIST hierarchy = m_frame->Schematic().Hierarchy();

    for( SCH_SHEET_PATH& sheet : hierarchy )
    {
        SCH_SCREEN* screen = sheet.LastScreen();
        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* candidate = static_cast<SCH_SYMBOL*>( item );
            wxString candidateRef = candidate->GetRef( &sheet, false );

            if( candidateRef.CmpNoCase( reference ) == 0 )
            {
                symbol = candidate;
                symbolSheet = sheet;
                break;
            }
        }

        if( symbol )
            break;
    }

    if( !symbol )
    {
        DisplayError( m_frame,
                      wxString::Format( _( "Component with reference \"%s\" not found." ), reference ) );
        return false;
    }

    // Get new position
    double xMm = aPayload.value( "x", 0.0 );
    double yMm = aPayload.value( "y", 0.0 );

    VECTOR2I newPos( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) );
    VECTOR2I currentPos = symbol->GetPosition();
    VECTOR2I delta = newPos - currentPos;

    // Move the symbol
    SCH_SCREEN* screen = symbolSheet.LastScreen();
    if( !screen )
        return false;

    SCH_COMMIT commit( m_frame );
    commit.Modify( symbol, screen );
    symbol->Move( delta );

    commit.Push( wxString::Format( _( "Move component %s" ), reference ) );

    if( m_frame->GetToolManager() )
    {
        m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, symbol );
    }

    return true;
}

bool SCH_OLLAMA_AGENT_TOOL::HandleAddWireTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    struct PIN_LOC
    {
        VECTOR2I pos;
        SCH_SCREEN* screen = nullptr;
    };

    // Resolve pin locations on the CURRENT sheet only (keeps results visible; avoids cross-sheet surprises).
    auto findPinLocOnCurrentSheet = [&]( const wxString& aReference, const wxString& aPin ) -> std::optional<PIN_LOC>
    {
        wxString ref = aReference;
        wxString pinKey = aPin;
        ref.Trim( true ).Trim( false );
        pinKey.Trim( true ).Trim( false );

        if( ref.IsEmpty() || pinKey.IsEmpty() )
            return std::nullopt;

        SCH_SHEET_PATH& sheet = m_frame->GetCurrentSheet();
        SCH_SCREEN* sc = sheet.LastScreen();
        if( !sc )
            return std::nullopt;

        for( SCH_ITEM* item : sc->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            if( !sym )
                continue;

            wxString candidateRef = sym->GetRef( &sheet, false );
            if( candidateRef.CmpNoCase( ref ) != 0 )
                continue;

            std::vector<SCH_PIN*> pins = sym->GetPins( &sheet );
            for( SCH_PIN* pin : pins )
            {
                if( !pin )
                    continue;

                wxString name = pin->GetShownName();
                wxString number = pin->GetShownNumber();

                if( ( !name.IsEmpty() && name.CmpNoCase( pinKey ) == 0 )
                    || ( !number.IsEmpty() && number.CmpNoCase( pinKey ) == 0 ) )
                {
                    return PIN_LOC{ pin->GetPosition(), sc };
                }
            }
        }

        return std::nullopt;
    };

    VECTOR2I start;
    VECTOR2I end;
    SCH_SCREEN* targetScreen = nullptr;

    // Mode A: explicit coordinates (mm)
    if( aPayload.contains( "x1" ) && aPayload.contains( "y1" ) && aPayload.contains( "x2" ) && aPayload.contains( "y2" ) )
    {
        if( !aPayload["x1"].is_number() || !aPayload["y1"].is_number() || !aPayload["x2"].is_number() || !aPayload["y2"].is_number() )
        {
            DisplayError( m_frame, _( "add_wire tool fields x1, y1, x2, y2 must be numbers (mm)." ) );
            return false;
        }

        double x1Mm = aPayload["x1"].get<double>();
        double y1Mm = aPayload["y1"].get<double>();
        double x2Mm = aPayload["x2"].get<double>();
        double y2Mm = aPayload["y2"].get<double>();

        start = VECTOR2I( schIUScale.mmToIU( x1Mm ), schIUScale.mmToIU( y1Mm ) );
        end   = VECTOR2I( schIUScale.mmToIU( x2Mm ), schIUScale.mmToIU( y2Mm ) );
        targetScreen = m_frame->GetCurrentSheet().LastScreen();
    }
    // Mode B: pin-to-pin
    else if( aPayload.contains( "from" ) && aPayload.contains( "to" ) && aPayload["from"].is_object() && aPayload["to"].is_object() )
    {
        const json& from = aPayload["from"];
        const json& to = aPayload["to"];

        if( !from.contains( "reference" ) || !from.contains( "pin" ) || !to.contains( "reference" ) || !to.contains( "pin" ) )
        {
            DisplayError( m_frame, _( "add_wire pin mode requires: from{reference,pin}, to{reference,pin}." ) );
            return false;
        }

        if( !from["reference"].is_string() || !from["pin"].is_string() || !to["reference"].is_string() || !to["pin"].is_string() )
        {
            DisplayError( m_frame, _( "add_wire pin mode fields must be strings." ) );
            return false;
        }

        wxString fromRef = wxString::FromUTF8( from["reference"].get<std::string>() );
        wxString fromPin = wxString::FromUTF8( from["pin"].get<std::string>() );
        wxString toRef = wxString::FromUTF8( to["reference"].get<std::string>() );
        wxString toPin = wxString::FromUTF8( to["pin"].get<std::string>() );

        auto startOpt = findPinLocOnCurrentSheet( fromRef, fromPin );
        auto endOpt = findPinLocOnCurrentSheet( toRef, toPin );

        if( !startOpt || !endOpt )
        {
            DisplayError( m_frame, _( "add_wire: could not resolve one or both pin locations on the current sheet." ) );
            return false;
        }

        start = startOpt->pos;
        end = endOpt->pos;
        targetScreen = startOpt->screen;

        if( !targetScreen || endOpt->screen != targetScreen )
            return false;
    }
    else
    {
        DisplayError( m_frame, _( "add_wire requires either x1,y1,x2,y2 (mm) or from/to pin objects." ) );
        return false;
    }

    if( !targetScreen )
        return false;

    // Optional: prefer net labels for long connections if net name is provided.
    wxString netName;
    if( aPayload.contains( "net" ) && aPayload["net"].is_string() )
        netName = wxString::FromUTF8( aPayload["net"].get<std::string>() );

    long long manhattanIU = llabs( (long long) ( end.x - start.x ) ) + llabs( (long long) ( end.y - start.y ) );
    const long long labelThresholdIU = schIUScale.mmToIU( 60.0 ); // ~60mm

    if( !netName.IsEmpty() && manhattanIU > labelThresholdIU )
    {
        // Place labels at both endpoints; local labels will connect nets within the sheet.
        SCH_LABEL* l1 = new SCH_LABEL();
        l1->SetPosition( start );
        l1->SetText( netName );
        l1->SetParent( targetScreen );

        SCH_LABEL* l2 = new SCH_LABEL();
        l2->SetPosition( end );
        l2->SetText( netName );
        l2->SetParent( targetScreen );

        SCH_COMMIT commit( m_frame );
        commit.Added( l1, targetScreen );
        commit.Added( l2, targetScreen );
        commit.Push( _( "Add net labels" ) );

        if( m_frame->GetToolManager() )
        {
            m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, l1 );
            m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, l2 );
        }

        if( m_frame->GetCanvas() )
            m_frame->GetCanvas()->Refresh();

        return true;
    }

    // Default: orthogonal (Manhattan) routing to avoid diagonal wires.
    auto addWireSeg = [&]( SCH_COMMIT& aCommit, const VECTOR2I& aA, const VECTOR2I& aB ) -> SCH_LINE*
    {
        SCH_LINE* w = new SCH_LINE();
        w->SetStartPoint( aA );
        w->SetEndPoint( aB );
        w->SetLayer( LAYER_WIRE );
        w->SetStroke( STROKE_PARAMS() );
        w->SetParent( targetScreen );
        aCommit.Added( w, targetScreen );
        return w;
    };

    auto segmentHitsSymbol = [&]( const VECTOR2I& aA, const VECTOR2I& aB ) -> int
    {
        if( aA == aB )
            return 0;

        // Only score axis-aligned segments.
        const bool vertical = aA.x == aB.x;
        const bool horizontal = aA.y == aB.y;
        if( !vertical && !horizontal )
            return 0;

        int hits = 0;

        for( SCH_ITEM* item : targetScreen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            if( !sym )
                continue;

            BOX2I bbox = sym->GetBoundingBox();

            // Expand by ~1mm margin
            const int margin = schIUScale.mmToIU( 1.0 );
            bbox.Inflate( margin, margin );

            const int xMin = bbox.GetX();
            const int xMax = bbox.GetRight();
            const int yMin = bbox.GetY();
            const int yMax = bbox.GetBottom();

            if( vertical )
            {
                const int x = aA.x;
                const int y1 = std::min( aA.y, aB.y );
                const int y2 = std::max( aA.y, aB.y );
                if( x >= xMin && x <= xMax && !( y2 < yMin || y1 > yMax ) )
                    hits++;
            }
            else if( horizontal )
            {
                const int y = aA.y;
                const int x1 = std::min( aA.x, aB.x );
                const int x2 = std::max( aA.x, aB.x );
                if( y >= yMin && y <= yMax && !( x2 < xMin || x1 > xMax ) )
                    hits++;
            }
        }

        return hits;
    };

    VECTOR2I bend1( end.x, start.y );
    VECTOR2I bend2( start.x, end.y );

    int score1 = segmentHitsSymbol( start, bend1 ) + segmentHitsSymbol( bend1, end );
    int score2 = segmentHitsSymbol( start, bend2 ) + segmentHitsSymbol( bend2, end );

    VECTOR2I bend = ( score2 < score1 ) ? bend2 : bend1;

    SCH_COMMIT commit( m_frame );
    std::vector<SCH_LINE*> newWires;

    if( start.x == end.x || start.y == end.y )
    {
        newWires.push_back( addWireSeg( commit, start, end ) );
    }
    else
    {
        newWires.push_back( addWireSeg( commit, start, bend ) );
        newWires.push_back( addWireSeg( commit, bend, end ) );
    }

    commit.Push( _( "Add wire" ) );

    if( m_frame->GetToolManager() )
    {
        for( SCH_LINE* w : newWires )
            m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, w );
    }

    // Ensure the canvas refreshes so the new wire is visible immediately.
    if( m_frame->GetCanvas() )
    {
        if( auto view = m_frame->GetCanvas()->GetView() )
        {
            for( SCH_LINE* w : newWires )
                view->Update( w );
        }

        m_frame->GetCanvas()->Refresh();
    }

    return true;
}


void SCH_OLLAMA_AGENT_TOOL::setTransitions()
{
    Go( &SCH_OLLAMA_AGENT_TOOL::ProcessRequest, SCH_ACTIONS::ollamaAgentRequest.MakeEvent() );
    Go( &SCH_OLLAMA_AGENT_TOOL::ShowAgentDialog, SCH_ACTIONS::ollamaAgentDialog.MakeEvent() );
}


bool SCH_OLLAMA_AGENT_TOOL::RunToolCommand( const wxString& aToolName, const wxString& aPayload )
{
    return ExecuteToolCommand( aToolName, aPayload );
}
