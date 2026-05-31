// Copyright RoadForge Contributors. Licensed under the MIT License.

using UnrealBuildTool;

public class RoadForge : ModuleRules
{
	public RoadForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ProceduralMeshComponent", // Runtime mesh generation backend
			"PCG",                     // Custom PCG node (UPCGSettings/FPCGElement) drives generation
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTP",      // Download OSM data from the Overpass API
			"Json",      // Parse the Overpass JSON response
			"XmlParser", // Parse raw OSM .osm XML (node/way/nd/tag)
		});

		// Editor-only: the "Import OSM File" action opens a native file picker via DesktopPlatform.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("DesktopPlatform");
		}
	}
}
