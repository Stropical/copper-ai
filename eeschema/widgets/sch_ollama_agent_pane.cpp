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
#include <wx/dcbuffer.h>
#include <wx/thread.h>
#include <widgets/ui_common.h>
#include <wx/stc/stc.h>
#include <scintilla_tricks.h>
#include <algorithm>
#include <thread>
#include <mutex>

namespace
{
const wxColour CURSOR_BG( 15, 17, 22 );
const wxColour CURSOR_SURFACE( 26, 28, 36 );
const wxColour CURSOR_HEADER( 20, 22, 30 );
const wxColour CURSOR_BORDER( 48, 52, 63 );
const wxColour CURSOR_MUTED( 150, 152, 168 );
const wxColour CURSOR_PRIMARY( 228, 230, 238 );
const wxColour CURSOR_AGENT_BUBBLE( 34, 36, 45 );
const wxColour CURSOR_AGENT_BORDER( 50, 54, 66 );
const wxColour CURSOR_AGENT_TEXT( 220, 222, 233 );
const wxColour CURSOR_USER_BUBBLE( 26, 26, 26 );
const wxColour CURSOR_USER_BORDER( 40, 44, 56 );
const wxColour CURSOR_THINK_BUBBLE( 26, 28, 36 );
const wxColour CURSOR_THINK_BORDER( 40, 44, 56 );
const wxColour CURSOR_ACCENT( 124, 101, 255 );
const wxColour CURSOR_SUCCESS( 79, 224, 182 );
const wxColour CURSOR_DANGER( 233, 97, 74 );
}

// Message bubble panel for chat messages with Cursor-inspired styling.
class MESSAGE_BUBBLE : public wxPanel
{
public:
    MESSAGE_BUBBLE( wxWindow* aParent, const wxString& aMessage, bool aIsUser, bool aIsThinking = false ) :
        wxPanel( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE ),
        m_textLabel( nullptr ),
        m_isUser( aIsUser ),
        m_isThinking( aIsThinking ),
        m_cornerRadius( 12 ),
        m_padding( 12 ),
        m_maxWidth( 520 )
    {
        if( aIsThinking )
        {
            m_bgColor = CURSOR_THINK_BUBBLE;
            m_textColor = CURSOR_MUTED;
            m_borderColor = CURSOR_THINK_BORDER;
        }
        else if( aIsUser )
        {
            m_bgColor = CURSOR_USER_BUBBLE;
            m_textColor = *wxWHITE;
            m_borderColor = CURSOR_USER_BORDER;
        }
        else
        {
            m_bgColor = CURSOR_AGENT_BUBBLE;
            m_textColor = CURSOR_AGENT_TEXT;
            m_borderColor = CURSOR_AGENT_BORDER;
        }

        SetBackgroundColour( CURSOR_BG );
        SetBackgroundStyle( wxBG_STYLE_PAINT );
        SetDoubleBuffered( true );

        wxBoxSizer* mainSizer = new wxBoxSizer( wxHORIZONTAL );
        if( !aIsUser )
            mainSizer->Add( 0, 0, 0, wxEXPAND, 0 );

        m_contentSizer = new wxBoxSizer( wxVERTICAL );

        if( aIsThinking )
        {
            wxStaticText* thinkingText = new wxStaticText( this, wxID_ANY, _( "Thinking..." ) );
            thinkingText->SetForegroundColour( m_textColor );
            wxFont font = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
            font.SetPointSize( 11 );
            thinkingText->SetFont( font );
            thinkingText->Wrap( m_maxWidth - ( m_padding * 2 ) );
            m_contentSizer->Add( thinkingText, 0, wxEXPAND | wxALL, m_padding );
        }
        else
        {
            m_textLabel = new wxStaticText( this, wxID_ANY, aMessage, wxDefaultPosition, wxDefaultSize,
                                            wxST_NO_AUTORESIZE );
            m_textLabel->SetBackgroundStyle( wxBG_STYLE_TRANSPARENT );
            m_textLabel->SetForegroundColour( m_textColor );

            wxFont font = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
            font.SetPointSize( 11 );
            font.SetFamily( wxFONTFAMILY_DEFAULT );
            m_textLabel->SetFont( font );

            m_contentSizer->Add( m_textLabel, 0, wxEXPAND | wxALL, m_padding );
            UpdateTextControl( aMessage );
        }

        mainSizer->Add( m_contentSizer, 0,
                        aIsUser ? ( wxALIGN_RIGHT | wxEXPAND ) : ( wxALIGN_LEFT | wxEXPAND ), 0 );

        if( aIsUser )
            mainSizer->Add( 0, 0, 1, wxEXPAND, 0 );
        else
            mainSizer->Add( 0, 0, 0, wxEXPAND, 0 );

        SetSizer( mainSizer );
        Layout();
        Fit();

        Bind( wxEVT_PAINT, &MESSAGE_BUBBLE::OnPaint, this );
    }

