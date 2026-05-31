// Copyright RoadForge Contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Format-agnostic OSM loader. Turns either a raw OpenStreetMap **.osm XML** export (where <node>s are listed
 * once and <way>s reference them by <nd ref>) or an **Overpass `out geom` .json** response into one common
 * "Overpass-style" root JSON object so the rest of the generator has a single code path:
 *
 *   { "elements": [ { "type":"way", "id":<id>, "tags":{..}, "nodes":[id,..], "geometry":[{lat,lon},..] }, .. ] }
 *
 * The XML path resolves each way's geometry from the node table; the JSON path is returned as-is.
 */
namespace RoadForgeOSM
{
	/** Detect the format of a file on disk (by extension, then by sniffing the first non-space byte). */
	ROADFORGE_API bool IsXmlFile(const FString& Path);

	/** Load + normalise an OSM file into OutRoot. Returns false (with OutError set) on failure. */
	ROADFORGE_API bool LoadOSM(const FString& Path, TSharedPtr<FJsonObject>& OutRoot, FString& OutError);

	/** Like LoadOSM but returns Overpass-style JSON **text** (a .json file is returned verbatim; a .osm XML
	 *  file is converted), so the existing string-based BuildFromJson path works unchanged for both formats. */
	ROADFORGE_API bool LoadOSMText(const FString& Path, FString& OutJsonText, FString& OutError);

	/** Parse an in-memory OSM XML string into the Overpass-style root. */
	ROADFORGE_API bool ParseXml(const FString& XmlText, TSharedPtr<FJsonObject>& OutRoot, FString& OutError);
}
