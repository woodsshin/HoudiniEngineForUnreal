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
#include "HoudiniEngineUtils.h"
#include "HoudiniAssetInput.h"
#include "HoudiniSplineComponent.h"
#include "HoudiniAssetParameter.h"
#include "HoudiniAssetComponent.h"
#include "HoudiniAssetParameterInt.h"
#include "HoudiniAssetParameterChoice.h"
#include "HoudiniAssetParameterToggle.h"
#include "HoudiniEngine.h"
#include "HoudiniAssetActor.h"
#include "HoudiniApi.h"
#include "HoudiniPluginSerializationVersion.h"
#include "HoudiniEngineString.h"
#include "Components/SplineComponent.h"

void
FHoudiniAssetInputOutlinerMesh::Serialize( FArchive & Ar )
{
    Ar.UsingCustomVersion( FHoudiniCustomSerializationVersion::GUID );

    HoudiniAssetParameterVersion = VER_HOUDINI_PLUGIN_SERIALIZATION_AUTOMATIC_VERSION;
    Ar << HoudiniAssetParameterVersion;

    Ar << Actor;

    Ar << StaticMeshComponent;
    Ar << StaticMesh;
    Ar << ActorTransform;

    Ar << AssetId;
    if (Ar.IsLoading() && !Ar.IsTransacting())
        AssetId = -1;

    if ( HoudiniAssetParameterVersion >= VER_HOUDINI_PLUGIN_SERIALIZATION_VERSION_ADDED_UNREAL_SPLINE )
    {
	Ar << SplineComponent;
	Ar << NumberOfSplineControlPoints;
	Ar << SplineLength;
	Ar << SplineResolution;
	Ar << ComponentTransform;
    }

    if ( HoudiniAssetParameterVersion >= VER_HOUDINI_PLUGIN_SERIALIZATION_VERSION_ADDED_KEEP_TRANSFORM )
	Ar << KeepWorldTransform;
}

void 
FHoudiniAssetInputOutlinerMesh::RebuildSplineTransformsArrayIfNeeded()
{
    // Rebuilding the SplineTransform array after reloading the asset
    // This is required to properly detect Transform changes after loading the asset.

    // We need an Unreal spline
    if (!SplineComponent)
        return;

    // If those are different, the input component has changed
    if (NumberOfSplineControlPoints != SplineComponent->GetNumberOfSplinePoints())
        return;

    // If those are equals, there's no need to rebuild the array
    if (SplineControlPointsTransform.Num() == SplineComponent->GetNumberOfSplinePoints())
        return;

    SplineControlPointsTransform.SetNumUninitialized(SplineComponent->GetNumberOfSplinePoints());
    for (int32 n = 0; n < SplineControlPointsTransform.Num(); n++)
        SplineControlPointsTransform[n] = SplineComponent->GetTransformAtSplinePoint(n, ESplineCoordinateSpace::Local, true);
}

bool
FHoudiniAssetInputOutlinerMesh::HasSplineComponentChanged(float fCurrentSplineResolution) const
{
    if (!SplineComponent)
        return false;

    // Total length of the spline has changed ?
    if (SplineComponent->GetSplineLength() != SplineLength)
        return true;

    // Number of CVs has changed ?
    if (NumberOfSplineControlPoints != SplineComponent->GetNumberOfSplinePoints())
        return true;
    
    if (SplineControlPointsTransform.Num() != SplineComponent->GetNumberOfSplinePoints())
        return true;

    // Current Spline resolution has changed?
    if (fCurrentSplineResolution == -1.0)
    {
        const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
        if (HoudiniRuntimeSettings)
            fCurrentSplineResolution = HoudiniRuntimeSettings->MarshallingSplineResolution;
        else
            fCurrentSplineResolution = HAPI_UNREAL_PARAM_SPLINE_RESOLUTION_DEFAULT;
    }
    
    if ( SplineResolution != fCurrentSplineResolution )
        return true;

    // Has any of the CV's transform been modified?
    for (int32 n = 0; n < SplineControlPointsTransform.Num(); n++)
    {
        if ( !SplineControlPointsTransform[n].GetLocation().Equals(SplineComponent->GetLocationAtSplinePoint(n, ESplineCoordinateSpace::Local) ) )
            return true;

        if ( !SplineControlPointsTransform[n].GetRotation().Equals(SplineComponent->GetQuaternionAtSplinePoint(n, ESplineCoordinateSpace::World) ) )
            return true;

        if (!SplineControlPointsTransform[n].GetScale3D().Equals(SplineComponent->GetScaleAtSplinePoint(n) ) )
            return true;
    }

    return false;
}


bool 
FHoudiniAssetInputOutlinerMesh::HasActorTransformChanged() const
{
    if (!Actor)
	return false;

    if (!ActorTransform.Equals(Actor->GetTransform()))
	return true;

    return false;
}


bool
FHoudiniAssetInputOutlinerMesh::HasComponentTransformChanged() const
{
    if (!SplineComponent && !StaticMeshComponent)
	return false;

    if (SplineComponent)
	return !ComponentTransform.Equals(SplineComponent->GetComponentTransform());

    if (StaticMeshComponent)
	return !ComponentTransform.Equals(StaticMeshComponent->GetComponentTransform());

    return false;

}

UHoudiniAssetInput::UHoudiniAssetInput( const FObjectInitializer & ObjectInitializer )
    : Super( ObjectInitializer )
    , InputCurve( nullptr )
    , InputAssetComponent( nullptr )
    , InputLandscapeProxy( nullptr )
    , ConnectedAssetId( -1 )
    , InputIndex( 0 )
    , ChoiceIndex( EHoudiniAssetInputType::GeometryInput )
    , UnrealSplineResolution( -1.0f )
    , HoudiniAssetInputFlagsPacked( 0u )
{
    // flags
    bLandscapeExportMaterials = true;
    bKeepWorldTransform = 2;

    ChoiceStringValue = TEXT( "" );
}

UHoudiniAssetInput::~UHoudiniAssetInput()
{}

UHoudiniAssetInput *
UHoudiniAssetInput::Create( UHoudiniAssetComponent * InHoudiniAssetComponent, int32 InInputIndex )
{
    UHoudiniAssetInput * HoudiniAssetInput = nullptr;

    // Get name of this input.
    HAPI_StringHandle InputStringHandle;
    if ( FHoudiniApi::GetInputName(
        FHoudiniEngine::Get().GetSession(),
        InHoudiniAssetComponent->GetAssetId(),
        InInputIndex, HAPI_INPUT_GEOMETRY, &InputStringHandle ) != HAPI_RESULT_SUCCESS )
    {
        return HoudiniAssetInput;
    }

    HoudiniAssetInput = NewObject< UHoudiniAssetInput >(
        InHoudiniAssetComponent,
        UHoudiniAssetInput::StaticClass(),
        NAME_None, RF_Public | RF_Transactional );

    // Set component and other information.
    HoudiniAssetInput->HoudiniAssetComponent = InHoudiniAssetComponent;
    HoudiniAssetInput->InputIndex = InInputIndex;

    // Get input string from handle.
    HoudiniAssetInput->SetNameAndLabel( InputStringHandle );

    // By default geometry input is chosen.
    HoudiniAssetInput->ChoiceIndex = EHoudiniAssetInputType::GeometryInput;

    // Create necessary widget resources.
    HoudiniAssetInput->CreateWidgetResources();

    return HoudiniAssetInput;
}

void
UHoudiniAssetInput::CreateWidgetResources()
{
    ChoiceStringValue = TEXT( "" );
    StringChoiceLabels.Empty();

    {
        FString * ChoiceLabel = new FString( TEXT( "Geometry Input" ) );
        StringChoiceLabels.Add( TSharedPtr< FString >( ChoiceLabel ) );

        if ( ChoiceIndex == EHoudiniAssetInputType::GeometryInput )
            ChoiceStringValue = *ChoiceLabel;
    }
    {
        FString * ChoiceLabel = new FString( TEXT( "Asset Input" ) );
        StringChoiceLabels.Add( TSharedPtr< FString >( ChoiceLabel ) );

        if ( ChoiceIndex == EHoudiniAssetInputType::AssetInput )
            ChoiceStringValue = *ChoiceLabel;
    }
    {
        FString * ChoiceLabel = new FString( TEXT( "Curve Input" ) );
        StringChoiceLabels.Add( TSharedPtr< FString >( ChoiceLabel ) );

        if ( ChoiceIndex == EHoudiniAssetInputType::CurveInput )
            ChoiceStringValue = *ChoiceLabel;
    }
    {
        FString * ChoiceLabel = new FString( TEXT( "Landscape Input" ) );
        StringChoiceLabels.Add( TSharedPtr< FString >( ChoiceLabel ) );

        if ( ChoiceIndex == EHoudiniAssetInputType::LandscapeInput )
            ChoiceStringValue = *ChoiceLabel;
    }
    {
        FString * ChoiceLabel = new FString( TEXT( "World Outliner Input" ) );
        StringChoiceLabels.Add( TSharedPtr< FString >( ChoiceLabel ) );

        if ( ChoiceIndex == EHoudiniAssetInputType::WorldInput )
            ChoiceStringValue = *ChoiceLabel;
    }
}

void
UHoudiniAssetInput::DisconnectAndDestroyInputAsset()
{
    if ( ChoiceIndex == EHoudiniAssetInputType::AssetInput )
    {
        if ( InputAssetComponent )
            InputAssetComponent->RemoveDownstreamAsset( HoudiniAssetComponent, InputIndex );

        InputAssetComponent = nullptr;
        ConnectedAssetId = -1;
    }
    else
    {
        if ( HoudiniAssetComponent )
        {
            HAPI_AssetId HostAssetId = HoudiniAssetComponent->GetAssetId();
            if( FHoudiniEngineUtils::IsValidAssetId(ConnectedAssetId) && FHoudiniEngineUtils::IsValidAssetId(HostAssetId) )
                FHoudiniEngineUtils::HapiDisconnectAsset(HostAssetId, InputIndex);                
        }

        // World Input Actors' Meshes need to have their corresponding Input Assets destroyed too.
        if ( ChoiceIndex == EHoudiniAssetInputType::WorldInput )
        {
            for ( int32 n = 0; n < InputOutlinerMeshArray.Num(); n++)
            {
                if ( FHoudiniEngineUtils::IsValidAssetId(InputOutlinerMeshArray[n].AssetId ) )
                {
                    FHoudiniEngineUtils::HapiDisconnectAsset(ConnectedAssetId, InputOutlinerMeshArray[n].AssetId);
                    FHoudiniEngineUtils::DestroyHoudiniAsset(InputOutlinerMeshArray[n].AssetId );
                    InputOutlinerMeshArray[n].AssetId = -1;
                }
            }
        }
        // Destroy all the geo input assets
        else if ( ChoiceIndex == EHoudiniAssetInputType::GeometryInput )
        {
            for ( HAPI_NodeId AssetNodeId : GeometryInputAssetIds )
            {
                FHoudiniEngineUtils::DestroyHoudiniAsset( AssetNodeId );
            }
            GeometryInputAssetIds.Empty();
        }

        if (FHoudiniEngineUtils::IsValidAssetId(ConnectedAssetId))
        {
            FHoudiniEngineUtils::DestroyHoudiniAsset(ConnectedAssetId);
            ConnectedAssetId = -1;
	}
    }
}

bool
UHoudiniAssetInput::CreateParameter(
    UHoudiniAssetComponent * InHoudiniAssetComponent,
    UHoudiniAssetParameter * InParentParameter,
    HAPI_NodeId InNodeId, const HAPI_ParmInfo & ParmInfo)
{
    // This implementation is not a true parameter. This method should not be called.
    check( false );
    return false;
}

#if WITH_EDITOR