    void UpdateText( const wxString& aMessage )
    {
        if( !m_textLabel )
            return;

        m_textLabel->SetLabel( aMessage );
        UpdateTextControl( aMessage );

        if( wxWindow* parent = GetParent() )
            parent->Layout();

        Refresh();
    }

private:
    void OnPaint( wxPaintEvent& )
    {
        wxAutoBufferedPaintDC dc( this );
        wxSize size = GetClientSize();

        dc.SetPen( wxPen( m_borderColor, 1 ) );
        dc.SetBrush( wxBrush( m_bgColor ) );
        dc.DrawRoundedRectangle( 0, 0, size.GetWidth(), size.GetHeight(), m_cornerRadius );
    }

    void UpdateTextControl( const wxString& aMessage )
    {
        if( !m_textLabel )
            return;

        wxWindow* parent = GetParent();
        int minWidth = 240;
        int parentWidth = parent ? parent->GetClientSize().GetWidth() : m_maxWidth;

        if( parentWidth <= 0 )
            parentWidth = m_maxWidth;

        int availableWidth = std::max( minWidth, parentWidth - 80 );    // keep left/right padding
        int wrapWidth = std::min( m_maxWidth, availableWidth );

        m_textLabel->Wrap( wrapWidth - ( m_padding * 2 ) );

        wxSize bestSize = m_textLabel->GetBestSize();
        m_textLabel->SetMinSize( wxSize( wrapWidth, bestSize.GetHeight() ) );

        m_contentSizer->Layout();
        Layout();
        Fit();
        Refresh();
    }

    wxStaticText* m_textLabel;
    wxBoxSizer* m_contentSizer;
    wxColour m_bgColor;
    wxColour m_textColor;
    wxColour m_borderColor;
    bool m_isUser;
    bool m_isThinking;
    int m_cornerRadius;
    int m_padding;
    int m_maxWidth;
};


