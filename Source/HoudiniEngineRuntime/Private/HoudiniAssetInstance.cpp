/*
* Copyright (c) <2017> Side Effects Software Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Produced by:
*      Mykola Konyk
*      Side Effects Software Inc
*      123 Front Street West, Suite 1401
*      Toronto, Ontario
*      Canada   M5J 2M2
*      416-504-9876
*
*/

#include "HoudiniEngineRuntimePrivatePCH.h"
#include "HoudiniAssetInstance.h"
#include "HoudiniPluginSerializationVersion.h"
#include "HoudiniAsset.h"
#include "HoudiniEngineString.h"
#include "HoudiniEngineUtils.h"
#include "HoudiniApi.h"
#include "HoudiniEngine.h"
#include "HoudiniEngineUtils.h"
#include "HoudiniApi.h"
#include "HoudiniEngine.h"


UHoudiniAssetInstance::UHoudiniAssetInstance( const FObjectInitializer & ObjectInitializer )
    : Super( ObjectInitializer )
    , HoudiniAsset( nullptr )
    , InstantiatedAssetName( TEXT( "" ) )
    , AssetId( -1 )
    , AssetCookCount( 0 )
    , bIsAssetBeingAsyncInstantiatedOrCooked( false )
    , Transform( FTransform::Identity )
    , HoudiniAssetInstanceFlagsPacked( 0u )
    , HoudiniAssetInstanceVersion( VER_HOUDINI_PLUGIN_SERIALIZATION_VERSION_BASE )
{}

UHoudiniAssetInstance::~UHoudiniAssetInstance()
{}

UHoudiniAssetInstance *
UHoudiniAssetInstance::CreateAssetInstance( UObject * Outer, UHoudiniAsset * HoudiniAsset )
{
    if ( !HoudiniAsset )
        return nullptr;

    UHoudiniAssetInstance * HoudiniAssetInstance = NewObject< UHoudiniAssetInstance >(
        Outer, UHoudiniAssetInstance::StaticClass(), NAME_None, RF_Public | RF_Transactional );

    HoudiniAssetInstance->HoudiniAsset = HoudiniAsset;
    return HoudiniAssetInstance;
}

UHoudiniAsset *
UHoudiniAssetInstance::GetHoudiniAsset() const
{
    return HoudiniAsset;
}

bool
UHoudiniAssetInstance::IsValidAssetInstance() const
{
    return FHoudiniEngineUtils::IsValidAssetId( AssetId );
}

int32
UHoudiniAssetInstance::GetAssetCookCount() const
{
    return AssetCookCount;
}

const FTransform &
UHoudiniAssetInstance::GetAssetTransform() const
{
    return Transform;
}

void
UHoudiniAssetInstance::AddReferencedObjects( UObject * InThis, FReferenceCollector & Collector )
{
    UHoudiniAssetInstance * HoudiniAssetInstance = Cast< UHoudiniAssetInstance >( InThis );
    if ( HoudiniAssetInstance && !HoudiniAssetInstance->IsPendingKill() )
    {
        if ( HoudiniAssetInstance->HoudiniAsset )
            Collector.AddReferencedObject( HoudiniAssetInstance->HoudiniAsset, InThis );
    }

    Super::AddReferencedObjects( InThis, Collector );
}

void
UHoudiniAssetInstance::Serialize( FArchive & Ar )
{
    Super::Serialize( Ar );

    Ar.UsingCustomVersion( FHoudiniCustomSerializationVersion::GUID );

    HoudiniAssetInstanceVersion = VER_HOUDINI_PLUGIN_SERIALIZATION_AUTOMATIC_VERSION;
    Ar << HoudiniAssetInstanceVersion;

    Ar << HoudiniAssetInstanceFlagsPacked;

    Ar << HoudiniAsset;
    Ar << DefaultPresetBuffer;
    Ar << Transform;

    HAPI_AssetId AssetIdTemp = AssetId;
    Ar << AssetIdTemp;

    if ( Ar.IsLoading() )
        AssetId = AssetIdTemp;
}

void
UHoudiniAssetInstance::FinishDestroy()
{
    Super::FinishDestroy();
    DeleteAsset();
}

bool
UHoudiniAssetInstance::InstantiateAsset( bool * bInstantiatedWithErrors )
{
    return InstantiateAsset( FHoudiniEngineString(), bInstantiatedWithErrors );
}

