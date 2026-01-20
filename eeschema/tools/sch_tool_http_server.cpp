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

#include "sch_tool_http_server.h"
#include "sch_ollama_agent_tool.h"
#include <wx/socket.h>
#include <wx/log.h>
#include <wx/string.h>
#include <wx/app.h>
#include <wx/thread.h>
#include <wx/event.h>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

SCH_TOOL_HTTP_SERVER::SCH_TOOL_HTTP_SERVER( SCH_OLLAMA_AGENT_TOOL* aTool, int aPort ) :
        wxThread( wxTHREAD_JOINABLE ),
        m_tool( aTool ),
        m_port( aPort ),
        m_running( false ),
        m_server( nullptr )
{
    wxLog::SetLogLevel( wxLOG_Max );
}


SCH_TOOL_HTTP_SERVER::~SCH_TOOL_HTTP_SERVER()
{
    // Ensure server is stopped and thread is joined before destruction
    StopServer();

    // Tool pointer is already cleared in StopServer(), no need to access mutex here
    // as the thread is guaranteed to be stopped
}


bool SCH_TOOL_HTTP_SERVER::StartServer()
{
    {
        wxMutexLocker lock( m_mutex );
        if( m_running )
        {
            return true;
        }
    }

    // Initialize wxSocket
    wxSocketBase::Initialize();

    // Create server socket
    wxIPV4address addr;
    addr.Service( m_port );
    addr.Hostname( wxT( "127.0.0.1" ) );

    m_server = new wxSocketServer( addr, wxSOCKET_REUSEADDR );

    if( !m_server->IsOk() )
    {
        wxLogError( wxT( "[ToolServer] Failed to create server socket on port %d" ), m_port );
        delete m_server;
        m_server = nullptr;
        return false;
    }

    // Set server to non-blocking mode with timeout
    m_server->SetFlags( wxSOCKET_NOWAIT );
    m_server->SetTimeout( 1 ); // 1 second timeout for Accept()

    {
        wxMutexLocker lock( m_mutex );
        m_running = true;
    }

    // Start the server thread
    if( Create() != wxTHREAD_NO_ERROR )
    {
        wxLogError( wxT( "[ToolServer] Failed to create server thread" ) );
        {
            wxMutexLocker lock( m_mutex );
            delete m_server;
            m_server = nullptr;
            m_running = false;
        }
        return false;
    }

    if( Run() != wxTHREAD_NO_ERROR )
    {
        wxLogError( wxT( "[ToolServer] Failed to start server thread" ) );
        {
            wxMutexLocker lock( m_mutex );
            delete m_server;
            m_server = nullptr;
            m_running = false;
        }
        return false;
    }

    wxLogMessage( wxT( "[ToolServer] HTTP server started on http://127.0.0.1:%d" ), m_port );
    return true;
}


void SCH_TOOL_HTTP_SERVER::StopServer()
{
    // Check if already stopped
    bool wasRunning = false;
    {
        wxMutexLocker lock( m_mutex );
        if( !m_running )
        {
            return;
        }
        wasRunning = true;
        m_running = false;

        // CRITICAL FIX for macOS CFSocket crash:
        // Disable socket notifications BEFORE closing/destroying the socket.
        // On macOS, CFSocket uses run loop callbacks that may still be pending
        // after the socket is freed. If a callback fires on a freed socket,
        // it causes CFRelease(NULL) crash in wxSocketImplMac::DoClose().
        if( m_server )
        {
            m_server->SetNotify( 0 );
            m_server->Notify( false );
            m_server->Close();
        }

        // Clear tool pointer to prevent access to potentially destroyed object
        m_tool = nullptr;
    }

    // Always wait for thread to finish if it was running (blocking)
    // Note: Wait() must be called outside the mutex to avoid deadlock
    // We check wasRunning instead of IsRunning() because IsRunning() might
    // return false even if the thread is still executing Entry()
    if( wasRunning )
    {
        // Wait for thread to exit - this is critical to prevent accessing
        // destroyed object members from the thread
        Wait();
    }

    // Now safe to delete the server socket (thread is guaranteed to be stopped)
    {
        wxMutexLocker lock( m_mutex );
        if( m_server )
        {
            // Use Destroy() instead of delete for proper cleanup
            m_server->Destroy();
            m_server = nullptr;
        }
    }

    wxLogMessage( wxT( "[ToolServer] HTTP server stopped" ) );
}

