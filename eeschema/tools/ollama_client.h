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

#ifndef OLLAMA_CLIENT_H
#define OLLAMA_CLIENT_H

#include <functional>
#include <wx/string.h>
#include <memory>
#include <string>
#include <atomic>

class KICAD_CURL_EASY;

/**
 * Client for communicating with the Python agent that wraps Ollama.
 * Uses KICAD_CURL_EASY for HTTP requests.
 * The Python agent then communicates with Ollama.
 */
class OLLAMA_CLIENT
{
public:
    OLLAMA_CLIENT( const wxString& aBaseUrl = wxS( "http://127.0.0.1:5001" ) );
    ~OLLAMA_CLIENT();

    /**
     * Send a chat completion request to Ollama
     * @param aModel Model name (e.g., "llama2", "mistral")
     * @param aPrompt User prompt
     * @param aResponse Output response text
     * @return true if successful
     */
    bool ChatCompletion( const wxString& aModel, const wxString& aPrompt, wxString& aResponse,
                         const wxString& aSystemPrompt = wxString() );

    using StreamCallback = std::function<void( const wxString& )>;
    bool StreamChatCompletion( const wxString& aModel, const wxString& aPrompt,
                               StreamCallback aChunkCallback, wxString& aResponse,
                               const std::atomic<bool>* aCancelFlag = nullptr,
                               const wxString& aSystemPrompt = wxString() );

    /**
     * Check if Ollama server is available
     * @return true if server is reachable
     */
    bool IsAvailable();

    /**
     * Set the base URL for Ollama API
     */
    void SetBaseUrl( const wxString& aBaseUrl ) { m_baseUrl = aBaseUrl; }

    /**
     * Get the base URL
     */
    wxString GetBaseUrl() const { return m_baseUrl; }

private:
    wxString m_baseUrl;
    std::unique_ptr<KICAD_CURL_EASY> m_curl;
};

#endif // OLLAMA_CLIENT_H