bool
UHoudiniAssetInstance::InstantiateAsset(
    const FHoudiniEngineString& AssetNameToInstantiate,
    bool * bInstantiatedWithErrors )
{
    HOUDINI_LOG_MESSAGE( TEXT( "HAPI Synchronous Instantiation Started. HoudiniAsset = 0x%x, " ), HoudiniAsset );

    if ( bInstantiatedWithErrors )
        *bInstantiatedWithErrors = false;

    if ( !HoudiniAsset )
    {
        HOUDINI_LOG_MESSAGE( TEXT( "Error instantiating asset, asset is null!" ) );
        return false;
    }

    if ( !FHoudiniEngineUtils::IsInitialized() )
    {
        HOUDINI_LOG_MESSAGE( TEXT( "Error instantiating asset, HAPI is not initialized!" ) );
        return false;
    }

    if ( IsValidAssetInstance() )
    {
        HOUDINI_LOG_MESSAGE( TEXT( "Error instantiating asset, asset is already instantiated!" ) );
        return false;
    }

    FHoudiniEngineString AssetName( AssetNameToInstantiate );

    HAPI_AssetLibraryId AssetLibraryId = -1;
    HAPI_Result Result = HAPI_RESULT_SUCCESS;

    if ( !AssetName.HasValidId() )
    {
        // No asset was specified, retrieve assets.

        TArray< HAPI_StringHandle > AssetNames;
        if ( !FHoudiniEngineUtils::GetAssetNames( HoudiniAsset, AssetLibraryId, AssetNames ) )
        {
            HOUDINI_LOG_MESSAGE( TEXT( "Error instantiating the asset, error retrieving asset names from HDA." ) );
            return false;
        }

        if ( !AssetNames.Num() )
        {
            HOUDINI_LOG_MESSAGE( TEXT( "Error instantiating the asset, HDA does not contain assets." ) );
            return false;
        }

        AssetName = FHoudiniEngineString( AssetNames[ 0 ] );
        if ( !AssetName.HasValidId() )
        {
            HOUDINI_LOG_MESSAGE( TEXT( "Error instantiating the asset, HDA specifies invalid asset." ) );
            return false;
        }
    }

    std::string AssetNameString;
    if ( !AssetName.ToStdString( AssetNameString ) )
    {
        HOUDINI_LOG_MESSAGE( TEXT( "Error instantiating the asset, error translating the asset name." ) );
        return false;
    }

    FString AssetNameUnreal = ANSI_TO_TCHAR( AssetNameString.c_str() );
    double TimingStart = FPlatformTime::Seconds();

    // We instantiate without cooking.
    HAPI_AssetId AssetIdNew = -1;
    if ( FHoudiniApi::InstantiateAsset(
        FHoudiniEngine::Get().GetSession(), &AssetNameString[ 0 ], false, &AssetIdNew ) != HAPI_RESULT_SUCCESS )
    {
        HOUDINI_LOG_MESSAGE(
            TEXT( "Error instantiating the asset, %s failed InstantiateAsset API call." ),
            *AssetNameUnreal );
        return false;
    }

    AssetId = AssetIdNew;
    AssetCookCount = 0;
    bool bResultSuccess = false;

    while ( true )
    {
        int Status = HAPI_STATE_STARTING_COOK;
        if ( FHoudiniApi::GetStatus(
            FHoudiniEngine::Get().GetSession(), HAPI_STATUS_COOK_STATE, &Status ) != HAPI_RESULT_SUCCESS )
        {
            HOUDINI_LOG_MESSAGE(
                TEXT( "Error instantiating the asset, %s failed GetStatus API call." ),
                *AssetNameUnreal );
            return false;
        }

        if ( Status == HAPI_STATE_READY )
        {
            HOUDINI_LOG_MESSAGE( TEXT( "Instantiated asset %s successfully."), *AssetNameUnreal );
            bResultSuccess = true;
            break;
        }
        else if ( Status == HAPI_STATE_READY_WITH_COOK_ERRORS )
        {
            if ( bInstantiatedWithErrors )
                *bInstantiatedWithErrors = true;

            HOUDINI_LOG_MESSAGE( TEXT( "Instantiated asset %s with errors." ), *AssetNameUnreal );
            bResultSuccess = true;
            break;
        }
        else if ( Status == HAPI_STATE_READY_WITH_FATAL_ERRORS )
        {
            if ( bInstantiatedWithErrors )
                *bInstantiatedWithErrors = true;

            HOUDINI_LOG_MESSAGE( TEXT( "Failed to instantiate the asset %s ." ), *AssetNameUnreal );
            bResultSuccess = false;
            break;
        }

        FPlatformProcess::Sleep( 0.0f );
    }

    if ( bResultSuccess )
    {
        double AssetOperationTiming = FPlatformTime::Seconds() - TimingStart;
        HOUDINI_LOG_MESSAGE(
            TEXT( "Instantiation of asset %s took %f seconds." ), *AssetNameUnreal,
            AssetOperationTiming );

        InstantiatedAssetName = AssetNameUnreal;

        PostInstantiateAsset();
    }

    return bResultSuccess;
}