SCH_OLLAMA_AGENT_PANE::SCH_OLLAMA_AGENT_PANE( SCH_EDIT_FRAME* aParent ) :
    WX_PANEL( aParent ),
    m_frame( aParent ),
    m_tool( nullptr ),
    m_chatPanel( nullptr ),
    m_chatSizer( nullptr ),
    m_inputCtrl( nullptr ),
    m_sendButton( nullptr ),
    m_clearButton( nullptr ),
    m_cancelButton( nullptr ),
    m_statusText( nullptr ),
    m_isProcessing( false ),
    m_cancelRequested( false ),
    m_thinkingBubble( nullptr ),
    m_streamingBubble( nullptr )
{
    wxASSERT( dynamic_cast<SCH_EDIT_FRAME*>( aParent ) );

    SetBackgroundColour( CURSOR_BG );
    SetDoubleBuffered( true );

    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    wxPanel* headerPanel = new wxPanel( this, wxID_ANY );
    headerPanel->SetBackgroundColour( CURSOR_HEADER );
    wxBoxSizer* headerSizer = new wxBoxSizer( wxHORIZONTAL );

    wxStaticText* titleText = new wxStaticText( headerPanel, wxID_ANY, _( "Schematic AI Agent" ) );
    wxFont titleFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    titleFont.SetPointSize( 12 );
    titleFont.SetWeight( wxFONTWEIGHT_BOLD );
    titleText->SetFont( titleFont );
    titleText->SetForegroundColour( CURSOR_PRIMARY );
    headerSizer->Add( titleText, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 16 );

    m_statusText = new wxStaticText( headerPanel, wxID_ANY, _( "Checking Python agent..." ) );
    m_statusText->SetForegroundColour( CURSOR_MUTED );
    headerSizer->Add( m_statusText, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12 );

    headerSizer->AddStretchSpacer();

    m_clearButton = new wxButton( headerPanel, wxID_ANY, _( "Clear" ) );
    m_clearButton->SetMinSize( wxSize( 90, 32 ) );
    headerSizer->Add( m_clearButton, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6 );

    headerPanel->SetSizer( headerSizer );
    mainSizer->Add( headerPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6 );

    m_chatPanel = new wxScrolledWindow( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                       wxVSCROLL | wxBORDER_NONE );
    m_chatPanel->SetBackgroundColour( CURSOR_BG );
    m_chatPanel->SetScrollRate( 0, 15 );
    m_chatPanel->EnableScrolling( false, true );

    m_chatSizer = new wxBoxSizer( wxVERTICAL );
    m_chatPanel->SetSizer( m_chatSizer );
    m_chatSizer->AddSpacer( 8 );

    wxStaticText* welcomeText = new wxStaticText( m_chatPanel, wxID_ANY,
        _( "Cursor-style assistant at your service. Ask me to rework nets, labels, or entire schematics." ) );
    welcomeText->SetForegroundColour( CURSOR_MUTED );
    wxFont welcomeFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    welcomeFont.SetPointSize( 10 );
    welcomeText->SetFont( welcomeFont );
    m_chatSizer->Add( welcomeText, 0, wxALL, 16 );
    m_chatSizer->AddSpacer( 6 );

    mainSizer->Add( m_chatPanel, 1, wxEXPAND | wxLEFT | wxRIGHT, 6 );

    wxPanel* inputPanel = new wxPanel( this, wxID_ANY );
    inputPanel->SetBackgroundColour( CURSOR_BG );
    inputPanel->SetDoubleBuffered( true );
    wxBoxSizer* inputSizer = new wxBoxSizer( wxVERTICAL );

    wxPanel* composerPanel = new wxPanel( inputPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxBORDER_SIMPLE );
    composerPanel->SetBackgroundColour( CURSOR_SURFACE );
    composerPanel->SetForegroundColour( CURSOR_BORDER );
    wxBoxSizer* composerSizer = new wxBoxSizer( wxHORIZONTAL );

    // Set up exactly like Text Properties dialog
    m_inputCtrl = new wxStyledTextCtrl( composerPanel, wxID_ANY,
                                        wxDefaultPosition, wxSize( -1, 90 ),
                                        wxBORDER_NONE );
    m_inputCtrl->SetBackgroundColour( CURSOR_SURFACE );
    m_inputCtrl->SetCaretForeground( CURSOR_PRIMARY );
    m_inputCtrl->StyleSetForeground( wxSTC_STYLE_DEFAULT, CURSOR_PRIMARY );
    wxFont inputFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    inputFont.SetPointSize( 11 );
    m_inputCtrl->StyleSetFont( wxSTC_STYLE_DEFAULT, inputFont );
    m_inputCtrl->SetMinSize( wxSize( -1, 90 ) );

    m_inputCtrl->SetEOLMode( wxSTC_EOL_LF );

#ifdef _WIN32
    // Without this setting, on Windows, some esoteric unicode chars create display issue
    // in a wxStyledTextCtrl.
    m_inputCtrl->SetTechnology( wxSTC_TECHNOLOGY_DIRECTWRITE );
#endif

    // Set up SCINTILLA_TRICKS exactly like Text Properties dialog
    m_scintillaTricks = new SCINTILLA_TRICKS( m_inputCtrl, wxT( "" ), false,
            // onAcceptFn - Ctrl/Cmd+Enter or Shift+Enter to send
            [this]( wxKeyEvent& aEvent )
            {
                if( ( aEvent.GetModifiers() == wxMOD_CONTROL || aEvent.GetModifiers() == wxMOD_CMD ) &&
                    ( aEvent.GetKeyCode() == WXK_RETURN || aEvent.GetKeyCode() == WXK_NUMPAD_ENTER ) )
                {
                    sendMessage();
                }
            },
            // onCharFn - no autocomplete needed
            []( wxStyledTextEvent& ) {} );

    // A hack which causes Scintilla to auto-size the text editor canvas
    // See: https://github.com/jacobslusser/ScintillaNET/issues/216
    m_inputCtrl->SetScrollWidth( 1 );
    m_inputCtrl->SetScrollWidthTracking( true );

    KIUI::RegisterHotkeySuppressor( m_inputCtrl );

    m_sendButton = new wxButton( composerPanel, wxID_OK, _( "Send" ) );
    m_sendButton->SetDefault();

    composerSizer->Add( m_inputCtrl, 1, wxEXPAND | wxALL, 12 );
    composerSizer->Add( m_sendButton, 0, wxALIGN_BOTTOM | wxRIGHT | wxTOP | wxBOTTOM, 12 );
    composerPanel->SetSizer( composerSizer );
    inputSizer->Add( composerPanel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12 );

    wxBoxSizer* footerSizer = new wxBoxSizer( wxHORIZONTAL );

    wxStaticText* helperText = new wxStaticText( inputPanel, wxID_ANY,
        _( "Shift+Enter for newline  |  Esc to cancel streaming" ) );
    helperText->SetForegroundColour( CURSOR_MUTED );
    wxFont helperFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    helperFont.SetPointSize( 9 );
    helperText->SetFont( helperFont );
    footerSizer->Add( helperText, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12 );

    m_cancelButton = new wxButton( inputPanel, wxID_ANY, _( "Stop" ) );
    m_cancelButton->SetMinSize( wxSize( 90, 32 ) );
    m_cancelButton->Enable( false );
    footerSizer->Add( m_cancelButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6 );

    inputSizer->Add( footerSizer, 0, wxEXPAND | wxALL, 12 );

    inputPanel->SetSizer( inputSizer );
    mainSizer->Add( inputPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6 );

    SetSizer( mainSizer );
    
    // Event handlers
    Bind( wxEVT_COMMAND_BUTTON_CLICKED, &SCH_OLLAMA_AGENT_PANE::onSendButton, this, wxID_OK );
    m_clearButton->Bind( wxEVT_COMMAND_BUTTON_CLICKED, 
                        [this]( wxCommandEvent& ) { ClearChat(); } );
    m_cancelButton->Bind( wxEVT_COMMAND_BUTTON_CLICKED,
                          [this]( wxCommandEvent& ) { CancelCurrentRequest(); } );
    
    // Bind async response handlers - all use wxCommandEvent/wxEVT_COMMAND_TEXT_UPDATED with different IDs
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnResponseReceived, this, ID_RESPONSE_RECEIVED );
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnRequestFailed, this, ID_REQUEST_FAILED );
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnResponsePartial, this, ID_RESPONSE_PARTIAL );
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnRequestCancelled, this, ID_REQUEST_CANCELLED );
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnConnectionCheckResult, this, ID_CONNECTION_CHECK_RESULT );
    
    // Focus on input (exactly like Text Properties dialog)
    m_inputCtrl->SetFocus();

    // Register the entire pane as a hotkey suppressor so any focus within the pane
    // (including buttons, etc.) will suppress editor hotkeys.
    KIUI::RegisterHotkeySuppressor( this, true );
}


