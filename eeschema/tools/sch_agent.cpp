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

#include "sch_agent.h"
#include <sch_junction.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_text.h>
#include <stroke_params.h>


SCH_AGENT::SCH_AGENT( SCH_EDIT_FRAME* aFrame ) :
    m_frame( aFrame ),
    m_commit( std::make_unique<SCH_COMMIT>( aFrame ) ),
    m_inBatch( false )
{
}


bool SCH_AGENT::AddJunction( const VECTOR2I& aPos )
{
    SCH_SCREEN* screen = m_frame->GetScreen();
    if( !screen )
        return false;

    std::unique_ptr<SCH_JUNCTION> junction = std::make_unique<SCH_JUNCTION>( aPos );
    junction->SetParent( screen ); // Must set parent before adding to commit
    m_commit->Add( junction.release(), screen );

    if( !m_inBatch )
    {
        m_commit->Push( _( "Added junction" ) );
        m_commit = std::make_unique<SCH_COMMIT>( m_frame );
    }

    return true;
}


bool SCH_AGENT::AddWire( const VECTOR2I& aStart, const VECTOR2I& aEnd )
{
    SCH_SCREEN* screen = m_frame->GetScreen();
    if( !screen )
        return false;

    std::unique_ptr<SCH_LINE> wire = std::make_unique<SCH_LINE>();
    wire->SetStartPoint( aStart );
    wire->SetEndPoint( aEnd );
    wire->SetLayer( LAYER_WIRE );
    wire->SetStroke( STROKE_PARAMS() );
    wire->SetParent( screen ); // Must set parent before adding to commit

    m_commit->Add( wire.release(), screen );

    if( !m_inBatch )
    {
        m_commit->Push( _( "Added wire" ) );
        m_commit = std::make_unique<SCH_COMMIT>( m_frame );
    }

    return true;
}


bool SCH_AGENT::AddLabel( const VECTOR2I& aPos, const wxString& aText )
{
    SCH_SCREEN* screen = m_frame->GetScreen();
    if( !screen )
        return false;

    std::unique_ptr<SCH_LABEL> label = std::make_unique<SCH_LABEL>();
    label->SetPosition( aPos );
    label->SetText( aText );
    label->SetParent( screen ); // Must set parent before adding to commit

    m_commit->Add( label.release(), screen );

    if( !m_inBatch )
    {
        m_commit->Push( _( "Added label" ) );
        m_commit = std::make_unique<SCH_COMMIT>( m_frame );
    }

    return true;
}


bool SCH_AGENT::AddText( const VECTOR2I& aPos, const wxString& aText )
{
    SCH_SCREEN* screen = m_frame->GetScreen();
    if( !screen )
        return false;

    std::unique_ptr<SCH_TEXT> text = std::make_unique<SCH_TEXT>();
    text->SetPosition( aPos );
    text->SetText( aText );
    text->SetParent( screen ); // Must set parent before adding to commit

    m_commit->Add( text.release(), screen );

    if( !m_inBatch )
    {
        m_commit->Push( _( "Added text" ) );
        m_commit = std::make_unique<SCH_COMMIT>( m_frame );
    }

    return true;
}


void SCH_AGENT::BeginBatch()
{
    m_inBatch = true;
    m_commit = std::make_unique<SCH_COMMIT>( m_frame );
}


void SCH_AGENT::EndBatch( const wxString& aMessage )
{
    if( m_inBatch && m_commit )
    {
        m_commit->Push( aMessage );
        m_commit = std::make_unique<SCH_COMMIT>( m_frame );
    }
    m_inBatch = false;
}