bool
UHoudiniAssetInstance::CookAsset( bool * bCookedWithErrors )
{
    HOUDINI_LOG_MESSAGE(
        TEXT( "HAPI Synchronous Cooking of %s Started. HoudiniAsset = 0x%x, " ),
        *InstantiatedAssetName, HoudiniAsset );

    if ( bCookedWithErrors )
        *bCookedWithErrors = false;

    if ( !HoudiniAsset )
    {
        HOUDINI_LOG_MESSAGE(
            TEXT( "Error cooking the %s asset, asset is null!" ),
            *InstantiatedAssetName );
        return false;
    }

    if ( !FHoudiniEngineUtils::IsInitialized() )
    {
        HOUDINI_LOG_MESSAGE(
            TEXT( "Error cooking the %s asset, HAPI is not initialized!" ),
            *InstantiatedAssetName );
        return false;
    }

    if ( !IsValidAssetInstance() )
    {
        HOUDINI_LOG_MESSAGE(
            TEXT( "Error cooking the %s asset, asset has not been instantiated!" ),
            *InstantiatedAssetName );
        return false;
    }

    double TimingStart = FPlatformTime::Seconds();

    if ( FHoudiniApi::CookAsset( FHoudiniEngine::Get().GetSession(), AssetId, nullptr ) != HAPI_RESULT_SUCCESS )
    {
        HOUDINI_LOG_MESSAGE(
            TEXT( "Error cooking the %s asset, failed CookAsset API call." ),
            *InstantiatedAssetName );
        return false;
    }

    bool bResultSuccess = false;

    while ( true )
    {
        int32 Status = HAPI_STATE_STARTING_COOK;
        if ( FHoudiniApi::GetStatus(
            FHoudiniEngine::Get().GetSession(), HAPI_STATUS_COOK_STATE, &Status) != HAPI_RESULT_SUCCESS )
        {
            HOUDINI_LOG_MESSAGE(
                TEXT( "Error cooking the %s asset, failed GetStatus API call." ),
                *InstantiatedAssetName );
            return false;
        }

        if ( Status == HAPI_STATE_READY )
        {
            HOUDINI_LOG_MESSAGE( TEXT( "Cooked asset %s successfully."), *InstantiatedAssetName );
            bResultSuccess = true;
            break;
        }
        else if ( Status == HAPI_STATE_READY_WITH_COOK_ERRORS )
        {
            HOUDINI_LOG_MESSAGE( TEXT( "Cooked asset %s with errors." ), *InstantiatedAssetName );

            if( bCookedWithErrors )
                *bCookedWithErrors = true;

            bResultSuccess = true;
            break;
        }
        else if ( Status == HAPI_STATE_READY_WITH_FATAL_ERRORS )
        {
            HOUDINI_LOG_MESSAGE( TEXT( "Failed to cook the asset %s ." ), *InstantiatedAssetName );

            if ( bCookedWithErrors )
                *bCookedWithErrors = true;

            bResultSuccess = false;
            break;
        }

        FPlatformProcess::Sleep( 0.0f );
    }

    AssetCookCount++;

    if ( bResultSuccess )
    {
        double AssetOperationTiming = FPlatformTime::Seconds() - TimingStart;
        HOUDINI_LOG_MESSAGE(
            TEXT( "Cooking of asset %s took %f seconds." ),
            *InstantiatedAssetName,
            AssetOperationTiming );

        PostCookAsset();
    }

    return bResultSuccess;
}

bool
UHoudiniAssetInstance::DeleteAsset()
{
    HOUDINI_LOG_MESSAGE(
        TEXT( "HAPI Synchronous Deletion of %s Started. HoudiniAsset = 0x%x, " ),
        *InstantiatedAssetName, HoudiniAsset );

    if ( FHoudiniEngineUtils::IsInitialized() && IsValidAssetInstance() )
        FHoudiniEngineUtils::DestroyHoudiniAsset( AssetId );

    AssetId = -1;
    return true;
}

