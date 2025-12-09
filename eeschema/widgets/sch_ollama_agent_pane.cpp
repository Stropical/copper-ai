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

#include "sch_ollama_agent_pane.h"
#include <sch_edit_frame.h>
#include <tools/sch_ollama_agent_tool.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <algorithm>

// Message bubble panel for chat messages
class MESSAGE_BUBBLE : public wxPanel
{
public:
    MESSAGE_BUBBLE( wxWindow* aParent, const wxString& aMessage, bool aIsUser ) :
        wxPanel( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE )
    {
        SetBackgroundColour( aIsUser ? wxColour( 0, 122, 255 ) : wxColour( 240, 240, 240 ) );
        
        wxBoxSizer* sizer = new wxBoxSizer( wxHORIZONTAL );
        
        wxTextCtrl* textCtrl = new wxTextCtrl( this, wxID_ANY, aMessage,
                                              wxDefaultPosition, wxDefaultSize,
                                              wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP | wxBORDER_NONE );
        
        textCtrl->SetBackgroundColour( GetBackgroundColour() );
        textCtrl->SetForegroundColour( aIsUser ? *wxWHITE : *wxBLACK );
        
        // Set font
        wxFont font = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
        font.SetPointSize( 10 );
        textCtrl->SetFont( font );
        
        // Calculate optimal width (max 500px, min 200px)
        int maxWidth = 500;
        int minWidth = 200;
        
        // Measure text width
        wxClientDC dc( this );
        dc.SetFont( font );
        wxSize textSize = dc.GetMultiLineTextExtent( aMessage );
        int textWidth = std::min( std::max( textSize.GetWidth() + 40, minWidth ), maxWidth );
        
        textCtrl->SetMinSize( wxSize( textWidth, -1 ) );
        
        sizer->Add( textCtrl, 1, wxEXPAND | wxALL, 10 );
        SetSizer( sizer );
        
        // Align user messages to right, agent messages to left
        if( aIsUser )
            sizer->AddStretchSpacer();
        
        Layout();
        
        // Set size based on content
        textCtrl->Fit();
        int height = textCtrl->GetSize().GetHeight() + 20;
        SetMinSize( wxSize( -1, height ) );
        SetMaxSize( wxSize( -1, height ) );
    }
};


SCH_OLLAMA_AGENT_PANE::SCH_OLLAMA_AGENT_PANE( SCH_EDIT_FRAME* aParent ) :
    WX_PANEL( aParent ),
    m_frame( aParent ),
    m_tool( nullptr ),
    m_isProcessing( false )
{
    wxASSERT( dynamic_cast<SCH_EDIT_FRAME*>( aParent ) );

    // Main sizer
    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );
    
    // Header
    wxPanel* headerPanel = new wxPanel( this, wxID_ANY );
    headerPanel->SetBackgroundColour( wxColour( 250, 250, 250 ) );
    wxBoxSizer* headerSizer = new wxBoxSizer( wxHORIZONTAL );
    
    wxStaticText* titleText = new wxStaticText( headerPanel, wxID_ANY, _( "Schematic AI Agent" ) );
    wxFont titleFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    titleFont.SetPointSize( 12 );
    titleFont.SetWeight( wxFONTWEIGHT_BOLD );
    titleText->SetFont( titleFont );
    
    headerSizer->Add( titleText, 0, wxALL, 10 );
    headerSizer->AddStretchSpacer();
    
    m_clearButton = new wxButton( headerPanel, wxID_ANY, _( "Clear" ) );
    headerSizer->Add( m_clearButton, 0, wxALL, 5 );
    
    headerPanel->SetSizer( headerSizer );
    mainSizer->Add( headerPanel, 0, wxEXPAND );
    
    // Chat area (scrollable)
    m_chatPanel = new wxScrolledWindow( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxVSCROLL | wxHSCROLL | wxBORDER_SIMPLE );
    m_chatPanel->SetBackgroundColour( *wxWHITE );
    m_chatPanel->SetScrollRate( 0, 10 );
    
    m_chatSizer = new wxBoxSizer( wxVERTICAL );
    m_chatPanel->SetSizer( m_chatSizer );
    
    // Add welcome message
    AddAgentMessage( _( "Hello! I'm your schematic AI assistant. I can help you create junctions, wires, labels, and text elements.\n\nTry asking me to:\n- Add a junction at 100mm, 50mm\n- Draw a wire from 50mm, 50mm to 150mm, 50mm\n- Add a label 'VCC' at 100mm, 100mm" ) );
    
    mainSizer->Add( m_chatPanel, 1, wxEXPAND | wxALL, 5 );
    
    // Input area
    wxPanel* inputPanel = new wxPanel( this, wxID_ANY );
    inputPanel->SetBackgroundColour( wxColour( 250, 250, 250 ) );
    wxBoxSizer* inputSizer = new wxBoxSizer( wxHORIZONTAL );
    
    m_inputCtrl = new wxTextCtrl( inputPanel, wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxSize( -1, 80 ),
                                  wxTE_MULTILINE | wxTE_PROCESS_ENTER );
    m_inputCtrl->SetHint( _( "Type your request here... (Press Ctrl+Enter to send)" ) );
    
    m_sendButton = new wxButton( inputPanel, wxID_OK, _( "Send" ) );
    m_sendButton->SetDefault();
    m_sendButton->SetMinSize( wxSize( 80, -1 ) );
    
    inputSizer->Add( m_inputCtrl, 1, wxEXPAND | wxALL, 5 );
    inputSizer->Add( m_sendButton, 0, wxALIGN_BOTTOM | wxALL, 5 );
    
    inputPanel->SetSizer( inputSizer );
    mainSizer->Add( inputPanel, 0, wxEXPAND | wxALL, 5 );
    
    // Status bar
    wxStaticLine* line = new wxStaticLine( this, wxID_ANY );
    mainSizer->Add( line, 0, wxEXPAND | wxLEFT | wxRIGHT, 5 );
    
    wxPanel* statusPanel = new wxPanel( this, wxID_ANY );
    statusPanel->SetBackgroundColour( wxColour( 250, 250, 250 ) );
    wxBoxSizer* statusSizer = new wxBoxSizer( wxHORIZONTAL );
    
    wxStaticText* statusText = new wxStaticText( statusPanel, wxID_ANY, 
                                                _( "Connected to Ollama" ) );
    statusText->SetForegroundColour( wxColour( 100, 100, 100 ) );
    statusSizer->Add( statusText, 0, wxALL, 5 );
    statusSizer->AddStretchSpacer();
    
    statusPanel->SetSizer( statusSizer );
    mainSizer->Add( statusPanel, 0, wxEXPAND );
    
    SetSizer( mainSizer );
    
    // Event handlers
    Bind( wxEVT_COMMAND_BUTTON_CLICKED, &SCH_OLLAMA_AGENT_PANE::onSendButton, this, wxID_OK );
    m_clearButton->Bind( wxEVT_COMMAND_BUTTON_CLICKED, 
                        [this]( wxCommandEvent& ) { ClearChat(); } );
    m_inputCtrl->Bind( wxEVT_KEY_DOWN, &SCH_OLLAMA_AGENT_PANE::onInputKeyDown, this );
    
    // Focus on input
    m_inputCtrl->SetFocus();
}


