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
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

class SCH_OLLAMA_AGENT_DIALOG;

class SCH_EDIT_FRAME;

class SCH_OLLAMA_TOOL_CALL_HANDLER
{
public:
    virtual ~SCH_OLLAMA_TOOL_CALL_HANDLER() = default;
    virtual void HandleToolCall( const wxString& aToolName, const wxString& aPayload ) = 0;
};

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
     * Build a complete context snapshot of the currently loaded schematic hierarchy.
     * This is intended to be appended to user requests before sending to the Python pcb_agent.
     *
     * @param aMaxChars Maximum size of the returned context string. If exceeded, the output is truncated.
     */
    wxString GetFullSchematicContext( size_t aMaxChars = 50000 );

    /**
     * Parse and execute response (for dialog access)
     */
    bool ParseAndExecute( const wxString& aResponse );


    /**
     * Register a handler that will be notified when TOOL lines are encountered.
     * When set, TOOL execution is delegated to the handler (for async UI display).
     */
    void SetToolCallHandler( SCH_OLLAMA_TOOL_CALL_HANDLER* aHandler ) { m_toolCallHandler = aHandler; }

    /**
     * Execute a tool command immediately (used by asynchronous handlers).
     */
    bool RunToolCommand( const wxString& aToolName, const wxString& aPayload );
    wxString GetLastToolError() const { return m_lastToolError; }
    wxString GetLastToolResult() const { return m_lastToolResult; }

private:
    /**
     * Finds a symbol by its reference (e.g. "U1"), or if not found, its value (e.g. "MCP2551")
     * or symbol name (e.g. "Device:R").
     * @return the symbol and the sheet path it was found on.
     */
    struct SYMBOL_MATCH
    {
        SCH_SYMBOL*     symbol = nullptr;
        SCH_SHEET_PATH  sheet;
    };

    SYMBOL_MATCH findSymbolByRefOrValue( const wxString& aIdentifier, bool aCurrentSheetOnly = false );

    bool ExecuteToolCommand( const wxString& aToolName, const wxString& aPayload );
    bool HandlePlaceComponentTool( const nlohmann::json& aPayload );
    bool HandleMoveComponentTool( const nlohmann::json& aPayload );
    bool HandleAddWireTool( const nlohmann::json& aPayload );
    bool HandleAddNetLabelTool( const nlohmann::json& aPayload );
    bool HandleConnectWithNetLabelTool( const nlohmann::json& aPayload );
    bool HandleGetDatasheetTool( const nlohmann::json& aPayload );
    bool HandleSearchSymbolTool( const nlohmann::json& aPayload );
    wxString GetCurrentSchematicContent();

    std::unique_ptr<SCH_AGENT> m_agent;
    std::unique_ptr<OLLAMA_CLIENT> m_ollama;
    wxString m_model;  // Default model name
    SCH_OLLAMA_TOOL_CALL_HANDLER* m_toolCallHandler = nullptr;
    wxString m_lastToolError;
    wxString m_lastToolResult;
};

#endif // SCH_OLLAMA_AGENT_TOOL_H