bool
UHoudiniAssetInstance::InstantiateAssetAsync()
{
    check( 0 );

    FPlatformAtomics::InterlockedExchange( &bIsAssetBeingAsyncInstantiatedOrCooked, 1 );

    return false;
}

bool
UHoudiniAssetInstance::CookAssetAsync()
{
    check( 0 );

    FPlatformAtomics::InterlockedExchange( &bIsAssetBeingAsyncInstantiatedOrCooked, 1 );

    return false;
}

bool
UHoudiniAssetInstance::DeleteAssetAsync()
{
    check( 0 );
    return false;
}

bool
UHoudiniAssetInstance::IsAssetBeingAsyncInstantiatedOrCooked() const
{
    return bIsAssetBeingAsyncInstantiatedOrCooked == 1;
}

bool
UHoudiniAssetInstance::HasAssetFinishedAsyncInstantiation( bool * bInstantiatedWithErrors ) const
{
    check( 0 );
    return false;
}

bool
UHoudiniAssetInstance::HasAssetFinishedAsyncCooking( bool * bCookedWithErrors ) const
{
    check( 0 );
    return false;
}

bool
UHoudiniAssetInstance::GetGeoPartObjects( TArray< FHoudiniGeoPartObject > & InGeoPartObjects ) const
{
    InGeoPartObjects.Empty();

    if ( !IsValidAssetInstance() )
        return false;

    TArray< HAPI_ObjectInfo > ObjectInfos;
    if ( !HapiGetObjectInfos( ObjectInfos ) )
        return false;

    TArray< FTransform > ObjectTransforms;
    if ( !HapiGetObjectTransforms( ObjectTransforms ) )
        return false;

    check( ObjectInfos.Num() == ObjectTransforms.Num() );

    if ( !ObjectInfos.Num() )
        return true;

    for ( int32 ObjectIdx = 0, ObjectNum = ObjectInfos.Num(); ObjectIdx < ObjectNum; ++ObjectIdx )
    {
        const HAPI_ObjectInfo & ObjectInfo = ObjectInfos[ ObjectIdx ];
        const FTransform & ObjectTransform = ObjectTransforms[ ObjectIdx ];

        FString ObjectName = TEXT( "" );
        FHoudiniEngineString HoudiniEngineStringObjectName( ObjectInfo.nameSH );
        HoudiniEngineStringObjectName.ToFString( ObjectName );

        for ( int32 GeoIdx = 0; GeoIdx < ObjectInfo.geoCount; ++GeoIdx )
        {
            HAPI_GeoInfo GeoInfo;
            if ( !HapiGetGeoInfo( ObjectInfo.id, GeoIdx, GeoInfo ) )
                continue;

            if ( !GeoInfo.isDisplayGeo )
                continue;

            for ( int32 PartIdx = 0; PartIdx < GeoInfo.partCount; ++PartIdx )
            {
                HAPI_PartInfo PartInfo;
                if ( !HapiGetPartInfo( ObjectInfo.id, GeoInfo.id, PartIdx, PartInfo ) )
                    continue;

                FString PartName = TEXT( "" );
                FHoudiniEngineString HoudiniEngineStringPartName( PartInfo.nameSH );
                HoudiniEngineStringPartName.ToFString( PartName );

                InGeoPartObjects.Add( FHoudiniGeoPartObject(
                    ObjectTransform, ObjectName, PartName, AssetId, ObjectInfo.id,
                    GeoInfo.id, PartInfo.id ) );
            }
        }
    }

    return true;
}

bool
UHoudiniAssetInstance::GetParameterObjects( TMap< FString, FHoudiniParameterObject > & InParameterObjects ) const
{
    TArray< HAPI_ParmInfo > ParmInfos;
    if ( !HapiGetParmInfos( ParmInfos ) )
        return false;

    HAPI_NodeId NodeId = HapiGetNodeId();
    if ( NodeId < 0 )
        return false;

    for ( int32 ParmIdx = 0, ParmNum = ParmInfos.Num(); ParmIdx < ParmNum; ++ParmIdx )
    {
        FString ParameterName = TEXT( "" );
        FHoudiniParameterObject HoudiniParameterObject( NodeId, ParmInfos[ ParmIdx ] );
        if ( HoudiniParameterObject.HapiGetName( ParameterName ) )
            InParameterObjects.Add( ParameterName, HoudiniParameterObject );
    }

    return true;
}