SCH_OLLAMA_AGENT_PANE::~SCH_OLLAMA_AGENT_PANE()
{
}


void SCH_OLLAMA_AGENT_PANE::AddUserMessage( const wxString& aMessage )
{
    if( aMessage.IsEmpty() )
        return;
    
    addMessageToChat( aMessage, true );
}


void SCH_OLLAMA_AGENT_PANE::AddAgentMessage( const wxString& aMessage )
{
    if( aMessage.IsEmpty() )
        return;
    
    addMessageToChat( aMessage, false );
}


void SCH_OLLAMA_AGENT_PANE::addMessageToChat( const wxString& aMessage, bool aIsUser )
{
    MESSAGE_BUBBLE* bubble = new MESSAGE_BUBBLE( m_chatPanel, aMessage, aIsUser );
    
    // Align user messages to right
    if( aIsUser )
    {
        m_chatSizer->AddStretchSpacer();
        m_chatSizer->Add( bubble, 0, wxALIGN_RIGHT | wxALL, 5 );
    }
    else
    {
        m_chatSizer->Add( bubble, 0, wxALIGN_LEFT | wxALL, 5 );
    }
    
    m_chatSizer->Layout();
    m_chatPanel->Layout();
    scrollToBottom();
    
    // Refresh to show new message
    m_chatPanel->Refresh();
    Update();
}


void SCH_OLLAMA_AGENT_PANE::ClearChat()
{
    m_chatSizer->Clear( true );
    AddAgentMessage( _( "Chat cleared. How can I help you?" ) );
}


void SCH_OLLAMA_AGENT_PANE::onSendButton( wxCommandEvent& aEvent )
{
    sendMessage();
}


void SCH_OLLAMA_AGENT_PANE::onInputKeyDown( wxKeyEvent& aEvent )
{
    // Ctrl+Enter or Cmd+Enter to send
    if( ( aEvent.GetModifiers() == wxMOD_CONTROL || aEvent.GetModifiers() == wxMOD_CMD ) &&
        aEvent.GetKeyCode() == WXK_RETURN )
    {
        sendMessage();
    }
    else
    {
        aEvent.Skip();
    }
}


void SCH_OLLAMA_AGENT_PANE::sendMessage()
{
    wxString message = m_inputCtrl->GetValue().Trim();
    
    if( message.IsEmpty() || m_isProcessing || !m_tool )
        return;
    
    // Add user message to chat
    AddUserMessage( message );
    
    // Clear input
    m_inputCtrl->Clear();
    m_inputCtrl->SetFocus();
    
    // Disable send button
    m_isProcessing = true;
    m_sendButton->Enable( false );
    m_sendButton->SetLabel( _( "Processing..." ) );
    
    // Process request
    wxString response;
    bool success = false;
    
    if( m_tool->GetOllama() && m_tool->GetOllama()->IsAvailable() )
    {
        wxString prompt = m_tool->BuildPrompt( message );
        success = m_tool->GetOllama()->ChatCompletion( m_tool->GetModel(), prompt, response );
        
        if( success )
        {
            AddAgentMessage( response );
            m_tool->ParseAndExecute( response );
        }
        else
        {
            AddAgentMessage( _( "Error: Failed to communicate with Ollama server. Make sure Ollama is running on localhost:11434" ) );
        }
    }
    else
    {
        AddAgentMessage( _( "Error: Ollama server not available. Make sure Ollama is running on localhost:11434" ) );
    }
    
    // Process pending events to update UI
    wxYield();
    
    // Re-enable send button
    m_isProcessing = false;
    m_sendButton->Enable( true );
    m_sendButton->SetLabel( _( "Send" ) );
    
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::scrollToBottom()
{
    wxSize size = m_chatPanel->GetVirtualSize();
    m_chatPanel->Scroll( 0, size.GetHeight() );
    m_chatPanel->Refresh();
}

