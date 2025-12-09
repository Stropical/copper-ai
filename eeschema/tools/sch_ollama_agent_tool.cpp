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
    m_model( wxS( "llama2" ) )  // Default model
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

    // Build prompt
    wxString prompt = BuildPrompt( userRequest );

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
    if( !m_ollama->ChatCompletion( m_model, prompt, response ) )
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
    wxString prompt = wxS( "You are an AI assistant helping to create electronic schematics in KiCad. " );
    prompt += wxS( "When the user requests schematic operations, respond with simple commands in this format:\n" );
    prompt += wxS( "- JUNCTION x y (add junction at position x, y in millimeters)\n" );
    prompt += wxS( "- WIRE x1 y1 x2 y2 (add wire from x1,y1 to x2,y2 in millimeters)\n" );
    prompt += wxS( "- LABEL x y \"text\" (add label at x,y with text)\n" );
    prompt += wxS( "- TEXT x y \"text\" (add text at x,y)\n" );
    prompt += wxS( "\nUser request: " );
    prompt += aUserRequest;
    prompt += wxS( "\n\nRespond with only the commands, one per line." );

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

        // Parse JUNCTION command
        if( line.Upper().StartsWith( wxS( "JUNCTION" ) ) )
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
        else if( line.Upper().StartsWith( wxS( "WIRE" ) ) )
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
        else if( line.Upper().StartsWith( wxS( "LABEL" ) ) )
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
        else if( line.Upper().StartsWith( wxS( "TEXT" ) ) )
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


void SCH_OLLAMA_AGENT_TOOL::setTransitions()
{
    Go( &SCH_OLLAMA_AGENT_TOOL::ProcessRequest, SCH_ACTIONS::ollamaAgentRequest.MakeEvent() );
    Go( &SCH_OLLAMA_AGENT_TOOL::ShowAgentDialog, SCH_ACTIONS::ollamaAgentDialog.MakeEvent() );
}

