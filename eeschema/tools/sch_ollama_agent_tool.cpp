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
            DisplayError( m_frame, _( "Failed to initialize Ollama client. Please check your network configuration." ) );
            return 0;
        }
    }

    // Send to Ollama
    wxString response;
    if( !m_ollama->ChatCompletion( m_model, prompt, response, systemPrompt ) )
    {
        DisplayError( m_frame, _( "Failed to communicate with Ollama server." ) );
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


wxString SCH_OLLAMA_AGENT_TOOL::BuildPrompt( const wxString& aUserRequest )
{
    wxString prompt;
    prompt << wxS( "USER REQUEST:\n" ) << aUserRequest << wxS( "\n\n" );
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
            wxS( "mock.selection_inspector" ),
            _( "Mock tool that inspects the current selection and returns a JSON summary." ),
            wxS( "TOOL mock.selection_inspector {\"scope\":\"sheet\"}" ) } );

    wxString prompt;
    prompt << wxS( "You are an AI assistant helping to create and document KiCad schematics. " )
           << wxS( "Always respond using two sections:\n" )
           << wxS( "TASKS:\n- bullet list explaining how you will update the schematic\n\n" )
           << wxS( "COMMANDS:\n- one command per line using the supported syntax (JUNCTION, WIRE, LABEL, TEXT).\n" )
           << wxS( "Never mix prose inside COMMANDS.\n" );

    if( !m_toolCatalog.empty() )
    {
        prompt << wxS( "\nAvailable tools:\n" );

        for( const TOOL_DESCRIPTOR& tool : m_toolCatalog )
        {
            prompt << wxS( "- " ) << tool.name << wxS( ": " ) << tool.description << wxS( "\n  Usage: " )
                   << tool.usage << wxS( "\n" );
        }

        prompt << wxS( "When you must call a tool, emit a line that begins with TOOL <name> followed by a JSON "
                        "payload describing the input. After the tool call, continue with TASKS/COMMANDS.\n" );
    }

    prompt << wxS( "\nKeep the tone concise and professional. Mention units in millimeters when coordinates "
                    "are required.\n" );

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

    wxLogWarning( wxS( "[OllamaAgent] Unknown tool requested: %s" ), aToolName.wx_str() );
    return false;
}


void SCH_OLLAMA_AGENT_TOOL::setTransitions()
{
    Go( &SCH_OLLAMA_AGENT_TOOL::ProcessRequest, SCH_ACTIONS::ollamaAgentRequest.MakeEvent() );
    Go( &SCH_OLLAMA_AGENT_TOOL::ShowAgentDialog, SCH_ACTIONS::ollamaAgentDialog.MakeEvent() );
}