void SCH_TOOL_HTTP_SERVER::ClearTool()
{
    wxMutexLocker lock( m_mutex );
    m_tool = nullptr;
}


/**
 * Worker thread for handling HTTP clients
 */
class SCH_HTTP_CLIENT_HANDLER : public wxThread
{
public:
    SCH_HTTP_CLIENT_HANDLER( SCH_TOOL_HTTP_SERVER* aServer, wxSocketBase* aSocket ) :
            wxThread( wxTHREAD_DETACHED ),
            m_server( aServer ),
            m_socket( aSocket )
    {
    }

    ~SCH_HTTP_CLIENT_HANDLER()
    {
        // Socket is cleaned up via CallAfter to main thread - nothing to do here
    }

protected:
    void* Entry() override
    {
        if( m_server && m_socket )
        {
            m_server->HandleClient( m_socket );
        }

        // CRITICAL FIX for macOS CFSocket crash:
        // Socket cleanup MUST happen on the main thread because CFSocket callbacks
        // run on the main thread's run loop. Closing from a worker thread races
        // with pending callbacks, causing CFRelease(NULL) crashes.
        //
        // Solution: Queue cleanup to main thread using CallAfter.
        if( m_socket )
        {
            wxSocketBase* socketToDelete = m_socket;
            m_socket = nullptr; // Prevent any further access from this thread

            // Queue cleanup to main thread
            wxTheApp->CallAfter(
                    [socketToDelete]()
                    {
                        if( socketToDelete )
                        {
                            socketToDelete->SetNotify( 0 );
                            socketToDelete->Notify( false );
                            socketToDelete->Close();
                            delete socketToDelete;
                        }
                    } );
        }

        return nullptr;
    }

private:
    SCH_TOOL_HTTP_SERVER* m_server;
    wxSocketBase*         m_socket;
};


void* SCH_TOOL_HTTP_SERVER::Entry()
{
    while( true )
    {
        // Check if we should stop (check frequently to allow quick shutdown)
        // Use a local copy of m_running to avoid holding mutex for long
        bool            shouldRun = false;
        wxSocketServer* server = nullptr;
        {
            wxMutexLocker lock( m_mutex );
            shouldRun = m_running;
            if( shouldRun )
            {
                server = m_server;
                if( !server )
                    shouldRun = false;
                else if( !server->IsOk() )
                    shouldRun = false;
            }
        }

        if( !shouldRun || !server )
            break;

        // Check for new connections (non-blocking)
        // Note: server might be closed between the check above and here,
        // but Accept() will handle that gracefully
        std::unique_ptr<wxSocketBase> client( server->Accept( false ) );

        if( client && client->IsOk() )
        {
            // Handle client in a separate thread to support concurrent requests
            // Set socket to non-blocking to avoid hanging
            client->SetFlags( wxSOCKET_NOWAIT );
            // Note: HandleClient will likely set its own timeout/blocking mode

            // Create worker thread, passing ownership of the socket
            SCH_HTTP_CLIENT_HANDLER* handler = new SCH_HTTP_CLIENT_HANDLER( this, client.release() );

            if( handler->Create() != wxTHREAD_NO_ERROR )
            {
                wxLogError( wxT( "[ToolServer] Failed to create client handler thread" ) );
                delete handler; // Destructor cleans up socket
            }
            else if( handler->Run() != wxTHREAD_NO_ERROR )
            {
                wxLogError( wxT( "[ToolServer] Failed to run client handler thread" ) );
                delete handler;
            }
        }
        else
        {
            // Sleep a bit to avoid busy-waiting
            // Use shorter sleep to allow faster shutdown response
            wxThread::Sleep( 50 );
        }
    }

    return nullptr;
}


