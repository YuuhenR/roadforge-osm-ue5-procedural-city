// Copyright RoadForge Contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class UMaterialInterface;

/**
 * Editor module for RoadForge. Exposes Tools menu entries to regenerate the shared PBR material
 * (/RoadForge/M_RoadForge_VC), add a road generator, and drop in a cinematic lighting rig
 * (Sun + Sky + Atmosphere + volumetric fog + post-process) so generated roads are lit like the
 * reference project. Startup intentionally avoids writing assets because UE's validation system is
 * still registering validators during editor load.
 */
class FRoadForgeEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void OnGenerateMaterial();

	/** Spawn an AOSMRoadGenerator at the world origin and select it (so the user sees the Details buttons). */
	void OnAddGenerator();

	/** Spawn a generator, open a file picker to import any OSM/Overpass JSON, generate it, and light the level. */
	void OnImportOSM();

	/** Spawn (or retune) a cinematic lighting rig in the current editor world. */
	void OnAddLightingRig();

	/**
	 * Startup hook. Kept for hot-reload compatibility, but it only loads existing assets; it does not
	 * create or save assets during editor startup.
	 */
	void EnsureDefaultMaterialDeferred();

	/** Create (and save) /RoadForge/M_RoadForge_VC if missing (or always when bForceRecreate). */
	static UMaterialInterface* EnsureVertexColorMaterial(bool bForceRecreate);

	FDelegateHandle EngineInitCompleteHandle;
};
