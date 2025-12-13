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
#include <nlohmann/json.hpp>

using json = nlohmann::json;
#include <sch_symbol.h>
#include <sch_commit.h>
#include <lib_id.h>


SCH_OLLAMA_AGENT_TOOL::SCH_OLLAMA_AGENT_TOOL() :
    SCH_TOOL_BASE<SCH_EDIT_FRAME>( "eeschema.OllamaAgentTool" ),
    m_model( wxS( "qwen3:4b" ) )  // Default model
{
    initializeSystemPrompt();
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

    // Build prompt
    wxString prompt = BuildPrompt( userRequest );
    const wxString& systemPrompt = GetSystemPrompt();

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

    // Send to Python agent (which forwards to Ollama)
    wxString response;
    if( !m_ollama->ChatCompletion( m_model, prompt, response, systemPrompt ) )
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

    wxString content;
    content << wxS( "CURRENT SCHEMATIC CONTENT:\n" );
    content << wxS( "Sheet: " ) << m_frame->GetFullScreenDesc() << wxS( "\n\n" );

    // Collect items by type for organized output
    std::vector<SCH_JUNCTION*> junctions;
    std::vector<SCH_LINE*> wires;
    std::vector<SCH_LABEL*> labels;
    std::vector<SCH_TEXT*> texts;

    for( SCH_ITEM* item : screen->Items() )
    {
        switch( item->Type() )
        {
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

    // Format junctions
    if( !junctions.empty() )
    {
        content << wxS( "Junctions:\n" );
        for( SCH_JUNCTION* junction : junctions )
        {
            VECTOR2I pos = junction->GetPosition();
            double x_mm = schIUScale.IUTomm( pos.x );
            double y_mm = schIUScale.IUTomm( pos.y );
            content << wxString::Format( wxS( "  - Junction at (%.2f, %.2f) mm\n" ), x_mm, y_mm );
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
            content << wxString::Format( wxS( "  - Wire from (%.2f, %.2f) to (%.2f, %.2f) mm\n" ),
                                        x1_mm, y1_mm, x2_mm, y2_mm );
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
            content << wxString::Format( wxS( "  - Label \"%s\" at (%.2f, %.2f) mm\n" ),
                                        labelText, x_mm, y_mm );
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

    if( junctions.empty() && wires.empty() && labels.empty() && texts.empty() )
    {
        content << wxS( "  (Schematic is empty)\n" );
    }

    return content;
}


wxString SCH_OLLAMA_AGENT_TOOL::BuildPrompt( const wxString& aUserRequest )
{
    wxString prompt;
    prompt << wxS( "USER REQUEST:\n" ) << aUserRequest << wxS( "\n\n" );
    
    // Add current schematic content
    wxString schematicContent = GetCurrentSchematicContent();
    if( !schematicContent.IsEmpty() )
    {
        prompt << schematicContent << wxS( "\n" );
    }
    
    prompt << wxS( "Remember to respond with TASKS, a blank line, then COMMANDS. " )
           << wxS( "Emit TOOL <name> <json> lines if a tool invocation is required before COMMANDS.\n" );
    return prompt;
}


bool SCH_OLLAMA_AGENT_TOOL::ParseAndExecute( const wxString& aResponse )
{
    bool success = false;
    m_agent->BeginBatch();

    wxStringTokenizer tokenizer( aResponse, wxS( "\n" ) );
    
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

            if( ExecuteToolCommand( toolName, payload ) )
                success = true;

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


void SCH_OLLAMA_AGENT_TOOL::initializeSystemPrompt()
{
    m_toolCatalog.clear();
    m_toolCatalog.emplace_back( TOOL_DESCRIPTOR{
            wxS( "schematic.place_component" ),
            _( "Places a schematic symbol from the library at the given coordinates (millimeters)." ),
            wxS( "TOOL schematic.place_component {\"symbol\":\"Device:R\",\"x\":100,\"y\":50,\"reference\":\"R1\"}" ) } );

    wxString prompt;
    prompt << wxS( "You are an expert schematic design assistant specializing in electronic circuit design " )
           << wxS( "and KiCad schematic capture. You understand electrical engineering principles, " )
           << wxS( "circuit topology, signal flow, power distribution, and schematic design best practices.\n\n" )
           << wxS( "Your role is to help designers create clear, well-organized schematics by:\n" )
           << wxS( "- Understanding circuit requirements and translating them into schematic elements\n" )
           << wxS( "- Placing components, junctions, and connections logically\n" )
           << wxS( "- Adding appropriate labels for nets, power rails, and signals\n" )
           << wxS( "- Organizing schematics for readability and maintainability\n" )
           << wxS( "- Following schematic design conventions (power at top, ground at bottom, signal flow left-to-right)\n" )
           << wxS( "- Ensuring proper net connectivity and avoiding common design errors\n\n" )
           << wxS( "=== RESPONSE FORMAT ===\n" )
           << wxS( "Always respond using this exact structure:\n\n" )
           << wxS( "TASKS:\n" )
           << wxS( "- Explain your design approach and what you're creating in the schematic\n" )
           << wxS( "- Describe the circuit topology, signal flow, and component placement strategy\n" )
           << wxS( "- Note any design considerations or best practices you're applying\n\n" )
           << wxS( "COMMANDS:\n" )
           << wxS( "- One command per line using the supported syntax (JUNCTION, WIRE, LABEL, TEXT)\n" )
           << wxS( "- Use clear, descriptive labels that follow naming conventions (e.g., VCC, GND, CLK, DATA)\n" )
           << wxS( "- Place elements with appropriate spacing for readability\n" )
           << wxS( "- Never mix prose inside COMMANDS.\n" )
           << wxS( "- All coordinates must be in millimeters (mm)\n\n" );

    if( !m_toolCatalog.empty() )
    {
        prompt << wxS( "=== TOOL CALLING ===\n" )
               << wxS( "You have access to tools that can perform actions on the schematic. " )
               << wxS( "Use tools when you need to place components or perform operations that require tool execution.\n\n" )
               << wxS( "TOOL CALL SYNTAX:\n" )
               << wxS( "When you need to call a tool, emit a single line with this exact format:\n" )
               << wxS( "TOOL <tool_name> <json_object>\n\n" )
               << wxS( "CRITICAL RULES FOR TOOL CALLS:\n" )
               << wxS( "1. The line MUST start with the word 'TOOL' (all caps) followed by a space\n" )
               << wxS( "2. Next comes the tool name (exactly as listed below) followed by a space\n" )
               << wxS( "3. Then a valid JSON object with no line breaks (all on one line)\n" )
               << wxS( "4. The JSON object must be properly formatted with double quotes for keys and string values\n" )
               << wxS( "5. Do NOT include any text before or after the TOOL line\n" )
               << wxS( "6. Tool calls should appear BEFORE the COMMANDS section if needed\n\n" )
               << wxS( "EXAMPLE OF CORRECT TOOL CALL:\n" )
               << wxS( "TOOL schematic.place_component {\"symbol\":\"Device:R\",\"x\":100.0,\"y\":50.0,\"reference\":\"R1\"}\n\n" )
               << wxS( "EXAMPLE OF INCORRECT TOOL CALL (DO NOT DO THIS):\n" )
               << wxS( "I will place a resistor: TOOL schematic.place_component {...}  <-- WRONG: extra text\n" )
               << wxS( "TOOL schematic.place_component {symbol:'R', x:100}  <-- WRONG: single quotes, missing quotes\n" )
               << wxS( "TOOL schematic.place_component\n" )
               << wxS( "{\"symbol\":\"R\"}  <-- WRONG: JSON split across lines\n\n" )
               << wxS( "=== AVAILABLE TOOLS ===\n\n" );

        for( const TOOL_DESCRIPTOR& tool : m_toolCatalog )
        {
            if( tool.name == wxS( "schematic.place_component" ) )
            {
                prompt << wxS( "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" )
                       << wxS( "TOOL: schematic.place_component\n" )
                       << wxS( "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n" )
                       << wxS( "DESCRIPTION:\n" )
                       << wxS( "Places a schematic symbol (component) from the KiCad symbol library onto the schematic\n" )
                       << wxS( "at the specified coordinates. This tool is used to add components like resistors,\n" )
                       << wxS( "capacitors, ICs, connectors, and power symbols to the schematic.\n\n" )
                       << wxS( "WHEN TO USE:\n" )
                       << wxS( "- When you need to place any component (resistor, capacitor, IC, connector, etc.)\n" )
                       << wxS( "- When placing power symbols (VCC, GND, +5V, etc.)\n" )
                       << wxS( "- When you need to add a component that exists in the KiCad symbol libraries\n\n" )
                       << wxS( "SYNTAX:\n" )
                       << wxS( "TOOL schematic.place_component <json_object>\n\n" )
                       << wxS( "JSON PARAMETERS:\n" )
                       << wxS( "{\n" )
                       << wxS( "  \"symbol\": string (REQUIRED)\n" )
                       << wxS( "    - Library symbol identifier in format \"LibraryName:SymbolName\"\n" )
                       << wxS( "    - Examples: \"Device:R\", \"Device:C\", \"power:+5V\", \"power:GND\"\n" )
                       << wxS( "    - Common libraries: Device, power, Connector, Regulator_Linear\n" )
                       << wxS( "    - Must be a valid symbol from the KiCad symbol libraries\n\n" )
                       << wxS( "  \"x\": number (REQUIRED)\n" )
                       << wxS( "    - X coordinate in millimeters where to place the component\n" )
                       << wxS( "    - Example: 100.0, 50.5, 0.0\n\n" )
                       << wxS( "  \"y\": number (REQUIRED)\n" )
                       << wxS( "    - Y coordinate in millimeters where to place the component\n" )
                       << wxS( "    - Example: 100.0, 50.5, 0.0\n\n" )
                       << wxS( "  \"reference\": string (OPTIONAL)\n" )
                       << wxS( "    - Reference designator for the component (e.g., \"R1\", \"C1\", \"U1\", \"J1\")\n" )
                       << wxS( "    - If not provided, KiCad will auto-assign based on component type\n" )
                       << wxS( "    - Examples: \"R1\", \"C2\", \"U3\", \"J1\", \"D1\"\n\n" )
                       << wxS( "  \"unit\": number (OPTIONAL, default: 1)\n" )
                       << wxS( "    - Unit number for multi-unit parts (e.g., multi-gate ICs)\n" )
                       << wxS( "    - Only needed for components with multiple units per package\n" )
                       << wxS( "    - Example: 1, 2, 3, 4\n\n" )
                       << wxS( "  \"rotation\": number (OPTIONAL, default: 0)\n" )
                       << wxS( "    - Rotation angle in degrees\n" )
                       << wxS( "    - Valid values: 0, 90, 180, 270\n" )
                       << wxS( "    - 0 = normal orientation, 90 = rotated clockwise, etc.\n\n" )
                       << wxS( "}\n\n" )
                       << wxS( "COMMON SYMBOL LIBRARIES AND EXAMPLES:\n" )
                       << wxS( "- Device library: \"Device:R\" (resistor), \"Device:C\" (capacitor), \"Device:L\" (inductor),\n" )
                       << wxS( "                  \"Device:D\" (diode), \"Device:Q\" (transistor)\n" )
                       << wxS( "- Power library: \"power:+5V\", \"power:+3V3\", \"power:GND\", \"power:VCC\"\n" )
                       << wxS( "- Connector library: \"Connector:Conn_01x02_Male\", \"Connector:USB_C_Receptacle\"\n" )
                       << wxS( "- Regulator_Linear: \"Regulator_Linear:LM1117-3.3\", \"Regulator_Linear:LM7805\"\n\n" )
                       << wxS( "EXAMPLES:\n\n" )
                       << wxS( "Example 1: Place a resistor at (100mm, 50mm):\n" )
                       << wxS( "TOOL schematic.place_component {\"symbol\":\"Device:R\",\"x\":100.0,\"y\":50.0}\n\n" )
                       << wxS( "Example 2: Place a capacitor with reference C1 at (150mm, 75mm):\n" )
                       << wxS( "TOOL schematic.place_component {\"symbol\":\"Device:C\",\"x\":150.0,\"y\":75.0,\"reference\":\"C1\"}\n\n" )
                       << wxS( "Example 3: Place a +5V power symbol at (0mm, 0mm):\n" )
                       << wxS( "TOOL schematic.place_component {\"symbol\":\"power:+5V\",\"x\":0.0,\"y\":0.0}\n\n" )
                       << wxS( "Example 4: Place a GND symbol rotated 180 degrees:\n" )
                       << wxS( "TOOL schematic.place_component {\"symbol\":\"power:GND\",\"x\":50.0,\"y\":100.0,\"rotation\":180}\n\n" )
                       << wxS( "Example 5: Place unit 2 of a multi-unit IC:\n" )
                       << wxS( "TOOL schematic.place_component {\"symbol\":\"Device:74HC00\",\"x\":200.0,\"y\":100.0,\"unit\":2}\n\n" )
                       << wxS( "ERROR HANDLING:\n" )
                       << wxS( "- If the symbol library or symbol name is invalid, the tool will fail\n" )
                       << wxS( "- Always use the exact library:symbol format (case-sensitive)\n" )
                       << wxS( "- Coordinates must be numbers (integers or floats), not strings\n" )
                       << wxS( "- Rotation must be one of: 0, 90, 180, 270\n\n" )
                       << wxS( "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n" );
            }
            else
            {
                // Generic tool documentation for any other tools
                prompt << wxS( "Tool: " ) << tool.name << wxS( "\n" )
                       << wxS( "Description: " ) << tool.description << wxS( "\n" )
                       << wxS( "Example: " ) << tool.usage << wxS( "\n\n" );
            }
        }

        prompt << wxS( "=== TOOL USAGE WORKFLOW ===\n" )
               << wxS( "When you need to use a tool, follow this workflow:\n\n" )
               << wxS( "1. DECIDE: Determine if you need a tool or can use COMMANDS directly\n" )
               << wxS( "   - Use tools for: placing components (schematic.place_component)\n" )
               << wxS( "   - Use COMMANDS for: wires, junctions, labels, text annotations\n\n" )
               << wxS( "2. CALL TOOL: If you need a tool, emit the TOOL line BEFORE the TASKS section\n" )
               << wxS( "   - Format: TOOL <tool_name> <json_object>\n" )
               << wxS( "   - Must be valid JSON on a single line\n" )
               << wxS( "   - You can call multiple tools, each on its own line\n\n" )
               << wxS( "3. EXPLAIN: Continue with TASKS section explaining what you're doing\n" )
               << wxS( "   - Describe the component you just placed\n" )
               << wxS( "   - Explain the design rationale\n\n" )
               << wxS( "4. ADD CONNECTIONS: Provide COMMANDS for wires, labels, and other elements\n" )
               << wxS( "   - Connect the placed component with wires\n" )
               << wxS( "   - Add net labels as needed\n" )
               << wxS( "   - Add junctions where wires connect\n\n" )
               << wxS( "EXAMPLE COMPLETE WORKFLOW:\n" )
               << wxS( "TOOL schematic.place_component {\"symbol\":\"Device:R\",\"x\":100.0,\"y\":50.0,\"reference\":\"R1\"}\n" )
               << wxS( "TOOL schematic.place_component {\"symbol\":\"Device:C\",\"x\":150.0,\"y\":50.0,\"reference\":\"C1\"}\n\n" )
               << wxS( "TASKS:\n" )
               << wxS( "Placing a resistor-capacitor (RC) circuit. R1 is a 10kΩ resistor at (100mm, 50mm) and\n" )
               << wxS( "C1 is a 100nF capacitor at (150mm, 50mm). These will form a low-pass filter.\n\n" )
               << wxS( "COMMANDS:\n" )
               << wxS( "WIRE 100.0 50.0 150.0 50.0\n" )
               << wxS( "LABEL 125.0 45.0 \"SIGNAL_IN\"\n\n" );
    }

    prompt << wxS( "=== DESIGN GUIDELINES ===\n" )
           << wxS( "- Use standard schematic conventions: power rails at top, ground at bottom\n" )
           << wxS( "- Maintain consistent spacing (typically 2.54mm/0.1\" grid spacing)\n" )
           << wxS( "- Use descriptive net labels that indicate signal purpose (e.g., SPI_CLK, I2C_SDA)\n" )
           << wxS( "- Place junctions clearly at connection points\n" )
           << wxS( "- Organize related components and signals together\n" )
           << wxS( "- Consider signal flow direction when placing elements\n" )
           << wxS( "- Always use millimeters for all coordinates\n\n" )
           << wxS( "Remember: Tool calls must be valid JSON on a single line starting with 'TOOL'. " )
           << wxS( "Double-check your JSON syntax before emitting tool calls.\n" );

    m_systemPrompt = prompt;
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


void SCH_OLLAMA_AGENT_TOOL::setTransitions()
{
    Go( &SCH_OLLAMA_AGENT_TOOL::ProcessRequest, SCH_ACTIONS::ollamaAgentRequest.MakeEvent() );
    Go( &SCH_OLLAMA_AGENT_TOOL::ShowAgentDialog, SCH_ACTIONS::ollamaAgentDialog.MakeEvent() );
}
