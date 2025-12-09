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
#include <kicad_curl/kicad_curl_easy.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <wx/log.h>

using json = nlohmann::json;


OLLAMA_CLIENT::OLLAMA_CLIENT( const wxString& aBaseUrl ) :
    m_baseUrl( aBaseUrl ),
    m_curl( std::make_unique<KICAD_CURL_EASY>() )
{
}


OLLAMA_CLIENT::~OLLAMA_CLIENT()
{
}


bool OLLAMA_CLIENT::ChatCompletion( const wxString& aModel, const wxString& aPrompt, wxString& aResponse )
{
    if( !m_curl )
        return false;

    // Build JSON request
    json request;
    request["model"] = aModel.ToStdString();
    request["prompt"] = aPrompt.ToStdString();
    request["stream"] = false;

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