SCH_OLLAMA_AGENT_PANE::~SCH_OLLAMA_AGENT_PANE()
{
    delete m_scintillaTricks;

    KIUI::UnregisterHotkeySuppressor( this );
    KIUI::UnregisterHotkeySuppressor( m_inputCtrl );

    // Wait for any running thread to finish
    {
        std::lock_guard<std::mutex> lock( m_threadMutex );
        if( m_requestThread.joinable() )
        {
            m_requestThread.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock( m_connectionMutex );
        if( m_connectionThread.joinable() )
        {
            m_connectionThread.join();
        }
    }
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


void SCH_OLLAMA_AGENT_PANE::addMessageToChat( const wxString& aMessage, bool aIsUser, bool aIsThinking )
{
    MESSAGE_BUBBLE* bubble = new MESSAGE_BUBBLE( m_chatPanel, aMessage, aIsUser, aIsThinking );
    
    // Store reference if it's a thinking message
    if( aIsThinking )
    {
        m_thinkingBubble = bubble;
    }
    
    // Store reference if it's a streaming message (not thinking, not user)
    if( !aIsUser && !aIsThinking )
    {
        m_streamingBubble = bubble;
    }
    
    // Create horizontal wrapper to properly align the bubble
    wxBoxSizer* rowSizer = new wxBoxSizer( wxHORIZONTAL );
    if( aIsUser )
        rowSizer->Add( 0, 0, 1, wxEXPAND, 0 );  // Push to right
    
    // Allow bubble to expand vertically but constrain width
    rowSizer->Add( bubble, 0, wxALIGN_TOP | wxALL, 10 );
    
    if( !aIsUser )
        rowSizer->Add( 0, 0, 1, wxEXPAND, 0 );  // Empty space on right
    
    // Add row to chat sizer - allow it to expand horizontally but not vertically (bubble handles its own height)
    m_chatSizer->Add( rowSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 5 );
    
    // Force layout update to recalculate sizes
    m_chatSizer->Layout();
    m_chatPanel->SetVirtualSize( m_chatSizer->GetMinSize() );
    m_chatPanel->Layout();
    scrollToBottom();
    
    // Refresh to show new message
    m_chatPanel->Refresh();
}


void SCH_OLLAMA_AGENT_PANE::removeThinkingMessage()
{
    if( m_thinkingBubble )
    {
        m_chatSizer->Detach( m_thinkingBubble );
        m_thinkingBubble->Destroy();
        m_thinkingBubble = nullptr;
        
        m_chatSizer->Layout();
        m_chatPanel->Layout();
        m_chatPanel->Refresh();
    }
}


void SCH_OLLAMA_AGENT_PANE::ClearChat()
{
    m_chatSizer->Clear( true );
    m_streamingBubble = nullptr;
    m_streamingText.clear();
    m_thinkingBubble = nullptr;
    AddAgentMessage( _( "History cleared. Tell me what you want to build next." ) );
}


void SCH_OLLAMA_AGENT_PANE::SetTool( SCH_OLLAMA_AGENT_TOOL* aTool )
{
    m_tool = aTool;
    StartConnectionCheck();
}


void SCH_OLLAMA_AGENT_PANE::CancelCurrentRequest()
{
    if( !m_isProcessing || m_cancelRequested.load() )
        return;

    m_cancelRequested.store( true );

    if( m_cancelButton )
        m_cancelButton->Enable( false );

    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Cancelling request..." ) );
        m_statusText->SetForegroundColour( CURSOR_DANGER );
    }

    removeThinkingMessage();
    if( m_streamingBubble )
    {
        m_chatSizer->Detach( m_streamingBubble );
        m_streamingBubble->Destroy();
        m_streamingBubble = nullptr;
    }
    m_streamingText.clear();
}


void SCH_OLLAMA_AGENT_PANE::StartConnectionCheck()
{
    if( !m_tool )
        return;

    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Checking Python agent connection..." ) );
        m_statusText->SetForegroundColour( CURSOR_MUTED );
    }

    std::lock_guard<std::mutex> lock( m_connectionMutex );
    if( m_connectionThread.joinable() )
        m_connectionThread.join();

    m_connectionThread = std::thread( [this]()
    {
        bool success = false;

        if( m_tool )
        {
            if( OLLAMA_CLIENT* client = m_tool->GetOllama() )
            {
                // Use IsAvailable() for quick connection check (no timeout issues)
                success = client->IsAvailable();
            }
        }

        wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_CONNECTION_CHECK_RESULT );
        event->SetInt( success ? 1 : 0 );
        event->SetString( success ? _( "Connected to Python agent" ) : _( "Unable to reach Python agent" ) );
        wxQueueEvent( this, event );
    } );
}