void
UHoudiniAssetInput::CreateWidget( IDetailCategoryBuilder & LocalDetailCategoryBuilder )
{
    InputTypeComboBox.Reset();

    // Get thumbnail pool for this builder.
    IDetailLayoutBuilder & DetailLayoutBuilder = LocalDetailCategoryBuilder.GetParentLayout();
    TSharedPtr< FAssetThumbnailPool > AssetThumbnailPool = DetailLayoutBuilder.GetThumbnailPool();
    FDetailWidgetRow & Row = LocalDetailCategoryBuilder.AddCustomRow( FText::GetEmpty() );
    FText ParameterLabelText = FText::FromString( GetParameterLabel() );

    Row.NameWidget.Widget =
        SNew( STextBlock )
            .Text( ParameterLabelText )
            .ToolTipText( ParameterLabelText )
            .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) );

    TSharedRef< SVerticalBox > VerticalBox = SNew( SVerticalBox );

    if ( StringChoiceLabels.Num() > 0 )
    {
	// ComboBox :  Input Type
        VerticalBox->AddSlot().Padding( 2, 2, 5, 2 )
        [
            SAssignNew( InputTypeComboBox, SComboBox<TSharedPtr< FString > > )
            .OptionsSource( &StringChoiceLabels )
            .InitiallySelectedItem( StringChoiceLabels[ ChoiceIndex ] )
            .OnGenerateWidget( SComboBox< TSharedPtr< FString > >::FOnGenerateWidget::CreateUObject(
                this, &UHoudiniAssetInput::CreateChoiceEntryWidget ) )
            .OnSelectionChanged( SComboBox< TSharedPtr< FString > >::FOnSelectionChanged::CreateUObject(
                this, &UHoudiniAssetInput::OnChoiceChange ) )
            [
                SNew( STextBlock )
                .Text(TAttribute< FText >::Create( TAttribute< FText >::FGetter::CreateUObject(
                    this, &UHoudiniAssetInput::HandleChoiceContentText ) ) )
                .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) )
            ]
        ];
    }

    // Checkbox : Keep World Transform
    {	
	TSharedPtr< SCheckBox > CheckBoxTranformType;
	VerticalBox->AddSlot().Padding(2, 2, 5, 2).AutoHeight()
	[
	    SAssignNew(CheckBoxTranformType, SCheckBox)
	    .Content()
	    [
		SNew(STextBlock)
		.Text(LOCTEXT("KeepWorldTransformCheckbox", "Keep World Transform"))
		.ToolTipText(LOCTEXT("KeepWorldTransformCheckboxTip", "Set this Input's object_merge Transform Type to INTO_THIS_OBJECT. If unchecked, it will be set to NONE."))
		.Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")))
	    ]
	    .IsChecked(TAttribute< ECheckBoxState >::Create(
		TAttribute< ECheckBoxState >::FGetter::CreateUObject(
		this, &UHoudiniAssetInput::IsCheckedKeepWorldTransform)))
	    .OnCheckStateChanged(FOnCheckStateChanged::CreateUObject(
		this, &UHoudiniAssetInput::CheckStateChangedKeepWorldTransform))
	];
    }


    if ( ChoiceIndex == EHoudiniAssetInputType::GeometryInput )
    {
        int32 Ix = 0;
        const int32 NumInputs = InputObjects.Num();
        VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
        [
            SNew( SHorizontalBox )
            + SHorizontalBox::Slot()
            .Padding( 1.0f )
            .VAlign( VAlign_Center )
            .AutoWidth()
            [
                SNew( STextBlock )
                .Text( FText::Format( LOCTEXT( "NumArrayItemsFmt", "{0} elements" ), FText::AsNumber( NumInputs ) ) )
                .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) )
            ]
            + SHorizontalBox::Slot()
            .Padding( 1.0f )
            .VAlign( VAlign_Center )
            .AutoWidth()
            [
                PropertyCustomizationHelpers::MakeAddButton( FSimpleDelegate::CreateUObject( this, &UHoudiniAssetInput::OnAddToInputObjects ), LOCTEXT( "AddInput", "Adds a Geometry Input" ), true )
            ]
            + SHorizontalBox::Slot()
            .Padding( 1.0f )
            .VAlign( VAlign_Center )
            .AutoWidth()
            [
                PropertyCustomizationHelpers::MakeEmptyButton( FSimpleDelegate::CreateUObject( this, &UHoudiniAssetInput::OnEmptyInputObjects ), LOCTEXT( "EmptyInputs", "Removes All Inputs" ), true )
            ]
        ];
        do
        {
            UObject* InputObject = GetInputObject( Ix );
            CreateGeometryWidget( Ix, InputObject, AssetThumbnailPool, VerticalBox );
        } while ( ++Ix < NumInputs );
    }
    else if ( ChoiceIndex == EHoudiniAssetInputType::AssetInput )
    {
        // ActorPicker : Houdini Asset
        FMenuBuilder MenuBuilder = CreateCustomActorPickerWidget( LOCTEXT( "AssetInputSelectableActors", "Houdini Asset Actors" ), true );

        VerticalBox->AddSlot().Padding(2, 2, 5, 2).AutoHeight()
        [
            MenuBuilder.MakeWidget()
        ];
    }
    else if ( ChoiceIndex == EHoudiniAssetInputType::CurveInput )
    {
        // Go through all input curve parameters and build their widgets recursively.
        for ( TMap< FString, UHoudiniAssetParameter * >::TIterator
            IterParams( InputCurveParameters ); IterParams; ++IterParams )
        {
            UHoudiniAssetParameter * HoudiniAssetParameter = IterParams.Value();
            if ( HoudiniAssetParameter )
                HoudiniAssetParameter->CreateWidget( VerticalBox );
        }
    }
    else if ( ChoiceIndex == EHoudiniAssetInputType::LandscapeInput )
    {
        // ActorPicker : Landscape
        FMenuBuilder MenuBuilder = CreateCustomActorPickerWidget( LOCTEXT( "LandscapeInputSelectableActors", "Landscapes" ), true );
        VerticalBox->AddSlot().Padding(2, 2, 5, 2).AutoHeight()
        [
            MenuBuilder.MakeWidget()
        ];

        // CheckBox : Export Selected Only
        {
            TSharedPtr< SCheckBox > CheckBoxExportSelected;
            VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
            [
                SAssignNew( CheckBoxExportSelected, SCheckBox )
                .Content()
                [
                    SNew( STextBlock )
                    .Text( LOCTEXT( "LandscapeSelectedCheckbox", "Export Selected Landscape Only" ) )
                    .ToolTipText( LOCTEXT( "LandscapeSelectedCheckbox", "Export Selected Landscape Only" ) )
                    .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont") ) )
                ]
                .IsChecked( TAttribute< ECheckBoxState >::Create(
                    TAttribute< ECheckBoxState >::FGetter::CreateUObject(
                        this, &UHoudiniAssetInput::IsCheckedExportOnlySelected) ) )
                .OnCheckStateChanged( FOnCheckStateChanged::CreateUObject(
                    this, &UHoudiniAssetInput::CheckStateChangedExportOnlySelected ) )
            ];
        }

	// Checkbox : Export full geometry
        {
            TSharedPtr< SCheckBox > CheckBoxExportFullGeometry;
            VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
            [
                SAssignNew( CheckBoxExportFullGeometry, SCheckBox )
                .Content()
                [
                    SNew( STextBlock )
                    .Text( LOCTEXT( "LandscapeFullGeometryCheckbox", "Export Full Landscape Geometry" ) )
                    .ToolTipText( LOCTEXT( "LandscapeFullGeometryCheckbox", "Export Full Landscape Geometry" ) )
                    .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) )
                ]
                .IsChecked(TAttribute< ECheckBoxState >::Create(
                    TAttribute< ECheckBoxState >::FGetter::CreateUObject(
                        this, &UHoudiniAssetInput::IsCheckedExportFullGeometry ) ) )
                .OnCheckStateChanged( FOnCheckStateChanged::CreateUObject(
                    this, &UHoudiniAssetInput::CheckStateChangedExportFullGeometry ) )
            ];
        }

	// Checkbox : Export materials
        {
            TSharedPtr< SCheckBox > CheckBoxExportMaterials;
            VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
            [
                SAssignNew( CheckBoxExportMaterials, SCheckBox )
                .Content()
                [
                    SNew( STextBlock )
                    .Text( LOCTEXT( "LandscapeMaterialsCheckbox", "Export Landscape Materials" ) )
                    .ToolTipText( LOCTEXT( "LandscapeMaterialsCheckbox", "Export Landscape Materials" ) )
                    .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) )
                ]
                .IsChecked( TAttribute< ECheckBoxState >::Create(
                    TAttribute< ECheckBoxState >::FGetter::CreateUObject(
                    this, &UHoudiniAssetInput::IsCheckedExportMaterials ) ) )
                .OnCheckStateChanged( FOnCheckStateChanged::CreateUObject(
                    this, &UHoudiniAssetInput::CheckStateChangedExportMaterials ) )
            ];
        }

	// Checkbox : Export Tile UVs
        {
            TSharedPtr< SCheckBox > CheckBoxExportTileUVs;
            VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
            [
                SAssignNew( CheckBoxExportTileUVs, SCheckBox )
                .Content()
                [
                    SNew( STextBlock )
                    .Text( LOCTEXT( "LandscapeTileUVsCheckbox", "Export Landscape Tile UVs" ) )
                    .ToolTipText( LOCTEXT( "LandscapeTileUVsCheckbox", "Export Landscape Tile UVs" ) )
                    .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) )
                ]
                .IsChecked(TAttribute< ECheckBoxState >::Create(
                    TAttribute< ECheckBoxState >::FGetter::CreateUObject(
                    this, &UHoudiniAssetInput::IsCheckedExportTileUVs ) ) )
                .OnCheckStateChanged( FOnCheckStateChanged::CreateUObject(
                    this, &UHoudiniAssetInput::CheckStateChangedExportTileUVs ) )
            ];
        }

	// Checkbox : Export normalized UVs
        {
            TSharedPtr< SCheckBox > CheckBoxExportNormalizedUVs;
            VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
            [
                SAssignNew( CheckBoxExportNormalizedUVs, SCheckBox )
                .Content()
                [
                    SNew( STextBlock )
                    .Text( LOCTEXT( "LandscapeNormalizedUVsCheckbox", "Export Landscape Normalized UVs" ) )
                    .ToolTipText( LOCTEXT( "LandscapeNormalizedUVsCheckbox", "Export Landscape Normalized UVs" ) )
                    .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) )
                ]
                .IsChecked( TAttribute< ECheckBoxState >::Create(
                    TAttribute< ECheckBoxState >::FGetter::CreateUObject(
                        this, &UHoudiniAssetInput::IsCheckedExportNormalizedUVs ) ) )
                .OnCheckStateChanged(FOnCheckStateChanged::CreateUObject(
                    this, &UHoudiniAssetInput::CheckStateChangedExportNormalizedUVs ) )
            ];
        }

	// Checkbox : Export lighting
        {
            TSharedPtr< SCheckBox > CheckBoxExportLighting;
            VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
            [
                SAssignNew( CheckBoxExportLighting, SCheckBox )
                .Content()
                [
                    SNew( STextBlock )
                    .Text( LOCTEXT( "LandscapeLightingCheckbox", "Export Landscape Lighting" ) )
                    .ToolTipText( LOCTEXT( "LandscapeLightingCheckbox", "Export Landscape Lighting" ) )
                    .Font(FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) )
                ]
                .IsChecked( TAttribute< ECheckBoxState >::Create(
                    TAttribute< ECheckBoxState >::FGetter::CreateUObject(
                    this, &UHoudiniAssetInput::IsCheckedExportLighting ) ) )
                .OnCheckStateChanged( FOnCheckStateChanged::CreateUObject(
                    this, &UHoudiniAssetInput::CheckStateChangedExportLighting ) )
            ];
        }

	// Checkbox : Export landscape curves
        {
            TSharedPtr< SCheckBox > CheckBoxExportCurves;
            VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
            [
                SAssignNew( CheckBoxExportCurves, SCheckBox )
                .Content()
                [
                    SNew( STextBlock )
                    .Text( LOCTEXT( "LandscapeCurvesCheckbox", "Export Landscape Curves" ) )
                    .ToolTipText( LOCTEXT( "LandscapeCurvesCheckbox", "Export Landscape Curves" ) )
                    .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) )
                ]
                .IsChecked( TAttribute< ECheckBoxState >::Create(
                    TAttribute< ECheckBoxState >::FGetter::CreateUObject(
                    this, &UHoudiniAssetInput::IsCheckedExportCurves ) ) )
                .OnCheckStateChanged( FOnCheckStateChanged::CreateUObject(
                    this, &UHoudiniAssetInput::CheckStateChangedExportCurves ) )
            ];

            // Disable curves until we have them implemented.
            if ( CheckBoxExportCurves.IsValid() )
                CheckBoxExportCurves->SetEnabled( false );
        }

	// Button : Recommit
        VerticalBox->AddSlot().Padding( 2, 2, 5, 2 ).AutoHeight()
        [
            SNew( SHorizontalBox )
            +SHorizontalBox::Slot()
            .Padding( 1, 2, 4, 2 )
            [
                SNew( SButton )
                .VAlign( VAlign_Center )
                .HAlign( HAlign_Center )
                .Text( LOCTEXT( "LandscapeInputRecommit", "Recommit Landscape" ) )
                .ToolTipText( LOCTEXT( "LandscapeInputRecommit", "Recommit Landscape" ) )
                .OnClicked( FOnClicked::CreateUObject( this, &UHoudiniAssetInput::OnButtonClickRecommit ) )
            ]
        ];
    }
    else if ( ChoiceIndex == EHoudiniAssetInputType::WorldInput )
    {
        // Button : Start Selection / Use current selection + refresh
        {
            TSharedPtr< SHorizontalBox > HorizontalBox = NULL;
            FPropertyEditorModule & PropertyModule =
                FModuleManager::Get().GetModuleChecked< FPropertyEditorModule >("PropertyEditor");

            // Locate the details panel.
            FName DetailsPanelName = "LevelEditorSelectionDetails";
            TSharedPtr< IDetailsView > DetailsView = PropertyModule.FindDetailView(DetailsPanelName);

            auto ButtonLabel = LOCTEXT("WorldInputStartSelection", "Start Selection (Lock Details Panel)");
            if (DetailsView->IsLocked())
                ButtonLabel = LOCTEXT("WorldInputUseCurrentSelection", "Use Current Selection (Unlock Details Panel)");

            VerticalBox->AddSlot().Padding(2, 2, 5, 2).AutoHeight()
            [
                SAssignNew(HorizontalBox, SHorizontalBox)
                + SHorizontalBox::Slot()
                [
                    SNew(SButton)
                    .VAlign(VAlign_Center)
                    .HAlign(HAlign_Center)
                    .Text(ButtonLabel)
                    .OnClicked(FOnClicked::CreateUObject(this, &UHoudiniAssetInput::OnButtonClickSelectActors))		    
                ]
            ];
        }

        // ActorPicker : World Outliner
        {
            FMenuBuilder MenuBuilder = CreateCustomActorPickerWidget( LOCTEXT( "WorldInputSelectedActors", "Currently Selected Actors"), false );
            
            VerticalBox->AddSlot().Padding(2, 2, 5, 2).AutoHeight()
            [
                MenuBuilder.MakeWidget()
            ];
        }

        {
            // Spline Resolution
            TSharedPtr< SNumericEntryBox< float > > NumericEntryBox;
            int32 Idx = 0;

            VerticalBox->AddSlot().Padding(2, 2, 5, 2).AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("SplineRes", "Unreal Spline Resolution"))
                    .ToolTipText(LOCTEXT("SplineResTooltip", "Resolution used when marshalling the Unreal Splines to HoudiniEngine.\n(step in cm betweem control points)\nSet this to 0 to only export the control points."))
                    .Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")))
                ]
                + SHorizontalBox::Slot()
                .Padding(2.0f, 0.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(SNumericEntryBox< float >)
                    .AllowSpin(true)

                    .Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")))

                    .MinValue(-1.0f)
                    .MaxValue(1000.0f)
                    .MinSliderValue(0.0f)
                    .MaxSliderValue(1000.0f)

                    .Value(TAttribute< TOptional< float > >::Create(TAttribute< TOptional< float > >::FGetter::CreateUObject(
                        this, &UHoudiniAssetInput::GetSplineResolutionValue) ) )
                    .OnValueChanged(SNumericEntryBox< float >::FOnValueChanged::CreateUObject(
                        this, &UHoudiniAssetInput::SetSplineResolutionValue) )
                    .IsEnabled(TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateUObject(
                        this, &UHoudiniAssetInput::IsSplineResolutionEnabled)))

                    .SliderExponent(1.0f)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(2.0f, 0.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(SButton)
                    .ToolTipText(LOCTEXT("SplineResToDefault", "Reset to default value."))
                    .ButtonStyle(FEditorStyle::Get(), "NoBorder")
                    .ContentPadding(0)
                    .Visibility(EVisibility::Visible)
                    .OnClicked(FOnClicked::CreateUObject(this, &UHoudiniAssetInput::OnResetSplineResolutionClicked))
                    [
                        SNew(SImage)
                        .Image(FEditorStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
                    ]
                ]
            ];
        }
    }

    Row.ValueWidget.Widget = VerticalBox;
    Row.ValueWidget.MinDesiredWidth( HAPI_UNREAL_DESIRED_ROW_VALUE_WIDGET_WIDTH );
}


