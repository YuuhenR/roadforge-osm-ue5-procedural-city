// Copyright RoadForge Contributors. Licensed under the MIT License.

#include "RoadForgeEditor.h"

#include "OSMRoadGenerator.h"

#include "ToolMenus.h"
#include "Editor.h"                   // GEditor
#include "EngineUtils.h"             // TActorIterator
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "UObject/SavePackage.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"

// --- Lighting rig actors / components ---
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"      // ASkyAtmosphere
#include "Components/VolumetricCloudComponent.h"    // AVolumetricCloud
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/Scene.h"                           // FPostProcessSettings

// --- Material graph ---
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MaterialEditingLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "SceneTypes.h"                             // EMaterialProperty (MP_BaseColor, ...)
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionDesaturation.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"

#define LOCTEXT_NAMESPACE "FRoadForgeEditorModule"

DEFINE_LOG_CATEGORY_STATIC(LogRoadForgeEditor, Log, All);

namespace
{
	const TCHAR* GMaterialPackagePath = TEXT("/RoadForge/M_RoadForge_VC");
	const TCHAR* GMaterialAssetName = TEXT("M_RoadForge_VC");

	// CC0 detail textures (ambientCG, public domain). The plugin ships the PNG sources under
	// Plugins/RoadForge/Art/CC0_Textures; once the user imports them into /RoadForge/Textures the
	// material auto-upgrades from procedural noise to photo-scanned surface detail on regenerate.
	const TCHAR* GAsphaltNormalPath    = TEXT("/RoadForge/Textures/T_RF_Asphalt_N.T_RF_Asphalt_N");
	const TCHAR* GAsphaltRoughnessPath = TEXT("/RoadForge/Textures/T_RF_Asphalt_R.T_RF_Asphalt_R");
	const TCHAR* GAsphaltColorPath     = TEXT("/RoadForge/Textures/T_RF_Asphalt_BC.T_RF_Asphalt_BC");

	const TCHAR* GAsphaltNormalPackage    = TEXT("/RoadForge/Textures/T_RF_Asphalt_N");
	const TCHAR* GAsphaltRoughnessPackage = TEXT("/RoadForge/Textures/T_RF_Asphalt_R");
	const TCHAR* GAsphaltColorPackage     = TEXT("/RoadForge/Textures/T_RF_Asphalt_BC");

	// VertexColor / TextureSample output pin indices (all VertexColor outputs share the name "",
	// so they can only be addressed by index via FExpressionInput::Connect).
	enum { VCOUT_RGB = 0, VCOUT_A = 4 };

	/** Return the first actor of class TActor in World, or spawn one. bOutSpawned reports which. */
	template <typename TActor>
	TActor* FindOrSpawnActor(UWorld* World, const FVector& Loc, const FRotator& Rot, bool& bOutSpawned)
	{
		for (TActorIterator<TActor> It(World); It; ++It)
		{
			bOutSpawned = false;
			return *It;
		}
		FActorSpawnParameters Params;
		TActor* Actor = World->SpawnActor<TActor>(Loc, Rot, Params);
		bOutSpawned = (Actor != nullptr);
		return Actor;
	}

	float RoadNoise(float X, float Y)
	{
		const float A = FMath::PerlinNoise2D(FVector2D(X * 5.0f, Y * 5.0f)) * 0.5f + 0.5f;
		const float B = FMath::PerlinNoise2D(FVector2D(X * 21.0f + 7.1f, Y * 21.0f - 3.7f)) * 0.5f + 0.5f;
		const float C = FMath::PerlinNoise2D(FVector2D(X * 73.0f - 2.4f, Y * 73.0f + 5.9f)) * 0.5f + 0.5f;
		return FMath::Clamp(A * 0.55f + B * 0.30f + C * 0.15f, 0.0f, 1.0f);
	}

