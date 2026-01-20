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

#ifndef SCH_TOOL_HTTP_SERVER_H
#define SCH_TOOL_HTTP_SERVER_H

#include <wx/socket.h>
#include <wx/thread.h>
#include <memory>
#include <string>

class SCH_OLLAMA_AGENT_TOOL;
class SCH_EDIT_FRAME;

/**
 * HTTP server that exposes KiCad schematic tools via REST API.
 * 
 * Listens on a configurable port (default 54321) and handles POST requests
 * to /tool endpoint. Forwards tool calls to SCH_OLLAMA_AGENT_TOOL.
 */
class SCH_TOOL_HTTP_SERVER : public wxThread
{
public:
    /**
     * Constructor
     * @param aTool The SCH_OLLAMA_AGENT_TOOL instance to forward tool calls to
     * @param aPort Port to listen on (default: 54321)
     */
    SCH_TOOL_HTTP_SERVER( SCH_OLLAMA_AGENT_TOOL* aTool, int aPort = 54321 );

    /**
     * Destructor - stops the server
     */
    ~SCH_TOOL_HTTP_SERVER();

    /**
     * Start the HTTP server
     * @return true if server started successfully
     */
    bool StartServer();

    /**
     * Stop the HTTP server
     */
    void StopServer();

    /**
     * Clear the tool pointer (called before destruction to prevent access to destroyed object)
     */
    void ClearTool();

    /**
     * Check if server is running
     */
    bool IsRunning() const { return m_running; }

    /**
     * Get the port the server is listening on
     */
    /**
     * Get the port the server is listening on
     */
    int GetPort() const { return m_port; }

    /**
     * Handle a client connection (public for worker threads)
     */
    void HandleClient( wxSocketBase* aSocket );

protected:
    /**
     * Thread entry point - runs the server loop
     */
    void* Entry() override;

private:
    /**
     * Parse HTTP request
     */
    bool ParseRequest( const wxString& aRequest, wxString& aMethod, wxString& aPath, wxString& aBody );

    /**
     * Send HTTP response
     */
    void SendResponse( wxSocketBase* aSocket, int aStatusCode, const wxString& aContentType, const wxString& aBody );

    /**
     * Handle tool execution request
     */
    wxString HandleToolRequest( const wxString& aToolName, const wxString& aArgsJson );

    /**
     * Create JSON error response
     */
    wxString CreateErrorResponse( const wxString& aError, const wxString& aCode = "ERROR" );

    /**
     * Create JSON success response
     */
    wxString CreateSuccessResponse( const wxString& aResult );

    SCH_OLLAMA_AGENT_TOOL* m_tool;
    int                    m_port;
    bool                   m_running;
    wxSocketServer*        m_server;
    wxMutex                m_mutex;
};

#endif // SCH_TOOL_HTTP_SERVER_H