void 
UHoudiniAssetInput::CreateGeometryWidget( int32 AtIndex, UObject* InputObject, TSharedPtr<FAssetThumbnailPool> AssetThumbnailPool, TSharedRef<SVerticalBox> VerticalBox )
{
    // Create thumbnail for this static mesh.
    TSharedPtr< FAssetThumbnail > StaticMeshThumbnail = MakeShareable(
        new FAssetThumbnail( InputObject, 64, 64, AssetThumbnailPool ) );

    TSharedPtr< SHorizontalBox > HorizontalBox = NULL;
    // Drop Target: Static Mesh
    VerticalBox->AddSlot().Padding( 0, 2 ).AutoHeight()
        [
            SNew( SAssetDropTarget )
            .OnIsAssetAcceptableForDrop( SAssetDropTarget::FIsAssetAcceptableForDrop::CreateLambda(
                []( const UObject* InObject ) {
                    return InObject && InObject->IsA< UStaticMesh >();
            } ) )
            .OnAssetDropped( SAssetDropTarget::FOnAssetDropped::CreateUObject(
                this, &UHoudiniAssetInput::OnStaticMeshDropped, AtIndex ) )
            [
                SAssignNew( HorizontalBox, SHorizontalBox )
            ]
        ];

    // Thumbnail : Static Mesh
    FText ParameterLabelText = FText::FromString( GetParameterLabel() );
    TSharedPtr< SBorder > StaticMeshThumbnailBorder;

    HorizontalBox->AddSlot().Padding( 0.0f, 0.0f, 2.0f, 0.0f ).AutoWidth()
    [
        SAssignNew( StaticMeshThumbnailBorder, SBorder )
        .Padding( 5.0f )
        .OnMouseDoubleClick( FPointerEventHandler::CreateUObject( this, &UHoudiniAssetInput::OnThumbnailDoubleClick, AtIndex ) )
        [
            SNew( SBox )
            .WidthOverride( 64 )
            .HeightOverride( 64 )
            .ToolTipText( ParameterLabelText )
            [
                StaticMeshThumbnail->MakeThumbnailWidget()
            ]
        ]
    ];

    StaticMeshThumbnailBorder->SetBorderImage( TAttribute< const FSlateBrush * >::Create(
        TAttribute< const FSlateBrush * >::FGetter::CreateLambda( [StaticMeshThumbnailBorder]() {
        if ( StaticMeshThumbnailBorder.IsValid() && StaticMeshThumbnailBorder->IsHovered() )
            return FEditorStyle::GetBrush( "PropertyEditor.AssetThumbnailLight" );
        else
            return FEditorStyle::GetBrush( "PropertyEditor.AssetThumbnailShadow" );
    } ) ) );

    FText MeshNameText = FText::GetEmpty();
    if ( InputObject )
        MeshNameText = FText::FromString( InputObject->GetName() );

    // ComboBox : Static Mesh
    TSharedPtr< SComboButton > StaticMeshComboButton;

    TSharedPtr< SHorizontalBox > ButtonBox;
    HorizontalBox->AddSlot()
        .FillWidth( 1.0f )
        .Padding( 0.0f, 4.0f, 4.0f, 4.0f )
        .VAlign( VAlign_Center )
        [
            SNew( SVerticalBox )
            + SVerticalBox::Slot()
            .HAlign( HAlign_Fill )
            [
                SAssignNew( ButtonBox, SHorizontalBox )
                + SHorizontalBox::Slot()
                [
                    SAssignNew( StaticMeshComboButton, SComboButton )
                    .ButtonStyle( FEditorStyle::Get(), "PropertyEditor.AssetComboStyle" )
                    .ForegroundColor( FEditorStyle::GetColor( "PropertyEditor.AssetName.ColorAndOpacity" ) )
                    .ContentPadding( 2.0f )
                    .ButtonContent()
                    [
                        SNew( STextBlock )
                        .TextStyle( FEditorStyle::Get(), "PropertyEditor.AssetClass" )
                        .Font( FEditorStyle::GetFontStyle( FName( TEXT( "PropertyWindow.NormalFont" ) ) ) )
                        .Text( MeshNameText )
                    ]
                ]
            ]
        ];

    StaticMeshComboButton->SetOnGetMenuContent( FOnGetContent::CreateLambda(
        [this, AtIndex, StaticMeshComboButton]() {
            TArray< const UClass * > AllowedClasses;
            AllowedClasses.Add( UStaticMesh::StaticClass() );

            TArray< UFactory * > NewAssetFactories;
            return PropertyCustomizationHelpers::MakeAssetPickerWithMenu(
                FAssetData( GetInputObject( AtIndex ) ),
                true,
                AllowedClasses,
                NewAssetFactories,
                OnShouldFilterStaticMesh,
                FOnAssetSelected::CreateLambda( [this, AtIndex, StaticMeshComboButton]( const FAssetData & AssetData ) {
                    if ( StaticMeshComboButton.IsValid() )
                    {
                        StaticMeshComboButton->SetIsOpen( false );

                        UObject * Object = AssetData.GetAsset();
                        OnStaticMeshDropped( Object, AtIndex );
                    }
                } ),
                FSimpleDelegate::CreateLambda( []() {} ) );
        } ) );

    // Create tooltip.
    FFormatNamedArguments Args;
    Args.Add( TEXT( "Asset" ), MeshNameText );
    FText StaticMeshTooltip = FText::Format(
        LOCTEXT( "BrowseToSpecificAssetInContentBrowser",
            "Browse to '{Asset}' in Content Browser" ), Args );

    // Button : Browse Static Mesh
    ButtonBox->AddSlot()
        .AutoWidth()
        .Padding( 2.0f, 0.0f )
        .VAlign( VAlign_Center )
        [
            PropertyCustomizationHelpers::MakeBrowseButton(
                FSimpleDelegate::CreateUObject( this, &UHoudiniAssetInput::OnStaticMeshBrowse, AtIndex ),
                TAttribute< FText >( StaticMeshTooltip ) )
        ];

    // ButtonBox : Reset
    ButtonBox->AddSlot()
        .AutoWidth()
        .Padding( 2.0f, 0.0f )
        .VAlign( VAlign_Center )
        [
            SNew( SButton )
            .ToolTipText( LOCTEXT( "ResetToBase", "Reset to default static mesh" ) )
            .ButtonStyle( FEditorStyle::Get(), "NoBorder" )
            .ContentPadding( 0 )
            .Visibility( EVisibility::Visible )
            .OnClicked( FOnClicked::CreateUObject( this, &UHoudiniAssetInput::OnResetStaticMeshClicked, AtIndex ) )
            [
                SNew( SImage )
                .Image( FEditorStyle::GetBrush( "PropertyWindow.DiffersFromDefault" ) )
            ]
        ];

    ButtonBox->AddSlot()
        .Padding( 1.0f )
        .VAlign( VAlign_Center )
        .AutoWidth()
        [
            PropertyCustomizationHelpers::MakeInsertDeleteDuplicateButton(
            FExecuteAction::CreateLambda( [this, AtIndex]() {
                FScopedTransaction Transaction(
                    TEXT( HOUDINI_MODULE_RUNTIME ),
                    LOCTEXT( "HoudiniInputChange", "Houdini Input Geometry Change" ),
                    HoudiniAssetComponent );
                Modify();
                MarkPreChanged();
                InputObjects.Insert( nullptr, AtIndex );
                bStaticMeshChanged = true;
                MarkChanged();
                HoudiniAssetComponent->UpdateEditorProperties( false );
                } 
            ),
            FExecuteAction::CreateLambda( [this, AtIndex]() {
                    if ( ensure(InputObjects.IsValidIndex( AtIndex ) ) )
                    {
                        FScopedTransaction Transaction(
                            TEXT( HOUDINI_MODULE_RUNTIME ),
                            LOCTEXT( "HoudiniInputChange", "Houdini Input Geometry Change" ),
                            HoudiniAssetComponent );
                        Modify();
                        MarkPreChanged();
                        InputObjects.RemoveAt( AtIndex );
                        bStaticMeshChanged = true;
                        MarkChanged();
                        HoudiniAssetComponent->UpdateEditorProperties( false );
                    }
                } 
            ),
            FExecuteAction::CreateLambda( [this, AtIndex]() {
                    if ( ensure( InputObjects.IsValidIndex( AtIndex ) ) )
                    {
                        FScopedTransaction Transaction(
                            TEXT( HOUDINI_MODULE_RUNTIME ),
                            LOCTEXT( "HoudiniInputChange", "Houdini Input Geometry Change" ),
                            HoudiniAssetComponent );
                        Modify();
                        MarkPreChanged();
                        UObject* Dupe = InputObjects[ AtIndex ];
                        InputObjects.Insert( Dupe , AtIndex );
                        bStaticMeshChanged = true;
                        MarkChanged();
                        HoudiniAssetComponent->UpdateEditorProperties( false );
                    }
                } 
            ) )
        ];
}


void
UHoudiniAssetInput::PostEditUndo()
{
    Super::PostEditUndo();

    if ( InputCurve && ChoiceIndex == EHoudiniAssetInputType::CurveInput )
    {
        auto * owner = HoudiniAssetComponent->GetOwner();
        owner->AddOwnedComponent( InputCurve );

        InputCurve->AttachToComponent( HoudiniAssetComponent, FAttachmentTransformRules::KeepRelativeTransform );
        InputCurve->RegisterComponent();
        InputCurve->SetVisibility( true );
    }
}

#endif


