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

#ifndef SCH_OLLAMA_AGENT_PANE_H
#define SCH_OLLAMA_AGENT_PANE_H

#include <widgets/wx_panel.h>
#include <wx/string.h>
#include <wx/event.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <wx/timer.h>

#include <tools/sch_ollama_agent_tool.h>

class wxStyledTextCtrl;
class wxButton;
class SCINTILLA_TRICKS;
class wxScrolledWindow;
class wxBoxSizer;
class wxWindow;
class wxStaticText;
class SCH_EDIT_FRAME;
class TOOL_CALL_BUBBLE;

enum class CHAT_BUBBLE_KIND
{
    USER,
    AGENT,
    THINKING
};

/**
 * Chat-style panel for interacting with Ollama agent.
 * Similar to Cursor's chat interface with message history.
 * This panel is dockable in the schematic editor alongside hierarchy and properties panels.
 */
class SCH_OLLAMA_AGENT_PANE : public WX_PANEL, public SCH_OLLAMA_TOOL_CALL_HANDLER
{
public:
    SCH_OLLAMA_AGENT_PANE( SCH_EDIT_FRAME* aParent );

    ~SCH_OLLAMA_AGENT_PANE() override;

    /**
     * Add a user message to the chat
     */
    void AddUserMessage( const wxString& aMessage );

    /**
     * Add an agent response to the chat
     */
    void AddAgentMessage( const wxString& aMessage );

    /**
     * Clear the chat history
     */
    void ClearChat();

    /**
     * Set the tool instance for processing requests
     */
    void SetTool( SCH_OLLAMA_AGENT_TOOL* aTool );

private:
    void HandleToolCall( const wxString& aToolName, const wxString& aPayload ) override;
    void onSendButton( wxCommandEvent& aEvent );
    void sendMessage();
    void scrollToBottom();
    wxWindow* addMessageToChat( const wxString& aMessage, CHAT_BUBBLE_KIND aKind );
    void CancelCurrentRequest();
    void StartConnectionCheck();
    void OnRequestCancelled( wxCommandEvent& aEvent );
    void OnConnectionCheckResult( wxCommandEvent& aEvent );
    void OnResponseReceived( wxCommandEvent& aEvent );
    void OnRequestFailed( wxCommandEvent& aEvent );
    void OnResponsePartial( wxCommandEvent& aEvent );
    void processStreamChunk( const wxString& aChunk );
    wxString filterToolLines( const wxString& aChunk, bool aFromStreaming );
    void queueToolCall( const wxString& aToolName, const wxString& aPayload );
    void processNextToolCall();
    void appendThinkingText( const wxString& aText );
    void appendAgentResponse( const wxString& aText );
    void finalizeThinkingBubble();
    void clearReasoningBubble();
    void flushStreamBubble();
    wxString sanitizeFinalResponse( const wxString& aResponse );
    void OnStreamUpdateTimer( wxTimerEvent& aEvent );

    SCH_OLLAMA_AGENT_TOOL* m_tool;
    wxScrolledWindow* m_chatPanel;
    wxBoxSizer* m_chatSizer;
    wxStyledTextCtrl* m_inputCtrl;
    SCINTILLA_TRICKS* m_scintillaTricks;
    wxButton* m_sendButton;
    wxButton* m_clearButton;
    wxButton* m_cancelButton;
    wxStaticText* m_statusText;
    bool m_isProcessing;
    std::thread m_requestThread;
    std::thread m_connectionThread;
    std::mutex m_threadMutex;
    std::mutex m_connectionMutex;
    wxWindow* m_streamingBubble;     // Reference to the streaming response bubble
    wxString m_streamingText;        // Accumulated streaming text
    std::atomic<bool> m_cancelRequested;
    wxWindow* m_reasoningBubble;
    bool m_inThinkSection;
    bool m_hasReasoningContent;
    wxString m_reasoningText;
    wxString m_responseAccumulator;
    wxTimer m_streamUpdateTimer;
    bool m_streamBubbleDirty;

    struct TOOL_CALL_REQUEST
    {
        wxString toolName;
        wxString payload;
        TOOL_CALL_BUBBLE* bubble;
    };

    std::deque<TOOL_CALL_REQUEST> m_toolCallQueue;
    bool m_toolCallActive;
    
    // Event IDs for async communication
    enum
    {
        ID_RESPONSE_RECEIVED = wxID_HIGHEST + 1,
        ID_REQUEST_FAILED,
        ID_RESPONSE_PARTIAL,
        ID_REQUEST_CANCELLED,
        ID_CONNECTION_CHECK_RESULT
    };
};

#endif // SCH_OLLAMA_AGENT_PANE_H
