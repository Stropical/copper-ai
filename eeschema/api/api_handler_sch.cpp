/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
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

#include <api/api_handler_sch.h>
#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <api/api_enums.h>
#include <algorithm>
#include <cmath>
#include <magic_enum.hpp>
#include <unordered_map>
#include <vector>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <sch_field.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <wx/filename.h>

#include <api/common/types/base_types.pb.h>
#include <api/schematic/schematic_types.pb.h>

using namespace kiapi::common::commands;
using kiapi::common::types::CommandStatus;
using kiapi::common::types::DocumentType;
using kiapi::common::types::ItemRequestStatus;

namespace
{
double normalizeRotation( double aRotation )
{
    double rotation = std::fmod( aRotation, 360.0 );

    if( rotation < 0.0 )
        rotation += 360.0;

    return std::round( rotation / 90.0 ) * 90.0;
}

double rotationFromOrientation( int aOrientation )
{
    int orient = aOrientation & ~( SYMBOL_ORIENTATION_T::SYM_MIRROR_X
                                   | SYMBOL_ORIENTATION_T::SYM_MIRROR_Y );

    switch( orient )
    {
    case SYMBOL_ORIENTATION_T::SYM_ORIENT_90:  return 90.0;
    case SYMBOL_ORIENTATION_T::SYM_ORIENT_180: return 180.0;
    case SYMBOL_ORIENTATION_T::SYM_ORIENT_270: return 270.0;
    case SYMBOL_ORIENTATION_T::SYM_ORIENT_0:
    default:                                   return 0.0;
    }
}

int orientationFromRequest( double aRotation, bool aMirrorX, bool aMirrorY )
{
    double rotation = normalizeRotation( aRotation );
    int    orientation = SYMBOL_ORIENTATION_T::SYM_ORIENT_0;

    if( rotation == 90.0 )
        orientation = SYMBOL_ORIENTATION_T::SYM_ORIENT_90;
    else if( rotation == 180.0 )
        orientation = SYMBOL_ORIENTATION_T::SYM_ORIENT_180;
    else if( rotation == 270.0 )
        orientation = SYMBOL_ORIENTATION_T::SYM_ORIENT_270;

    if( aMirrorX )
        orientation |= SYMBOL_ORIENTATION_T::SYM_MIRROR_X;

    if( aMirrorY )
        orientation |= SYMBOL_ORIENTATION_T::SYM_MIRROR_Y;

    return orientation;
}
} // namespace


API_HANDLER_SCH::API_HANDLER_SCH( SCH_EDIT_FRAME* aFrame ) :
        API_HANDLER_EDITOR( aFrame ),
        m_frame( aFrame )
{
    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_SCH::handleGetOpenDocuments );
    registerHandler<kiapi::schematic::commands::GetSchematicData,
                    kiapi::schematic::commands::GetSchematicDataResponse>(
            &API_HANDLER_SCH::handleGetSchematicData );
    registerHandler<kiapi::schematic::commands::PlaceSymbol,
                    kiapi::schematic::commands::PlaceSymbolResponse>(
            &API_HANDLER_SCH::handlePlaceSymbol );
    registerHandler<kiapi::schematic::commands::PlaceWire,
                    kiapi::schematic::commands::PlaceWireResponse>(
            &API_HANDLER_SCH::handlePlaceWire );
}


std::unique_ptr<COMMIT> API_HANDLER_SCH::createCommit()
{
    return std::make_unique<SCH_COMMIT>( m_frame );
}


bool API_HANDLER_SCH::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_SCHEMATIC )
        return false;

    // TODO(JE) need serdes for SCH_SHEET_PATH <> SheetPath
    return true;

    //wxString currentPath = m_frame->GetCurrentSheet().PathAsString();
    //return 0 == aDocument.sheet_path().compare( currentPath.ToStdString() );
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_SCH::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;

        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    common::types::DocumentSpecifier doc;

    wxFileName fn( m_frame->GetCurrentFileName() );

    doc.set_type( DocumentType::DOCTYPE_SCHEMATIC );
    doc.set_board_filename( fn.GetFullName() );

    response.mutable_documents()->Add( std::move( doc ) );
    return response;
}


