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
#include <nlohmann/json.hpp>
#include <set>

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

    // Send raw user request to Python agent (which handles all prompt building)
    wxString response;
    if( !m_ollama->ChatCompletion( m_model, userRequest, response ) )
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
