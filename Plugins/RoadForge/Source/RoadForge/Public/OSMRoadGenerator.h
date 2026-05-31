// Copyright RoadForge Contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IHttpRequest.h"
#include "OSMRoadGenerator.generated.h"

class UProceduralMeshComponent;
class UInstancedStaticMeshComponent;
class USplineComponent;
class UMaterialInterface;
struct FRoadMeshBuffers;

/**
 * AOSMRoadGenerator
 *
 * Procedurally builds a city block from real OpenStreetMap data, entirely in C++ (the
 * "different tech stack" counterpart to the PCG-graph based ProceduralCityBase):
 *
 *   1. Fetch    -> Overpass API query for every `highway` and `building` way in a WGS84 bbox.
 *   2. Project  -> equirectangular (lat,lon) -> local UE centimetres, centred on the bbox.
 *   3. Build    -> road surfaces, sidewalks, dashed-free centre lane lines, raised curbs,
 *                  extruded buildings (ProceduralMesh) and street-lamp poles (InstancedStaticMesh).
 *
 * Drop one in a level, set the bounding box, then click "Fetch And Generate" in the Details panel.
 */
UCLASS(BlueprintType, Blueprintable)
class ROADFORGE_API AOSMRoadGenerator : public AActor
{
	GENERATED_BODY()

public:
	AOSMRoadGenerator();

	// --- Area of interest (WGS84 degrees). Default = a slice of central Taipei (the doc demo). ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Area", meta = (ClampMin = "-90", ClampMax = "90"))
	double MinLatitude = 25.0385;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Area", meta = (ClampMin = "-90", ClampMax = "90"))
	double MaxLatitude = 25.0505;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Area", meta = (ClampMin = "-180", ClampMax = "180"))
	double MinLongitude = 121.5340;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Area", meta = (ClampMin = "-180", ClampMax = "180"))
	double MaxLongitude = 121.5470;