void SCH_OLLAMA_AGENT_PANE::OnRequestCancelled( wxCommandEvent& aEvent )
{
    (void)aEvent;
    removeThinkingMessage();

    if( m_streamingBubble )
    {
        m_chatSizer->Detach( m_streamingBubble );
        m_streamingBubble->Destroy();
        m_streamingBubble = nullptr;
    }
    m_streamingText.clear();

    m_isProcessing = false;
    if( m_sendButton )
    {
        m_sendButton->Enable( true );
        m_sendButton->SetLabel( _( "Send" ) );
    }
    if( m_cancelButton )
        m_cancelButton->Enable( false );

    m_cancelRequested.store( false );

    AddAgentMessage( _( "Request cancelled. Ready when you are." ) );
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::OnConnectionCheckResult( wxCommandEvent& aEvent )
{
    if( !m_statusText )
        return;

    bool success = ( aEvent.GetInt() == 1 );
    m_statusText->SetLabel( aEvent.GetString() );
    m_statusText->SetForegroundColour( success ? CURSOR_SUCCESS : CURSOR_DANGER );
    m_statusText->Refresh();
}


void SCH_OLLAMA_AGENT_PANE::onSendButton( wxCommandEvent& aEvent )
{
    sendMessage();
}






void SCH_OLLAMA_AGENT_PANE::sendMessage()
{
    wxString message = m_inputCtrl->GetText().Trim();
    
    if( message.IsEmpty() || m_isProcessing || !m_tool )
        return;
    
    m_cancelRequested.store( false );
    // Check if previous thread is still running
    {
        std::lock_guard<std::mutex> lock( m_threadMutex );
        if( m_requestThread.joinable() )
        {
            m_requestThread.join();
        }
    }
    
    // Add user message to chat
    AddUserMessage( message );
    m_streamingBubble = nullptr;
    m_streamingText.clear();
    
    // Clear input
    m_inputCtrl->ClearAll();
    m_inputCtrl->SetFocus();
    
    m_isProcessing = true;
    if( m_sendButton )
    {
        m_sendButton->Enable( false );
        m_sendButton->SetLabel( _( "Processing..." ) );
    }
    if( m_cancelButton )
        m_cancelButton->Enable( true );

    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Processing request..." ) );
        m_statusText->SetForegroundColour( CURSOR_MUTED );
    }

    // Show thinking indicator
    addMessageToChat( wxEmptyString, false, true );
    
    // Check availability first
    OLLAMA_CLIENT* client = m_tool->GetOllama();
    if( !client || !client->IsAvailable() )
    {
        removeThinkingMessage();
        AddAgentMessage( _( "Error: Python agent not available. Make sure the agent is running (default: http://127.0.0.1:5000)" ) );
        m_isProcessing = false;
        if( m_sendButton )
        {
            m_sendButton->Enable( true );
            m_sendButton->SetLabel( _( "Send" ) );
        }
        if( m_cancelButton )
            m_cancelButton->Enable( false );
        return;
    }
    
    // Build prompt on main thread
    wxString prompt = m_tool->BuildPrompt( message );
    wxString model = m_tool->GetModel();
    const wxString& systemPrompt = m_tool->GetSystemPrompt();
    
    // Run Ollama request in background thread
    {
        std::lock_guard<std::mutex> lock( m_threadMutex );
        m_requestThread = std::thread( [this, prompt, model, systemPrompt]()
        {
                wxString response;
                bool success = false;
                
                if( m_tool && m_tool->GetOllama() )
                {
                    auto chunkCallback = [this]( const wxString& chunk )
                    {
                        if( chunk.IsEmpty() || m_cancelRequested.load() )
                            return;

                        wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_RESPONSE_PARTIAL );
                        event->SetString( chunk );
                        wxQueueEvent( this, event );
                    };

                    success = m_tool->GetOllama()->StreamChatCompletion( model, prompt, chunkCallback,
                                                                          response, &m_cancelRequested,
                                                                          systemPrompt );
                }
                
                if( m_cancelRequested.load() )
                {
                    wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_REQUEST_CANCELLED );
                    wxQueueEvent( this, event );
                    return;
                }

                if( success )
                {
                    wxString responseCopy = response;
                    wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_RESPONSE_RECEIVED );
                    event->SetString( responseCopy );
                    wxQueueEvent( this, event );
                }
                else
                {
                    wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_REQUEST_FAILED );
                    wxQueueEvent( this, event );
                }
            } );
    }
    
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::OnResponseReceived( wxCommandEvent& aEvent )
{
    wxString response = aEvent.GetString();
    
    if( m_cancelRequested.load() )
        return;

    // Remove thinking indicator
    removeThinkingMessage();
    
    // Re-enable send button
    m_isProcessing = false;
    m_sendButton->Enable( true );
    m_sendButton->SetLabel( _( "Send" ) );
    if( m_cancelButton )
        m_cancelButton->Enable( false );
    m_cancelRequested.store( false );

    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Connected to Python agent" ) );
        m_statusText->SetForegroundColour( CURSOR_SUCCESS );
    }
    
    // Ensure the full response is displayed in the chat
    // If we were streaming, update the bubble with the complete response
    if( m_streamingBubble )
    {
        MESSAGE_BUBBLE* bubble = dynamic_cast<MESSAGE_BUBBLE*>( m_streamingBubble );
        if( bubble )
        {
            // Update with the complete response (includes TASKS, COMMANDS, etc.)
            bubble->UpdateText( response );
            m_chatSizer->Layout();
            m_chatPanel->Layout();
        }
    }
    else
    {
        // If no streaming bubble exists, create one with the full response
        addMessageToChat( response, false );
    }
    
    // Parse and execute commands from the response
    if( m_tool )
    {
        m_tool->ParseAndExecute( response );
    }
    
    m_streamingBubble = nullptr;
    m_streamingText.clear();
    
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::OnResponsePartial( wxCommandEvent& aEvent )
{
    wxString chunk = aEvent.GetString();
    if( chunk.IsEmpty() )
        return;

    if( m_cancelRequested.load() )
        return;

    // Remove thinking bubble on first chunk
    if( m_thinkingBubble )
    {
        removeThinkingMessage();
    }

    // Accumulate streaming text
    m_streamingText += chunk;

    // Create streaming bubble on first chunk
    if( !m_streamingBubble )
    {
        addMessageToChat( m_streamingText, false );
    }
    else
    {
        // Update the existing streaming bubble
        MESSAGE_BUBBLE* bubble = dynamic_cast<MESSAGE_BUBBLE*>( m_streamingBubble );
        if( bubble )
        {
            bubble->UpdateText( m_streamingText );
            // Update virtual size for scrolling
            m_chatSizer->Layout();
            m_chatPanel->SetVirtualSize( m_chatSizer->GetMinSize() );
            m_chatPanel->Layout();
        }
    }

    scrollToBottom();
    m_chatPanel->Refresh();
}


