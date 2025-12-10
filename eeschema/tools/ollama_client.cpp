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

            if( chunk.contains( "response" ) && chunk["response"].is_string() )
            {
                std::string response = chunk["response"].get<std::string>();
                if( response.size() > aContext.lastResponse.size() )
                {
                    std::string addition = response.substr( aContext.lastResponse.size() );
                    aContext.lastResponse = response;
                    sendChunk( addition );
                }
                else
                {
                    aContext.lastResponse = response;
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

            if( line.empty() )
            {
                ProcessStreamEvent( *context );
                continue;
            }

            constexpr const char* dataPrefix = "data:";
            if( line.rfind( dataPrefix, 0 ) == 0 )
            {
                std::string value = TrimWhitespace( line.substr( std::strlen( dataPrefix ) ) );
                if( !context->eventPayload.empty() )
                    context->eventPayload += "\n";
                context->eventPayload += value;
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
        wxLogError( wxS( "Ollama request failed with code: %d" ), result );
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
            wxLogError( wxS( "Ollama error: %s" ), error );
            return false;
        }
    }
    catch( const json::exception& e )
    {
        wxLogError( wxS( "Failed to parse Ollama response: %s" ), wxString::FromUTF8( e.what() ) );
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
    streamCurl.SetHeader( "Accept", "text/event-stream" );
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
        wxLogError( wxS( "Ollama streaming request failed with code: %d" ), result );
        return false;
    }

    if( !context.eventPayload.empty() )
    {
        ProcessStreamEvent( context );
    }

    aResponse = wxString::FromUTF8( context.lastResponse );
    return true;
}


bool OLLAMA_CLIENT::IsAvailable()
{
    if( !m_curl )
        return false;

    // Try a simple request to check if server is up
    wxString url = m_baseUrl + wxS( "/api/tags" );
    m_curl->SetURL( url.ToUTF8().data() );
    
    int result = m_curl->Perform();
    return result == 0;
}