	/** Overpass API endpoint. Swap to a mirror (e.g. overpass.kumi.systems) if rate-limited. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Area")
	FString OverpassEndpoint = TEXT("https://overpass-api.de/api/interpreter");

	/** Optional: read a saved Overpass JSON response from disk instead of hitting the network. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Area", meta = (FilePathFilter = "json"))
	FString LocalOSMJsonPath;

	// --- What to build ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateRoads = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateFootways = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateLaneLines = true;

	/** Solid white edge lines along wide roads (extra styling). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateEdgeLines = true;

	/** Green/bike-coloured side lane strips when OSM tags indicate cycle lanes (cycleway/cycleway:left/right).
	 *  Off by default: the edge-offset strip self-intersects into green "diamonds" on sharp curves / ramps /
	 *  short segments. Re-enable only on flat, gently-curved street grids. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateBikeLanes = false;

	/** Painted direction arrows on one-way / link roads and wider arterials. Off by default — they clutter
	 *  dense junctions; turn on only if you want lane-direction arrows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateDirectionArrows = false;

	/** Fill road junctions (nodes shared by 2+ roads) with an asphalt patch so crossings look clean
	 *  instead of criss-crossing lane lines. Needs the OSM `nodes` arrays (real Overpass data). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateIntersections = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateCurbs = true;

	/** Build side barriers / parapets and support pillars under raised bridges & overpasses (OSM `layer`>0,
	 *  bridge=yes). When OSM has no pier geometry the pillars are synthesised along the deck so elevated
	 *  decks read as real viaducts instead of floating ribbons. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateBridges = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateBuildings = true;

	/** Add procedural facade details (windows + roof parapets) to OSM building prisms. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features", meta = (EditCondition = "bGenerateBuildings"))
	bool bGenerateBuildingDetails = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateStreetLamps = true;

	/** A flat fill plane under the whole bbox. Leave OFF when the level already has a Landscape/terrain
	 *  (otherwise the two surfaces z-fight into dark flickering patches). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Features")
	bool bGenerateGroundPlane = false;

	// --- Tuning ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "0.1"))
	double WidthScale = 1.0;

	/** Road surface height above the actor origin, cm (avoids z-fighting with the ground). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning")
	double RoadZOffset = 3.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "1.0"))
	double CurbHeightCm = 15.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "1.0"))
	double CurbWidthCm = 18.0;

	/** Paint the centre lane line as dashes (like the original's RoadnetDECAL) instead of a solid stripe. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning")
	bool bDashedCenterLine = true;

	/** Length of each painted dash, cm (only used when Dashed Center Line is on). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "20.0", EditCondition = "bDashedCenterLine"))
	double DashLengthCm = 300.0;

	/** Gap between dashes, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "0.0", EditCondition = "bDashedCenterLine"))
	double DashGapCm = 600.0;

	/** Vertical separation between OSM `layer` levels, cm. Bridges/overpasses (layer>0) sit this much
	 *  higher per level so they no longer overlap surface roads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "0.0"))
	double LayerHeightCm = 450.0;

	/** Skip ways below ground (tunnels / negative `layer`) so they don't draw inside the terrain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning")
	bool bSkipTunnels = true;

	/** How far (cm) a bridge/overpass ramps up from ground level at each end before reaching full layer
	 *  height. Larger = gentler slope. The middle of long bridges stays at full height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "100.0"))
	double RampLengthCm = 3500.0;

	/** Spacing between synthesised bridge support pillars along an elevated deck, metres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "5.0", EditCondition = "bGenerateBridges"))
	double BridgePillarSpacingMeters = 22.0;

	/** Height (cm) of the side barrier / parapet wall along a raised bridge deck. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "10.0", EditCondition = "bGenerateBridges"))
	double BridgeBarrierHeightCm = 75.0;

	/** Extra vertical clearance (cm) added on top of a deck's own layer height to avoid roads at different
	 *  OSM layers fighting at shared nodes. Also the small per-overlap z lift applied to crossing surface
	 *  roads that do NOT share an OSM node, so they stop z-fighting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "0.0"))
	double OverlapZBiasCm = 4.0;

	/** Junction patch radius as a multiple of the widest incident road's half-width. Also the radius within
	 *  which lane markings are cut, so a slightly generous value keeps crossings clean. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "0.5", EditCondition = "bGenerateIntersections"))
	double IntersectionRadiusScale = 1.15;

	/** Metres of building height per `building:levels` when no explicit `height` tag exists. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "1.0"))
	double MetersPerLevel = 3.2;

	/** Fallback building height (metres) when neither height nor levels are tagged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "1.0"))
	double DefaultBuildingHeight = 12.0;

	/** Spacing between street lamps along a road, in metres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "5.0"))
	double LampSpacingMeters = 35.0;

	/** Distance between repeated road direction arrows, in metres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "8.0", EditCondition = "bGenerateDirectionArrows"))
	double ArrowSpacingMeters = 55.0;

	/** Distance between building windows along each facade, in metres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning", meta = (ClampMin = "1.5", EditCondition = "bGenerateBuildingDetails"))
	double WindowSpacingMeters = 3.2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning")
	bool bGenerateCollision = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Tuning")
	bool bGenerateOnBeginPlay = false;

	// --- Spline pipeline (the "样条 / spline" half of the chosen "C++ spline + procedural mesh" tech stack). ---

	/** Route each OSM road way through a USplineComponent and build the mesh from resampled spline points. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Spline")
	bool bUseSplines = true;

	/** Catmull-Rom smoothing through the OSM nodes (rounds hard polyline corners). Off = faithful linear polyline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Spline", meta = (EditCondition = "bUseSplines"))
	bool bSmoothSplines = true;

	/** Distance between resampled points along each road spline, cm. Smaller = smoother roads but heavier meshes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Spline", meta = (ClampMin = "20.0", EditCondition = "bUseSplines"))
	double SplineSampleStepCm = 120.0;

	/** Keep an editable USplineComponent per road in the level (selectable/tweakable). Off = transient, used only for math. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Spline", meta = (EditCondition = "bUseSplines"))
	bool bShowSplineComponents = false;

	// --- Materials (optional). If left null, the auto-generated /RoadForge/M_RoadForge_VC is used. ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Material")
	TObjectPtr<UMaterialInterface> OverrideMaterial = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoadForge|Material")
	TObjectPtr<UStaticMesh> LampMesh = nullptr;

	// --- Actions (buttons in the Details panel) ---
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RoadForge|Actions")
	void FetchAndGenerate();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RoadForge|Actions")
	void GenerateFromLocalFile();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RoadForge|Actions")
	void ClearAll();

	/** One-click high-quality preset: finer splines, richer markings, intersections, building details. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RoadForge|Actions")
	void ApplyReferenceQualityPreset();

	/** Open a file picker to choose an OSM/Overpass JSON file, store it in LocalOSMJsonPath and generate.
	 *  Makes importing different OSM exports a single click (editor only). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RoadForge|Actions")
	void ImportOSMFile();

	/** Rebuild the meshes from the current editable road splines (after you have dragged their path points).
	 *  Requires a previous build with Show Spline Components on. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RoadForge|Actions")
	void RebuildFromSplines();

	/** Fill LocalOSMJsonPath with SampleData/taipei_osm.json and generate. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RoadForge|Actions")
	void GenerateTaipeiSample();

	/** Fill LocalOSMJsonPath with SampleData/barcelona_osm.json and generate (larger European street grid sample). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RoadForge|Actions")
	void GenerateBarcelonaSample();

	virtual void BeginPlay() override;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RoadForge")
	TObjectPtr<UProceduralMeshComponent> GeneratedMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RoadForge")
	TObjectPtr<UInstancedStaticMeshComponent> LampInstances;

	/** Reusable, transient spline used purely for resampling math when bShowSplineComponents is off. */
	UPROPERTY(Transient)
	TObjectPtr<USplineComponent> SamplingSpline;