bool
UHoudiniAssetInput::UploadParameterValue()
{
    bool Success = true;

    if(HoudiniAssetComponent == nullptr)
        return false;

    HAPI_AssetId HostAssetId = HoudiniAssetComponent->GetAssetId();

    switch ( ChoiceIndex )
    {
        case EHoudiniAssetInputType::GeometryInput:
        {
            if ( ! InputObjects.Num() )
            {
                // Either mesh was reset or null mesh has been assigned.
                DisconnectAndDestroyInputAsset();
            }
            else
            {
                if ( bStaticMeshChanged || bLoadedParameter )
                {
                    // Disconnect and destroy currently connected asset, if there's one.
                    DisconnectAndDestroyInputAsset();

                    // Connect input and create connected asset. Will return by reference.
                    if ( !FHoudiniEngineUtils::HapiCreateAndConnectAsset( HostAssetId, InputIndex, InputObjects, ConnectedAssetId, GeometryInputAssetIds ) )
                    {
                        bChanged = false;
                        ConnectedAssetId = -1;
                        return false;
                    }

                    bStaticMeshChanged = false;
                }
                
                Success &= UpdateObjectMergeTransformType();
            }

            break;
        }
    
        case EHoudiniAssetInputType::AssetInput:
        {
            // Process connected asset.
            if ( InputAssetComponent && FHoudiniEngineUtils::IsValidAssetId( InputAssetComponent->GetAssetId() )
                && !bInputAssetConnectedInHoudini )
            {
                ConnectInputAssetActor();
                Success &= UpdateObjectMergeTransformType();
            }
            else if ( bInputAssetConnectedInHoudini && !InputAssetComponent )
            {
                DisconnectInputAssetActor();
            }
            else
            {
                bChanged = false;
                return false;
            }

            break;
        }

        case EHoudiniAssetInputType::CurveInput:
        {
            bool bCreated = false;
            // If we have no curve asset, create it.
            if ( !FHoudiniEngineUtils::IsValidAssetId( ConnectedAssetId ) )
            {
                if ( !FHoudiniEngineUtils::HapiCreateCurve( ConnectedAssetId ) )
                {
                    bChanged = false;
                    ConnectedAssetId = -1;
                    return false;
                }          

                // Connect asset.
                if ( !FHoudiniEngineUtils::HapiConnectAsset(ConnectedAssetId, 0, HostAssetId, InputIndex) )
                {
                    bChanged = false;
                    ConnectedAssetId = -1;
                    return false;
                }

                bCreated = true;
            }

            if ( bLoadedParameter || bCreated)
            {
                HAPI_AssetInfo CurveAssetInfo;
                FHoudiniApi::GetAssetInfo( FHoudiniEngine::Get().GetSession(), ConnectedAssetId, &CurveAssetInfo ); 

                // If we just loaded or created our curve, we need to set parameters.
                for ( TMap< FString, UHoudiniAssetParameter * >::TIterator
                    IterParams( InputCurveParameters ); IterParams; ++IterParams )
                {
                    UHoudiniAssetParameter * Parameter = IterParams.Value();
                    if ( Parameter )
                    {
                        // We need to update node id for loaded parameters.
                        Parameter->SetNodeId( CurveAssetInfo.nodeId );
                        // Upload parameter value.
                        Success &= Parameter->UploadParameterValue();
                    }
                }
            }
            

            HAPI_NodeId LocalNodeId = -1;
            if ( FHoudiniEngineUtils::HapiGetNodeId( ConnectedAssetId, 0, 0, LocalNodeId ) && InputCurve )            
            {
                // The curve node has now been created and set up, we can upload points and rotation/scale attributes
                const TArray< FTransform > & CurvePoints = InputCurve->GetCurvePoints();
                TArray<FVector> Positions;
                InputCurve->GetCurvePositions(Positions);

                TArray<FQuat> Rotations;
                InputCurve->GetCurveRotations(Rotations);

                TArray<FVector> Scales;
                InputCurve->GetCurveScales(Scales);

                if(!FHoudiniEngineUtils::HapiCreateCurveAsset(
                        HostAssetId,
                        ConnectedAssetId,
                        &Positions,
                        &Rotations,
                        &Scales,
                        nullptr))
                {
                    bChanged = false;
                    ConnectedAssetId = -1;
                    return false;
                }

                if (!FHoudiniEngineUtils::HapiConnectAsset(ConnectedAssetId, 0, HostAssetId, InputIndex))
                {
                    bChanged = false;
                    ConnectedAssetId = -1;
                    return false;
                }
            }
            
            if (bCreated && InputCurve)
            {
                // We need to check that the SplineComponent has no offset.
                // if the input was set to WorldOutliner before, it might have one
                FTransform CurveTransform = InputCurve->GetRelativeTransform();	
                if (!CurveTransform.GetLocation().IsZero())
                    InputCurve->SetRelativeLocation(FVector::ZeroVector);
            }

            Success &= UpdateObjectMergeTransformType();

            // Cook the spline asset.
            if( HAPI_RESULT_SUCCESS != FHoudiniApi::CookAsset(FHoudiniEngine::Get().GetSession(), ConnectedAssetId, nullptr) )
            {
                Success = false;
            }

            // We need to update the curve.
            Success &= UpdateInputCurve();

            bSwitchedToCurve = false;

            break;
        }

        case EHoudiniAssetInputType::LandscapeInput:
        {
            if ( InputLandscapeProxy )
            {
                // Disconnect and destroy currently connected asset, if there's one.
                DisconnectAndDestroyInputAsset();

                // Connect input and create connected asset. Will return by reference.
                if ( !FHoudiniEngineUtils::HapiCreateAndConnectAsset(
                    HostAssetId, InputIndex, InputLandscapeProxy,
                    ConnectedAssetId, bLandscapeInputSelectionOnly, bLandscapeExportCurves,
                    bLandscapeExportMaterials, bLandscapeExportFullGeometry, bLandscapeExportLighting,
                    bLandscapeExportNormalizedUVs, bLandscapeExportTileUVs ) )
                {
                    bChanged = false;
                    ConnectedAssetId = -1;
                    return false;
                }

                Success &= UpdateObjectMergeTransformType();
            }
            else
            {
                // Either landscape was reset or null landscape has been assigned.
                DisconnectAndDestroyInputAsset();
            }

            break;
        }

        case EHoudiniAssetInputType::WorldInput:
        {
            if ( InputOutlinerMeshArray.Num() > 0 )
            {
                if ( bStaticMeshChanged || bLoadedParameter )
                {
                    // Disconnect and destroy currently connected asset, if there's one.
                    DisconnectAndDestroyInputAsset();

                    // Connect input and create connected asset. Will return by reference.
                    if ( !FHoudiniEngineUtils::HapiCreateAndConnectAsset(
                            HostAssetId, InputIndex, InputOutlinerMeshArray, ConnectedAssetId, UnrealSplineResolution ) )
                    {
                        bChanged = false;
                        ConnectedAssetId = -1;
                        return false;
                    }

                    bStaticMeshChanged = false;
                }

                Success &= UpdateObjectMergeTransformType();

            }
            else
            {
                // Either mesh was reset or null mesh has been assigned.
                DisconnectAndDestroyInputAsset();
            }

            break;
        }

        default:
        {
            check( 0 );
        }
    }
    
    bLoadedParameter = false;
    return Success & Super::UploadParameterValue();
}

uint32
UHoudiniAssetInput::GetDefaultTranformTypeValue() const
{
    switch (ChoiceIndex)
    {
	// NONE
	case EHoudiniAssetInputType::CurveInput:
	case EHoudiniAssetInputType::GeometryInput:	
	    return 0;	
	
	// INTO THIS OBJECT
	case EHoudiniAssetInputType::AssetInput:
	case EHoudiniAssetInputType::LandscapeInput:
	case EHoudiniAssetInputType::WorldInput:
	    return 1;
    }

    return 0;
}

UObject*
UHoudiniAssetInput::GetInputObject( int32 AtIndex ) const
{
    return InputObjects.IsValidIndex( AtIndex ) ? InputObjects[ AtIndex ] : nullptr;
}

bool
UHoudiniAssetInput::UpdateObjectMergeTransformType()
{
    if (HoudiniAssetComponent == nullptr)
        return false;

    uint32 nTransformType = -1;
    if (bKeepWorldTransform == 2)
        nTransformType = GetDefaultTranformTypeValue();
    else if (bKeepWorldTransform)
        nTransformType = 1; 
    else
        nTransformType = 0;

    // We need the host asset info to get the host node id
    HAPI_AssetInfo HostAssetInfo;
    HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetAssetInfo(
        FHoudiniEngine::Get().GetSession(),
        HoudiniAssetComponent->GetAssetId(),
        &HostAssetInfo), false);

    // Get the Input node ID from the host ID
    HAPI_NodeId InputNodeId = -1;
    HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::QueryNodeInput(
        FHoudiniEngine::Get().GetSession(), HostAssetInfo.nodeId,
        InputIndex, &InputNodeId), false);

    // Change Parameter xformtype
    const std::string sXformType = "xformtype";
    HOUDINI_CHECK_ERROR_RETURN( FHoudiniApi::SetParmIntValue(
            FHoudiniEngine::Get().GetSession(), InputNodeId,
        sXformType.c_str(), 0, nTransformType), false);

    // We need the asset info to get the node id
    HAPI_AssetInfo AssetInfo;
    HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::GetAssetInfo(
        FHoudiniEngine::Get().GetSession(),
        ConnectedAssetId,
        &AssetInfo), false);

    // If the input is a world outliner, we also need to modify
    // the transform types of the merge node's inputs
    for (int n = 0; n < InputOutlinerMeshArray.Num(); n++)
    {
        // Get the Input node ID from the host ID
        InputNodeId = -1;
        HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::QueryNodeInput(
            FHoudiniEngine::Get().GetSession(), AssetInfo.nodeId,
            n, &InputNodeId), false);

        if (InputNodeId == -1)
            continue;

        // Change Parameter xformtype
        HOUDINI_CHECK_ERROR_RETURN(FHoudiniApi::SetParmIntValue(
            FHoudiniEngine::Get().GetSession(), InputNodeId,
            sXformType.c_str(), 0, nTransformType), false);
    }
    
    return true;
}


void
UHoudiniAssetInput::BeginDestroy()
{
    Super::BeginDestroy();

    // Destroy anything curve related.
    DestroyInputCurve();

    // Disconnect and destroy the asset we may have connected.
    DisconnectAndDestroyInputAsset();
}

void
UHoudiniAssetInput::PostLoad()
{
    Super::PostLoad();

    // Generate widget related resources.
    CreateWidgetResources();

    // Patch input curve parameter links.
    for ( TMap< FString, UHoudiniAssetParameter * >::TIterator IterParams( InputCurveParameters ); IterParams; ++IterParams )
    {
        FString ParameterKey = IterParams.Key();
        UHoudiniAssetParameter * Parameter = IterParams.Value();

        if ( Parameter )
        {
            Parameter->SetHoudiniAssetComponent( nullptr );
            Parameter->SetParentParameter( this );
        }
    }
  
    if ( InputCurve )
    {
        if (ChoiceIndex == EHoudiniAssetInputType::CurveInput)
        {
            // Set input callback object for this curve.
            InputCurve->SetHoudiniAssetInput(this);
            InputCurve->AttachToComponent(HoudiniAssetComponent, FAttachmentTransformRules::KeepRelativeTransform);
        }
        else
        {
            // Manually destroying the "ghost" curve
            InputCurve->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
            InputCurve->UnregisterComponent();
            InputCurve->DestroyComponent();
            InputCurve = nullptr;
        }
    }

    if (InputOutlinerMeshArray.Num() > 0)
    {
        // The spline Transform array might need to be rebuilt after loading
        for (auto & OutlinerMesh : InputOutlinerMeshArray)
            OutlinerMesh.RebuildSplineTransformsArrayIfNeeded();
#if WITH_EDITOR
        StartWorldOutlinerTicking();
#endif
    }
	
}

void
UHoudiniAssetInput::Serialize( FArchive & Ar )
{
    // Call base implementation.
    Super::Serialize( Ar );

    Ar.UsingCustomVersion( FHoudiniCustomSerializationVersion::GUID );

    // Serialize current choice selection.
    SerializeEnumeration< EHoudiniAssetInputType::Enum >( Ar, ChoiceIndex );
    Ar << ChoiceStringValue;

    // We need these temporary variables for undo state tracking.
    bool bLocalInputAssetConnectedInHoudini = bInputAssetConnectedInHoudini;
    UHoudiniAssetComponent * LocalInputAssetComponent = InputAssetComponent;

    Ar << HoudiniAssetInputFlagsPacked;

    // Serialize input index.
    Ar << InputIndex;

    // Serialize input objects (if it's assigned).
    if ( HoudiniAssetParameterVersion >= VER_HOUDINI_PLUGIN_SERIALIZATION_VERSION_MULTI_GEO_INPUT )
    {
        Ar << InputObjects;
    }
    else
    {
        UObject* InputObject = nullptr;
        Ar << InputObject;
        InputObjects.Empty();
        InputObjects.Add( InputObject );
    }

    // Serialize input asset.
    Ar << InputAssetComponent;

    // Serialize curve and curve parameters (if we have those).
    Ar << InputCurve;
    Ar << InputCurveParameters;

    // Serialize landscape used for input.
    if ( HoudiniAssetParameterVersion >= VER_HOUDINI_ENGINE_PARAM_LANDSCAPE_INPUT )
        Ar << InputLandscapeProxy;

    // Serialize world outliner inputs.
    if ( HoudiniAssetParameterVersion >= VER_HOUDINI_ENGINE_PARAM_WORLD_OUTLINER_INPUT )
    {
        Ar << InputOutlinerMeshArray;
    }

    // Create necessary widget resources.
    if ( Ar.IsLoading() )
    {
        bLoadedParameter = true;

        if ( Ar.IsTransacting() )
        {
            bInputAssetConnectedInHoudini = bLocalInputAssetConnectedInHoudini;

            if ( LocalInputAssetComponent != InputAssetComponent )
            {
                if ( InputAssetComponent )
                    bInputAssetConnectedInHoudini = false;

                if ( LocalInputAssetComponent )
                    LocalInputAssetComponent->RemoveDownstreamAsset( HoudiniAssetComponent, InputIndex );
            }
        }
        else
        {
            // If we're loading for real for the first time we need to reset this
            // flag so we can reconnect when we get our parameters uploaded.
            bInputAssetConnectedInHoudini = false;
        }
    }

    if ( HoudiniAssetParameterVersion >= VER_HOUDINI_PLUGIN_SERIALIZATION_VERSION_UNREAL_SPLINE_RESOLUTION_PER_INPUT )
        Ar << UnrealSplineResolution;
}

