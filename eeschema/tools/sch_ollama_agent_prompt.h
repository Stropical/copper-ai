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

#ifndef SCH_OLLAMA_AGENT_PROMPT_H
#define SCH_OLLAMA_AGENT_PROMPT_H

#include <wx/string.h>
#include <vector>

class SCH_OLLAMA_AGENT_TOOL;

/**
 * Structure for tool descriptors used in prompt generation
 */
struct SCH_OLLAMA_TOOL_DESCRIPTOR
{
    wxString name;
    wxString description;
    wxString usage;
};

/**
 * Generate the system prompt for the Ollama agent.
 * 
 * @param aToolCatalog List of available tools with their descriptions
 * @return The complete system prompt as a wxString
 */
wxString GenerateOllamaAgentSystemPrompt( const std::vector<SCH_OLLAMA_TOOL_DESCRIPTOR>& aToolCatalog );

#endif // SCH_OLLAMA_AGENT_PROMPT_H