HANDLER_RESULT<kiapi::schematic::commands::GetSchematicDataResponse>
API_HANDLER_SCH::handleGetSchematicData(
        const HANDLER_CONTEXT<kiapi::schematic::commands::GetSchematicData>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_SCREEN* screen = m_frame->GetScreen();

    if( !screen )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "No schematic document is currently active" );
        return tl::unexpected( e );
    }

    kiapi::schematic::commands::GetSchematicDataResponse response;
    SCH_SHEET_PATH& sheetPath = m_frame->Schematic().CurrentSheet();

    auto uuidSorter =
            []( const EDA_ITEM* aLeft, const EDA_ITEM* aRight )
            {
                return aLeft->m_Uuid.AsStdString() < aRight->m_Uuid.AsStdString();
            };

    std::vector<SCH_SYMBOL*> symbols;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        symbols.emplace_back( static_cast<SCH_SYMBOL*>( item ) );

    std::sort( symbols.begin(), symbols.end(), uuidSorter );

    for( SCH_SYMBOL* symbol : symbols )
    {
        response.mutable_symbols()->Add()->CopyFrom( buildSymbolMessage( *symbol, sheetPath ) );
    }

    auto serializeLines =
            [&]( SCH_LAYER_ID aLayer, google::protobuf::RepeatedPtrField<kiapi::schematic::types::Line>* aDest )
            {
                std::vector<SCH_LINE*> lines;

                for( SCH_ITEM* item : screen->Items().OfType( SCH_LINE_T ) )
                {
                    SCH_LINE* line = static_cast<SCH_LINE*>( item );

                    if( line->GetLayer() == aLayer )
                        lines.emplace_back( line );
                }

                std::sort( lines.begin(), lines.end(), uuidSorter );

                for( SCH_LINE* line : lines )
                {
                    google::protobuf::Any serialized;
                    line->Serialize( serialized );
                    kiapi::schematic::types::Line packed;

                    if( serialized.UnpackTo( &packed ) )
                        aDest->Add()->CopyFrom( packed );
                }
            };

    serializeLines( LAYER_WIRE, response.mutable_wires() );
    serializeLines( LAYER_BUS, response.mutable_buses() );

    std::vector<SCH_LABEL*> localLabels;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_LABEL_T ) )
        localLabels.emplace_back( static_cast<SCH_LABEL*>( item ) );

    std::sort( localLabels.begin(), localLabels.end(), uuidSorter );

    for( SCH_LABEL* label : localLabels )
    {
        google::protobuf::Any serialized;
        label->Serialize( serialized );
        kiapi::schematic::types::LocalLabel packed;

        if( serialized.UnpackTo( &packed ) )
            response.mutable_local_labels()->Add()->CopyFrom( packed );
    }

    std::vector<SCH_GLOBALLABEL*> globalLabels;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_GLOBAL_LABEL_T ) )
        globalLabels.emplace_back( static_cast<SCH_GLOBALLABEL*>( item ) );

    std::sort( globalLabels.begin(), globalLabels.end(), uuidSorter );

    for( SCH_GLOBALLABEL* label : globalLabels )
    {
        google::protobuf::Any serialized;
        label->Serialize( serialized );
        kiapi::schematic::types::GlobalLabel packed;

        if( serialized.UnpackTo( &packed ) )
            response.mutable_global_labels()->Add()->CopyFrom( packed );
    }

    std::vector<SCH_HIERLABEL*> hierarchicalLabels;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_HIER_LABEL_T ) )
        hierarchicalLabels.emplace_back( static_cast<SCH_HIERLABEL*>( item ) );

    std::sort( hierarchicalLabels.begin(), hierarchicalLabels.end(), uuidSorter );

    for( SCH_HIERLABEL* label : hierarchicalLabels )
    {
        google::protobuf::Any serialized;
        label->Serialize( serialized );
        kiapi::schematic::types::HierarchicalLabel packed;

        if( serialized.UnpackTo( &packed ) )
            response.mutable_hierarchical_labels()->Add()->CopyFrom( packed );
    }

    return response;
}


HANDLER_RESULT<kiapi::schematic::commands::PlaceSymbolResponse> API_HANDLER_SCH::handlePlaceSymbol(
        const HANDLER_CONTEXT<kiapi::schematic::commands::PlaceSymbol>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    LIB_ID libId = kiapi::common::LibIdFromProto( aCtx.Request.lib_id() );

    if( !libId.IsValid() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "A valid library identifier must be provided" );
        return tl::unexpected( e );
    }

    SCH_SCREEN* screen = m_frame->GetScreen();

    if( !screen )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "No schematic document is currently active" );
        return tl::unexpected( e );
    }

    SCH_SHEET_PATH& sheetPath = m_frame->Schematic().CurrentSheet();
    LIB_SYMBOL* libSymbol = m_frame->GetLibSymbol( libId );

    if( !libSymbol )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "The requested symbol could not be found in the current libraries" );
        return tl::unexpected( e );
    }

    VECTOR2I position = kiapi::common::UnpackVector2( aCtx.Request.position() );
    int unit = aCtx.Request.unit() > 0 ? static_cast<int>( aCtx.Request.unit() ) : 1;

    SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSymbol, libId, &sheetPath, unit, 0, position,
                                         &m_frame->Schematic() );
    symbol->SetPosition( position );

    int orientation = orientationFromRequest( aCtx.Request.rotation(), aCtx.Request.mirror_x(),
                                              aCtx.Request.mirror_y() );
    symbol->SetOrientation( orientation );

    wxString reference = wxString::FromUTF8( aCtx.Request.reference() );

    if( !reference.IsEmpty() )
        symbol->SetRef( &sheetPath, reference );

    wxString value = wxString::FromUTF8( aCtx.Request.value() );

    if( !value.IsEmpty() )
        symbol->GetField( FIELD_T::VALUE )->SetText( value );

    m_frame->AddToScreen( symbol, screen );

    if( m_frame->eeconfig() && m_frame->eeconfig()->m_AutoplaceFields.enable )
        symbol->AutoplaceFields( screen, AUTOPLACE_AUTO );

    COMMIT* commit = getCurrentCommit( aCtx.ClientName );
    commit->Add( symbol, screen );

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Placed schematic symbol via API" ) );

    kiapi::schematic::commands::PlaceSymbolResponse response;
    response.mutable_symbol()->CopyFrom( buildSymbolMessage( *symbol, sheetPath ) );
    return response;
}