void
UHoudiniAssetInput::AddReferencedObjects( UObject * InThis, FReferenceCollector & Collector )
{
    UHoudiniAssetInput * HoudiniAssetInput = Cast< UHoudiniAssetInput >( InThis );
    if ( HoudiniAssetInput )
    {
        // Add reference to held geometry object.
        if ( HoudiniAssetInput->InputObjects.Num() )
            Collector.AddReferencedObjects( HoudiniAssetInput->InputObjects, InThis );

        // Add reference to held input asset component, if we have one.
        if ( HoudiniAssetInput->InputAssetComponent )
            Collector.AddReferencedObject( HoudiniAssetInput->InputAssetComponent, InThis );

        // Add reference to held curve object.
        if ( HoudiniAssetInput->InputCurve )
            Collector.AddReferencedObject( HoudiniAssetInput->InputCurve, InThis );

        // Add reference to held landscape.
        if ( HoudiniAssetInput->InputLandscapeProxy )
            Collector.AddReferencedObject( HoudiniAssetInput->InputLandscapeProxy, InThis );

        // Add references for all curve input parameters.
        for ( TMap< FString, UHoudiniAssetParameter * >::TIterator IterParams( HoudiniAssetInput->InputCurveParameters );
            IterParams; ++IterParams )
        {
            UHoudiniAssetParameter * HoudiniAssetParameter = IterParams.Value();
            if ( HoudiniAssetParameter )
                Collector.AddReferencedObject( HoudiniAssetParameter, InThis );
        }
    }

    // Call base implementation.
    Super::AddReferencedObjects( InThis, Collector );
}

void
UHoudiniAssetInput::ClearInputCurveParameters()
{
    for( TMap< FString, UHoudiniAssetParameter * >::TIterator IterParams( InputCurveParameters ); IterParams; ++IterParams )
    {
        UHoudiniAssetParameter * HoudiniAssetParameter = IterParams.Value();
        if ( HoudiniAssetParameter )
            HoudiniAssetParameter->ConditionalBeginDestroy();
    }

    InputCurveParameters.Empty();
}

void
UHoudiniAssetInput::DisconnectInputCurve()
{
    // If we have spline, delete it.
    if ( InputCurve )
    {
        InputCurve->DetachFromComponent( FDetachmentTransformRules::KeepRelativeTransform );
        InputCurve->UnregisterComponent();
    }
}

void
UHoudiniAssetInput::DestroyInputCurve()
{
    // If we have spline, delete it.
    if ( InputCurve )
    {
        InputCurve->DetachFromComponent( FDetachmentTransformRules::KeepRelativeTransform );
        InputCurve->UnregisterComponent();
        InputCurve->DestroyComponent();

        InputCurve = nullptr;
    }

    ClearInputCurveParameters();
}

#if WITH_EDITOR

void
UHoudiniAssetInput::OnStaticMeshDropped( UObject * InObject, int32 AtIndex )
{
    UObject* InputObject = GetInputObject( AtIndex );
    if ( InObject != InputObject )
    {
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Input Geometry Change" ),
            HoudiniAssetComponent );
        Modify();

        MarkPreChanged();
        if ( InputObjects.IsValidIndex( AtIndex ) )
        {
            InputObjects[ AtIndex ] = InObject;
        }
        else
        {
            check( AtIndex == 0 );
            InputObjects.Add( InObject );
        }
        bStaticMeshChanged = true;
        MarkChanged();

        HoudiniAssetComponent->UpdateEditorProperties( false );
    }
}

FReply
UHoudiniAssetInput::OnThumbnailDoubleClick( const FGeometry & InMyGeometry, const FPointerEvent & InMouseEvent, int32 AtIndex )
{
    UObject* InputObject = GetInputObject( AtIndex );
    if ( InputObject && InputObject->IsA( UStaticMesh::StaticClass() ) && GEditor )
        GEditor->EditObject( InputObject );

    return FReply::Handled();
}

TSharedRef< SWidget >
UHoudiniAssetInput::CreateChoiceEntryWidget( TSharedPtr< FString > ChoiceEntry )
{
    FText ChoiceEntryText = FText::FromString( *ChoiceEntry );

    return SNew( STextBlock )
        .Text( ChoiceEntryText )
        .ToolTipText( ChoiceEntryText )
        .Font( FEditorStyle::GetFontStyle( TEXT( "PropertyWindow.NormalFont" ) ) );
}

void 
UHoudiniAssetInput::OnStaticMeshBrowse( int32 AtIndex )
{
    UObject* InputObject = GetInputObject( AtIndex );
    if ( GEditor && InputObject )
    {
        TArray< UObject * > Objects;
        Objects.Add( InputObject );
        GEditor->SyncBrowserToObjects( Objects );
    }
}

FReply
UHoudiniAssetInput::OnResetStaticMeshClicked( int32 AtIndex )
{
    OnStaticMeshDropped( nullptr, AtIndex );
    return FReply::Handled();
}


void
UHoudiniAssetInput::OnChoiceChange( TSharedPtr< FString > NewChoice, ESelectInfo::Type SelectType )
{
    if ( !NewChoice.IsValid() )
        return;

    ChoiceStringValue = *( NewChoice.Get() );

    // We need to match selection based on label.
    bool bLocalChanged = false;
    int32 ActiveLabel = 0;

    for ( int32 LabelIdx = 0; LabelIdx < StringChoiceLabels.Num(); ++LabelIdx )
    {
        FString * ChoiceLabel = StringChoiceLabels[ LabelIdx ].Get();

        if ( ChoiceLabel && ChoiceLabel->Equals( ChoiceStringValue ) )
        {
            bLocalChanged = true;
            ActiveLabel = LabelIdx;
            break;
        }
    }

    if ( !bLocalChanged )
        return;

    FScopedTransaction Transaction(
        TEXT( HOUDINI_MODULE_RUNTIME ),
        LOCTEXT( "HoudiniInputChange", "Houdini Input Type Change" ),
        HoudiniAssetComponent );
    Modify();

    // Switch mode.
    EHoudiniAssetInputType::Enum newChoice = static_cast< EHoudiniAssetInputType::Enum >( ActiveLabel );
    ChangeInputType(newChoice);
}

bool
UHoudiniAssetInput::ChangeInputType(const EHoudiniAssetInputType::Enum& newType)
{
    switch (ChoiceIndex)
    {
        case EHoudiniAssetInputType::GeometryInput:
        {
            // We are switching away from geometry input.
            break;
        }

        case EHoudiniAssetInputType::AssetInput:
        {
            // We are switching away from asset input.
            DisconnectInputAssetActor();
            break;
        }

        case EHoudiniAssetInputType::CurveInput:
        {
            // We are switching away from curve input.
            DisconnectInputCurve();
            break;
        }

        case EHoudiniAssetInputType::LandscapeInput:
        {
            // We are switching away from landscape input.

            // Reset selected landscape.
            InputLandscapeProxy = nullptr;
            break;
        }

        case EHoudiniAssetInputType::WorldInput:
        {
            // We are switching away from World Outliner input.

            // Stop monitoring the Actors for transform changes.
            StopWorldOutlinerTicking();

            break;
        }

        default:
        {
            // Unhandled new input type?
            check( 0 );
            break;
        }
    }    

    // Disconnect currently connected asset.
    DisconnectAndDestroyInputAsset();

    // Make sure we'll fully update the editor properties
    if ( ChoiceIndex != newType && InputAssetComponent )
        InputAssetComponent->bEditorPropertiesNeedFullUpdate = true;

    // Switch mode.
    ChoiceIndex = newType;

    switch ( newType )
    {
        case EHoudiniAssetInputType::GeometryInput:
        {
            // We are switching to geometry input.
            if ( InputObjects.Num() )
                bStaticMeshChanged = true;
            break;
        }

        case EHoudiniAssetInputType::AssetInput:
        {
            // We are switching to asset input.
            ConnectInputAssetActor();
            break;
        }

        case EHoudiniAssetInputType::CurveInput:
        {
            // We are switching to curve input.

            // Create new spline component if necessary.
            if ( !InputCurve )
                InputCurve = NewObject< UHoudiniSplineComponent >(
                    HoudiniAssetComponent->GetOwner(), UHoudiniSplineComponent::StaticClass(),
                    NAME_None, RF_Public | RF_Transactional );

            // Attach or re-attach curve component to asset.
            InputCurve->AttachToComponent( HoudiniAssetComponent, FAttachmentTransformRules::KeepRelativeTransform );
            InputCurve->RegisterComponent();
            InputCurve->SetVisibility( true );
            InputCurve->SetHoudiniAssetInput( this );

            bSwitchedToCurve = true;
            break;
        }

        case EHoudiniAssetInputType::LandscapeInput:
        {
            // We are switching to Landscape input.
            break;
        }

        case EHoudiniAssetInputType::WorldInput:
        {
            // We are switching to World Outliner input.

            // Start monitoring the Actors for transform changes.
            StartWorldOutlinerTicking();

            // Force recook and reconnect of the input assets.
            HAPI_AssetId HostAssetId = HoudiniAssetComponent->GetAssetId();
            FHoudiniEngineUtils::HapiCreateAndConnectAsset(
                HostAssetId, InputIndex, InputOutlinerMeshArray, ConnectedAssetId, UnrealSplineResolution );

            break;
        }

        default:
        {
            // Unhandled new input type?
            check( 0 );
            break;
        }
    }

    // If we have input object and geometry asset, we need to connect it back.
    MarkPreChanged();
    MarkChanged();

    return true;
}

bool
UHoudiniAssetInput::OnShouldFilterActor( const AActor * const Actor ) const
{
    if ( !Actor )
        return false;

    if ( ChoiceIndex == EHoudiniAssetInputType::AssetInput )
    {
        // Only return HoudiniAssetActors
        if ( Actor->IsA(AHoudiniAssetActor::StaticClass() ) )
        {
            // But not our own Asset Actor
            if ( HoudiniAssetComponent && ( HoudiniAssetComponent->GetHoudiniAssetActorOwner() != Actor ) )
                return true;
            else
                return false;
        }
    }
    else if ( ChoiceIndex == EHoudiniAssetInputType::LandscapeInput )
    {
        return Actor->IsA( ALandscapeProxy::StaticClass() );
    }        
    else if ( ChoiceIndex == EHoudiniAssetInputType::WorldInput )
    {
        for ( auto & OutlinerMesh : InputOutlinerMeshArray )
            if ( OutlinerMesh.Actor == Actor )
                return true;
    }

    return false;
}

void
UHoudiniAssetInput::OnActorSelected( AActor * Actor )
{
    if ( ChoiceIndex == EHoudiniAssetInputType::AssetInput )
        return OnInputActorSelected( Actor );
    else if ( ChoiceIndex == EHoudiniAssetInputType::WorldInput )
        return OnWorldOutlinerActorSelected( Actor );
    else if ( ChoiceIndex == EHoudiniAssetInputType::LandscapeInput )
        return OnLandscapeActorSelected( Actor );
}

void
UHoudiniAssetInput::OnInputActorSelected( AActor * Actor )
{
    if ( !Actor && InputAssetComponent )
    {
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Input Asset Change" ),
            HoudiniAssetComponent );
        Modify();

        // Tell the old input asset we are no longer connected.
        InputAssetComponent->RemoveDownstreamAsset( HoudiniAssetComponent, InputIndex );

        // We cleared the selection so just reset all the values.
        InputAssetComponent = nullptr;
        ConnectedAssetId = -1;
    }
    else
    {
        AHoudiniAssetActor * HoudiniAssetActor = (AHoudiniAssetActor *) Actor;
        if (HoudiniAssetActor == nullptr)
            return;

        UHoudiniAssetComponent * ConnectedHoudiniAssetComponent = HoudiniAssetActor->GetHoudiniAssetComponent();

        // If we just selected the already selected Actor do nothing.
        if ( ConnectedHoudiniAssetComponent == InputAssetComponent )
            return;

        // Do not allow the input asset to be ourself!
        if ( ConnectedHoudiniAssetComponent == HoudiniAssetComponent )
            return;

        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Input Asset Change" ),
            HoudiniAssetComponent );
        Modify();

        // Tell the old input asset we are no longer connected.
        if ( InputAssetComponent)
            InputAssetComponent->RemoveDownstreamAsset( HoudiniAssetComponent, InputIndex );

        InputAssetComponent = ConnectedHoudiniAssetComponent;
        ConnectedAssetId = InputAssetComponent->GetAssetId();

        // Mark as disconnected since we need to reconnect to the new asset.
        bInputAssetConnectedInHoudini = false;
    }

    MarkPreChanged();
    MarkChanged();
}