bool
UHoudiniAssetInstance::GetInputObjects( TArray< FHoudiniInputObject > & InInputObjects ) const
{
    InInputObjects.Empty();

    HAPI_AssetInfo AssetInfo;
    if ( !HapiGetAssetInfo( AssetInfo ) )
        return false;

    if ( AssetInfo.geoInputCount > 0 )
    {
        InInputObjects.SetNumUninitialized( AssetInfo.geoInputCount );
        for ( int32 InputIdx = 0; InputIdx < AssetInfo.geoInputCount; ++InputIdx )
            InInputObjects[ InputIdx ] = FHoudiniInputObject( InputIdx );
    }

    return true;
}

HAPI_NodeId
UHoudiniAssetInstance::HapiGetNodeId() const
{
    HAPI_AssetInfo AssetInfo;

    if ( !HapiGetAssetInfo( AssetInfo ) )
        return -1;

    return AssetInfo.nodeId;
}

bool
UHoudiniAssetInstance::HapiGetNodeInfo( HAPI_NodeInfo & NodeInfo ) const
{
    FMemory::Memset< HAPI_NodeInfo >( NodeInfo, 0 );

    HAPI_NodeId NodeId = HapiGetNodeId();
    if ( NodeId < 0 )
        return false;

    if ( FHoudiniApi::GetNodeInfo( FHoudiniEngine::Get().GetSession(), NodeId, &NodeInfo ) != HAPI_RESULT_SUCCESS )
        return false;

    return true;
}

bool
UHoudiniAssetInstance::HapiGetAssetInfo( HAPI_AssetInfo & AssetInfo ) const
{
    FMemory::Memset< HAPI_AssetInfo >( AssetInfo, 0 );

    if ( !IsValidAssetInstance() )
        return false;

    if ( FHoudiniApi::GetAssetInfo( FHoudiniEngine::Get().GetSession(), AssetId, &AssetInfo ) != HAPI_RESULT_SUCCESS )
        return false;

    return true;
}

bool
UHoudiniAssetInstance::HapiGetObjectInfos( TArray< HAPI_ObjectInfo > & ObjectInfos ) const
{
    ObjectInfos.Empty();

    if ( !IsValidAssetInstance() )
        return false;

    HAPI_AssetInfo AssetInfo;
    if ( !HapiGetAssetInfo( AssetInfo ) )
        return false;

    if ( AssetInfo.objectCount > 0 )
    {
        ObjectInfos.SetNumUninitialized( AssetInfo.objectCount );
        if ( FHoudiniApi::GetObjects(
            FHoudiniEngine::Get().GetSession(), AssetId, &ObjectInfos[ 0 ],
            0, AssetInfo.objectCount ) != HAPI_RESULT_SUCCESS )
        {
            return false;
        }
    }

    return true;
}

bool
UHoudiniAssetInstance::HapiGetObjectTransforms( TArray< FTransform > & ObjectTransforms ) const
{
    ObjectTransforms.Empty();

    if ( !IsValidAssetInstance() )
        return false;

    HAPI_AssetInfo AssetInfo;
    if ( !HapiGetAssetInfo( AssetInfo ) )
        return false;

    if ( AssetInfo.objectCount > 0 )
    {
        TArray< HAPI_Transform > HapiObjectTransforms;
        HapiObjectTransforms.SetNumUninitialized( AssetInfo.objectCount );
        ObjectTransforms.SetNumUninitialized( AssetInfo.objectCount );

        if ( FHoudiniApi::GetObjectTransforms(
            FHoudiniEngine::Get().GetSession(), AssetId,
            HAPI_SRT, &HapiObjectTransforms[ 0 ], 0, AssetInfo.objectCount ) != HAPI_RESULT_SUCCESS )
        {
            return false;
        }

        for ( int32 Idx = 0; Idx < AssetInfo.objectCount; ++Idx )
            FHoudiniEngineUtils::TranslateHapiTransform( HapiObjectTransforms[ Idx ], ObjectTransforms[ Idx ] );
    }

    return true;
}

bool
UHoudiniAssetInstance::HapiGetAssetTransform( FTransform & InTransform ) const
{
    InTransform.SetIdentity();

    if ( !IsValidAssetInstance() )
        return false;

    HAPI_TransformEuler AssetEulerTransform;
    if ( FHoudiniApi::GetAssetTransform(
        FHoudiniEngine::Get().GetSession(), AssetId,
        HAPI_SRT, HAPI_XYZ, &AssetEulerTransform ) != HAPI_RESULT_SUCCESS )
    {
        return false;
    }

    // Convert HAPI Euler transform to Unreal one.
    FHoudiniEngineUtils::TranslateHapiTransform( AssetEulerTransform, InTransform );
    return true;
}