void SCH_TOOL_HTTP_SERVER::HandleClient( wxSocketBase* aSocket )
{
    if( !aSocket || !aSocket->IsOk() )
        return;

    // Check if we're shutting down before handling client
    {
        wxMutexLocker lock( m_mutex );
        if( !m_running )
            return;
    }

    // Note: Socket cleanup is handled by the unique_ptr<wxSocketBase> in Entry().
    // We do NOT explicitly call Close() here to avoid double-close which causes
    // CFRelease(NULL) crash on macOS when the socket's internal CFSocket is already freed.

    // Read request
    char     buffer[8192];
    size_t   totalRead = 0;
    wxString request;

    aSocket->SetTimeout( 5 ); // 5 second timeout
    // Use blocking mode but with timeout to prevent infinite hangs
    // Note: wxSOCKET_WAITALL can cause issues, so we'll read in chunks
    aSocket->SetFlags( wxSOCKET_BLOCK );

    // Read HTTP request headers first (up to \r\n\r\n)
    // Then read body if Content-Length is specified
    bool headersComplete = false;
    long contentLength = 0;
    int  headerEndPos = wxNOT_FOUND;

    // First, read until we get headers (ends with \r\n\r\n)
    int       readAttempts = 0;
    const int maxReadAttempts = 100; // Max ~5 seconds (100 * 50ms timeout)

    while( totalRead < sizeof( buffer ) - 1 && !headersComplete && readAttempts < maxReadAttempts )
    {
        // Check if we're shutting down
        {
            wxMutexLocker lock( m_mutex );
            if( !m_running )
                return; // Exit immediately if shutting down
        }

        size_t toRead = sizeof( buffer ) - totalRead - 1;
        if( toRead == 0 )
            break;

        aSocket->Read( buffer + totalRead, toRead );
        size_t bytesRead = aSocket->LastCount();

        if( bytesRead == 0 )
        {
            if( !aSocket->IsOk() || aSocket->Error() )
                break;
            // Timeout or no data - check if we have headers anyway
            if( totalRead > 0 )
            {
                wxString currentRequest = wxString::FromUTF8( buffer, totalRead );
                headerEndPos = currentRequest.Find( wxT( "\r\n\r\n" ) );
                if( headerEndPos != wxNOT_FOUND )
                {
                    headersComplete = true;
                    break;
                }
            }
            readAttempts++;
            if( readAttempts >= maxReadAttempts )
                break;
            continue;
        }

        readAttempts = 0; // Reset counter on successful read

        totalRead += bytesRead;
        buffer[totalRead] = '\0';

        // Check if we have complete headers
        wxString currentRequest = wxString::FromUTF8( buffer, totalRead );
        headerEndPos = currentRequest.Find( wxT( "\r\n\r\n" ) );

        if( headerEndPos != wxNOT_FOUND )
        {
            headersComplete = true;

            // Parse Content-Length header
            wxString headers = currentRequest.Left( headerEndPos );
            int      contentLengthPos = headers.Lower().Find( wxT( "content-length:" ) );
            if( contentLengthPos != wxNOT_FOUND )
            {
                size_t   colonPos = static_cast<size_t>( contentLengthPos + 15 ); // length of "content-length:"
                wxString lengthStr = headers.Mid( colonPos );
                lengthStr.Trim( false ).Trim( true ); // trim whitespace
                int crlfPos = lengthStr.Find( wxT( "\r\n" ) );
                if( crlfPos != wxNOT_FOUND )
                    lengthStr = lengthStr.Left( crlfPos );
                lengthStr.ToLong( &contentLength );
            }
            break;
        }
    }

    // If we have headers, read body if Content-Length is specified
    if( headersComplete && contentLength > 0 )
    {
        size_t    bodyStart = static_cast<size_t>( headerEndPos ) + 4; // Skip "\r\n\r\n"
        size_t    bodyRead = totalRead - bodyStart;
        int       bodyReadAttempts = 0;
        const int maxBodyReadAttempts = 50; // Max ~2.5 seconds for body

        // Read remaining body data
        while( bodyRead < (size_t) contentLength && totalRead < sizeof( buffer ) - 1
               && bodyReadAttempts < maxBodyReadAttempts )
        {
            // Check if we're shutting down
            {
                wxMutexLocker lock( m_mutex );
                if( !m_running )
                    return; // Exit immediately if shutting down
            }

            size_t toRead = (size_t) contentLength - bodyRead;
            size_t maxRead = sizeof( buffer ) - totalRead - 1;
            if( toRead > maxRead )
                toRead = maxRead;

            aSocket->Read( buffer + totalRead, toRead );
            size_t bytesRead = aSocket->LastCount();

            if( bytesRead == 0 )
            {
                if( !aSocket->IsOk() || aSocket->Error() )
                    break;
                // Timeout - increment counter
                bodyReadAttempts++;
                if( bodyReadAttempts >= maxBodyReadAttempts )
                    break;
                continue;
            }

            totalRead += bytesRead;
            buffer[totalRead] = '\0';
            bodyRead = totalRead - bodyStart;
            bodyReadAttempts = 0; // Reset on successful read
        }
    }

    if( totalRead == 0 )
    {
        SendResponse( aSocket, 400, wxT( "text/plain" ), wxT( "Bad Request - No data received" ) );
        return;
    }

    if( totalRead == 0 )
    {
        SendResponse( aSocket, 400, wxT( "text/plain" ), wxT( "Bad Request - No data received" ) );
        return;
    }

    if( !headersComplete )
    {
        // Headers incomplete - try to parse what we have anyway
        wxString partialRequest = wxString::FromUTF8( buffer, totalRead );
        if( partialRequest.Find( wxT( "\r\n\r\n" ) ) == wxNOT_FOUND
            && partialRequest.Find( wxT( "\n\n" ) ) == wxNOT_FOUND )
        {
            SendResponse( aSocket, 400, wxT( "text/plain" ), wxT( "Bad Request - Incomplete headers" ) );
            return;
        }
    }

    request = wxString::FromUTF8( buffer, totalRead );

    // Parse request
    wxString method, path, body;
    if( !ParseRequest( request, method, path, body ) )
    {
        SendResponse( aSocket, 400, wxT( "text/plain" ), wxT( "Bad Request" ) );
        return;
    }

    // Handle CORS preflight
    if( method == wxT( "OPTIONS" ) )
    {
        wxString corsHeaders =
                wxT( "Access-Control-Allow-Origin: *\r\n" ) wxT( "Access-Control-Allow-Methods: POST, OPTIONS\r\n" )
                        wxT( "Access-Control-Allow-Headers: Content-Type\r\n" );
        wxString response = wxString::Format( wxT( "HTTP/1.1 200 OK\r\n%s\r\n\r\n" ), corsHeaders );
        aSocket->Write( response.mb_str(), response.length() );
        return;
    }

    // Only handle POST requests to /tool
    if( method != wxT( "POST" ) || path != wxT( "/tool" ) )
    {
        SendResponse( aSocket, 404, wxT( "text/plain" ), wxT( "Not Found" ) );
        return;
    }

    // Parse JSON request body
    try
    {
        json requestJson = json::parse( body.ToStdString() );

        if( !requestJson.is_object() || !requestJson.contains( "tool" ) )
        {
            SendResponse( aSocket, 400, wxT( "application/json" ),
                          CreateErrorResponse( wxT( "Missing 'tool' field in request" ), wxT( "INVALID_REQUEST" ) ) );
            return;
        }

        wxString toolName = wxString::FromUTF8( requestJson["tool"].get<std::string>() );
        json     args = requestJson.value( "args", json::object() );
        wxString argsJson = wxString::FromUTF8( args.dump() );

        // Execute tool
        wxString result = HandleToolRequest( toolName, argsJson );

        // Send response
        SendResponse( aSocket, 200, wxT( "application/json" ), result );
    }
    catch( const json::exception& e )
    {
        wxString errorMsg = wxString::Format( wxT( "JSON parse error: %s" ), wxString::FromUTF8( e.what() ) );
        SendResponse( aSocket, 400, wxT( "application/json" ), CreateErrorResponse( errorMsg, wxT( "INVALID_JSON" ) ) );
    }
    catch( const std::exception& e )
    {
        wxString errorMsg = wxString::Format( wxT( "Error: %s" ), wxString::FromUTF8( e.what() ) );
        SendResponse( aSocket, 500, wxT( "application/json" ),
                      CreateErrorResponse( errorMsg, wxT( "INTERNAL_ERROR" ) ) );
    }
}


