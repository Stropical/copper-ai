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

#include "sch_ollama_agent_dialog.h"
#include "sch_ollama_agent_tool.h"
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/timer.h>
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


SCH_OLLAMA_AGENT_DIALOG::SCH_OLLAMA_AGENT_DIALOG( wxWindow* aParent, SCH_OLLAMA_AGENT_TOOL* aTool ) :
    wxDialog( aParent, wxID_ANY, _( "Ollama Schematic Agent" ), 
             wxDefaultPosition, wxSize( 600, 700 ),
             wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX ),
    m_tool( aTool ),
    m_isProcessing( false ),
    m_currentBubble( nullptr )
{
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
                                                _( "Connected to Python agent" ) );
    statusText->SetForegroundColour( wxColour( 100, 100, 100 ) );
    statusSizer->Add( statusText, 0, wxALL, 5 );
    statusSizer->AddStretchSpacer();
    
    statusPanel->SetSizer( statusSizer );
    mainSizer->Add( statusPanel, 0, wxEXPAND );
    
    SetSizer( mainSizer );
    
    // Event handlers
    Bind( wxEVT_COMMAND_BUTTON_CLICKED, &SCH_OLLAMA_AGENT_DIALOG::onSendButton, this, wxID_OK );
    m_clearButton->Bind( wxEVT_COMMAND_BUTTON_CLICKED, 
                        [this]( wxCommandEvent& ) { ClearChat(); } );
    m_inputCtrl->Bind( wxEVT_KEY_DOWN, &SCH_OLLAMA_AGENT_DIALOG::onInputKeyDown, this );
    Bind( wxEVT_CLOSE_WINDOW, &SCH_OLLAMA_AGENT_DIALOG::onClose, this );
    
    // Focus on input
    m_inputCtrl->SetFocus();
    
    Centre();
}


SCH_OLLAMA_AGENT_DIALOG::~SCH_OLLAMA_AGENT_DIALOG()
{
}


void SCH_OLLAMA_AGENT_DIALOG::AddUserMessage( const wxString& aMessage )
{
    if( aMessage.IsEmpty() )
        return;
    
    addMessageToChat( aMessage, true );
}


void SCH_OLLAMA_AGENT_DIALOG::AddAgentMessage( const wxString& aMessage )
{
    if( aMessage.IsEmpty() )
        return;
    
    addMessageToChat( aMessage, false );
}


void SCH_OLLAMA_AGENT_DIALOG::addMessageToChat( const wxString& aMessage, bool aIsUser )
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


void SCH_OLLAMA_AGENT_DIALOG::ClearChat()
{
    m_chatSizer->Clear( true );
    AddAgentMessage( _( "Chat cleared. How can I help you?" ) );
}


void SCH_OLLAMA_AGENT_DIALOG::onSendButton( wxCommandEvent& aEvent )
{
    sendMessage();
}


void SCH_OLLAMA_AGENT_DIALOG::onInputKeyDown( wxKeyEvent& aEvent )
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


void SCH_OLLAMA_AGENT_DIALOG::onClose( wxCloseEvent& aEvent )
{
    if( m_isProcessing )
    {
        if( wxMessageBox( _( "A request is being processed. Close anyway?" ),
                         _( "Close Dialog" ), wxYES_NO | wxICON_QUESTION ) != wxYES )
        {
            aEvent.Veto();
            return;
        }
    }
    
    aEvent.Skip();
}


void SCH_OLLAMA_AGENT_DIALOG::sendMessage()
{
    wxString message = m_inputCtrl->GetValue().Trim();
    
    // Log the captured message for debugging
    wxLogMessage( wxS( "[OllamaAgent] Dialog captured user message: %s" ), message.wx_str() );
    
    if( message.IsEmpty() || m_isProcessing )
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
    
    // Initialize streaming state
    m_currentResponse.Clear();
    m_currentBubble = nullptr;
    
    // Process request with streaming
    wxString fullResponse;
    bool success = false;
    
    if( m_tool && m_tool->GetOllama() )
    {
        // Create a streaming callback that updates the UI incrementally
        auto chunkCallback = [this]( const wxString& chunk )
        {
            if( chunk.IsEmpty() )
                return;
            
            // Accumulate response
            m_currentResponse += chunk;
            
            // Create or update the streaming bubble
            if( !m_currentBubble )
            {
                // Create a new bubble for streaming
                m_currentBubble = new MESSAGE_BUBBLE( m_chatPanel, m_currentResponse, false );
                m_chatSizer->Add( m_currentBubble, 0, wxALIGN_LEFT | wxALL, 5 );
                m_chatSizer->Layout();
                m_chatPanel->Layout();
            }
            else
            {
                // Update the bubble text by finding the text control inside
                wxWindowList& children = m_currentBubble->GetChildren();
                for( wxWindowList::iterator it = children.begin(); it != children.end(); ++it )
                {
                    wxTextCtrl* textCtrl = dynamic_cast<wxTextCtrl*>( *it );
                    if( textCtrl )
                    {
                        textCtrl->SetValue( m_currentResponse );
                        // Force a refresh and layout update
                        textCtrl->Refresh();
                        m_currentBubble->Layout();
                        break;
                    }
                }
            }
            
            // Process events frequently to keep UI responsive during streaming
            // This is critical to prevent freezing
            wxYield();
            
            // Scroll to bottom periodically (not on every chunk to reduce overhead)
            static int scrollCounter = 0;
            if( (++scrollCounter % 10) == 0 )  // Scroll every 10 chunks
            {
                scrollToBottom();
            }
        };
        
        // Send raw user request to Python agent (which handles all prompt building)
        success = m_tool->GetOllama()->StreamChatCompletion( 
            m_tool->GetModel(), message, chunkCallback, fullResponse );
        
        if( success )
        {
            // Finalize the bubble with the complete response
            if( m_currentBubble )
            {
                // Update with final response
                wxWindowList& children = m_currentBubble->GetChildren();
                for( wxWindowList::iterator it = children.begin(); it != children.end(); ++it )
                {
                    wxTextCtrl* textCtrl = dynamic_cast<wxTextCtrl*>( *it );
                    if( textCtrl )
                    {
                        textCtrl->SetValue( fullResponse );
                        textCtrl->Refresh();
                        break;
                    }
                }
                m_chatSizer->Layout();
                m_chatPanel->Layout();
                m_currentBubble = nullptr;
            }
            else
            {
                // No streaming happened, add as normal message
                AddAgentMessage( fullResponse );
            }
            
            m_tool->ParseAndExecute( fullResponse );
        }
        else
        {
            // Clean up streaming bubble on error
            if( m_currentBubble )
            {
                m_chatSizer->Detach( m_currentBubble );
                m_currentBubble->Destroy();
                m_currentBubble = nullptr;
                m_chatSizer->Layout();
                m_chatPanel->Layout();
            }
            
            AddAgentMessage( _( "Error: Failed to communicate with Python agent. Make sure the agent is running (default: http://127.0.0.1:5001)" ) );
        }
    }
    
    // Process pending events to update UI
    wxYield();
    
    // Re-enable send button
    m_isProcessing = false;
    m_sendButton->Enable( true );
    m_sendButton->SetLabel( _( "Send" ) );
    
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_DIALOG::scrollToBottom()
{
    wxSize size = m_chatPanel->GetVirtualSize();
    m_chatPanel->Scroll( 0, size.GetHeight() );
    m_chatPanel->Refresh();
}