void
UHoudiniAssetInput::OnLandscapeActorSelected( AActor * Actor )
{
    ALandscapeProxy * LandscapeProxy = Cast< ALandscapeProxy >( Actor );
    if ( LandscapeProxy )
    {
        // If we just selected the already selected landscape, do nothing.
        if ( LandscapeProxy == InputLandscapeProxy )
            return;

        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Input Landscape Change." ),
            HoudiniAssetComponent );
        Modify();

        // Store new landscape.
        InputLandscapeProxy = LandscapeProxy;
    }
    else
    {
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Input Landscape Change." ),
            HoudiniAssetComponent );
        Modify();

        InputLandscapeProxy = nullptr;
    }

    MarkPreChanged();
    MarkChanged();
}

void
UHoudiniAssetInput::OnWorldOutlinerActorSelected( AActor * )
{
    // Do nothing.
}

void
UHoudiniAssetInput::TickWorldOutlinerInputs()
{
    bool bLocalChanged = false;
    TArray< UStaticMeshComponent * > InputOutlinerMeshArrayPendingKill;
    for ( auto & OutlinerMesh : InputOutlinerMeshArray )
    {
        if ( OutlinerMesh.Actor->IsPendingKill() )
        {
            if ( !bLocalChanged )
            {
                Modify();
                MarkPreChanged();
                bLocalChanged = true;
            }

            // Destroy Houdini asset.
            if ( FHoudiniEngineUtils::IsValidAssetId( OutlinerMesh.AssetId ) )
            {
                FHoudiniEngineUtils::DestroyHoudiniAsset( OutlinerMesh.AssetId );
                OutlinerMesh.AssetId = -1;
            }

            // Mark mesh for deletion.
            InputOutlinerMeshArrayPendingKill.Add( OutlinerMesh.StaticMeshComponent );
        }
        else if ( OutlinerMesh.HasActorTransformChanged() && (OutlinerMesh.AssetId >= 0))
        {
            if (!bLocalChanged)
            {
                Modify();
                MarkPreChanged();
                bLocalChanged = true;
            }

            // Updates to the new Transform
            UpdateWorldOutlinerTransforms(OutlinerMesh);

	    // Apply it to the asset
            HAPI_TransformEuler HapiTransform;
            FHoudiniEngineUtils::TranslateUnrealTransform(OutlinerMesh.ComponentTransform, HapiTransform);

	    FHoudiniApi::SetAssetTransform(
                FHoudiniEngine::Get().GetSession(),
		OutlinerMesh.AssetId, &HapiTransform);
        }
        else if ( OutlinerMesh.HasComponentTransformChanged() 
                || OutlinerMesh.HasSplineComponentChanged(UnrealSplineResolution)
                || (OutlinerMesh.KeepWorldTransform != bKeepWorldTransform) )
        {
            if ( !bLocalChanged )
            {
                Modify();
                MarkPreChanged();
                bLocalChanged = true;
            }

            // Update to the new Transforms
            UpdateWorldOutlinerTransforms(OutlinerMesh);

            // The component or spline has been modified so so we need to indicate that the "static mesh" 
            // has changed in order to rebuild the asset properly in UploadParameterValue()
            bStaticMeshChanged = true;
        }
    }

    if ( bLocalChanged )
    {
        // Delete all tracked meshes slated for deletion above.
        while ( InputOutlinerMeshArrayPendingKill.Num() > 0 )
        {
            auto OutlinerMeshToKill = InputOutlinerMeshArrayPendingKill.Pop( false );

            InputOutlinerMeshArray.RemoveAll( [ & ]( FHoudiniAssetInputOutlinerMesh & Element )
            {
                return Element.StaticMeshComponent == OutlinerMeshToKill;
            } );
        }

	MarkChanged();
    }
}

#endif

void
UHoudiniAssetInput::ConnectInputAssetActor()
{
    if ( InputAssetComponent && FHoudiniEngineUtils::IsValidAssetId( InputAssetComponent->GetAssetId() )
        && !bInputAssetConnectedInHoudini )
    {
        FHoudiniEngineUtils::HapiConnectAsset(
            InputAssetComponent->GetAssetId(),
            0, // We just pick the first OBJ since we have no way letting the user pick.
            HoudiniAssetComponent->GetAssetId(),
            InputIndex );

        ConnectedAssetId = InputAssetComponent->GetAssetId();

        InputAssetComponent->AddDownstreamAsset( HoudiniAssetComponent, InputIndex );
        bInputAssetConnectedInHoudini = true;
    }
}

void
UHoudiniAssetInput::DisconnectInputAssetActor()
{
    if ( bInputAssetConnectedInHoudini && !InputAssetComponent )
    {
        FHoudiniEngineUtils::HapiDisconnectAsset( HoudiniAssetComponent->GetAssetId(), InputIndex );
        bInputAssetConnectedInHoudini = false;
    }
}

void
UHoudiniAssetInput::ConnectLandscapeActor()
{}

void
UHoudiniAssetInput::DisconnectLandscapeActor()
{}

HAPI_AssetId
UHoudiniAssetInput::GetConnectedAssetId() const
{
    return ConnectedAssetId;
}

bool
UHoudiniAssetInput::IsGeometryAssetConnected() const
{
    if ( FHoudiniEngineUtils::IsValidAssetId( ConnectedAssetId ) )
    {
        for ( auto InputObject : InputObjects )
        {
            if ( InputObject )
                return true;
        }
    }

    return false;
}

bool
UHoudiniAssetInput::IsInputAssetConnected() const
{
    if ( FHoudiniEngineUtils::IsValidAssetId( ConnectedAssetId ) && InputAssetComponent && bInputAssetConnectedInHoudini )
        return true;

    return false;
}

bool
UHoudiniAssetInput::IsCurveAssetConnected() const
{
    if (InputCurve && ( ChoiceIndex == EHoudiniAssetInputType::CurveInput ) )
        return true;

    return false;
}

bool
UHoudiniAssetInput::IsLandscapeAssetConnected() const
{
    if ( FHoudiniEngineUtils::IsValidAssetId( ConnectedAssetId ) )
    {
        if ( ChoiceIndex == EHoudiniAssetInputType::LandscapeInput )
            return true;
    }

    return false;
}

bool
UHoudiniAssetInput::IsWorldInputAssetConnected() const
{
    if ( FHoudiniEngineUtils::IsValidAssetId( ConnectedAssetId ) )
    {
        if ( ChoiceIndex == EHoudiniAssetInputType::WorldInput )
            return true;
    }

    return false;
}

void
UHoudiniAssetInput::OnInputCurveChanged()
{
    MarkPreChanged();
    MarkChanged();
}

void
UHoudiniAssetInput::ExternalDisconnectInputAssetActor()
{
    InputAssetComponent = nullptr;
    ConnectedAssetId = -1;

    MarkPreChanged();
    MarkChanged();
}

bool
UHoudiniAssetInput::DoesInputAssetNeedInstantiation()
{
    if ( ChoiceIndex != EHoudiniAssetInputType::AssetInput )
        return false;

    if ( InputAssetComponent == nullptr )
        return false;

    if ( !FHoudiniEngineUtils::IsValidAssetId(InputAssetComponent->GetAssetId() ) )
        return true;

    return false;
}

UHoudiniAssetComponent *
UHoudiniAssetInput::GetConnectedInputAssetComponent()
{
    return InputAssetComponent;
}

void
UHoudiniAssetInput::NotifyChildParameterChanged( UHoudiniAssetParameter * HoudiniAssetParameter )
{
    if ( HoudiniAssetParameter && ChoiceIndex == EHoudiniAssetInputType::CurveInput )
    {
        MarkPreChanged();
        
        if ( FHoudiniEngineUtils::IsValidAssetId( ConnectedAssetId ) )
        {
            // We need to upload changed param back to HAPI.
            if(! HoudiniAssetParameter->UploadParameterValue())
            {
                HOUDINI_LOG_ERROR(TEXT("%s UploadParameterValue failed"), InputAssetComponent ? *InputAssetComponent->GetOwner()->GetName() : TEXT("unknown"));
            }
        }

        MarkChanged();
    }
}

bool
UHoudiniAssetInput::UpdateInputCurve()
{
    bool Success = true;
    FString CurvePointsString;
    EHoudiniSplineComponentType::Enum CurveTypeValue = EHoudiniSplineComponentType::Bezier;
    EHoudiniSplineComponentMethod::Enum CurveMethodValue = EHoudiniSplineComponentMethod::CVs;
    int32 CurveClosed = 1;

    HAPI_NodeId LocalNodeId = -1;
    if ( FHoudiniEngineUtils::HapiGetNodeId( ConnectedAssetId, 0, 0, LocalNodeId ) )
    {
        FHoudiniEngineUtils::HapiGetParameterDataAsString(
            LocalNodeId, HAPI_UNREAL_PARAM_CURVE_COORDS, TEXT( "" ),
            CurvePointsString );
        FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
            LocalNodeId, HAPI_UNREAL_PARAM_CURVE_TYPE,
            (int32) EHoudiniSplineComponentType::Bezier, (int32 &) CurveTypeValue );
        FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
            LocalNodeId, HAPI_UNREAL_PARAM_CURVE_METHOD,
            (int32) EHoudiniSplineComponentMethod::CVs, (int32 &) CurveMethodValue );
        FHoudiniEngineUtils::HapiGetParameterDataAsInteger(
            LocalNodeId, HAPI_UNREAL_PARAM_CURVE_CLOSED, 1, CurveClosed );
    }

    // Construct geo part object.
    FHoudiniGeoPartObject HoudiniGeoPartObject( ConnectedAssetId, 0, 0, 0 );
    HoudiniGeoPartObject.bIsCurve = true;

    HAPI_AttributeInfo AttributeRefinedCurvePositions;
    TArray< float > RefinedCurvePositions;
    FHoudiniEngineUtils::HapiGetAttributeDataAsFloat(
        HoudiniGeoPartObject, HAPI_UNREAL_ATTRIB_POSITION,
        AttributeRefinedCurvePositions, RefinedCurvePositions );

    // Process coords string and extract positions.
    TArray< FVector > CurvePoints;
    FHoudiniEngineUtils::ExtractStringPositions( CurvePointsString, CurvePoints );

    TArray< FVector > CurveDisplayPoints;
    FHoudiniEngineUtils::ConvertScaleAndFlipVectorData( RefinedCurvePositions, CurveDisplayPoints );

    if (InputCurve != nullptr)
    {
        InputCurve->Construct(
            HoudiniGeoPartObject, CurveDisplayPoints, CurveTypeValue, CurveMethodValue,
            ( CurveClosed == 1 ) );
    }
    
    // We also need to construct curve parameters we care about.
    TMap< FString, UHoudiniAssetParameter * > NewInputCurveParameters;

    {
        HAPI_NodeInfo NodeInfo;
        HOUDINI_CHECK_ERROR_RETURN(
            FHoudiniApi::GetNodeInfo( FHoudiniEngine::Get().GetSession(), LocalNodeId, &NodeInfo ),
            false );

        TArray< HAPI_ParmInfo > ParmInfos;
        ParmInfos.SetNumUninitialized( NodeInfo.parmCount );
        HOUDINI_CHECK_ERROR_RETURN(
            FHoudiniApi::GetParameters(
                FHoudiniEngine::Get().GetSession(), LocalNodeId, &ParmInfos[ 0 ], 0, NodeInfo.parmCount ),
            false);

        // Retrieve integer values for this asset.
        TArray< int32 > ParmValueInts;
        ParmValueInts.SetNumZeroed( NodeInfo.parmIntValueCount );
        if ( NodeInfo.parmIntValueCount > 0 )
        {
            HOUDINI_CHECK_ERROR_RETURN(
                FHoudiniApi::GetParmIntValues(
                    FHoudiniEngine::Get().GetSession(), LocalNodeId, &ParmValueInts[ 0 ], 0, NodeInfo.parmIntValueCount ),
                false );
        }

        // Retrieve float values for this asset.
        TArray< float > ParmValueFloats;
        ParmValueFloats.SetNumZeroed( NodeInfo.parmFloatValueCount );
        if ( NodeInfo.parmFloatValueCount > 0 )
        {
            HOUDINI_CHECK_ERROR_RETURN(
                FHoudiniApi::GetParmFloatValues(
                    FHoudiniEngine::Get().GetSession(), LocalNodeId, &ParmValueFloats[ 0 ], 0, NodeInfo.parmFloatValueCount ),
                false );
        }

        // Retrieve string values for this asset.
        TArray< HAPI_StringHandle > ParmValueStrings;
        ParmValueStrings.SetNumZeroed( NodeInfo.parmStringValueCount );
        if ( NodeInfo.parmStringValueCount > 0 )
        {
            HOUDINI_CHECK_ERROR_RETURN(
                FHoudiniApi::GetParmStringValues(
                    FHoudiniEngine::Get().GetSession(), LocalNodeId, true, &ParmValueStrings[ 0 ], 0, NodeInfo.parmStringValueCount ),
                false );
        }

        // Create properties for parameters.
        for ( int32 ParamIdx = 0; ParamIdx < NodeInfo.parmCount; ++ParamIdx )
        {
            // Retrieve param info at this index.
            const HAPI_ParmInfo & ParmInfo = ParmInfos[ ParamIdx ];

            // If parameter is invisible, skip it.
            if ( ParmInfo.invisible )
                continue;

            FString CurrentParameterName;
            FHoudiniEngineString HoudiniEngineString( ParmInfo.nameSH );
            if ( !HoudiniEngineString.ToFString( CurrentParameterName ) )
            {
                // We had trouble retrieving name of this parameter, skip it.
                continue;
            }

            // See if it's one of parameters we are interested in.
            if ( !CurrentParameterName.Equals( TEXT( HAPI_UNREAL_PARAM_CURVE_METHOD ) ) &&
                !CurrentParameterName.Equals( TEXT( HAPI_UNREAL_PARAM_CURVE_TYPE ) ) &&
                !CurrentParameterName.Equals( TEXT( HAPI_UNREAL_PARAM_CURVE_CLOSED ) ) )
            {
                // Not parameter we are interested in.
                continue;
            }

            // See if this parameter has already been created.
            UHoudiniAssetParameter * const * FoundHoudiniAssetParameter = InputCurveParameters.Find( CurrentParameterName );
            UHoudiniAssetParameter * HoudiniAssetParameter = nullptr;

            // If parameter exists, we can reuse it.
            if ( FoundHoudiniAssetParameter )
            {
                HoudiniAssetParameter = *FoundHoudiniAssetParameter;

                // Remove parameter from current map.
                InputCurveParameters.Remove( CurrentParameterName );

                // Reinitialize parameter and add it to map.
                HoudiniAssetParameter->CreateParameter( nullptr, this, LocalNodeId, ParmInfo );
                NewInputCurveParameters.Add( CurrentParameterName, HoudiniAssetParameter );
                continue;
            }
            else
            {
                if ( ParmInfo.type == HAPI_PARMTYPE_INT )
                {
                    if ( !ParmInfo.choiceCount )
                        HoudiniAssetParameter = UHoudiniAssetParameterInt::Create( nullptr, this, LocalNodeId, ParmInfo );
                    else
                        HoudiniAssetParameter = UHoudiniAssetParameterChoice::Create( nullptr, this, LocalNodeId, ParmInfo );
                }
                else if ( ParmInfo.type == HAPI_PARMTYPE_TOGGLE )
                {
                    HoudiniAssetParameter = UHoudiniAssetParameterToggle::Create( nullptr, this, LocalNodeId, ParmInfo );
                }
                else
                {
                    check( false );
                    Success = false;
                }

                NewInputCurveParameters.Add( CurrentParameterName, HoudiniAssetParameter );
            }
        }

        ClearInputCurveParameters();
        InputCurveParameters = NewInputCurveParameters;
    }

    if ( bSwitchedToCurve )
    {

#if WITH_EDITOR
        // We need to trigger details panel update.
        HoudiniAssetComponent->UpdateEditorProperties( false );

        // The editor caches the current selection visualizer, so we need to trick
        // and pretend the selection has changed so that the HSplineVisualizer can be drawn immediately
        if (GUnrealEd)
            GUnrealEd->NoteSelectionChange();
#endif

        bSwitchedToCurve = false;
    }
    return Success;
}