bool SCH_TOOL_HTTP_SERVER::ParseRequest( const wxString& aRequest, wxString& aMethod, wxString& aPath, wxString& aBody )
{
    // Parse HTTP request line: "METHOD /path HTTP/1.1"
    int firstLineEnd = aRequest.Find( wxT( "\r\n" ) );
    if( firstLineEnd == wxNOT_FOUND )
    {
        // Try with just \n as fallback
        firstLineEnd = aRequest.Find( wxT( "\n" ) );
        if( firstLineEnd == wxNOT_FOUND )
            return false;
    }

    wxString firstLine = aRequest.Left( firstLineEnd );
    firstLine.Trim( false ).Trim( true ); // Trim whitespace
    wxArrayString parts = wxSplit( firstLine, wxT( ' ' ), wxT( '\0' ) );

    if( parts.GetCount() < 2 )
        return false;

    aMethod = parts[0].Upper(); // Normalize to uppercase
    aPath = parts[1];

    // Find body (after double CRLF)
    int bodyStart = aRequest.Find( wxT( "\r\n\r\n" ) );
    if( bodyStart == wxNOT_FOUND )
    {
        // Try with \n\n as fallback
        bodyStart = aRequest.Find( wxT( "\n\n" ) );
        if( bodyStart != wxNOT_FOUND )
        {
            bodyStart += 2; // Skip "\n\n"
        }
    }
    else
    {
        bodyStart += 4; // Skip "\r\n\r\n"
    }

    if( bodyStart != wxNOT_FOUND && static_cast<size_t>( bodyStart ) < aRequest.length() )
    {
        aBody = aRequest.Mid( static_cast<size_t>( bodyStart ) );
        aBody.Trim( false ).Trim( true ); // Trim whitespace
    }
    else
    {
        aBody = wxT( "" );
    }

    return true;
}


