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

#ifndef SCH_AGENT_H
#define SCH_AGENT_H

#include <sch_edit_frame.h>
#include <sch_commit.h>
#include <sch_screen.h>
#include <math/vector2d.h>
#include <wx/string.h>

class SCH_JUNCTION;
class SCH_LINE;
class SCH_LABEL;
class SCH_TEXT;

/**
 * Simple schematic agent for direct schematic manipulation.
 * Uses option #2 approach - direct manipulation without API layer.
 */
class SCH_AGENT
{
public:
    SCH_AGENT( SCH_EDIT_FRAME* aFrame );
    ~SCH_AGENT() {}

    /**
     * Add a junction at the specified position
     * @param aPos Position in internal units
     * @return true if successful
     */
    bool AddJunction( const VECTOR2I& aPos );

    /**
     * Add a wire segment between two points
     * @param aStart Start position
     * @param aEnd End position
     * @return true if successful
     */
    bool AddWire( const VECTOR2I& aStart, const VECTOR2I& aEnd );

    /**
     * Add a text label at the specified position
     * @param aPos Position
     * @param aText Label text
     * @return true if successful
     */
    bool AddLabel( const VECTOR2I& aPos, const wxString& aText );

    /**
     * Add a text element at the specified position
     * @param aPos Position
     * @param aText Text content
     * @return true if successful
     */
    bool AddText( const VECTOR2I& aPos, const wxString& aText );

    /**
     * Begin a batch operation (single commit for multiple operations)
     */
    void BeginBatch();

    /**
     * End batch operation and commit all changes
     * @param aMessage Commit message
     */
    void EndBatch( const wxString& aMessage = _( "Batch operation" ) );

    /**
     * Get the current commit (for advanced operations)
     */
    SCH_COMMIT* GetCommit() { return m_commit.get(); }

    /**
     * Get the current screen
     */
    SCH_SCREEN* GetScreen() { return m_frame->GetScreen(); }

private:
    SCH_EDIT_FRAME* m_frame;
    SCH_SCREEN* m_screen;
    std::unique_ptr<SCH_COMMIT> m_commit;
    bool m_inBatch;
};

#endif // SCH_AGENT_H