	UTexture2D* EnsureGeneratedAsphaltTexture(const TCHAR* PackagePath, const TCHAR* AssetName, int32 Kind)
	{
		if (FPackageName::DoesPackageExist(PackagePath))
		{
			return LoadObject<UTexture2D>(nullptr, *FString::Printf(TEXT("%s.%s"), PackagePath, AssetName));
		}

		UPackage* Package = CreatePackage(PackagePath);
		if (!Package) { return nullptr; }

		constexpr int32 Size = 512;
		TArray<uint8> Pixels;
		Pixels.SetNumZeroed(Size * Size * 4);

		auto WriteBGRA = [&](int32 X, int32 Y, uint8 R, uint8 G, uint8 B, uint8 A)
		{
			const int32 I = (Y * Size + X) * 4;
			Pixels[I + 0] = B;
			Pixels[I + 1] = G;
			Pixels[I + 2] = R;
			Pixels[I + 3] = A;
		};

		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const float U = static_cast<float>(X) / static_cast<float>(Size);
				const float V = static_cast<float>(Y) / static_cast<float>(Size);
				const float N = RoadNoise(U, V);
				const float Speckle = RoadNoise(U + 17.0f, V - 9.0f);
				if (Kind == 0) // Base color
				{
					const uint8 C = static_cast<uint8>(FMath::Clamp(42.0f + N * 38.0f + Speckle * 10.0f, 20.0f, 96.0f));
					WriteBGRA(X, Y, C, C, static_cast<uint8>(FMath::Max(18, C - 6)), 255);
				}
				else if (Kind == 1) // Roughness
				{
					const uint8 C = static_cast<uint8>(FMath::Clamp(145.0f + N * 80.0f, 110.0f, 245.0f));
					WriteBGRA(X, Y, C, C, C, 255);
				}
				else // Normal
				{
					const float E = 1.0f / static_cast<float>(Size);
					const float Dx = RoadNoise(U + E, V) - RoadNoise(U - E, V);
					const float Dy = RoadNoise(U, V + E) - RoadNoise(U, V - E);
					const FVector Normal = FVector(-Dx * 5.0f, -Dy * 5.0f, 1.0f).GetSafeNormal();
					const uint8 R = static_cast<uint8>((Normal.X * 0.5f + 0.5f) * 255.0f);
					const uint8 G = static_cast<uint8>((Normal.Y * 0.5f + 0.5f) * 255.0f);
					const uint8 B = static_cast<uint8>((Normal.Z * 0.5f + 0.5f) * 255.0f);
					WriteBGRA(X, Y, R, G, B, 255);
				}
			}
		}

		UTexture2D* Texture = NewObject<UTexture2D>(Package, FName(AssetName), RF_Public | RF_Standalone);
		if (!Texture) { return nullptr; }
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Pixels.GetData());
		Texture->SRGB = (Kind == 0);
		if (Kind == 2)
		{
			Texture->CompressionSettings = TC_Normalmap;
		}
		else if (Kind == 1)
		{
			Texture->CompressionSettings = TC_Grayscale;
		}
		Texture->UpdateResource();
		FAssetRegistryModule::AssetCreated(Texture);
		Texture->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(
			PackagePath, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FileName), /*Tree*/ true);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone | RF_Public;
		SaveArgs.SaveFlags = SAVE_NoError;
		UPackage::SavePackage(Package, Texture, *FileName, SaveArgs);

		return Texture;
	}

	void EnsureGeneratedAsphaltTextureSet()
	{
		EnsureGeneratedAsphaltTexture(GAsphaltColorPackage, TEXT("T_RF_Asphalt_BC"), 0);
		EnsureGeneratedAsphaltTexture(GAsphaltRoughnessPackage, TEXT("T_RF_Asphalt_R"), 1);
		EnsureGeneratedAsphaltTexture(GAsphaltNormalPackage, TEXT("T_RF_Asphalt_N"), 2);
	}

	/**
	 * Drop in (or retune) a cinematic lighting rig so the level matches the reference project's lit look:
	 * a warm Sun set as the atmosphere light, a real-time-captured SkyLight, SkyAtmosphere + VolumetricCloud,
	 * volumetric ExponentialHeightFog, and an unbound PostProcessVolume with exposure / Lumen-quality tuning.
	 *
	 * Idempotent: if the level already has an actor of a given type (e.g. from the OpenWorld template) it is
	 * reused and retuned instead of spawning a duplicate, so it is safe to run on any level.
	 */
	void SpawnLightingRig(UWorld* World, int32& OutCreated, int32& OutTuned)
	{
		OutCreated = 0;
		OutTuned = 0;
		if (!World) { return; }
		auto Tally = [&](bool bSpawned) { if (bSpawned) { ++OutCreated; } else { ++OutTuned; } };

		// --- Sun (directional light, drives the atmosphere) ---
		const FRotator SunRot(-46.0, -130.0, 0.0); // pitch down from the sky, raking angle for long shadows
		bool bSpawned = false;
		if (ADirectionalLight* Sun = FindOrSpawnActor<ADirectionalLight>(World, FVector(0, 0, 50000), SunRot, bSpawned))
		{
			Sun->SetActorRotation(SunRot);
			if (UDirectionalLightComponent* L = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				L->SetMobility(EComponentMobility::Movable);
				L->SetIntensity(8.0f);              // lux (ExtendDefaultLuminanceRange is on in this project)
				L->SetUseTemperature(true);
				L->SetTemperature(5600.0f);         // slightly warm daylight
				L->SetAtmosphereSunLight(true);     // this light defines the sun disc / sky colour
				L->MarkRenderStateDirty();
			}
			if (bSpawned) { Sun->SetActorLabel(TEXT("RoadForge_Sun")); }
			Sun->Tags.AddUnique(TEXT("RoadForgeRig"));
			Tally(bSpawned);
		}

		// --- Sky atmosphere (Earth-like defaults look good) ---
		bSpawned = false;
		if (ASkyAtmosphere* Atmo = FindOrSpawnActor<ASkyAtmosphere>(World, FVector::ZeroVector, FRotator::ZeroRotator, bSpawned))
		{
			if (bSpawned) { Atmo->SetActorLabel(TEXT("RoadForge_SkyAtmosphere")); }
			Atmo->Tags.AddUnique(TEXT("RoadForgeRig"));
			Tally(bSpawned);
		}

		// --- Volumetric clouds (defaults pair well with the atmosphere) ---
		bSpawned = false;
		if (AVolumetricCloud* Cloud = FindOrSpawnActor<AVolumetricCloud>(World, FVector::ZeroVector, FRotator::ZeroRotator, bSpawned))
		{
			if (bSpawned) { Cloud->SetActorLabel(TEXT("RoadForge_VolumetricCloud")); }
			Cloud->Tags.AddUnique(TEXT("RoadForgeRig"));
			Tally(bSpawned);
		}

		// --- Sky light (real-time capture so Lumen gets ambient sky/cloud bounce) ---
		bSpawned = false;
		if (ASkyLight* Sky = FindOrSpawnActor<ASkyLight>(World, FVector(0, 0, 30000), FRotator::ZeroRotator, bSpawned))
		{
			if (USkyLightComponent* S = Sky->GetLightComponent())
			{
				S->SetMobility(EComponentMobility::Movable);
				S->SourceType = SLS_CapturedScene;
				S->bRealTimeCapture = true;
				S->bLowerHemisphereIsBlack = false; // let the ground/road catch a little ambient instead of going black
				S->SetIntensity(2.2f);              // stronger sky fill so shadowed building facades aren't black
				S->MarkRenderStateDirty();
				S->RecaptureSky();
			}
			if (bSpawned) { Sky->SetActorLabel(TEXT("RoadForge_SkyLight")); }
			Sky->Tags.AddUnique(TEXT("RoadForgeRig"));
			Tally(bSpawned);
		}

		// --- Exponential height fog with volumetric fog (depth, haze, sun shafts) ---
		bSpawned = false;
		if (AExponentialHeightFog* Fog = FindOrSpawnActor<AExponentialHeightFog>(World, FVector(0, 0, 300), FRotator::ZeroRotator, bSpawned))
		{
			if (UExponentialHeightFogComponent* F = Fog->GetComponent())
			{
				F->SetFogDensity(0.02f);
				F->SetFogHeightFalloff(0.2f);
				F->SetVolumetricFog(true);
				F->MarkRenderStateDirty();
			}
			if (bSpawned) { Fog->SetActorLabel(TEXT("RoadForge_HeightFog")); }
			Fog->Tags.AddUnique(TEXT("RoadForgeRig"));
			Tally(bSpawned);
		}

		// --- Unbound post-process volume (exposure + Lumen quality + a touch of bloom) ---
		bSpawned = false;
		APostProcessVolume* PPV = nullptr;
		for (TActorIterator<APostProcessVolume> It(World); It; ++It)
		{
			if (It->bUnbound) { PPV = *It; break; }
		}
		if (!PPV)
		{
			FActorSpawnParameters Params;
			PPV = World->SpawnActor<APostProcessVolume>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
			bSpawned = (PPV != nullptr);
		}
		if (PPV)
		{
			PPV->bUnbound = true;
			PPV->Priority = 1.0f;
			FPostProcessSettings& S = PPV->Settings;
			S.bOverride_AutoExposureBias = true;        S.AutoExposureBias = 1.3f;        // a touch brighter overall
			// Constrain auto-exposure so a sky-heavy frame can't crush the streetscape into darkness.
			S.bOverride_AutoExposureMinBrightness = true; S.AutoExposureMinBrightness = 0.4f;
			S.bOverride_AutoExposureMaxBrightness = true; S.AutoExposureMaxBrightness = 2.0f;
			S.bOverride_BloomIntensity = true;          S.BloomIntensity = 0.5f;
			S.bOverride_LumenFinalGatherQuality = true; S.LumenFinalGatherQuality = 2.0f; // cleaner GI
			S.bOverride_LumenReflectionQuality = true;  S.LumenReflectionQuality = 2.0f;  // sharper road reflections
			if (bSpawned) { PPV->SetActorLabel(TEXT("RoadForge_PostProcess")); }
			PPV->Tags.AddUnique(TEXT("RoadForgeRig"));
			Tally(bSpawned);
		}
	}
}

void FRoadForgeEditorModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FRoadForgeEditorModule::RegisterMenus));

	// Creating/compiling a material must wait until the engine is fully up. If the module is (re)loaded
	// after that point (e.g. Live Coding), the delegate has already fired, so run it immediately.
	if (GIsRunning)
	{
		EnsureDefaultMaterialDeferred();
	}
	else
	{
		EngineInitCompleteHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddRaw(
			this, &FRoadForgeEditorModule::EnsureDefaultMaterialDeferred);
	}
}

void FRoadForgeEditorModule::ShutdownModule()
{
	if (EngineInitCompleteHandle.IsValid())
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(EngineInitCompleteHandle);
		EngineInitCompleteHandle.Reset();
	}
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FRoadForgeEditorModule::EnsureDefaultMaterialDeferred()
{
	// Skip in cook/commandlet/-game; the material is only needed for editing.
	if (IsRunningCommandlet() || !GIsEditor)
	{
		return;
	}

	// Important: do NOT create/save assets during editor startup. UE's DataValidation module may still
	// be registering validators, and validating freshly-created plugin assets at this point produces the
	// "UpdateValidators request made before RegisterBlueprintValidators" Message Log errors. Existing
	// material assets are loaded lazily by the generator; missing assets are created from explicit user
	// actions (Regenerate Material / Add Generator) when the editor is fully interactive.
	if (FPackageName::DoesPackageExist(GMaterialPackagePath))
	{
		LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("%s.%s"), GMaterialPackagePath, GMaterialAssetName));
	}
}

void FRoadForgeEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	if (UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools"))
	{
		FToolMenuSection& Section = ToolsMenu->FindOrAddSection("RoadForge", LOCTEXT("RoadForgeSection", "RoadForge"));

		Section.AddMenuEntry(
			"RoadForge_AddGenerator",
			LOCTEXT("AddGenerator", "Add OSM Road Generator to Level"),
			LOCTEXT("AddGeneratorTooltip", "Spawn an OSM Road Generator at the origin, generate from SampleData, and add a cinematic lighting rig."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FRoadForgeEditorModule::OnAddGenerator)));

		Section.AddMenuEntry(
			"RoadForge_ImportOSM",
			LOCTEXT("ImportOSM", "Import OSM File (New Generator)..."),
			LOCTEXT("ImportOSMTooltip", "Spawn an OSM Road Generator, open a file picker to choose any OSM/Overpass JSON export, generate it, and add the lighting rig. The fastest way to bring in a different map."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FRoadForgeEditorModule::OnImportOSM)));

		Section.AddMenuEntry(
			"RoadForge_AddLightingRig",
			LOCTEXT("AddLightingRig", "Add Cinematic Lighting Rig"),
			LOCTEXT("AddLightingRigTooltip", "Spawn or retune Sun + SkyLight + SkyAtmosphere + VolumetricCloud + volumetric HeightFog + an unbound PostProcessVolume so the level is lit like the reference project."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FRoadForgeEditorModule::OnAddLightingRig)));

		Section.AddMenuEntry(
			"RoadForge_GenerateMaterial",
			LOCTEXT("GenerateMaterial", "Regenerate RoadForge Material"),
			LOCTEXT("GenerateMaterialTooltip", "Rebuild the shared PBR material /RoadForge/M_RoadForge_VC (wires the CC0 detail textures if they have been imported)."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FRoadForgeEditorModule::OnGenerateMaterial)));
	}
}