void SCH_TOOL_HTTP_SERVER::SendResponse( wxSocketBase* aSocket, int aStatusCode, const wxString& aContentType,
                                         const wxString& aBody )
{
    if( !aSocket || !aSocket->IsOk() )
        return;

    wxString statusText;
    switch( aStatusCode )
    {
    case 200: statusText = wxT( "OK" ); break;
    case 400: statusText = wxT( "Bad Request" ); break;
    case 404: statusText = wxT( "Not Found" ); break;
    case 500: statusText = wxT( "Internal Server Error" ); break;
    default: statusText = wxT( "Unknown" ); break;
    }

    wxString response = wxString::Format(
            wxT( "HTTP/1.1 %d %s\r\n" ) wxT( "Content-Type: %s\r\n" ) wxT( "Access-Control-Allow-Origin: *\r\n" )
                    wxT( "Content-Length: %zu\r\n" ) wxT( "Connection: close\r\n" ) wxT( "\r\n" ) wxT( "%s" ),
            aStatusCode, statusText, aContentType, aBody.length(), aBody );

    // Write response in non-blocking mode
    aSocket->SetFlags( wxSOCKET_NOWAIT );
    wxScopedCharBuffer buffer = response.ToUTF8();
    aSocket->Write( buffer.data(), buffer.length() );

    // Give it a moment to send, then close
    wxThread::Sleep( 10 );
}