	/** Editable spline components kept in the level when bShowSplineComponents is on (one per road way). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RoadForge")
	TArray<TObjectPtr<USplineComponent>> RoadSplineComponents;

	/** OSM way id matching each entry in RoadSplineComponents (so an edited spline can be matched back to its
	 *  road way when rebuilding). Parallel array; kept in sync with RoadSplineComponents. */
	UPROPERTY()
	TArray<int64> RoadSplineWayIds;

private:
	double OriginLatitude = 0.0;
	double OriginLongitude = 0.0;
	bool bRequestInFlight = false;

	/** Last OSM/Overpass JSON that was built, cached so Rebuild From Splines works without re-reading/fetching. */
	FString CachedOSMJson;

	/** When true, BuildFromJson replaces a road way's OSM polyline with the user-edited spline points in
	 *  EditedSplinePoints (keyed by OSM way id). Set only for the duration of RebuildFromSplines. */
	bool bApplyEditedSplines = false;
	TMap<int64, TArray<FVector2D>> EditedSplinePoints;

	FString BuildOverpassQuery() const;
	void OnOverpassResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);

	/** Parse an Overpass JSON document and (re)build everything. Returns number of elements built. */
	int32 BuildFromJson(const FString& JsonText);

	/** Equirectangular projection: WGS84 (lat,lon) -> local cm (X = North, Y = East). */
	FVector2D ProjectLatLon(double Lat, double Lon) const;

	UMaterialInterface* ResolveMaterial() const;

	// --- Spline pipeline helpers ---

	/** Load the projected polyline into Spline (smooth or linear) and resample it at SplineSampleStepCm. */
	void BuildSplineSamples(const TArray<FVector2D>& In, TArray<FVector2D>& Out, USplineComponent* Spline) const;

	/** Lazily create / reuse the transient sampling spline (no scene presence). */
	USplineComponent* AcquireSamplingSpline();

	/** Create a registered, editable spline component attached to this actor and track it for cleanup. */
	USplineComponent* CreatePersistentRoadSpline(int64 WayId);

	/** Destroy every persistent road spline component created by a previous build. */
	void ClearSplineComponents();
};