bool
UHoudiniAssetInstance::HapiGetGeoInfo( HAPI_ObjectId ObjectId, int32 GeoIdx, HAPI_GeoInfo& GeoInfo ) const
{
    FMemory::Memset< HAPI_GeoInfo >( GeoInfo, 0 );

    if ( !IsValidAssetInstance() )
        return false;

    if ( FHoudiniApi::GetGeoInfo(
        FHoudiniEngine::Get().GetSession(), AssetId, ObjectId, GeoIdx,
        &GeoInfo ) != HAPI_RESULT_SUCCESS )
    {
        return false;
    }

    return true;
}

bool
UHoudiniAssetInstance::HapiGetPartInfo(
    HAPI_ObjectId ObjectId, HAPI_GeoId GeoId, int32 PartIdx,
    HAPI_PartInfo & PartInfo ) const
{
    FMemory::Memset< HAPI_PartInfo >( PartInfo, 0 );

    if ( !IsValidAssetInstance() )
        return false;

    if ( FHoudiniApi::GetPartInfo(
        FHoudiniEngine::Get().GetSession(), AssetId, ObjectId, GeoId,
        PartIdx, &PartInfo ) != HAPI_RESULT_SUCCESS )
    {
        return false;
    }

    return true;
}

bool
UHoudiniAssetInstance::HapiGetParmInfos( TArray< HAPI_ParmInfo > & ParmInfos ) const
{
    ParmInfos.Empty();

    if ( !IsValidAssetInstance() )
        return false;

    HAPI_NodeInfo NodeInfo;
    if ( !HapiGetNodeInfo( NodeInfo ) )
        return false;

    if ( NodeInfo.parmCount > 0 )
    {
        if( FHoudiniApi::GetParameters(
            FHoudiniEngine::Get().GetSession(), NodeInfo.id,
            &ParmInfos[ 0 ], 0, NodeInfo.parmCount ) != HAPI_RESULT_SUCCESS )
        {
            return false;
        }
    }

    return true;
}

bool
UHoudiniAssetInstance::HapiGetAssetPreset( TArray< char > & PresetBuffer ) const
{
    PresetBuffer.Empty();

    HAPI_NodeId NodeId = HapiGetNodeId();
    if ( NodeId < 0 )
        return false;

    int32 BufferLength = 0;
    if ( FHoudiniApi::GetPresetBufLength(
        FHoudiniEngine::Get().GetSession(), NodeId,
        HAPI_PRESETTYPE_BINARY, nullptr, &BufferLength ) != HAPI_RESULT_SUCCESS )
    {
        return false;
    }

    PresetBuffer.SetNumZeroed( BufferLength );
    if( FHoudiniApi::GetPreset(
        FHoudiniEngine::Get().GetSession(), NodeId, &PresetBuffer[0],
        BufferLength ) != HAPI_RESULT_SUCCESS )
    {
        PresetBuffer.Empty();
        return false;
    }

    return true;
}

bool
UHoudiniAssetInstance::HapiSetAssetPreset( const TArray< char > & PresetBuffer ) const
{
    if ( !PresetBuffer.Num() )
        return false;

    HAPI_NodeId NodeId = HapiGetNodeId();
    if ( NodeId < 0 )
        return false;

    if ( FHoudiniApi::SetPreset(
        FHoudiniEngine::Get().GetSession(), NodeId,
        HAPI_PRESETTYPE_BINARY, nullptr, &PresetBuffer[ 0 ], PresetBuffer.Num() ) != HAPI_RESULT_SUCCESS )
    {
        return false;
    }

    return true;
}

bool
UHoudiniAssetInstance::HapiSetDefaultPreset() const
{
    return HapiSetAssetPreset( DefaultPresetBuffer );
}

bool
UHoudiniAssetInstance::PostInstantiateAsset()
{
    HapiGetAssetPreset( DefaultPresetBuffer );

    HapiGetAssetTransform( Transform );
    GetParameterObjects( ParameterObjects );
    GetInputObjects( InputObjects );

    return true;
}

bool
UHoudiniAssetInstance::PostCookAsset()
{
    HapiGetAssetTransform( Transform );
    GetParameterObjects( ParameterObjects );
    GetGeoPartObjects( GeoPartObjects );
    GetInputObjects( InputObjects );

    return true;
}