FText
UHoudiniAssetInput::HandleChoiceContentText() const
{
    return FText::FromString( ChoiceStringValue );
}

#if WITH_EDITOR

void
UHoudiniAssetInput::CheckStateChangedExportOnlySelected( ECheckBoxState NewState )
{
    int32 bState = ( NewState == ECheckBoxState::Checked );

    if ( bLandscapeInputSelectionOnly != bState )
    {
        // Record undo information.
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Export Landscape Selection mode change." ),
            HoudiniAssetComponent );
        Modify();

        MarkPreChanged();

        bLandscapeInputSelectionOnly = bState;

        // Mark this parameter as changed.
        MarkChanged();
    }
}

ECheckBoxState
UHoudiniAssetInput::IsCheckedExportOnlySelected() const
{
    if ( bLandscapeInputSelectionOnly )
        return ECheckBoxState::Checked;

    return ECheckBoxState::Unchecked;
}

void
UHoudiniAssetInput::CheckStateChangedExportCurves( ECheckBoxState NewState )
{
    int32 bState = ( NewState == ECheckBoxState::Checked );

    if ( bLandscapeExportCurves != bState )
    {
        // Record undo information.
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Export Landscape Curve mode change." ),
            HoudiniAssetComponent );
        Modify();

        MarkPreChanged();

        bLandscapeExportCurves = bState;

        // Mark this parameter as changed.
        MarkChanged();
    }
}

ECheckBoxState
UHoudiniAssetInput::IsCheckedExportCurves() const
{
    if ( bLandscapeExportCurves )
        return ECheckBoxState::Checked;

    return ECheckBoxState::Unchecked;
}

void
UHoudiniAssetInput::CheckStateChangedExportFullGeometry( ECheckBoxState NewState )
{
    int32 bState = ( NewState == ECheckBoxState::Checked );

    if ( bLandscapeExportFullGeometry != bState )
    {
        // Record undo information.
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Export Landscape Full Geometry mode change." ),
            HoudiniAssetComponent );
        Modify();

        MarkPreChanged();

        bLandscapeExportFullGeometry = bState;

        // Mark this parameter as changed.
        MarkChanged();
    }
}

ECheckBoxState
UHoudiniAssetInput::IsCheckedExportFullGeometry() const
{
    if ( bLandscapeExportFullGeometry )
        return ECheckBoxState::Checked;

    return ECheckBoxState::Unchecked;
}

void
UHoudiniAssetInput::CheckStateChangedExportMaterials( ECheckBoxState NewState )
{
    int32 bState = ( NewState == ECheckBoxState::Checked );

    if ( bLandscapeExportMaterials != bState )
    {
        // Record undo information.
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Export Landscape Materials mode change." ),
            HoudiniAssetComponent );
        Modify();

        MarkPreChanged();

        bLandscapeExportMaterials = bState;

        // Mark this parameter as changed.
        MarkChanged();
    }
}

ECheckBoxState
UHoudiniAssetInput::IsCheckedExportMaterials() const
{
    if ( bLandscapeExportMaterials )
        return ECheckBoxState::Checked;

    return ECheckBoxState::Unchecked;
}

void
UHoudiniAssetInput::CheckStateChangedExportLighting( ECheckBoxState NewState )
{
    int32 bState = ( NewState == ECheckBoxState::Checked );

    if ( bLandscapeExportLighting != bState )
    {
        // Record undo information.
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Export Landscape Lighting mode change." ),
            HoudiniAssetComponent );
        Modify();

        MarkPreChanged();

        bLandscapeExportLighting = bState;

        // Mark this parameter as changed.
        MarkChanged();
    }
}

ECheckBoxState
UHoudiniAssetInput::IsCheckedExportLighting() const
{
    if ( bLandscapeExportLighting )
        return ECheckBoxState::Checked;

    return ECheckBoxState::Unchecked;
}

void
UHoudiniAssetInput::CheckStateChangedExportNormalizedUVs( ECheckBoxState NewState )
{
    int32 bState = ( NewState == ECheckBoxState::Checked );

    if ( bLandscapeExportNormalizedUVs != bState )
    {
        // Record undo information.
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Export Landscape Normalized UVs mode change." ),
            HoudiniAssetComponent );
        Modify();

        MarkPreChanged();

        bLandscapeExportNormalizedUVs = bState;

        // Mark this parameter as changed.
        MarkChanged();
    }
}

ECheckBoxState
UHoudiniAssetInput::IsCheckedExportNormalizedUVs() const
{
    if ( bLandscapeExportNormalizedUVs )
        return ECheckBoxState::Checked;

    return ECheckBoxState::Unchecked;
}

void
UHoudiniAssetInput::CheckStateChangedExportTileUVs( ECheckBoxState NewState )
{
    int32 bState = ( NewState == ECheckBoxState::Checked );

    if ( bLandscapeExportTileUVs != bState )
    {
        // Record undo information.
        FScopedTransaction Transaction(
            TEXT( HOUDINI_MODULE_RUNTIME ),
            LOCTEXT( "HoudiniInputChange", "Houdini Export Landscape Tile UVs mode change." ),
            HoudiniAssetComponent );
        Modify();

        MarkPreChanged();

        bLandscapeExportTileUVs = bState;

        // Mark this parameter as changed.
        MarkChanged();
    }
}


ECheckBoxState
UHoudiniAssetInput::IsCheckedExportTileUVs() const
{
    if ( bLandscapeExportTileUVs )
        return ECheckBoxState::Checked;

    return ECheckBoxState::Unchecked;
}


void
UHoudiniAssetInput::CheckStateChangedKeepWorldTransform(ECheckBoxState NewState)
{ 
    int32 bState = (NewState == ECheckBoxState::Checked);

    if (bKeepWorldTransform == bState)
	return;

    // Record undo information.
    FScopedTransaction Transaction(
	TEXT(HOUDINI_MODULE_RUNTIME),
	LOCTEXT("HoudiniInputChange", "Houdini Input Transform Type change."),
	HoudiniAssetComponent);
    Modify();

    MarkPreChanged();

    bKeepWorldTransform = bState;

    // Mark this parameter as changed.
    MarkChanged();
}


ECheckBoxState
UHoudiniAssetInput::IsCheckedKeepWorldTransform() const
{
    if (bKeepWorldTransform == 2)
    {
	if (GetDefaultTranformTypeValue())
	    return ECheckBoxState::Checked;
	else
	    return ECheckBoxState::Unchecked;
    }
    else if (bKeepWorldTransform)
	return ECheckBoxState::Checked;
    
    return ECheckBoxState::Unchecked;
}


FReply
UHoudiniAssetInput::OnButtonClickRecommit()
{
    // There's no undo operation for button.

    MarkPreChanged();
    MarkChanged();

    return FReply::Handled();
}