void SCH_OLLAMA_AGENT_PANE::OnRequestFailed( wxCommandEvent& aEvent )
{
    if( m_cancelRequested.load() )
        return;

    // Remove thinking indicator
    removeThinkingMessage();
    
    if( m_streamingBubble )
    {
        m_streamingBubble = nullptr;
        m_streamingText.clear();
    }

    // Re-enable send button
    m_isProcessing = false;
    if( m_sendButton )
    {
        m_sendButton->Enable( true );
        m_sendButton->SetLabel( _( "Send" ) );
    }
    if( m_cancelButton )
        m_cancelButton->Enable( false );
    m_cancelRequested.store( false );
    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Unable to reach Ollama" ) );
        m_statusText->SetForegroundColour( CURSOR_DANGER );
    }
    
    AddAgentMessage( _( "Error: Failed to communicate with Python agent. Make sure the agent is running (default: http://127.0.0.1:5000)" ) );
    
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::scrollToBottom()
{
    // Update virtual size first to ensure scrolling works correctly
    wxSize sizerSize = m_chatSizer->GetMinSize();
    int panelWidth = m_chatPanel->GetClientSize().GetWidth();
    if( panelWidth <= 0 )
        panelWidth = sizerSize.GetWidth();

    m_chatPanel->SetVirtualSize( wxSize( panelWidth, sizerSize.GetHeight() ) );
    
    // Scroll to bottom
    int scrollUnitY = 0;
    m_chatPanel->GetScrollPixelsPerUnit( nullptr, &scrollUnitY );
    if( scrollUnitY > 0 )
    {
        int maxY = ( sizerSize.GetHeight() + scrollUnitY - 1 ) / scrollUnitY;
        m_chatPanel->Scroll( 0, maxY );
    }
    else
    {
        // Fallback: scroll to maximum
        wxSize virtualSize = m_chatPanel->GetVirtualSize();
        m_chatPanel->Scroll( 0, virtualSize.GetHeight() );
    }
    
    m_chatPanel->Refresh();
}
