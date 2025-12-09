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

#ifndef SCH_OLLAMA_AGENT_TOOL_H
#define SCH_OLLAMA_AGENT_TOOL_H

#include "sch_tool_base.h"
#include "sch_agent.h"
#include "ollama_client.h"

class SCH_OLLAMA_AGENT_DIALOG;

class SCH_EDIT_FRAME;

/**
 * Tool that integrates Ollama AI with schematic manipulation.
 * Uses the simple schematic agent for direct manipulation.
 */
class SCH_OLLAMA_AGENT_TOOL : public SCH_TOOL_BASE<SCH_EDIT_FRAME>
{
public:
    SCH_OLLAMA_AGENT_TOOL();
    ~SCH_OLLAMA_AGENT_TOOL() override {}

    /// @copydoc TOOL_INTERACTIVE::Init()
    bool Init() override;

    void Reset( RESET_REASON aReason ) override {}

    /**
     * Process a natural language request and execute schematic operations
     */
    int ProcessRequest( const TOOL_EVENT& aEvent );

    /**
     * Show dialog to interact with Ollama agent
     */
    int ShowAgentDialog( const TOOL_EVENT& aEvent );

    /**
     * Set up event handlers
     */
    void setTransitions() override;

    /**
     * Get Ollama client (for dialog access)
     * Creates the client lazily if it doesn't exist
     */
    OLLAMA_CLIENT* GetOllama();

    /**
     * Get current model name
     */
    wxString GetModel() const { return m_model; }

    /**
     * Parse and execute response (for dialog access)
     */
    bool ParseAndExecute( const wxString& aResponse );

    /**
     * Build a prompt for the LLM based on user request
     */
    wxString BuildPrompt( const wxString& aUserRequest );

private:
    std::unique_ptr<SCH_AGENT> m_agent;
    std::unique_ptr<OLLAMA_CLIENT> m_ollama;
    wxString m_model;  // Default model name
};

#endif // SCH_OLLAMA_AGENT_TOOL_H

