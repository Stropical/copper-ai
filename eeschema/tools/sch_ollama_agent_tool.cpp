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
#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>
#include <nlohmann/json.hpp>
#include <set>
#include <map>
#include <vector>
#include <limits>

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
    std::vector<SCH_GLOBALLABEL*> globalLabels;
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
        case SCH_GLOBAL_LABEL_T:
            globalLabels.push_back( static_cast<SCH_GLOBALLABEL*>( item ) );
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
            
            BOX2I bbox = symbol->GetBoundingBox();
            double bxmin = schIUScale.IUTomm( bbox.GetX() );
            double bymin = schIUScale.IUTomm( bbox.GetY() );
            double bxmax = schIUScale.IUTomm( bbox.GetRight() );
            double bymax = schIUScale.IUTomm( bbox.GetBottom() );
            double width = bxmax - bxmin;
            double height = bymax - bymin;
            
            content << wxString::Format( wxS( "  - Component %s (%s) at (%.2f, %.2f) mm, size=(%.2f, %.2f)mm\n" ),
                                        ref, libId, x_mm, y_mm, width, height );
            
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

    // Format global labels
    if( !globalLabels.empty() )
    {
        content << wxS( "Global Labels:\n" );
        for( SCH_GLOBALLABEL* label : globalLabels )
        {
            VECTOR2I pos = label->GetPosition();
            double x_mm = schIUScale.IUTomm( pos.x );
            double y_mm = schIUScale.IUTomm( pos.y );
            wxString labelText = label->GetText();

            wxString netName = wxS( "" );
            if( SCH_CONNECTION* conn = label->Connection( &sheet ) )
                netName = conn->Name();

            if( !netName.IsEmpty() && netName != labelText )
            {
                content << wxString::Format( wxS( "  - Global Label \"%s\" at (%.2f, %.2f) mm (net: %s)\n" ),
                                             labelText, x_mm, y_mm, netName );
            }
            else
            {
                content << wxString::Format( wxS( "  - Global Label \"%s\" at (%.2f, %.2f) mm\n" ),
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

    if( symbols.empty() && junctions.empty() && wires.empty() && labels.empty() && globalLabels.empty() && texts.empty() )
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
    out << wxS( "Coordinate system: mm. +X is right. +Y is down. Values are schematic sheet coordinates.\n" );
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
            VECTOR2I symPos = symbol->GetPosition();
            double sx = schIUScale.IUTomm( symPos.x );
            double sy = schIUScale.IUTomm( symPos.y );
            int orientProp = static_cast<int>( symbol->GetOrientationProp() ); // 0/90/180/270

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

            BOX2I bbox = symbol->GetBoundingBox();
            double bxmin = schIUScale.IUTomm( bbox.GetX() );
            double bymin = schIUScale.IUTomm( bbox.GetY() );
            double bxmax = schIUScale.IUTomm( bbox.GetRight() );
            double bymax = schIUScale.IUTomm( bbox.GetBottom() );
            double width = bxmax - bxmin;
            double height = bymax - bymin;

            out << wxString::Format( wxS( " - %s (%s) value=%s footprint=%s datasheet=%s pos=(%.2f, %.2f) rot=%d size=(%.2f, %.2f)mm bbox=(%.2f, %.2f, %.2f, %.2f)\n" ),
                                     ref, libId, value, footprint, datasheet, sx, sy, orientProp, width, height, bxmin, bymin, bxmax, bymax );

            std::vector<SCH_PIN*> pins = symbol->GetPins( &sheetPath );
            for( SCH_PIN* pin : pins )
            {
                if( !pin )
                    continue;

                wxString pinNumber = pin->GetShownNumber();
                wxString pinName = pin->GetShownName();
                VECTOR2I pinPos = pin->GetPosition();
                double px = schIUScale.IUTomm( pinPos.x );
                double py = schIUScale.IUTomm( pinPos.y );

                wxString pinOrient = wxS( "UNKNOWN" );
                switch( pin->GetOrientation() )
                {
                default:
                    break;
                case PIN_ORIENTATION::PIN_RIGHT: pinOrient = wxS( "RIGHT" ); break;
                case PIN_ORIENTATION::PIN_LEFT:  pinOrient = wxS( "LEFT" ); break;
                case PIN_ORIENTATION::PIN_UP:    pinOrient = wxS( "UP" ); break;
                case PIN_ORIENTATION::PIN_DOWN:  pinOrient = wxS( "DOWN" ); break;
                }

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
                node << wxString::Format( wxS( "@(%.2f,%.2f,%s)" ), px, py, pinOrient );

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

            // Bug fix: Updated to include all tools that ExecuteToolCommand actually handles
            bool supportedTool =
                    lowerTool == wxS( "schematic.place_component" )
                    || lowerTool == wxS( "schematic.move_component" )
                    || lowerTool == wxS( "schematic.add_wire" )
                    || lowerTool == wxS( "schematic.add_net_label" )
                    || lowerTool == wxS( "schematic.add_global_label" )
                    || lowerTool == wxS( "schematic.add_label" )
                    || lowerTool == wxS( "schematic.connect_with_net_label" )
                    || lowerTool == wxS( "schematic.connect_with_global_label" )
                    || lowerTool == wxS( "schematic.get_datasheet" )
            || lowerTool == wxS( "schematic.get_symbol_info" )
                    || lowerTool == wxS( "schematic.search_symbol" )
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
    m_lastToolError.clear();
    m_lastToolResult.clear();

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

    if( aToolName.CmpNoCase( wxS( "schematic.add_net_label" ) ) == 0
        || aToolName.CmpNoCase( wxS( "schematic.add_global_label" ) ) == 0
        || aToolName.CmpNoCase( wxS( "schematic.add_label" ) ) == 0 )
    {
        try
        {
            json payload = aPayload.IsEmpty() ? json::object() : json::parse( aPayload.ToStdString() );
            return HandleAddNetLabelTool( payload );
        }
        catch( const json::exception& e )
        {
            m_lastToolError = wxString::Format( _( "add_label payload parse error: %s" ),
                                                wxString::FromUTF8( e.what() ) );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }
    }

    if( aToolName.CmpNoCase( wxS( "schematic.connect_with_net_label" ) ) == 0
        || aToolName.CmpNoCase( wxS( "schematic.connect_with_global_label" ) ) == 0 )
    {
        try
        {
            json payload = aPayload.IsEmpty() ? json::object() : json::parse( aPayload.ToStdString() );
            return HandleConnectWithNetLabelTool( payload );
        }
        catch( const json::exception& e )
        {
            m_lastToolError = wxString::Format( _( "connect_with_net_label payload parse error: %s" ),
                                                wxString::FromUTF8( e.what() ) );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }
    }

    if( aToolName.CmpNoCase( wxS( "schematic.get_datasheet" ) ) == 0 )
    {
        try
        {
            json payload = aPayload.IsEmpty() ? json::object() : json::parse( aPayload.ToStdString() );
            return HandleGetDatasheetTool( payload );
        }
        catch( const json::exception& e )
        {
            m_lastToolError = wxString::Format( _( "get_datasheet payload parse error: %s" ),
                                                wxString::FromUTF8( e.what() ) );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }
    }

    if( aToolName.CmpNoCase( wxS( "schematic.search_symbol" ) ) == 0 )
    {
        try
        {
            json payload = aPayload.IsEmpty() ? json::object() : json::parse( aPayload.ToStdString() );
            return HandleSearchSymbolTool( payload );
        }
        catch( const json::exception& e )
        {
            m_lastToolError = wxString::Format( _( "search_symbol payload parse error: %s" ),
                                                wxString::FromUTF8( e.what() ) );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }
    }

    if( aToolName.CmpNoCase( wxS( "schematic.get_symbol_info" ) ) == 0 )
    {
        try
        {
            json payload = aPayload.IsEmpty() ? json::object() : json::parse( aPayload.ToStdString() );
            return HandleGetSymbolInfoTool( payload );
        }
        catch( const json::exception& e )
        {
            m_lastToolError = wxString::Format( _( "get_symbol_info payload parse error: %s" ),
                                                wxString::FromUTF8( e.what() ) );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
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
    m_lastToolError = wxString::Format( _( "Unknown tool requested: %s" ), aToolName );
    return false;
}


bool SCH_OLLAMA_AGENT_TOOL::HandleGetDatasheetTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    if( !aPayload.contains( "reference" ) || !aPayload["reference"].is_string() )
    {
        m_lastToolError = _( "get_datasheet requires \"reference\" (string), e.g. {\"reference\":\"U7\"}." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    wxString reference = wxString::FromUTF8( aPayload["reference"].get<std::string>() );
    reference.Trim( true ).Trim( false );

    if( reference.IsEmpty() )
    {
        m_lastToolError = _( "get_datasheet requires a non-empty reference." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    SYMBOL_MATCH match = findSymbolByRefOrValue( reference );
    SCH_SYMBOL* found = match.symbol;
    SCH_SHEET_PATH foundSheet = match.sheet;

    if( !found )
    {
        m_lastToolError = wxString::Format( _( "get_datasheet: component \"%s\" not found." ), reference );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    wxString value;
    wxString footprint;
    wxString datasheet;

    if( SCH_FIELD* f = found->GetField( FIELD_T::VALUE ) )
        value = f->GetText();
    if( SCH_FIELD* f = found->GetField( FIELD_T::FOOTPRINT ) )
        footprint = f->GetText();
    if( SCH_FIELD* f = found->GetField( FIELD_T::DATASHEET ) )
        datasheet = f->GetText();

    json out = json::object();
    out["reference"] = reference.ToStdString();
    out["value"] = value.ToStdString();
    out["footprint"] = footprint.ToStdString();
    out["datasheet"] = datasheet.ToStdString();
    out["sheet_path"] = foundSheet.PathHumanReadable( false, true ).ToStdString();

    m_lastToolResult = wxString::FromUTF8( out.dump( 2 ) );
    return true;
}


bool SCH_OLLAMA_AGENT_TOOL::HandleSearchSymbolTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    wxString query;
    if( aPayload.contains( "query" ) && aPayload["query"].is_string() )
        query = wxString::FromUTF8( aPayload["query"].get<std::string>() );

    query.Trim( true ).Trim( false );

    int limit = 10;
    if( aPayload.contains( "limit" ) && aPayload["limit"].is_number_integer() )
        limit = std::max( 1, std::min( 50, aPayload["limit"].get<int>() ) );

    if( query.IsEmpty() )
    {
        m_lastToolError = _( "search_symbol requires \"query\" (string)." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &m_frame->Prj() );
    if( !adapter )
    {
        m_lastToolError = _( "search_symbol: symbol library adapter not available." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    auto normalize = [&]( const wxString& s ) -> wxString
    {
        // Lowercase, strip non-alnum, and normalize numeric runs by stripping leading zeros.
        wxString in = s.Lower();
        wxString out;
        out.reserve( in.length() );

        auto flushNumber = [&]( wxString& num )
        {
            if( num.IsEmpty() )
                return;

            // Strip leading zeros but keep a single zero if the number is all zeros.
            size_t i = 0;
            while( i + 1 < num.length() && num[i] == '0' )
                i++;
            out << num.Mid( i );
            num.Clear();
        };

        wxString num;
        for( size_t i = 0; i < in.length(); i++ )
        {
            const wxChar c = in[i];
            if( wxIsdigit( c ) )
            {
                num << c;
                continue;
            }

            flushNumber( num );

            if( wxIsalnum( c ) )
                out << c;
        }

        flushNumber( num );
        return out;
    };

    wxString qLower = query.Lower();
    wxString qAfterColon;
    if( query.Contains( wxS( ":" ) ) )
        qAfterColon = query.AfterFirst( ':' ).Trim();

    wxString qNorm = normalize( query );
    wxString qAfterNorm;
    if( !qAfterColon.IsEmpty() )
        qAfterNorm = normalize( qAfterColon );

    struct MATCH
    {
        int score = 0;
        wxString lib;
        wxString name;
    };

    std::vector<MATCH> matches;

    // Enumerate libraries and symbol names (can be expensive; keep limit small).
    std::vector<wxString> libs = adapter->GetLibraryNames();
    for( const wxString& lib : libs )
    {
        // Load if needed so enumeration works.
        adapter->LoadOne( lib );

        std::vector<wxString> names = adapter->GetSymbolNames( lib );
        for( const wxString& name : names )
        {
            wxString nLower = name.Lower();
            wxString nNorm = normalize( name );

            auto scoreAgainst = [&]( const wxString& q, const wxString& qn ) -> int
            {
                if( q.IsEmpty() && qn.IsEmpty() )
                    return 0;

                int s = 0;

                if( !q.IsEmpty() )
                {
                    if( nLower == q )
                        s = std::max( s, 1000 );
                    else if( nLower.StartsWith( q ) )
                        s = std::max( s, 900 );
                    else if( nLower.Find( q ) != wxNOT_FOUND )
                        s = std::max( s, 700 );
                }

                if( !qn.IsEmpty() )
                {
                    if( nNorm == qn )
                        s = std::max( s, 980 );
                    else if( nNorm.StartsWith( qn ) )
                        s = std::max( s, 880 );
                    else if( nNorm.Find( qn ) != wxNOT_FOUND )
                        s = std::max( s, 680 );
                }

                return s;
            };

            int score = 0;
            score = std::max( score, scoreAgainst( qLower, qNorm ) );
            score = std::max( score, scoreAgainst( qAfterColon.Lower(), qAfterNorm ) );

            if( score > 0 )
                matches.push_back( MATCH{ score, lib, name } );
        }
    }

    std::sort( matches.begin(), matches.end(),
               []( const MATCH& a, const MATCH& b )
               {
                   if( a.score != b.score )
                       return a.score > b.score;
                   if( a.lib != b.lib )
                       return a.lib < b.lib;
                   return a.name < b.name;
               } );

    if( (int) matches.size() > limit )
        matches.resize( limit );

    json out = json::object();
    out["query"] = query.ToStdString();
    out["count"] = (int) matches.size();
    out["matches"] = json::array();

    for( const MATCH& m : matches )
    {
        json row = json::object();
        row["library"] = m.lib.ToStdString();
        row["name"] = m.name.ToStdString();
        row["lib_id"] = ( m.lib + wxS( ":" ) + m.name ).ToStdString();
        row["score"] = m.score;
        out["matches"].push_back( row );
    }

    m_lastToolResult = wxString::FromUTF8( out.dump( 2 ) );
    return true;
}


bool SCH_OLLAMA_AGENT_TOOL::HandleGetSymbolInfoTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    if( !aPayload.contains( "symbol" ) || !aPayload["symbol"].is_string() )
    {
        m_lastToolError = _( "get_symbol_info requires \"symbol\" (string), e.g. {\"symbol\":\"Device:R\"}." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    wxString symbolId = wxString::FromUTF8( aPayload["symbol"].get<std::string>() );
    symbolId.Trim( true ).Trim( false );

    if( symbolId.IsEmpty() )
    {
        m_lastToolError = _( "get_symbol_info requires a non-empty symbol identifier (libnick:symbol_name)." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    LIB_ID libId;
    UTF8 utfSymbol( symbolId.ToStdString().c_str() );

    if( libId.Parse( utfSymbol ) >= 0 || !libId.IsValid() )
    {
        m_lastToolError = wxString::Format( _( "Unable to parse library identifier \"%s\". Use libnick:symbol_name." ),
                                            symbolId );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    LIB_SYMBOL* libSymbol = m_frame->GetLibSymbol( libId );
    if( !libSymbol )
    {
        m_lastToolError = wxString::Format( _( "Symbol \"%s\" not found in the current library tables." ), symbolId );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    wxString description = libSymbol->GetDescription();
    wxString keywords = libSymbol->GetKeyWords();
    wxString docFile = libSymbol->GetDatasheetProp();

    json out = json::object();
    out["symbol"] = symbolId.ToStdString();
    out["library"] = libId.GetLibNickname();
    out["name"] = libId.GetLibItemName();
    out["description"] = description.ToStdString();
    out["keywords"] = keywords.ToStdString();
    out["datasheet"] = docFile.ToStdString();

    // Provide pin count summary
    json pins = json::array();
    const std::vector<SCH_PIN*> pinList = libSymbol->GetPins();
    for( const SCH_PIN* p : pinList )
    {
        if( !p )
            continue;
        json pj = json::object();
        pj["number"] = p->GetNumber().ToStdString();
        pj["name"] = p->GetName().ToStdString();
        pj["type"] = p->GetElectricalTypeName().ToStdString();
        pins.push_back( pj );
    }
    out["pin_count"] = (int) pins.size();
    out["pins"] = pins;

    m_lastToolResult = wxString::FromUTF8( out.dump( 2 ) );
    return true;
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
        m_lastToolError = _( "place_component tool requires a \"symbol\" field such as \"Device:R\"." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    wxString symbolId = *symbolIdOpt;
    symbolId.Trim( true ).Trim( false );

    // Allow passing a part number (e.g. "MCP2551") without the "LibNick:SymbolName" prefix.
    // If no lib nickname is provided, use schematic.search_symbol to resolve the best match.
    if( !symbolId.Contains( wxS( ":" ) ) )
    {
        try
        {
            json q = json::object();
            q["query"] = symbolId.ToStdString();
            q["limit"] = 10;

            if( HandleSearchSymbolTool( q ) && !m_lastToolResult.IsEmpty() )
            {
                json r = json::parse( m_lastToolResult.ToStdString() );
                if( r.contains( "matches" ) && r["matches"].is_array() && !r["matches"].empty()
                    && r["matches"][0].is_object() && r["matches"][0].contains( "lib_id" )
                    && r["matches"][0]["lib_id"].is_string() )
                {
                    wxString resolved = wxString::FromUTF8( r["matches"][0]["lib_id"].get<std::string>() );
                    resolved.Trim( true ).Trim( false );
                    if( !resolved.IsEmpty() && resolved.Contains( wxS( ":" ) ) )
                    {
                        wxLogMessage( wxS( "[OllamaAgent] Resolved symbol \"%s\" -> \"%s\"" ),
                                      symbolId.wx_str(), resolved.wx_str() );
                        symbolId = resolved;
                    }
                }
            }
        }
        catch( ... )
        {
            // Fall back to parse error below if we couldn't resolve.
        }
    }

    LIB_ID libId;
    UTF8 utfSymbol( symbolId.ToStdString().c_str() );

    if( libId.Parse( utfSymbol ) >= 0 || !libId.IsValid() )
    {
        m_lastToolError = wxString::Format( _( "Unable to parse library identifier \"%s\". Use libnick:symbol_name." ),
                                            symbolId );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    LIB_SYMBOL* libSymbol = m_frame->GetLibSymbol( libId );

    if( !libSymbol )
    {
        // Provide actionable suggestions by searching symbol names across loaded libraries.
        wxString hint;
        try
        {
            json q = json::object();
            q["query"] = symbolId.ToStdString();
            q["limit"] = 8;
            if( HandleSearchSymbolTool( q ) && !m_lastToolResult.IsEmpty() )
                hint = wxS( "\nSuggestions (use the exact lib_id):\n" ) + m_lastToolResult;

            // If the user/model provided "LibNick:SymbolName" but the lib nickname is wrong,
            // also try searching by just the symbol name portion.
            if( hint.IsEmpty() && symbolId.Contains( wxS( ":" ) ) )
            {
                json q2 = json::object();
                q2["query"] = symbolId.AfterFirst( ':' ).ToStdString();
                q2["limit"] = 8;
                if( HandleSearchSymbolTool( q2 ) && !m_lastToolResult.IsEmpty() )
                    hint = wxS( "\nSuggestions (use the exact lib_id):\n" ) + m_lastToolResult;
            }
        }
        catch( ... )
        {
        }

        m_lastToolError = wxString::Format( _( "Symbol \"%s\" not found in the current library tables." ),
                                            symbolId )
                          + hint;
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
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

    // Optional: set value/footprint fields for passives, etc.
    std::optional<wxString> valueOpt = getString( "value" );
    if( valueOpt && !valueOpt->IsEmpty() )
    {
        if( SCH_FIELD* f = newSymbol->GetField( FIELD_T::VALUE ) )
            f->SetText( *valueOpt );
    }

    std::optional<wxString> footprintOpt = getString( "footprint" );
    if( footprintOpt && !footprintOpt->IsEmpty() )
    {
        if( SCH_FIELD* f = newSymbol->GetField( FIELD_T::FOOTPRINT ) )
            f->SetText( *footprintOpt );
    }

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

    // Avoid overlapping existing symbols/text by nudging the placement to the nearest free location.
    // This makes tool-driven placement robust even if the model provides naive coordinates.
    {
        const int stepIU = schIUScale.mmToIU( 5.08 );      // 0.2" grid-ish
        const int marginIU = schIUScale.mmToIU( 1.0 );     // keep a small clearance
        const int maxRadius = 30;                          // search radius in steps (~150mm)

        auto overlapsExisting = [&]( const BOX2I& aBox ) -> bool
        {
            BOX2I test = aBox;
            test.Inflate( marginIU );

            for( SCH_ITEM* item : screen->Items() )
            {
                if( !item )
                    continue;

                // Only avoid obvious clutter: other symbols and visible text/labels.
                const KICAD_T t = item->Type();
                if( t != SCH_SYMBOL_T && t != SCH_TEXT_T && t != SCH_LABEL_T && t != SCH_GLOBAL_LABEL_T )
                    continue;

                BOX2I bb = item->GetBoundingBox();
                bb.Inflate( marginIU );

                if( test.Intersects( bb ) )
                    return true;
            }

            return false;
        };

        const VECTOR2I basePos = newSymbol->GetPosition();
        VECTOR2I chosenPos = basePos;
        bool found = false;

        // Quick check at the requested position first.
        if( !overlapsExisting( newSymbol->GetBoundingBox() ) )
        {
            found = true;
        }
        else
        {
            // Spiral search on a grid, perimeter by perimeter.
            for( int r = 1; r <= maxRadius && !found; ++r )
            {
                for( int dx = -r; dx <= r && !found; ++dx )
                {
                    for( int dy = -r; dy <= r && !found; ++dy )
                    {
                        // Only check the perimeter of this square "ring"
                        if( std::abs( dx ) != r && std::abs( dy ) != r )
                            continue;

                        VECTOR2I cand = basePos + VECTOR2I( dx * stepIU, dy * stepIU );
                        newSymbol->SetPosition( cand );

                        if( !overlapsExisting( newSymbol->GetBoundingBox() ) )
                        {
                            chosenPos = cand;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }

        newSymbol->SetPosition( chosenPos );
    }

    SCH_COMMIT commit( m_frame );
    // Ensure the symbol is permanently added to the screen and view.
    m_frame->AddToScreen( newSymbol, screen );
    commit.Added( newSymbol, screen );
    commit.Push( _( "Place component" ) );
    
    // Ensure the canvas refreshes so the new component is visible immediately.
    if( m_frame->GetCanvas() )
    {
        if( auto view = m_frame->GetCanvas()->GetView() )
        {
            view->Update( newSymbol );
        }

        m_frame->GetCanvas()->Refresh();
    }

    m_frame->OnModify();
    
    // Return the assigned reference so the agent can use it for labels/wiring.
    json res = json::object();
    res["reference"] = newSymbol->GetRef( &sheet, false ).ToStdString();
    res["symbol"] = symbolId.ToStdString();
    m_lastToolResult = wxString::FromUTF8( res.dump( 2 ) );

    if( m_frame->GetToolManager() )
    {
        m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, newSymbol );
    }

    return true;
}


SCH_OLLAMA_AGENT_TOOL::SYMBOL_MATCH SCH_OLLAMA_AGENT_TOOL::findSymbolByRefOrValue(
        const wxString& aIdentifier, bool aCurrentSheetOnly )
{
    SYMBOL_MATCH bestMatch;
    wxString id = aIdentifier;
    id.Trim( true ).Trim( false );

    if( id.IsEmpty() || !m_frame )
        return bestMatch;

    SCH_SHEET_LIST hierarchy = m_frame->Schematic().Hierarchy();
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();

    // Strategy 1: Exact Reference Match (e.g. "U1")
    for( SCH_SHEET_PATH& sheet : hierarchy )
    {
        if( aCurrentSheetOnly && sheet != currentSheet )
            continue;

        SCH_SCREEN* screen = sheet.LastScreen();
        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            if( sym->GetRef( &sheet, false ).CmpNoCase( id ) == 0 )
            {
                bestMatch.symbol = sym;
                bestMatch.sheet = sheet;
                return bestMatch; // Perfect match found
            }
        }
    }

    // Strategy 2: Match by Value (e.g. "MCP2551")
    for( SCH_SHEET_PATH& sheet : hierarchy )
    {
        if( aCurrentSheetOnly && sheet != currentSheet )
            continue;

        SCH_SCREEN* screen = sheet.LastScreen();
        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            if( sym->GetValue( false, nullptr, false ).CmpNoCase( id ) == 0 )
            {
                bestMatch.symbol = sym;
                bestMatch.sheet = sheet;
                return bestMatch;
            }
        }
    }

    // Strategy 3: Match by Symbol Name (e.g. "Device:R" or "R")
    for( SCH_SHEET_PATH& sheet : hierarchy )
    {
        if( aCurrentSheetOnly && sheet != currentSheet )
            continue;

        SCH_SCREEN* screen = sheet.LastScreen();
        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            wxString libId = sym->GetLibId().Format();
            wxString itemName = wxString::FromUTF8( sym->GetLibId().GetLibItemName().c_str() );
            if( libId.CmpNoCase( id ) == 0 || itemName.CmpNoCase( id ) == 0 )
            {
                bestMatch.symbol = sym;
                bestMatch.sheet = sheet;
                return bestMatch;
            }
        }
    }

    return bestMatch;
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
        m_lastToolError = _( "move_component tool requires a \"reference\" field." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    wxString reference = *referenceOpt;
    reference.Trim( true ).Trim( false );

    // Find the symbol by reference (or value/symbol name as fallback)
    SYMBOL_MATCH match = findSymbolByRefOrValue( reference );
    SCH_SYMBOL* symbol = match.symbol;
    SCH_SHEET_PATH symbolSheet = match.sheet;

    if( !symbol )
    {
        m_lastToolError = wxString::Format( _( "Component \"%s\" not found." ), reference );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
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


bool SCH_OLLAMA_AGENT_TOOL::HandleAddNetLabelTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    auto asWxString = [&]( const json& v ) -> std::optional<wxString>
    {
        if( v.is_string() )
            return wxString::FromUTF8( v.get<std::string>() );
        if( v.is_number_integer() )
            return wxString::Format( wxS( "%lld" ), v.get<long long>() );
        return std::nullopt;
    };

    // Handle both "net" and "text" fields for labels.
    wxString labelText;
    if( aPayload.contains( "net" ) && aPayload["net"].is_string() )
        labelText = wxString::FromUTF8( aPayload["net"].get<std::string>() );
    else if( aPayload.contains( "text" ) && aPayload["text"].is_string() )
        labelText = wxString::FromUTF8( aPayload["text"].get<std::string>() );

    labelText.Trim( true ).Trim( false );
    if( labelText.IsEmpty() )
    {
        m_lastToolError = _( "add_label requires a \"net\" or \"text\" field." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    // Determine label type: global (default) or local.
    bool isLocal = false;
    if( aPayload.contains( "type" ) && aPayload["type"].is_string() )
    {
        wxString typeStr = wxString::FromUTF8( aPayload["type"].get<std::string>() );
        if( typeStr.CmpNoCase( wxS( "local" ) ) == 0 || typeStr.CmpNoCase( wxS( "net" ) ) == 0 )
            isLocal = true;
    }

    // Default placement target is the current sheet, but pin-mode may redirect to the sheet
    // where the referenced symbol actually lives (so connect-with-label can place both ends).
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
    SCH_SHEET_PATH targetSheet = currentSheet;
    SCH_SCREEN*    targetScreen = targetSheet.LastScreen();
    if( !targetScreen )
        return false;

    VECTOR2I pos;
    bool havePos = false;
    bool pinMode = false;
    VECTOR2I pinPos;
    SPIN_STYLE spinStyle = SPIN_STYLE::RIGHT;

    // Mode A: coordinates (mm)
    if( aPayload.contains( "x" ) && aPayload.contains( "y" ) && aPayload["x"].is_number() && aPayload["y"].is_number() )
    {
        pos = VECTOR2I( schIUScale.mmToIU( aPayload["x"].get<double>() ),
                        schIUScale.mmToIU( aPayload["y"].get<double>() ) );
        havePos = true;
    }

    // Mode B: at{reference,pin} — place label one grid step away from the symbol body at that pin.
    if( !havePos && aPayload.contains( "at" ) && aPayload["at"].is_object() )
    {
        pinMode = true;
        const json& at = aPayload["at"];
        if( !at.contains( "reference" ) || !at.contains( "pin" ) )
        {
            m_lastToolError = _( "add_net_label pin mode requires at{reference,pin}." );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }

        auto refOpt = asWxString( at["reference"] );
        auto pinOpt = asWxString( at["pin"] );
        if( !refOpt || !pinOpt )
        {
            m_lastToolError = _( "add_net_label: at.reference must be string, at.pin must be string/int." );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }

        wxString ref = *refOpt;
        wxString pinKey = *pinOpt;
        ref.Trim( true ).Trim( false );
        pinKey.Trim( true ).Trim( false );

        const int gridStepIU = schIUScale.mmToIU( 2.54 );
        const int obstacleMarginIU = schIUScale.mmToIU( 1.0 );

        // Find the referenced symbol (or value/name) anywhere in the loaded hierarchy.
        SYMBOL_MATCH match = findSymbolByRefOrValue( ref );
        SCH_SYMBOL* sym = match.symbol;
        targetSheet = match.sheet;
        targetScreen = targetSheet.LastScreen();

        if( !sym || !targetScreen )
        {
            m_lastToolError = wxString::Format( _( "add_net_label: component \"%s\" not found in schematic hierarchy." ), ref );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }

        SCH_PIN* targetPin = nullptr;
        for( SCH_PIN* p : sym->GetPins( &targetSheet ) )
        {
            if( !p )
                continue;

            wxString name = p->GetShownName();
            wxString number = p->GetShownNumber();

            if( ( !name.IsEmpty() && name.CmpNoCase( pinKey ) == 0 )
                || ( !number.IsEmpty() && number.CmpNoCase( pinKey ) == 0 ) )
            {
                targetPin = p;
                break;
            }
        }

        if( !targetPin )
        {
            m_lastToolError = wxString::Format( _( "add_net_label: pin \"%s\" not found on %s." ), pinKey, ref );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }

        pinPos = targetPin->GetPosition();
        BOX2I bbox = sym->GetBoundingBox();
        bbox.Inflate( obstacleMarginIU );

        const int dl = std::abs( pinPos.x - bbox.GetX() );
        const int dr = std::abs( bbox.GetRight() - pinPos.x );
        const int dt = std::abs( pinPos.y - bbox.GetY() );
        const int db = std::abs( bbox.GetBottom() - pinPos.y );
        const int best = std::min( std::min( dl, dr ), std::min( dt, db ) );

        if( best == dl )
            pos = VECTOR2I( pinPos.x - gridStepIU, pinPos.y );
        else if( best == dr )
            pos = VECTOR2I( pinPos.x + gridStepIU, pinPos.y );
        else if( best == dt )
            pos = VECTOR2I( pinPos.x, pinPos.y - gridStepIU );
        else
            pos = VECTOR2I( pinPos.x, pinPos.y + gridStepIU );

        // Orient the label so it faces "out" from the pin side.
        switch( targetPin->GetOrientation() )
        {
        default:
        case PIN_ORIENTATION::PIN_RIGHT:
            spinStyle = SPIN_STYLE::LEFT;
            break;
        case PIN_ORIENTATION::PIN_LEFT:
            spinStyle = SPIN_STYLE::RIGHT;
            break;
        case PIN_ORIENTATION::PIN_UP:
            spinStyle = SPIN_STYLE::BOTTOM;
            break;
        case PIN_ORIENTATION::PIN_DOWN:
            spinStyle = SPIN_STYLE::UP;
            break;
        }

        havePos = true;
    }

    if( !havePos )
    {
        m_lastToolError = _( "add_label requires either x,y (mm) or at{reference,pin}." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    SCH_LABEL_BASE* label = nullptr;
    if( isLocal )
    {
        label = new SCH_LABEL( pos, labelText );
    }
    else
    {
        label = new SCH_GLOBALLABEL( pos, labelText );
    }

    label->SetPosition( pos );
    label->SetText( labelText );
    label->SetParent( targetScreen );
    label->SetSpinStyle( spinStyle );

    SCH_COMMIT commit( m_frame );
    // In pin-mode, drop a short wire stub from the pin to the label anchor so it's electrically connected.
    SCH_LINE* stub = nullptr;
    if( pinMode && pinPos != pos )
    {
        stub = new SCH_LINE();
        stub->SetStartPoint( pinPos );
        stub->SetEndPoint( pos );
        stub->SetLayer( LAYER_WIRE );
        stub->SetStroke( STROKE_PARAMS() );
        stub->SetParent( targetScreen );
        m_frame->AddToScreen( stub, targetScreen );
        commit.Added( stub, targetScreen );
    }

    m_frame->AddToScreen( label, targetScreen );
    commit.Added( label, targetScreen );
    commit.Push( isLocal ? _( "Add net label" ) : _( "Add global label" ) );

    if( m_frame->GetCanvas() )
    {
        if( auto view = m_frame->GetCanvas()->GetView() )
        {
            if( stub ) view->Update( stub );
            view->Update( label );
        }

        m_frame->GetCanvas()->Refresh();
    }

    m_frame->OnModify();

    if( m_frame->GetToolManager() )
    {
        if( stub )
            m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, stub );
        m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, label );
    }

    return true;
}


bool SCH_OLLAMA_AGENT_TOOL::HandleConnectWithNetLabelTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    if( !aPayload.contains( "net" ) || !aPayload["net"].is_string() )
    {
        m_lastToolError = _( "connect_with_net_label requires \"net\" (string)." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    auto extractEndpoint = [&]( const json& obj ) -> const json*
    {
        if( !obj.is_object() )
            return nullptr;

        // direct: {reference,pin}
        if( obj.contains( "reference" ) && obj.contains( "pin" ) )
            return &obj;

        // nested: {at:{reference,pin}}
        if( obj.contains( "at" ) && obj["at"].is_object() && obj["at"].contains( "reference" ) && obj["at"].contains( "pin" ) )
            return &obj["at"];

        return nullptr;
    };

    const json* fromEp = nullptr;
    const json* toEp = nullptr;

    // Common shapes: from/to, a/b, or endpoints:[{..},{..}]
    if( aPayload.contains( "from" ) )
        fromEp = extractEndpoint( aPayload["from"] );
    else if( aPayload.contains( "a" ) )
        fromEp = extractEndpoint( aPayload["a"] );

    if( aPayload.contains( "to" ) )
        toEp = extractEndpoint( aPayload["to"] );
    else if( aPayload.contains( "b" ) )
        toEp = extractEndpoint( aPayload["b"] );

    if( ( !fromEp || !toEp ) && aPayload.contains( "endpoints" ) && aPayload["endpoints"].is_array()
        && aPayload["endpoints"].size() >= 2 )
    {
        if( !fromEp )
            fromEp = extractEndpoint( aPayload["endpoints"][0] );
        if( !toEp )
            toEp = extractEndpoint( aPayload["endpoints"][1] );
    }

    if( !fromEp || !toEp )
    {
        m_lastToolError =
                _( "connect_with_net_label requires endpoints in one of these forms: "
                   "from{reference,pin}/to{reference,pin}, "
                   "from{at{reference,pin}}/to{at{reference,pin}}, "
                   "a/b, or endpoints:[{reference,pin},{reference,pin}]." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    // Reuse add_net_label implementation twice.
    const std::string net = aPayload["net"].get<std::string>();
    json p1 = json::object();
    p1["net"] = net;
    p1["at"] = json::object();
    p1["at"]["reference"] = (*fromEp)["reference"];
    p1["at"]["pin"] = (*fromEp)["pin"];

    json p2 = json::object();
    p2["net"] = net;
    p2["at"] = json::object();
    p2["at"]["reference"] = (*toEp)["reference"];
    p2["at"]["pin"] = (*toEp)["pin"];

    bool ok1 = HandleAddNetLabelTool( p1 );
    wxString err1 = m_lastToolError;
    bool ok2 = HandleAddNetLabelTool( p2 );
    wxString err2 = m_lastToolError;

    if( ok1 && ok2 )
        return true;

    // Prefer the most informative error.
    if( !ok1 && !err1.IsEmpty() )
        m_lastToolError = err1;
    else if( !ok2 && !err2.IsEmpty() )
        m_lastToolError = err2;
    else
        m_lastToolError = _( "connect_with_net_label failed." );

    wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
    return false;
}

bool SCH_OLLAMA_AGENT_TOOL::HandleAddWireTool( const json& aPayload )
{
    if( !m_frame || !aPayload.is_object() )
        return false;

    struct PIN_LOC
    {
        VECTOR2I pos;
        SCH_SCREEN* screen = nullptr;
        BOX2I symbolBBox;
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

        SYMBOL_MATCH match = findSymbolByRefOrValue( ref, true );
        SCH_SYMBOL* sym = match.symbol;
        if( !sym )
            return std::nullopt;

        std::vector<SCH_PIN*> pins = sym->GetPins( &match.sheet );
        for( SCH_PIN* pin : pins )
        {
            if( !pin )
                continue;

            wxString name = pin->GetShownName();
            wxString number = pin->GetShownNumber();

            if( ( !name.IsEmpty() && name.CmpNoCase( pinKey ) == 0 )
                || ( !number.IsEmpty() && number.CmpNoCase( pinKey ) == 0 ) )
            {
                return PIN_LOC{ pin->GetPosition(), match.sheet.LastScreen(), sym->GetBoundingBox() };
            }
        }

        return std::nullopt;
    };

    VECTOR2I start;
    VECTOR2I end;
    SCH_SCREEN* targetScreen = nullptr;
    bool pinMode = false;
    PIN_LOC startLoc;
    PIN_LOC endLoc;

    // Mode A: explicit coordinates (mm)
    if( aPayload.contains( "x1" ) && aPayload.contains( "y1" ) && aPayload.contains( "x2" ) && aPayload.contains( "y2" ) )
    {
        if( !aPayload["x1"].is_number() || !aPayload["y1"].is_number() || !aPayload["x2"].is_number() || !aPayload["y2"].is_number() )
        {
            m_lastToolError = _( "add_wire tool fields x1, y1, x2, y2 must be numbers (mm)." );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
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
        pinMode = true;
        const json& from = aPayload["from"];
        const json& to = aPayload["to"];

        if( !from.contains( "reference" ) || !from.contains( "pin" ) || !to.contains( "reference" ) || !to.contains( "pin" ) )
        {
            m_lastToolError = _( "add_wire pin mode requires: from{reference,pin}, to{reference,pin}." );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }

        auto asWxString = [&]( const json& v ) -> std::optional<wxString>
        {
            if( v.is_string() )
                return wxString::FromUTF8( v.get<std::string>() );
            if( v.is_number_integer() )
                return wxString::Format( wxS( "%lld" ), v.get<long long>() );
            return std::nullopt;
        };

        auto fromRefOpt = asWxString( from["reference"] );
        auto fromPinOpt = asWxString( from["pin"] );
        auto toRefOpt = asWxString( to["reference"] );
        auto toPinOpt = asWxString( to["pin"] );

        if( !fromRefOpt || !fromPinOpt || !toRefOpt || !toPinOpt )
        {
            m_lastToolError = _( "add_wire pin mode fields must be strings or integers." );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }

        wxString fromRef = *fromRefOpt;
        wxString fromPin = *fromPinOpt;
        wxString toRef = *toRefOpt;
        wxString toPin = *toPinOpt;

        auto startOpt = findPinLocOnCurrentSheet( fromRef, fromPin );
        auto endOpt = findPinLocOnCurrentSheet( toRef, toPin );

        if( !startOpt || !endOpt )
        {
            m_lastToolError = _( "add_wire: could not resolve one or both pin locations on the current sheet." );
            wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
            return false;
        }

        startLoc = *startOpt;
        endLoc = *endOpt;
        start = startOpt->pos;
        end = endOpt->pos;
        targetScreen = startOpt->screen;

        if( !targetScreen || endOpt->screen != targetScreen )
            return false;
    }
    else
    {
        m_lastToolError = _( "add_wire requires either x1,y1,x2,y2 (mm) or from/to pin objects." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    if( !targetScreen )
        return false;

    if( start == end )
    {
        m_lastToolError = _( "add_wire produced a zero-length segment (start == end). Check pin resolution (reference/pin names) on the current sheet." );
        wxLogWarning( wxS( "[OllamaAgent] %s" ), m_lastToolError );
        return false;
    }

    // Escape away from pins/symbol bodies so vertical/horizontal runs don't "touch all pins".
    // "Add one" extra grid step vs a minimal escape.
    const int gridStepIU = schIUScale.mmToIU( 2.54 );
    const int escapeIU = gridStepIU * 2;                // 5.08mm away from pin
    const int obstacleMarginIU = schIUScale.mmToIU( 1.0 ); // keep away from objects

    auto escapeFromPin = [&]( const PIN_LOC& aLoc ) -> VECTOR2I
    {
        BOX2I bbox = aLoc.symbolBBox;
        bbox.Inflate( obstacleMarginIU );

        const int dl = std::abs( aLoc.pos.x - bbox.GetX() );
        const int dr = std::abs( bbox.GetRight() - aLoc.pos.x );
        const int dt = std::abs( aLoc.pos.y - bbox.GetY() );
        const int db = std::abs( bbox.GetBottom() - aLoc.pos.y );

        const int best = std::min( std::min( dl, dr ), std::min( dt, db ) );

        if( best == dl )
            return VECTOR2I( aLoc.pos.x - escapeIU, aLoc.pos.y );
        if( best == dr )
            return VECTOR2I( aLoc.pos.x + escapeIU, aLoc.pos.y );
        if( best == dt )
            return VECTOR2I( aLoc.pos.x, aLoc.pos.y - escapeIU );

        return VECTOR2I( aLoc.pos.x, aLoc.pos.y + escapeIU );
    };

    const VECTOR2I startEsc = pinMode ? escapeFromPin( startLoc ) : start;
    const VECTOR2I endEsc   = pinMode ? escapeFromPin( endLoc ) : end;

    // Optional: prefer net labels for long connections if net name is provided.
    wxString netName;
    if( aPayload.contains( "net" ) && aPayload["net"].is_string() )
        netName = wxString::FromUTF8( aPayload["net"].get<std::string>() );

    long long manhattanIU = llabs( (long long) ( endEsc.x - startEsc.x ) ) + llabs( (long long) ( endEsc.y - startEsc.y ) );
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
        // Ensure items are actually on the screen/view (commit only records undo/redo).
        m_frame->AddToScreen( l1, targetScreen );
        m_frame->AddToScreen( l2, targetScreen );
        commit.Added( l1, targetScreen );
        commit.Added( l2, targetScreen );
        commit.Push( _( "Add net labels" ) );

        if( m_frame->GetCanvas() )
        {
            if( auto view = m_frame->GetCanvas()->GetView() )
            {
                view->Update( l1 );
                view->Update( l2 );
            }

            m_frame->GetCanvas()->Refresh();
        }

        m_frame->OnModify();

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
        // Ensure wire is actually on the screen/view (commit only records undo/redo).
        m_frame->AddToScreen( w, targetScreen );
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

            bbox.Inflate( obstacleMarginIU );

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

    auto segmentHitsWire = [&]( const VECTOR2I& aA, const VECTOR2I& aB ) -> int
    {
        if( aA == aB )
            return 0;

        const bool vertical = aA.x == aB.x;
        const bool horizontal = aA.y == aB.y;
        if( !vertical && !horizontal )
            return 0;

        int hits = 0;

        for( SCH_ITEM* item : targetScreen->Items().OfType( SCH_LINE_T ) )
        {
            SCH_LINE* line = static_cast<SCH_LINE*>( item );
            if( !line || line->GetLayer() != LAYER_WIRE )
                continue;

            BOX2I bbox = line->GetBoundingBox();
            bbox.Inflate( obstacleMarginIU );

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
            else
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

    auto scoreTwoSeg = [&]( const VECTOR2I& aP0, const VECTOR2I& aP1, const VECTOR2I& aP2 ) -> long long
    {
        long long collisions = 0;
        collisions += segmentHitsSymbol( aP0, aP1 ) + segmentHitsSymbol( aP1, aP2 );
        collisions += segmentHitsWire( aP0, aP1 ) + segmentHitsWire( aP1, aP2 );
        long long len = llabs( (long long) ( aP2.x - aP0.x ) ) + llabs( (long long) ( aP2.y - aP0.y ) );
        return collisions * 1000000LL + len;
    };

    std::vector<VECTOR2I> bends;
    bends.push_back( VECTOR2I( endEsc.x, startEsc.y ) );
    bends.push_back( VECTOR2I( startEsc.x, endEsc.y ) );

    for( int k : { -2, -1, 1, 2 } )
    {
        bends.push_back( VECTOR2I( endEsc.x, startEsc.y + k * gridStepIU ) );
        bends.push_back( VECTOR2I( startEsc.x + k * gridStepIU, endEsc.y ) );
    }

    VECTOR2I bend = bends.front();
    long long bestScore = std::numeric_limits<long long>::max();

    for( const VECTOR2I& b : bends )
    {
        if( !( ( b.x == startEsc.x || b.y == startEsc.y ) && ( b.x == endEsc.x || b.y == endEsc.y ) ) )
            continue;

        long long s = scoreTwoSeg( startEsc, b, endEsc );
        if( s < bestScore )
        {
            bestScore = s;
            bend = b;
        }
    }

    SCH_COMMIT commit( m_frame );
    std::vector<SCH_LINE*> newWires;

    auto addSegmentIfNeeded = [&]( const VECTOR2I& aA, const VECTOR2I& aB )
    {
        if( aA == aB )
            return;

        if( aA.x == aB.x || aA.y == aB.y )
        {
            newWires.push_back( addWireSeg( commit, aA, aB ) );
        }
        else
        {
            VECTOR2I mid( aB.x, aA.y );
            newWires.push_back( addWireSeg( commit, aA, mid ) );
            newWires.push_back( addWireSeg( commit, mid, aB ) );
        }
    };

    // Pin escape segments first/last.
    if( pinMode )
    {
        addSegmentIfNeeded( start, startEsc );
        addSegmentIfNeeded( endEsc, end );
    }

    // Main route between escape points.
    if( startEsc.x == endEsc.x || startEsc.y == endEsc.y )
    {
        addSegmentIfNeeded( startEsc, endEsc );
    }
    else
    {
        addSegmentIfNeeded( startEsc, bend );
        addSegmentIfNeeded( bend, endEsc );
    }

    commit.Push( _( "Add wire" ) );

    if( m_frame->GetCanvas() )
    {
        if( auto view = m_frame->GetCanvas()->GetView() )
        {
            for( SCH_LINE* w : newWires )
                view->Update( w );
        }

        m_frame->GetCanvas()->Refresh();
    }

    m_frame->OnModify();

    if( m_frame->GetToolManager() )
    {
        for( SCH_LINE* w : newWires )
            m_frame->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, w );
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