void FRoadForgeEditorModule::OnGenerateMaterial()
{
	UMaterialInterface* Material = EnsureVertexColorMaterial(/*bForceRecreate*/ true);

	FNotificationInfo Info(Material
		? LOCTEXT("MaterialDone", "RoadForge material regenerated: /RoadForge/M_RoadForge_VC")
		: LOCTEXT("MaterialFail", "RoadForge material generation failed (see Output Log)."));
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	// Reveal it in the Content Browser (forces plugin-content visibility on too).
	if (Material && GEditor)
	{
		TArray<UObject*> Objects;
		Objects.Add(Material);
		GEditor->SyncBrowserToObjects(Objects);
	}
}

void FRoadForgeEditorModule::OnAddLightingRig()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogRoadForgeEditor, Error, TEXT("No editor world to add a lighting rig to."));
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddLightingRigTransaction", "Add RoadForge Lighting Rig"));
	int32 Created = 0, Tuned = 0;
	SpawnLightingRig(World, Created, Tuned);

	FNotificationInfo Info(FText::Format(
		LOCTEXT("LightingRigDone", "RoadForge lighting rig: {0} actor(s) created, {1} retuned."),
		FText::AsNumber(Created), FText::AsNumber(Tuned)));
	Info.ExpireDuration = 6.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogRoadForgeEditor, Log, TEXT("RoadForge lighting rig: %d created, %d retuned."), Created, Tuned);
}

void FRoadForgeEditorModule::OnImportOSM()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogRoadForgeEditor, Error, TEXT("No editor world to spawn the generator into."));
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("ImportOSMTransaction", "Import OSM (New Generator)"));
	EnsureVertexColorMaterial(/*bForceRecreate*/ false);

	FActorSpawnParameters Params;
	Params.Name = MakeUniqueObjectName(World, AOSMRoadGenerator::StaticClass(), TEXT("OSMRoadGenerator"));
	AOSMRoadGenerator* Generator = World->SpawnActor<AOSMRoadGenerator>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Generator)
	{
		UE_LOG(LogRoadForgeEditor, Error, TEXT("Failed to spawn AOSMRoadGenerator."));
		return;
	}

	Generator->ApplyReferenceQualityPreset();
	Generator->bGenerateGroundPlane = false;

	// Open the native file picker and generate the chosen export (does nothing if the user cancels).
	Generator->ImportOSMFile();

	int32 RigCreated = 0, RigTuned = 0;
	SpawnLightingRig(World, RigCreated, RigTuned);

	if (GEditor)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		GEditor->SelectActor(Generator, /*bInSelected*/ true, /*bNotify*/ true);
		GEditor->MoveViewportCamerasToActor(*Generator, /*bActiveViewportOnly*/ false);
	}

	const bool bGenerated = !Generator->LocalOSMJsonPath.IsEmpty();
	FNotificationInfo Info(bGenerated
		? LOCTEXT("ImportOSMDone", "Imported OSM file & generated road network + lighting rig. Drag the road splines and use RoadForge | Actions -> Rebuild From Splines to edit.")
		: LOCTEXT("ImportOSMCancelled", "Import cancelled. Set Local OSM Json Path, then RoadForge | Actions -> Generate From Local File."));
	Info.ExpireDuration = 7.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogRoadForgeEditor, Log, TEXT("Import OSM: generated=%d; lighting rig %d created, %d retuned."),
		bGenerated ? 1 : 0, RigCreated, RigTuned);
}

