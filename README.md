# RoadForge

**OSM-Based Procedural City Road Generator** · **基于 OSM 的 UE5 程序化城市道路生成器**

English | [简体中文](README.zh-CN.md)

RoadForge is a C++ Unreal Engine project for generating city road scenes from OpenStreetMap data. It converts OSM ways into splines, builds procedural road meshes, adds lane markings, curbs, sidewalks, elevated bridge decks, simple buildings, and instanced street lamps.

The project targets Unreal Engine 5.7 and uses runtime C++ mesh generation rather than a preauthored road asset library.

> If RoadForge is useful to you, a star is very welcome. It helps other people find the project. Thank you!
>
> If you have any ideas for improvements, feel free to reach out: yuuhen@outlook.com

## Screenshots

City blocks and road network generated from OSM data:

![City overview](docs/screenshots/city-overview.png)

Intersection with lane markings, curbs, and sidewalks:

![Intersection](docs/screenshots/intersection.png)

Elevated roads and bridge decks above the ground network:

![Elevated roads](docs/screenshots/elevated-roads.png)

## Features

- Fetch road and building data from the Overpass API, or generate from a local OSM JSON file.
- Project WGS84 coordinates into local Unreal Engine centimetres.
- Build roads, footways, curbs, lane markings, edge lines, elevated decks, junction patches, buildings, facade details, and lamp instances.
- Support OSM tags such as `highway`, `lanes`, `lanes:forward`, `lanes:backward`, `oneway`, `layer`, `bridge`, `tunnel`, `building`, `height`, and `building:levels`.
- Generate editor materials and helper lighting from the RoadForge tools menu.
- Optional spline components for inspecting or editing generated roads in the level.

## Requirements

- Unreal Engine 5.7
- Windows development environment for Unreal C++ projects
- Enabled Unreal plugins:
  - ProceduralMeshComponent
  - PCG
  - ModelingToolsEditorMode, editor only

## Getting Started

Clone the repository and open `ForPCG1.uproject` with Unreal Engine 5.7. If Unreal asks to rebuild modules, allow it.

To build from a Developer Command Prompt or PowerShell, set `UE_ROOT` to your Unreal installation and run:

```powershell
& "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" ForPCG1Editor Win64 Development -Project="$PWD\ForPCG1.uproject" -WaitMutex
```

Example:

```powershell
$env:UE_ROOT = "E:\UE\UE_5.7"
& "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" ForPCG1Editor Win64 Development -Project="$PWD\ForPCG1.uproject" -WaitMutex
```

## Basic Usage

1. Open the project in the Unreal Editor.
2. Use `Tools -> RoadForge -> Add OSM Road Generator to Level`.
3. Select the spawned `OSMRoadGenerator` actor.
4. Set a WGS84 bounding box in the Details panel, or provide a local Overpass JSON file.
5. Click `Fetch And Generate` or `Generate From Local File`.

For public Overpass servers, keep the selected area modest. A bounding box around one to two city blocks is a good starting point. Larger areas can hit rate limits or produce very heavy procedural meshes.

## Tools Menu

All four tools live under `Tools -> RoadForge` in the Unreal Editor.

- **Add OSM Road Generator to Level** - Spawns an `OSMRoadGenerator` at the world origin, generates a road network from the bundled sample data, and adds a cinematic lighting rig. This is the one-click way to get a result on screen right away.
- **Import OSM File (New Generator)...** - Spawns a fresh generator, opens a file picker for any OSM or Overpass JSON export, generates that map, and adds the lighting rig. The fastest way to load a different city.
- **Add Cinematic Lighting Rig** - Creates or retunes the lighting setup: directional sun, sky light, sky atmosphere, volumetric cloud, volumetric height fog, and an unbound post process volume, so the scene is lit close to the reference look.
- **Regenerate RoadForge Material** - Rebuilds the shared PBR material `/RoadForge/M_RoadForge_VC` and wires up the CC0 detail textures if they have been imported.

## OSM Data

This repository does not include downloaded OSM extracts by default. To use local data, download an Overpass JSON response containing road and building ways with geometry:

```text
[out:json][timeout:90];
(
  way["highway"](south,west,north,east);
  way["building"](south,west,north,east);
);
out geom;
```

Replace `south,west,north,east` with your bounding box values. OpenStreetMap data is available under the Open Database License; keep appropriate attribution when distributing generated datasets or derived map content.

## Repository Contents

- `ForPCG1.uproject` - Unreal project descriptor.
- `Source/ForPCG1/` - minimal host project module.
- `Plugins/RoadForge/` - RoadForge runtime and editor plugin.
- `Config/` - project defaults required to open and render the scene.

Generated folders such as `Binaries`, `Intermediate`, `Saved`, downloaded map extracts, private notes, and reference projects are intentionally ignored.

## Known Limitations

Complex multi-level interchanges are still the hardest case. RoadForge includes bridge layer handling, elevated merge patches, and ramp cleanup, but OSM topology varies a lot between cities. Dense flyovers may still need local tuning or manual spline cleanup.

The generated buildings are simple procedural extrusions. They are intended as context massing, not production-ready architecture.

## License

RoadForge is released under the MIT License. See `LICENSE`.
