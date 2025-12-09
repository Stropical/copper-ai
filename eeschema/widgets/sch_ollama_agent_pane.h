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

class wxTextCtrl;
class wxButton;
class wxScrolledWindow;
class wxBoxSizer;
class SCH_EDIT_FRAME;
class SCH_OLLAMA_AGENT_TOOL;

/**
 * Chat-style panel for interacting with Ollama agent.
 * Similar to Cursor's chat interface with message history.
 * This panel is dockable in the schematic editor alongside hierarchy and properties panels.
 */
class SCH_OLLAMA_AGENT_PANE : public WX_PANEL
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
    void SetTool( SCH_OLLAMA_AGENT_TOOL* aTool ) { m_tool = aTool; }

private:
    void onSendButton( wxCommandEvent& aEvent );
    void onInputKeyDown( wxKeyEvent& aEvent );
    void sendMessage();
    void scrollToBottom();
    void addMessageToChat( const wxString& aMessage, bool aIsUser );

    SCH_EDIT_FRAME* m_frame;
    SCH_OLLAMA_AGENT_TOOL* m_tool;
    wxScrolledWindow* m_chatPanel;
    wxBoxSizer* m_chatSizer;
    wxTextCtrl* m_inputCtrl;
    wxButton* m_sendButton;
    wxButton* m_clearButton;
    bool m_isProcessing;
};

#endif // SCH_OLLAMA_AGENT_PANE_H

