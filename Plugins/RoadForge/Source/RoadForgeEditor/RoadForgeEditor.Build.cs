// Copyright RoadForge Contributors. Licensed under the MIT License.

using UnrealBuildTool;

public class RoadForgeEditor : ModuleRules
{
	public RoadForgeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",        // UMaterialFactoryNew, GEditor, spawn/select actors
			"MaterialEditor",  // UMaterialEditingLibrary
			"AssetRegistry",   // FAssetRegistryModule::AssetCreated
			"ToolMenus",       // Tools menu entry
			"Slate",
			"SlateCore",
			"Projects",        // IPluginManager
			"RoadForge",       // AOSMRoadGenerator (spawn from the Tools menu)
			"PCG",             // Build the PCG_RoadForge graph asset programmatically
			"PCGEditor",       // PCG graph asset authoring helpers
			"DesktopPlatform", // Native file picker for Import OSM
		});
	}
}