FReply
UHoudiniAssetInput::OnButtonClickSelectActors()
{
    // There's no undo operation for button.

    FPropertyEditorModule & PropertyModule =
        FModuleManager::Get().GetModuleChecked< FPropertyEditorModule >( "PropertyEditor" );

    // Locate the details panel.
    FName DetailsPanelName = "LevelEditorSelectionDetails";
    TSharedPtr< IDetailsView > DetailsView = PropertyModule.FindDetailView( DetailsPanelName );

    if ( !DetailsView.IsValid() )
        return FReply::Handled();

    class SLocalDetailsView : public SDetailsViewBase
    {
        public:
        void LockDetailsView() { SDetailsViewBase::bIsLocked = true; }
        void UnlockDetailsView() { SDetailsViewBase::bIsLocked = false; }
    };
    auto * LocalDetailsView = static_cast< SLocalDetailsView * >( DetailsView.Get() );

    if ( !DetailsView->IsLocked() )
    {
        LocalDetailsView->LockDetailsView();
        check( DetailsView->IsLocked() );

        // Force refresh of details view.
        HoudiniAssetComponent->UpdateEditorProperties( false );

        // Select the previously chosen input Actors from the World Outliner.
        GEditor->SelectNone( false, true );
        for ( auto & OutlinerMesh : InputOutlinerMeshArray )
        {
            if ( OutlinerMesh.Actor )
                GEditor->SelectActor( OutlinerMesh.Actor, true, true );
        }

        return FReply::Handled();
    }

    if ( !GEditor || !GEditor->GetSelectedObjects() )
        return FReply::Handled();

    // If details panel is locked, locate selected actors and check if this component belongs to one of them.

    FScopedTransaction Transaction(
        TEXT( HOUDINI_MODULE_RUNTIME ),
        LOCTEXT( "HoudiniInputChange", "Houdini World Outliner Input Change" ),
        HoudiniAssetComponent );
    Modify();

    MarkPreChanged();
    bStaticMeshChanged = true;

    // Delete all assets and reset the array.
    // TODO: Make this process a little more efficient.
    DisconnectAndDestroyInputAsset();
    InputOutlinerMeshArray.Empty();

    USelection * SelectedActors = GEditor->GetSelectedActors();

    // If the builder brush is selected, first deselect it.
    for ( FSelectionIterator It( *SelectedActors ); It; ++It )
    {
        AActor * Actor = Cast< AActor >( *It );
        if ( !Actor )
            continue;

        // Don't allow selection of ourselves. Bad things happen if we do.
        if ( Actor == HoudiniAssetComponent->GetOwner() )
            continue;

	// Looking for StaticMeshes
        for ( UActorComponent * Component : Actor->GetComponentsByClass( UStaticMeshComponent::StaticClass() ) )
        {
            UStaticMeshComponent * StaticMeshComponent = CastChecked< UStaticMeshComponent >( Component );
            if ( !StaticMeshComponent )
                continue;

            UStaticMesh * StaticMesh = StaticMeshComponent->GetStaticMesh();
            if ( !StaticMesh )
                continue;

	    // Add the mesh to the array
	    FHoudiniAssetInputOutlinerMesh OutlinerMesh;

	    OutlinerMesh.Actor = Actor;
	    OutlinerMesh.StaticMeshComponent = StaticMeshComponent;
	    OutlinerMesh.StaticMesh = StaticMesh;
	    OutlinerMesh.SplineComponent = nullptr;
	    OutlinerMesh.AssetId = -1;

	    UpdateWorldOutlinerTransforms(OutlinerMesh);

	    InputOutlinerMeshArray.Add(OutlinerMesh);
        }

	// Looking for Splines
	for (UActorComponent * Component : Actor->GetComponentsByClass(USplineComponent::StaticClass()))
	{
	    USplineComponent * SplineComponent = CastChecked< USplineComponent >(Component);
	    if (!SplineComponent)
		continue;

	    // Add the spline to the array
	    FHoudiniAssetInputOutlinerMesh OutlinerMesh;

	    OutlinerMesh.Actor = Actor;
	    OutlinerMesh.StaticMeshComponent = nullptr;
	    OutlinerMesh.StaticMesh = nullptr;
	    OutlinerMesh.SplineComponent = SplineComponent;
	    OutlinerMesh.AssetId = -1;

	    UpdateWorldOutlinerTransforms(OutlinerMesh);

	    InputOutlinerMeshArray.Add(OutlinerMesh);
	}
    }

    MarkChanged();

    AHoudiniAssetActor * HoudiniAssetActor = HoudiniAssetComponent->GetHoudiniAssetActorOwner();

    if ( DetailsView->IsLocked() )
    {
        LocalDetailsView->UnlockDetailsView();
        check( !DetailsView->IsLocked() );

        TArray< UObject * > DummySelectedActors;
        DummySelectedActors.Add( HoudiniAssetActor );

        // Reset selected actor to itself, force refresh and override the lock.
        DetailsView->SetObjects( DummySelectedActors, true, true );
    }

    // Reselect the Asset Actor. If we don't do this, our Asset parameters will stop
    // refreshing and the user will be very confused. It is also resetting the state
    // of the selection before the input actor selection process was started.
    GEditor->SelectNone( false, true );
    GEditor->SelectActor( HoudiniAssetActor, true, true );

    // Update parameter layout.
    HoudiniAssetComponent->UpdateEditorProperties( false );

    // Start or stop the tick timer to check if the selected Actors have been transformed.
    if ( InputOutlinerMeshArray.Num() > 0 )
        StartWorldOutlinerTicking();
    else if ( InputOutlinerMeshArray.Num() <= 0 )
        StopWorldOutlinerTicking();

    return FReply::Handled();
}

void
UHoudiniAssetInput::StartWorldOutlinerTicking()
{
    if ( InputOutlinerMeshArray.Num() > 0 && !WorldOutlinerTimerDelegate.IsBound() && GEditor )
    {
        WorldOutlinerTimerDelegate = FTimerDelegate::CreateUObject( this, &UHoudiniAssetInput::TickWorldOutlinerInputs );

        // We need to register delegate with the timer system.
        static const float TickTimerDelay = 0.5f;
        GEditor->GetTimerManager()->SetTimer( WorldOutlinerTimerHandle, WorldOutlinerTimerDelegate, TickTimerDelay, true );
    }
}

void
UHoudiniAssetInput::StopWorldOutlinerTicking()
{
    if ( InputOutlinerMeshArray.Num() <= 0 && WorldOutlinerTimerDelegate.IsBound() && GEditor )
    {
        GEditor->GetTimerManager()->ClearTimer( WorldOutlinerTimerHandle );
        WorldOutlinerTimerDelegate.Unbind();
    }
}

void UHoudiniAssetInput::InvalidateNodeIds()
{
    ConnectedAssetId = -1;
    for (auto& OutlinerInputMesh : InputOutlinerMeshArray)
    {
        OutlinerInputMesh.AssetId = -1;
    }
}

void UHoudiniAssetInput::DuplicateCurves(UHoudiniAssetInput * OriginalInput)
{
    if (!InputCurve || !OriginalInput)
        return;

    // The previous call to DuplicateObject did not duplicate the curves properly
    // Both the original and duplicated Inputs now share the same InputCurve, so we 
    // need to create a proper copy of that curve

    // Keep the original pointer to the curve, as we need to duplicate its data
    UHoudiniSplineComponent* pOriginalCurve = InputCurve;

    // Creates a new Curve
    InputCurve = NewObject< UHoudiniSplineComponent >(
        HoudiniAssetComponent->GetOwner(), UHoudiniSplineComponent::StaticClass(),
        NAME_None, RF_Public | RF_Transactional);

    // Attach curve component to asset.
    InputCurve->AttachToComponent(HoudiniAssetComponent, FAttachmentTransformRules::KeepRelativeTransform);
    InputCurve->RegisterComponent();
    InputCurve->SetVisibility(true);

    // The new curve need do know that it is connected to this Input
    InputCurve->SetHoudiniAssetInput(this);
    
    // The call to DuplicateObject has actually modified the original object's Input
    // so we need to fix that as well.
    pOriginalCurve->SetHoudiniAssetInput(OriginalInput);

    // "Copy" the old curves parameters to the new one
    InputCurve->CopyFrom(pOriginalCurve);

    // to force rebuild...
    bSwitchedToCurve = true;
}


void 
UHoudiniAssetInput::RemoveWorldOutlinerInput( int32 AtIndex )
{
    FScopedTransaction Transaction(
        TEXT( HOUDINI_MODULE_RUNTIME ),
        LOCTEXT( "HoudiniInputChange", "Houdini World Outliner Input Change" ),
        HoudiniAssetComponent );
    Modify();

    MarkPreChanged();
    bStaticMeshChanged = true;
    InputOutlinerMeshArray.RemoveAt( AtIndex );
    MarkChanged();
}

void
UHoudiniAssetInput::UpdateWorldOutlinerTransforms(FHoudiniAssetInputOutlinerMesh& OutlinerMesh)
{
    // Update to the new Transforms
    OutlinerMesh.ActorTransform = OutlinerMesh.Actor->GetTransform();

    if (OutlinerMesh.StaticMeshComponent)
	OutlinerMesh.ComponentTransform = OutlinerMesh.StaticMeshComponent->GetComponentTransform();

    if (OutlinerMesh.SplineComponent)
	OutlinerMesh.ComponentTransform = OutlinerMesh.SplineComponent->GetComponentTransform();

    OutlinerMesh.KeepWorldTransform = bKeepWorldTransform;
}

void UHoudiniAssetInput::OnAddToInputObjects()
{
    FScopedTransaction Transaction(
        TEXT( HOUDINI_MODULE_RUNTIME ),
        LOCTEXT( "HoudiniInputChange", "Houdini Input Geometry Change" ),
        HoudiniAssetComponent );
    Modify();
    MarkPreChanged();
    InputObjects.Add( nullptr );
    MarkChanged();
    bStaticMeshChanged = true;
    HoudiniAssetComponent->UpdateEditorProperties( false );
}

void UHoudiniAssetInput::OnEmptyInputObjects()
{
    FScopedTransaction Transaction(
        TEXT( HOUDINI_MODULE_RUNTIME ),
        LOCTEXT( "HoudiniInputChange", "Houdini Input Geometry Change" ),
        HoudiniAssetComponent );
    Modify();
    MarkPreChanged();
    InputObjects.Empty();
    MarkChanged();
    bStaticMeshChanged = true;
    HoudiniAssetComponent->UpdateEditorProperties( false );
}

TOptional< float >
UHoudiniAssetInput::GetSplineResolutionValue() const
{
    if (UnrealSplineResolution != -1.0f)
	return UnrealSplineResolution;

    const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
    if (HoudiniRuntimeSettings)
	return HoudiniRuntimeSettings->MarshallingSplineResolution;
    else
	return HAPI_UNREAL_PARAM_SPLINE_RESOLUTION_DEFAULT;
}


void
UHoudiniAssetInput::SetSplineResolutionValue(float InValue)
{
    if (InValue < 0)
	OnResetSplineResolutionClicked();
    else
	UnrealSplineResolution = FMath::Clamp< float >(InValue, 0.0f, 10000.0f);
}


bool
UHoudiniAssetInput::IsSplineResolutionEnabled() const
{
    if (ChoiceIndex != EHoudiniAssetInputType::WorldInput)
	return false;

    for (int32 n = 0; n < InputOutlinerMeshArray.Num(); n++)
    {
	if (InputOutlinerMeshArray[n].SplineComponent)
	    return true;
    }

    return false;
}


FReply
UHoudiniAssetInput::OnResetSplineResolutionClicked()
{
    const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
    if (HoudiniRuntimeSettings)
	UnrealSplineResolution = HoudiniRuntimeSettings->MarshallingSplineResolution;
    else
	UnrealSplineResolution = HAPI_UNREAL_PARAM_SPLINE_RESOLUTION_DEFAULT;

    return FReply::Handled();
}
FMenuBuilder
UHoudiniAssetInput::CreateCustomActorPickerWidget( const TAttribute<FText>& HeadingText, const bool& bShowCurrentSelectionSection )
{
    // Custom Actor Picker showing only the desired Actor types.
    // Note: Code stolen from SPropertyMenuActorPicker.cpp
    FOnShouldFilterActor ActorFilter = FOnShouldFilterActor::CreateUObject( this, &UHoudiniAssetInput::OnShouldFilterActor );
        
    //FHoudiniEngineEditor & HoudiniEngineEditor = FHoudiniEngineEditor::Get();
    //TSharedPtr< ISlateStyle > StyleSet = GEditor->GetSlateStyle();
        
    FMenuBuilder MenuBuilder( true, NULL );

    if ( bShowCurrentSelectionSection )
    {
        MenuBuilder.BeginSection( NAME_None, LOCTEXT( "CurrentActorOperationsHeader", "Current Selection" ) );
        {
            MenuBuilder.AddMenuEntry(
                TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateUObject(this, &UHoudiniAssetInput::GetCurrentSelectionText)),
                TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateUObject(this, &UHoudiniAssetInput::GetCurrentSelectionText)),
                FSlateIcon(),//StyleSet->GetStyleSetName(), "HoudiniEngine.HoudiniEngineLogo")),
                FUIAction(),
                NAME_None,
                EUserInterfaceActionType::Button,
                NAME_None);
        }
        MenuBuilder.EndSection();
    }

    MenuBuilder.BeginSection(NAME_None, HeadingText );
    {
        TSharedPtr< SWidget > MenuContent;

        FSceneOutlinerModule & SceneOutlinerModule =
            FModuleManager::Get().LoadModuleChecked< FSceneOutlinerModule >( TEXT( "SceneOutliner" ) );

        SceneOutliner::FInitializationOptions InitOptions;
        InitOptions.Mode = ESceneOutlinerMode::ActorPicker;
        InitOptions.Filters->AddFilterPredicate( ActorFilter );
        InitOptions.bFocusSearchBoxWhenOpened = true;
        
        static const FVector2D SceneOutlinerWindowSize( 350.0f, 200.0f );
        MenuContent =
            SNew( SBox )
            .WidthOverride( SceneOutlinerWindowSize.X )
            .HeightOverride( SceneOutlinerWindowSize.Y )
            [
                SNew( SBorder )
                .BorderImage( FEditorStyle::GetBrush( "Menu.Background" ) )
                [
                    SceneOutlinerModule.CreateSceneOutliner(
                        InitOptions, FOnActorPicked::CreateUObject(
                            this, &UHoudiniAssetInput::OnActorSelected ) )
                ]
            ];

        MenuBuilder.AddWidget( MenuContent.ToSharedRef(), FText::GetEmpty(), true );
    }
    MenuBuilder.EndSection();

    return MenuBuilder;
}

FText
UHoudiniAssetInput::GetCurrentSelectionText() const
{
    FText CurrentSelectionText;
    if ( ChoiceIndex == EHoudiniAssetInputType::AssetInput )
    {
        if ( InputAssetComponent && InputAssetComponent->GetHoudiniAssetActorOwner() )
        {
            CurrentSelectionText = FText::FromString( InputAssetComponent->GetHoudiniAssetActorOwner()->GetName() );
        }
    }
    else if ( ChoiceIndex == EHoudiniAssetInputType::LandscapeInput )
    {
        if ( InputLandscapeProxy )
        {
            CurrentSelectionText = FText::FromString( InputLandscapeProxy->GetName() );
        }
    }

    return CurrentSelectionText;
}

#endif

FArchive &
operator<<( FArchive & Ar, FHoudiniAssetInputOutlinerMesh & HoudiniAssetInputOutlinerMesh )
{
    HoudiniAssetInputOutlinerMesh.Serialize( Ar );
    return Ar;
}