void FRoadForgeEditorModule::OnAddGenerator()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogRoadForgeEditor, Error, TEXT("No editor world to spawn the generator into."));
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddGeneratorTransaction", "Add OSM Road Generator"));

	// This is a user-triggered action after the editor has finished starting, so it is safe to create the
	// material here if it is missing. Startup itself deliberately avoids writing assets.
	EnsureVertexColorMaterial(/*bForceRecreate*/ false);

	FActorSpawnParameters Params;
	Params.Name = MakeUniqueObjectName(World, AOSMRoadGenerator::StaticClass(), TEXT("OSMRoadGenerator"));
	AOSMRoadGenerator* Generator = World->SpawnActor<AOSMRoadGenerator>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Generator)
	{
		UE_LOG(LogRoadForgeEditor, Error, TEXT("Failed to spawn AOSMRoadGenerator."));
		return;
	}

	// The level usually has its own terrain, so don't lay down the (z-fighting) fill plane.
	Generator->ApplyReferenceQualityPreset();
	Generator->bGenerateGroundPlane = false;

	// Pre-fill the local sample path so we can generate immediately (offline, no network).
	const FString TaipeiSample = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("SampleData/taipei_osm.json"));
	const FString BarcelonaSample = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("SampleData/barcelona_osm.json"));
	const FString SmallSample = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("SampleData/sample_osm.json"));
	if (FPaths::FileExists(TaipeiSample))      { Generator->LocalOSMJsonPath = TaipeiSample; }
	else if (FPaths::FileExists(BarcelonaSample)) { Generator->LocalOSMJsonPath = BarcelonaSample; }
	else if (FPaths::FileExists(SmallSample))  { Generator->LocalOSMJsonPath = SmallSample; }

	// Build straight away so the user sees the road network without an extra click.
	if (!Generator->LocalOSMJsonPath.IsEmpty())
	{
		Generator->GenerateFromLocalFile();
	}

	// Light the scene like the reference project (idempotent: reuses any lighting already present).
	int32 RigCreated = 0, RigTuned = 0;
	SpawnLightingRig(World, RigCreated, RigTuned);

	if (GEditor)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		GEditor->SelectActor(Generator, /*bInSelected*/ true, /*bNotify*/ true);
		GEditor->MoveViewportCamerasToActor(*Generator, /*bActiveViewportOnly*/ false);
	}

	const bool bGenerated = !Generator->LocalOSMJsonPath.IsEmpty();
	FNotificationInfo Info(bGenerated
		? FText::Format(LOCTEXT("AddGeneratorDoneGen", "Added & generated 'OSM Road Generator' from SampleData + lighting rig ({0} new). Use RoadForge | Actions to regenerate or Fetch live data."), FText::AsNumber(RigCreated))
		: LOCTEXT("AddGeneratorDone", "Added 'OSM Road Generator' at the origin. Set Local OSM Json Path, then RoadForge | Actions -> Generate From Local File."));
	Info.ExpireDuration = 7.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogRoadForgeEditor, Log, TEXT("Spawned OSM Road Generator (auto-generated=%d); lighting rig %d created, %d retuned."),
		bGenerated ? 1 : 0, RigCreated, RigTuned);
}

