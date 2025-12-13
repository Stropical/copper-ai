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

#include "ollama_client.h"
#include <curl/curl.h>
#include <cstring>
#include <kicad_curl/kicad_curl_easy.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <wx/log.h>
#include <atomic>

using json = nlohmann::json;

namespace
{
    std::string TrimWhitespace( const std::string& aValue )
    {
        size_t start = aValue.find_first_not_of( " \t\r\n" );
        if( start == std::string::npos )
            return {};

        size_t end = aValue.find_last_not_of( " \t\r\n" );
        return aValue.substr( start, end - start + 1 );
    }

    struct StreamContext
    {
        OLLAMA_CLIENT::StreamCallback callback;
        std::string buffer;
        std::string eventPayload;
        std::string lastResponse;
        bool done = false;
    };

    void ProcessStreamEvent( StreamContext& aContext )
    {
        std::string payload = TrimWhitespace( aContext.eventPayload );
        aContext.eventPayload.clear();

        if( payload.empty() )
            return;

        if( payload == "[DONE]" )
            return;

        try
        {
            json chunk = json::parse( payload );
            auto sendChunk = [&]( const std::string& aText )
            {
                if( aText.empty() || !aContext.callback )
                    return;

                aContext.callback( wxString::FromUTF8( aText ) );
            };

            // Ollama /api/generate streaming mode returns one JSON object per line.
            // Each object typically has a "response" field that contains the next
            // piece of text and a "done" flag when streaming is finished.
            // Some models also return "thinking" tokens that show the model's reasoning.
            
            // Check for done flag first - when Ollama is finished, it sends "done": true
            if( chunk.contains( "done" ) && chunk["done"].is_boolean() && chunk["done"].get<bool>() )
            {
                aContext.done = true;
                // Final chunk may still contain a response field with the last piece of text
                if( chunk.contains( "response" ) && chunk["response"].is_string() )
                {
                    std::string response = chunk["response"].get<std::string>();
                    if( !response.empty() )
                    {
                        aContext.lastResponse += response;
                        sendChunk( response );
                    }
                }
                return; // Stream is complete
            }
            
            // Check for thinking tokens first (some models like deepseek, qwen, etc. use this)
            // Thinking tokens show the model's reasoning process before the actual response
            if( chunk.contains( "thinking" ) && chunk["thinking"].is_string() )
            {
                std::string thinking = chunk["thinking"].get<std::string>();
                if( !thinking.empty() )
                {
                    // Stream thinking tokens - these show the model's reasoning process
                    // We include them in the response so users can see the model's thought process
                    aContext.lastResponse += thinking;
                    sendChunk( thinking );
                }
            }
            
            // Check for response tokens (the actual output)
            if( chunk.contains( "response" ) && chunk["response"].is_string() )
            {
                std::string response = chunk["response"].get<std::string>();
                // Treat the "response" value as a delta chunk and append it to the
                // accumulated text. Some implementations may emit empty chunks near
                // completion, so guard against that.
                if( !response.empty() )
                {
                    aContext.lastResponse += response;
                    sendChunk( response );
                }
            }
            else if( chunk.contains( "choices" ) && chunk["choices"].is_array() && !chunk["choices"].empty() )
            {
                const json& choice = chunk["choices"][0];
                if( choice.contains( "delta" ) && choice["delta"].contains( "content" ) &&
                    choice["delta"]["content"].is_string() )
                {
                    std::string delta = choice["delta"]["content"].get<std::string>();
                    aContext.lastResponse += delta;
                    sendChunk( delta );
                }
            }
            else if( chunk.contains( "token" ) && chunk["token"].is_string() )
            {
                std::string token = chunk["token"].get<std::string>();
                aContext.lastResponse += token;
                sendChunk( token );
            }
        }
        catch( const json::exception& )
        {
            // Ignore malformed SSE chunks
        }
    }

    size_t StreamWriteCallback( void* aContents, size_t aSize, size_t aNmemb, void* aUserp )
    {
        size_t realsize = aSize * aNmemb;
        StreamContext* context = static_cast<StreamContext*>( aUserp );

        if( !context )
            return realsize;

        context->buffer.append( static_cast<const char*>( aContents ), realsize );

        while( true )
        {
            size_t newline = context->buffer.find( '\n' );
            if( newline == std::string::npos )
                break;

            std::string line = context->buffer.substr( 0, newline );
            context->buffer.erase( 0, newline + 1 );

            if( !line.empty() && line.back() == '\r' )
                line.pop_back();

            std::string trimmed = TrimWhitespace( line );

            if( trimmed.empty() )
                continue;

            // For Ollama /api/generate, each non-empty line is a complete JSON object.
            // Store it as the current payload and process immediately.
            context->eventPayload = trimmed;
            ProcessStreamEvent( *context );
            
            // If stream is done, we can stop processing more data
            if( context->done )
            {
                break;
            }
        }

        return realsize;
    }
}

OLLAMA_CLIENT::OLLAMA_CLIENT( const wxString& aBaseUrl ) :
    m_baseUrl( aBaseUrl ),
    m_curl( std::make_unique<KICAD_CURL_EASY>() )
{
}


OLLAMA_CLIENT::~OLLAMA_CLIENT()
{
}