HANDLER_RESULT<kiapi::schematic::commands::PlaceWireResponse> API_HANDLER_SCH::handlePlaceWire(
        const HANDLER_CONTEXT<kiapi::schematic::commands::PlaceWire>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.points_size() < 2 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "At least two points are required to draw a wire" );
        return tl::unexpected( e );
    }

    SCH_SCREEN* screen = m_frame->GetScreen();

    if( !screen )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "No schematic document is currently active" );
        return tl::unexpected( e );
    }

    SCH_LAYER_ID layer = LAYER_WIRE;

    if( aCtx.Request.layer() != kiapi::schematic::types::SchematicLayer::SL_UNKNOWN )
        layer = FromProtoEnum<SCH_LAYER_ID, kiapi::schematic::types::SchematicLayer>( aCtx.Request.layer() );

    kiapi::schematic::commands::PlaceWireResponse response;
    COMMIT* commit = getCurrentCommit( aCtx.ClientName );

    VECTOR2I start = kiapi::common::UnpackVector2( aCtx.Request.points( 0 ) );

    for( int i = 1; i < aCtx.Request.points_size(); ++i )
    {
        VECTOR2I end = kiapi::common::UnpackVector2( aCtx.Request.points( i ) );

        if( end == start )
        {
            start = end;
            continue;
        }

        SCH_LINE* line = new SCH_LINE( start, layer );
        line->SetEndPoint( end );
        line->SetLayer( layer );

        m_frame->AddToScreen( line, screen );
        commit->Add( line, screen );

        google::protobuf::Any serialized;
        line->Serialize( serialized );
        kiapi::schematic::types::Line packed;

        if( serialized.UnpackTo( &packed ) )
            response.mutable_segments()->Add()->CopyFrom( packed );

        start = end;
    }

    if( response.segments_size() == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "None of the provided wire segments were valid" );
        return tl::unexpected( e );
    }

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Placed schematic wires via API" ) );

    return response;
}


HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> API_HANDLER_SCH::createItemForType( KICAD_T aType,
        EDA_ITEM* aContainer )
{
    if( !aContainer )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Tried to create an item in a null container" );
        return tl::unexpected( e );
    }

    if( aType == SCH_PIN_T && !dynamic_cast<SCH_SYMBOL*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a pin in {}, which is not a symbol",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == SCH_SYMBOL_T && !dynamic_cast<SCHEMATIC*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a symbol in {}, which is not a "
                                          "schematic",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<EDA_ITEM> created = CreateItemForType( aType, aContainer );

    if( !created )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create an item of type {}, which is unhandled",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    return created;
}


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_SCH::handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( ItemStatus, google::protobuf::Any )> aItemHandler )
{
    ApiResponseStatus e;

    auto containerResult = validateItemHeaderDocument( aHeader );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        e.CopyFrom( containerResult.error() );
        return tl::unexpected( e );
    }

    SCH_SCREEN* screen = m_frame->GetScreen();
    EE_RTREE& screenItems = screen->Items();

    std::map<KIID, EDA_ITEM*> itemUuidMap;

    std::for_each( screenItems.begin(), screenItems.end(),
                   [&]( EDA_ITEM* aItem )
                   {
                       itemUuidMap[aItem->m_Uuid] = aItem;
                   } );

    EDA_ITEM* container = nullptr;

    if( containerResult->has_value() )
    {
        const KIID& containerId = **containerResult;

        if( itemUuidMap.count( containerId ) )
        {
            container = itemUuidMap.at( containerId );

            if( !container )
            {
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format(
                        "The requested container {} is not a valid schematic item container",
                        containerId.AsStdString() ) );
                return tl::unexpected( e );
            }
        }
        else
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format(
                    "The requested container {} does not exist in this document",
                    containerId.AsStdString() ) );
            return tl::unexpected( e );
        }
    }

    COMMIT* commit = getCurrentCommit( aClientName );

    for( const google::protobuf::Any& anyItem : aItems )
    {
        ItemStatus status;
        std::optional<KICAD_T> type = TypeNameFromAny( anyItem );

        if( !type )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "Could not decode a valid type from {}",
                                                   anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> creationResult =
                createItemForType( *type, container );

        if( !creationResult )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( creationResult.error().error_message() );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<EDA_ITEM> item( std::move( *creationResult ) );

        if( !item->Deserialize( anyItem ) )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request",
                                              item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        if( aCreate && itemUuidMap.count( item->m_Uuid ) )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        else if( !aCreate && !itemUuidMap.count( item->m_Uuid ) )
        {
            status.set_code( ItemStatusCode::ISC_NONEXISTENT );
            status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            item->Serialize( newItem );
            commit->Add( item.release(), screen );

            if( !m_activeClients.count( aClientName ) )
                pushCurrentCommit( aClientName, _( "Added items via API" ) );
        }
        else
        {
            EDA_ITEM* edaItem = itemUuidMap[item->m_Uuid];

            if( SCH_ITEM* schItem = dynamic_cast<SCH_ITEM*>( edaItem ) )
            {
                schItem->SwapItemData( static_cast<SCH_ITEM*>( item.get() ) );
                schItem->Serialize( newItem );
                commit->Modify( schItem, screen );
            }
            else
            {
                wxASSERT( false );
            }

            if( !m_activeClients.count( aClientName ) )
                pushCurrentCommit( aClientName, _( "Created items via API" ) );
        }

        aItemHandler( status, newItem );
    }


    return ItemRequestStatus::IRS_OK;
}


void API_HANDLER_SCH::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                           const std::string& aClientName )
{
    SCH_SCREEN* screen = m_frame->GetScreen();

    if( !screen )
        return;

    std::vector<SCH_ITEM*> itemsToRemove;

    for( auto& [id, status] : aItemsToDelete )
    {
        if( std::optional<SCH_ITEM*> item = getItemById( id ) )
        {
            itemsToRemove.push_back( *item );
            status = ItemDeletionStatus::IDS_OK;
        }
    }

    if( itemsToRemove.empty() )
        return;

    COMMIT* commit = getCurrentCommit( aClientName );

    for( SCH_ITEM* item : itemsToRemove )
        commit->Remove( item, screen );

    if( !m_activeClients.count( aClientName ) )
        pushCurrentCommit( aClientName, _( "Deleted schematic items via API" ) );
}


std::optional<EDA_ITEM*> API_HANDLER_SCH::getItemFromDocument( const DocumentSpecifier& aDocument,
                                                               const KIID& aId )
{
    if( std::optional<SCH_ITEM*> item = getItemById( aId ) )
        return *item;

    return std::nullopt;
}


std::optional<SCH_ITEM*> API_HANDLER_SCH::getItemById( const KIID& aId ) const
{
    SCH_SCREEN* screen = m_frame->GetScreen();

    if( !screen )
        return std::nullopt;

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->m_Uuid == aId )
            return item;
    }

    return std::nullopt;
}


kiapi::schematic::types::Symbol API_HANDLER_SCH::buildSymbolMessage( const SCH_SYMBOL& aSymbol,
                                                                     const SCH_SHEET_PATH& aPath ) const
{
    kiapi::schematic::types::Symbol proto;

    proto.mutable_id()->set_value( aSymbol.m_Uuid.AsStdString() );
    proto.mutable_lib_id()->CopyFrom( kiapi::common::LibIdToProto( aSymbol.GetLibId() ) );
    kiapi::common::PackVector2( *proto.mutable_position(), aSymbol.GetPosition() );
    proto.set_unit( aSymbol.GetUnit() );

    SCH_SHEET_PATH& sheetPath = const_cast<SCH_SHEET_PATH&>( aPath );
    proto.set_reference( aSymbol.GetRef( &sheetPath ).ToStdString() );

    if( const SCH_FIELD* valueField = aSymbol.GetField( FIELD_T::VALUE ) )
        proto.set_value( valueField->GetText().ToStdString() );

    int orientation = aSymbol.GetOrientation();
    proto.set_rotation( rotationFromOrientation( orientation ) );
    proto.set_mirror_x( ( orientation & SYMBOL_ORIENTATION_T::SYM_MIRROR_X ) != 0 );
    proto.set_mirror_y( ( orientation & SYMBOL_ORIENTATION_T::SYM_MIRROR_Y ) != 0 );

    return proto;
}