wxString SCH_TOOL_HTTP_SERVER::HandleToolRequest( const wxString& aToolName, const wxString& aArgsJson )
{
    // Safely check if tool is still available
    SCH_OLLAMA_AGENT_TOOL* tool = nullptr;
    {
        wxMutexLocker lock( m_mutex );
        tool = m_tool;
    }

    if( !tool )
    {
        return CreateErrorResponse( wxT( "Tool instance not available" ), wxT( "TOOL_UNAVAILABLE" ) );
    }

    // Map tool names to match KiCad's internal naming
    wxString mappedToolName = aToolName;
    wxString mappedArgsJson = aArgsJson;

    // Handle search_symbols (plural) -> search_symbol (singular)
    if( mappedToolName == wxT( "search_symbols" ) )
    {
        mappedToolName = wxT( "schematic.search_symbol" );
    }
    // Handle other tools that need schematic. prefix
    else if( !mappedToolName.StartsWith( wxT( "schematic." ) )
             && ( mappedToolName == wxT( "get_symbol_info" ) || mappedToolName == wxT( "place_component" )
                  || mappedToolName == wxT( "remove_component" ) || mappedToolName == wxT( "set_property" )
                  || mappedToolName == wxT( "add_wire" ) || mappedToolName == wxT( "add_global_label" )
                  || mappedToolName == wxT( "get_netlist" ) || mappedToolName == wxT( "get_sheet_info" ) ) )
    {
        mappedToolName = wxT( "schematic." ) + mappedToolName;
    }

    // Map parameter names for tools that expect different parameter names
    // get_symbol_info and place_component expect "symbol" but requests send "symbol_id"
    if( mappedToolName == wxT( "schematic.get_symbol_info" ) || mappedToolName == wxT( "schematic.place_component" ) )
    {
        try
        {
            json args = json::parse( mappedArgsJson.ToStdString() );
            if( args.is_object() )
            {
                wxString symbolValue;
                bool     needsResolution = false;

                // Map symbol_id -> symbol
                if( args.contains( "symbol_id" ) && !args.contains( "symbol" ) )
                {
                    symbolValue = wxString::FromUTF8( args["symbol_id"].get<std::string>() );
                    args["symbol"] = args["symbol_id"];
                    args.erase( "symbol_id" );
                }
                // Also handle lib_id -> symbol for get_symbol_info
                else if( mappedToolName == wxT( "schematic.get_symbol_info" ) && args.contains( "lib_id" )
                         && !args.contains( "symbol" ) )
                {
                    symbolValue = wxString::FromUTF8( args["lib_id"].get<std::string>() );
                    args["symbol"] = args["lib_id"];
                    args.erase( "lib_id" );
                }
                else if( args.contains( "symbol" ) && args["symbol"].is_string() )
                {
                    symbolValue = wxString::FromUTF8( args["symbol"].get<std::string>() );
                }

                // Check if symbol needs library prefix resolution
                // If it doesn't contain ":", try to resolve it using search_symbol
                if( !symbolValue.IsEmpty() && !symbolValue.Contains( wxT( ":" ) ) )
                {
                    needsResolution = true;
                }

                // For get_symbol_info, resolve symbol if needed (place_component does this internally)
                if( needsResolution && mappedToolName == wxT( "schematic.get_symbol_info" ) && tool )
                {
                    // Try to resolve using search_symbol
                    json searchQuery = json::object();
                    searchQuery["query"] = symbolValue.ToStdString();
                    searchQuery["limit"] = 5; // Get more results to find best match

                    wxString searchArgs = wxString::FromUTF8( searchQuery.dump() );
                    if( tool->RunToolCommand( wxT( "schematic.search_symbol" ), searchArgs ) )
                    {
                        wxString searchResult = tool->GetLastToolResult();
                        if( !searchResult.IsEmpty() )
                        {
                            try
                            {
                                json searchResultJson = json::parse( searchResult.ToStdString() );
                                if( searchResultJson.contains( "matches" ) && searchResultJson["matches"].is_array()
                                    && !searchResultJson["matches"].empty()
                                    && searchResultJson["matches"][0].is_object()
                                    && searchResultJson["matches"][0].contains( "lib_id" )
                                    && searchResultJson["matches"][0]["lib_id"].is_string() )
                                {
                                    std::string resolvedLibId =
                                            searchResultJson["matches"][0]["lib_id"].get<std::string>();
                                    args["symbol"] = resolvedLibId;
                                    wxLogMessage( wxT( "[ToolServer] Resolved '%s' to '%s'" ), symbolValue,
                                                  wxString::FromUTF8( resolvedLibId ) );
                                }
                                else
                                {
                                    // No matches found - return helpful error
                                    int      matchCount = searchResultJson.value( "count", 0 );
                                    wxString errorMsg = wxString::Format( wxT( "Symbol '%s' not found in libraries. " ),
                                                                          symbolValue );
                                    if( matchCount == 0 )
                                    {
                                        errorMsg += wxT( "No matching symbols found. " );
                                        errorMsg += wxT( "Make sure symbol libraries are loaded and try searching with "
                                                         "a different query." );
                                    }
                                    return CreateErrorResponse( errorMsg, wxT( "SYMBOL_NOT_FOUND" ) );
                                }
                            }
                            catch( ... )
                            {
                                // Search result parse failed, continue with original symbol
                            }
                        }
                        else
                        {
                            // Search returned empty - symbol not found
                            wxString errorMsg = wxString::Format(
                                    wxT( "Symbol '%s' not found. Use format 'libnick:symbol_name' (e.g., 'Device:R') "
                                         "or ensure symbol libraries are loaded." ),
                                    symbolValue );
                            return CreateErrorResponse( errorMsg, wxT( "SYMBOL_NOT_FOUND" ) );
                        }
                    }
                    else
                    {
                        // Search failed - return error with suggestion
                        wxString searchError = tool->GetLastToolError();
                        wxString errorMsg = wxString::Format(
                                wxT( "Failed to resolve symbol '%s': %s. Use format 'libnick:symbol_name' (e.g., "
                                     "'Device:R')." ),
                                symbolValue, searchError.IsEmpty() ? wxT( "Search failed" ) : searchError );
                        return CreateErrorResponse( errorMsg, wxT( "SYMBOL_RESOLUTION_FAILED" ) );
                    }
                }

                mappedArgsJson = wxString::FromUTF8( args.dump() );
            }
        }
        catch( ... )
        {
            // JSON parse failed, use original args
        }
    }

    // Check for unsupported tools
    if( mappedToolName == wxT( "schematic.get_netlist" ) || mappedToolName == wxT( "schematic.get_sheet_info" ) )
    {
        return CreateErrorResponse( wxString::Format( wxT( "Tool '%s' is not yet implemented" ), mappedToolName ),
                                    wxT( "TOOL_NOT_IMPLEMENTED" ) );
    }

    // Execute tool command on the main thread using CallAfter + semaphore.
    // This is REQUIRED because tool commands manipulate GUI elements (symbols, wires, etc.)
    // and calling them from a worker thread causes crashes.
    // We use wxWakeUpIdle() to make the main thread process events immediately.

    struct TOOL_CONTEXT
    {
        SCH_OLLAMA_AGENT_TOOL* tool;
        wxString               name;
        wxString               payload;
        bool                   success;
        wxString               result;
        wxString               error;
        wxSemaphore            semaphore;
    };

    auto context = std::make_shared<TOOL_CONTEXT>();
    context->tool = tool;
    context->name = mappedToolName;
    context->payload = mappedArgsJson;
    context->success = false;

    // Queue execution on main thread
    wxTheApp->CallAfter(
            [context]()
            {
                context->success = context->tool->RunToolCommand( context->name, context->payload );
                if( context->success )
                    context->result = context->tool->GetLastToolResult();
                else
                    context->error = context->tool->GetLastToolError();

                context->semaphore.Post();
            } );

    // Wake up the main thread to process the event immediately
    // Use multiple mechanisms to ensure the event is processed ASAP
    wxWakeUpIdle();

    // Also post an idle event directly to force event processing
    if( wxTheApp && wxTheApp->GetTopWindow() )
    {
        wxIdleEvent idleEvent;
        wxTheApp->GetTopWindow()->GetEventHandler()->AddPendingEvent( idleEvent );
    }

    // Wait for the main thread to complete (10s timeout - should be more than enough)
    if( context->semaphore.WaitTimeout( 10000 ) != wxSEMA_NO_ERROR )
    {
        wxLogWarning( wxS( "[ToolServer] Timeout waiting for main thread to execute tool '%s'" ), mappedToolName );
        return CreateErrorResponse( wxS( "Main thread timeout - is KiCad responsive?" ), wxS( "MAIN_THREAD_TIMEOUT" ) );
    }

    if( context->success )
    {
        if( context->result.IsEmpty() )
        {
            // Return empty JSON object if no result
            return CreateSuccessResponse( wxT( "{}" ) );
        }
        return CreateSuccessResponse( context->result );
    }
    else
    {
        wxString error = context->error;
        if( error.IsEmpty() )
        {
            error = wxString::Format( wxT( "Tool '%s' execution failed" ), mappedToolName );
        }
        return CreateErrorResponse( error, wxT( "TOOL_EXECUTION_FAILED" ) );
    }
}


wxString SCH_TOOL_HTTP_SERVER::CreateErrorResponse( const wxString& aError, const wxString& aCode )
{
    json response;
    response["ok"] = false;
    response["error"] = json::object();
    response["error"]["code"] = aCode.ToStdString();
    response["error"]["message"] = aError.ToStdString();

    return wxString::FromUTF8( response.dump() );
}


wxString SCH_TOOL_HTTP_SERVER::CreateSuccessResponse( const wxString& aResult )
{
    json response;
    response["ok"] = true;

    // Try to parse result as JSON, if it's already JSON, use it as result
    // Otherwise, wrap it in a string
    try
    {
        json resultJson = json::parse( aResult.ToStdString() );
        response["result"] = resultJson;
    }
    catch( ... )
    {
        // If not valid JSON, treat as string
        response["result"] = aResult.ToStdString();
    }

    return wxString::FromUTF8( response.dump() );
}