bool OLLAMA_CLIENT::ChatCompletion( const wxString& aModel, const wxString& aPrompt, wxString& aResponse,
                                    const wxString& aSystemPrompt )
{
    if( !m_curl )
        return false;

    // Build JSON request
    json request;
    request["model"] = aModel.ToStdString();
    request["prompt"] = aPrompt.ToStdString();
    request["stream"] = false;
    if( !aSystemPrompt.IsEmpty() )
        request["system"] = aSystemPrompt.ToStdString();

    std::string requestBody = request.dump();

    // Set up curl
    wxString url = m_baseUrl + wxS( "/api/generate" );
    m_curl->SetURL( url.ToUTF8().data() );
    m_curl->SetHeader( "Content-Type", "application/json" );
    m_curl->SetPostFields( requestBody );

    // Perform request
    int result = m_curl->Perform();

    if( result != 0 )
    {
        wxLogError( wxS( "Python agent request failed with code: %d" ), result );
        return false;
    }

    // Parse response
    std::string responseBody = m_curl->GetBuffer();
    
    try
    {
        json response = json::parse( responseBody );
        
        if( response.contains( "response" ) )
        {
            aResponse = wxString::FromUTF8( response["response"].get<std::string>() );
            return true;
        }
        else if( response.contains( "error" ) )
        {
            wxString error = wxString::FromUTF8( response["error"].get<std::string>() );
            wxLogError( wxS( "Python agent error: %s" ), error );
            return false;
    }
    }
    catch( const json::exception& e )
    {
        wxLogError( wxS( "Failed to parse Python agent response: %s" ), wxString::FromUTF8( e.what() ) );
        return false;
    }

    return false;
}


bool OLLAMA_CLIENT::StreamChatCompletion( const wxString& aModel, const wxString& aPrompt,
                                         StreamCallback aChunkCallback, wxString& aResponse,
                                         const std::atomic<bool>* aCancelFlag,
                                         const wxString& aSystemPrompt )
{
    KICAD_CURL_EASY streamCurl;

    json request;
    request["model"] = aModel.ToStdString();
    request["prompt"] = aPrompt.ToStdString();
    request["stream"] = true;
    if( !aSystemPrompt.IsEmpty() )
        request["system"] = aSystemPrompt.ToStdString();

    std::string requestBody = request.dump();

    wxString url = m_baseUrl + wxS( "/api/generate" );
    streamCurl.SetURL( url.ToUTF8().data() );
    streamCurl.SetHeader( "Content-Type", "application/json" );
    // Ollama streams newline-delimited JSON rather than classic SSE.
    streamCurl.SetHeader( "Accept", "application/json" );
    streamCurl.SetPostFields( requestBody );

    if( aCancelFlag )
    {
        streamCurl.SetTransferCallback( [aCancelFlag]( size_t, size_t, size_t, size_t ) -> int
        {
            return aCancelFlag->load( std::memory_order_relaxed ) ? 1 : 0;
        }, 100 );
    }

    StreamContext context;
    context.callback = std::move( aChunkCallback );

    curl_easy_setopt( streamCurl.GetCurl(), CURLOPT_WRITEFUNCTION, StreamWriteCallback );
    curl_easy_setopt( streamCurl.GetCurl(), CURLOPT_WRITEDATA, static_cast<void*>( &context ) );

    int result = streamCurl.Perform();

    if( result != 0 )
    {
        wxLogError( wxS( "Python agent streaming request failed with code: %d" ), result );
        return false;
    }

    // Process any remaining data in the buffer
    if( !context.eventPayload.empty() )
    {
        ProcessStreamEvent( context );
    }
    
    // Also process any remaining buffer data
    if( !context.buffer.empty() )
    {
        std::string trimmed = TrimWhitespace( context.buffer );
        if( !trimmed.empty() )
        {
            context.eventPayload = trimmed;
            ProcessStreamEvent( context );
        }
    }

    aResponse = wxString::FromUTF8( context.lastResponse );
    
    if( context.done )
    {
        wxLogMessage( wxS( "Stream completed successfully. Total response length: %zu characters" ),
                     context.lastResponse.length() );
    }
    else
    {
        wxLogWarning( wxS( "Stream ended without 'done' flag. Response length: %zu characters" ),
                     context.lastResponse.length() );
    }
    
    return true;
}


bool OLLAMA_CLIENT::IsAvailable()
{
    if( !m_curl )
    {
        wxLogError( wxS( "OLLAMA_CLIENT: curl instance not available" ) );
        return false;
    }

    // Try a simple request to check if Python agent is up
    // The agent will proxy to Ollama, so this checks both agent and Ollama availability
    wxString url = m_baseUrl + wxS( "/api/tags" );
    m_curl->SetURL( url.ToUTF8().data() );
    
    // Set a reasonable timeout for availability check
    curl_easy_setopt( m_curl->GetCurl(), CURLOPT_TIMEOUT, 5L );
    curl_easy_setopt( m_curl->GetCurl(), CURLOPT_CONNECTTIMEOUT, 3L );
    
    int result = m_curl->Perform();
    
    if( result != 0 )
    {
        std::string errorText = m_curl->GetErrorText( result );
        wxLogError( wxS( "OLLAMA_CLIENT: Availability check failed for %s: %s (code: %d)" ),
                   url, wxString::FromUTF8( errorText ), result );
        return false;
    }
    
    int httpCode = m_curl->GetResponseStatusCode();
    if( httpCode != 200 )
    {
        wxLogError( wxS( "OLLAMA_CLIENT: Availability check returned HTTP %d for %s" ),
                   httpCode, url );
        return false;
    }
    
    wxLogDebug( wxS( "OLLAMA_CLIENT: Successfully connected to Python agent at %s" ), url );
    
    return true;
}