UMaterialInterface* FRoadForgeEditorModule::EnsureVertexColorMaterial(bool bForceRecreate)
{
	if (!bForceRecreate && FPackageName::DoesPackageExist(GMaterialPackagePath))
	{
		return LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("%s.%s"), GMaterialPackagePath, GMaterialAssetName));
	}

	UPackage* Package = CreatePackage(GMaterialPackagePath);
	if (!Package)
	{
		UE_LOG(LogRoadForgeEditor, Error, TEXT("Could not create package %s"), GMaterialPackagePath);
		return nullptr;
	}

	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(Factory->FactoryCreateNew(
		UMaterial::StaticClass(), Package, FName(GMaterialAssetName), RF_Standalone | RF_Public, nullptr, GWarn));
	if (!Material)
	{
		UE_LOG(LogRoadForgeEditor, Error, TEXT("Material factory failed."));
		return nullptr;
	}

	auto Make = [Material](UClass* Class, int32 X, int32 Y) -> UMaterialExpression*
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	};

	// Vertex colour: RGB = each section's base tint; A = emissive mask (lane lines author A>0 to glow).
	UMaterialExpressionVertexColor* VC = Cast<UMaterialExpressionVertexColor>(
		Make(UMaterialExpressionVertexColor::StaticClass(), -1200, 0));

	// Up-facing mask from the geometric normal: ~1 on roads/roofs (horizontal), ~0 on building walls and curb
	// faces (vertical). The top-down world-planar detail would smear into dark glossy streaks on walls, so we
	// use this to keep surface detail on horizontal faces and flatten it on vertical ones.
	UMaterialExpressionVertexNormalWS* VN = Cast<UMaterialExpressionVertexNormalWS>(
		Make(UMaterialExpressionVertexNormalWS::StaticClass(), -1200, 850));
	UMaterialExpressionComponentMask* UpMask = Cast<UMaterialExpressionComponentMask>(
		Make(UMaterialExpressionComponentMask::StaticClass(), -1000, 850));
	UpMask->R = false; UpMask->G = false; UpMask->B = true; UpMask->A = false; // take the Z component
	UpMask->Input.Connect(0, VN);

	// Roughness = lerp(matte 0.78 on vertical faces, detail roughness on horizontal faces).
	auto ConnectMaskedRoughness = [&](UMaterialExpression* Detail, int32 OutIdx)
	{
		UMaterialExpressionLinearInterpolate* L = Cast<UMaterialExpressionLinearInterpolate>(
			Make(UMaterialExpressionLinearInterpolate::StaticClass(), -150, 700));
		L->ConstA = 0.78f;
		L->B.Connect(OutIdx, Detail);
		L->Alpha.Connect(0, UpMask);
		UMaterialEditingLibrary::ConnectMaterialProperty(L, TEXT(""), MP_Roughness);
	};

	// CC0 asphalt detail set (ambientCG). Present => texture-detail PBR; absent => procedural noise fallback.
	UTexture* TexNormal = LoadObject<UTexture>(nullptr, GAsphaltNormalPath);
	UTexture* TexRough  = LoadObject<UTexture>(nullptr, GAsphaltRoughnessPath);
	UTexture* TexColor  = LoadObject<UTexture>(nullptr, GAsphaltColorPath);
	const bool bHaveDetail = (TexNormal != nullptr || TexRough != nullptr);

	// Make the sampler types valid regardless of how the user imported the PNGs (idempotent fix-up).
	if (TexNormal)
	{
		bool bChanged = false;
		if (TexNormal->SRGB) { TexNormal->SRGB = false; bChanged = true; }
		if (TexNormal->CompressionSettings != TC_Normalmap) { TexNormal->CompressionSettings = TC_Normalmap; bChanged = true; }
		if (bChanged) { TexNormal->PostEditChange(); TexNormal->MarkPackageDirty(); }
	}
	if (TexRough && TexRough->SRGB) { TexRough->SRGB = false; TexRough->PostEditChange(); TexRough->MarkPackageDirty(); }

	UMaterialExpression* BaseColorSource = VC; // float3 feeding MP_BaseColor

	if (bHaveDetail)
	{
		// World-planar UVs (top-down projection): UV = WorldPosition.xy * scale. No mesh UVs required,
		// and it tiles seamlessly across every road/ground section.
		UMaterialExpressionWorldPosition* WP = Cast<UMaterialExpressionWorldPosition>(
			Make(UMaterialExpressionWorldPosition::StaticClass(), -1900, 500));
		UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(
			Make(UMaterialExpressionComponentMask::StaticClass(), -1650, 500));
		Mask->R = true; Mask->G = true; Mask->B = false; Mask->A = false;
		Mask->Input.Connect(0, WP);
		UMaterialExpressionMultiply* UV = Cast<UMaterialExpressionMultiply>(
			Make(UMaterialExpressionMultiply::StaticClass(), -1450, 500));
		UV->A.Connect(0, Mask);
		UV->ConstB = 0.004f; // ~2.5 m per tile (world cm -> UV)

		if (TexNormal)
		{
			UMaterialExpressionTextureSample* TN = Cast<UMaterialExpressionTextureSample>(
				Make(UMaterialExpressionTextureSample::StaticClass(), -1100, 450));
			TN->Texture = TexNormal;
			TN->SamplerType = SAMPLERTYPE_Normal;
			TN->Coordinates.Connect(0, UV);
			// Flatten the normal map toward (0,0,1) on vertical faces so walls don't get smeared detail.
			UMaterialExpressionConstant3Vector* Flat = Cast<UMaterialExpressionConstant3Vector>(
				Make(UMaterialExpressionConstant3Vector::StaticClass(), -700, 300));
			Flat->Constant = FLinearColor(0.0f, 0.0f, 1.0f, 0.0f);
			UMaterialExpressionLinearInterpolate* NL = Cast<UMaterialExpressionLinearInterpolate>(
				Make(UMaterialExpressionLinearInterpolate::StaticClass(), -150, 450));
			NL->A.Connect(0, Flat);
			NL->B.Connect(0, TN);
			NL->Alpha.Connect(0, UpMask);
			UMaterialEditingLibrary::ConnectMaterialProperty(NL, TEXT(""), MP_Normal);
		}
		if (TexRough)
		{
			UMaterialExpressionTextureSample* TR = Cast<UMaterialExpressionTextureSample>(
				Make(UMaterialExpressionTextureSample::StaticClass(), -1100, 700));
			TR->Texture = TexRough;
			TR->SamplerType = SAMPLERTYPE_LinearGrayscale;
			TR->Coordinates.Connect(0, UV);
			ConnectMaskedRoughness(TR, /*R output*/ 1);
		}
		if (TexColor)
		{
			// Subtle grain only: desaturate the albedo and use it to lighten/darken the vertex tint,
			// so the detail reads on dark asphalt without recolouring sidewalks/buildings/lines.
			UMaterialExpressionTextureSample* TC = Cast<UMaterialExpressionTextureSample>(
				Make(UMaterialExpressionTextureSample::StaticClass(), -1100, 950));
			TC->Texture = TexColor;
			TC->SamplerType = SAMPLERTYPE_Color;
			TC->Coordinates.Connect(0, UV);
			UMaterialExpressionDesaturation* Desat = Cast<UMaterialExpressionDesaturation>(
				Make(UMaterialExpressionDesaturation::StaticClass(), -800, 950));
			Desat->Input.Connect(0, TC);
			UMaterialExpressionLinearInterpolate* Grain = Cast<UMaterialExpressionLinearInterpolate>(
				Make(UMaterialExpressionLinearInterpolate::StaticClass(), -550, 950));
			Grain->ConstA = 0.70f; Grain->ConstB = 1.25f;
			Grain->Alpha.Connect(0, Desat);
			UMaterialExpressionMultiply* BaseMul = Cast<UMaterialExpressionMultiply>(
				Make(UMaterialExpressionMultiply::StaticClass(), -300, 250));
			BaseMul->A.Connect(VCOUT_RGB, VC);
			BaseMul->B.Connect(0, Grain);
			BaseColorSource = BaseMul;
		}
	}
	else
	{
		// No textures yet: vary roughness and base tint with world-space noise so flat asphalt still
		// catches Lumen reflections / VSM instead of reading as one dead matte slab.
		UMaterialExpressionNoise* RoughNoise = Cast<UMaterialExpressionNoise>(
			Make(UMaterialExpressionNoise::StaticClass(), -700, 600));
		RoughNoise->Scale = 0.0009f; RoughNoise->OutputMin = 0.55f; RoughNoise->OutputMax = 0.92f;
		ConnectMaskedRoughness(RoughNoise, /*scalar output*/ 0);

		UMaterialExpressionNoise* GrainNoise = Cast<UMaterialExpressionNoise>(
			Make(UMaterialExpressionNoise::StaticClass(), -700, 300));
		GrainNoise->Scale = 0.0013f; GrainNoise->OutputMin = 0.85f; GrainNoise->OutputMax = 1.15f;
		UMaterialExpressionMultiply* BaseMul = Cast<UMaterialExpressionMultiply>(
			Make(UMaterialExpressionMultiply::StaticClass(), -300, 250));
		BaseMul->A.Connect(VCOUT_RGB, VC);
		BaseMul->B.Connect(0, GrainNoise);
		BaseColorSource = BaseMul;
	}

	if (BaseColorSource)
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(BaseColorSource, TEXT(""), MP_BaseColor);
	}

	// Emissive = VertexColor.rgb * VertexColor.a * strength. Lane lines (A>0) glow at dusk/night; every
	// other section authors A=0 and stays matte. VertexColor outputs are unnamed, so address A by index.
	{
		UMaterialExpressionMultiply* EmisCol = Cast<UMaterialExpressionMultiply>(
			Make(UMaterialExpressionMultiply::StaticClass(), -700, -150));
		EmisCol->A.Connect(VCOUT_RGB, VC);
		EmisCol->B.Connect(VCOUT_A, VC);
		UMaterialExpressionMultiply* EmisFinal = Cast<UMaterialExpressionMultiply>(
			Make(UMaterialExpressionMultiply::StaticClass(), -450, -150));
		EmisFinal->A.Connect(0, EmisCol);
		EmisFinal->ConstB = 1.6f;
		UMaterialEditingLibrary::ConnectMaterialProperty(EmisFinal, TEXT(""), MP_EmissiveColor);
	}

	// Two-sided so the single-faced ribbons/walls are never back-face culled.
	Material->TwoSided = true;

	UMaterialEditingLibrary::RecompileMaterial(Material);
	FAssetRegistryModule::AssetCreated(Material);
	Material->MarkPackageDirty();

	// Save to disk so it survives editor restarts / cooks.
	const FString FileName = FPackageName::LongPackageNameToFilename(
		GMaterialPackagePath, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FileName), /*Tree*/ true);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone | RF_Public;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, Material, *FileName, SaveArgs);

	UE_LOG(LogRoadForgeEditor, Log, TEXT("RoadForge material %s -> %s (saved=%d, detailTextures=%d)"),
		GMaterialPackagePath, *FileName, bSaved ? 1 : 0, bHaveDetail ? 1 : 0);

	return Material;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRoadForgeEditorModule, RoadForgeEditor)
