// Copyright RoadForge Contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h" // FProcMeshTangent

/**
 * A plain (non-UObject) vertex buffer that maps 1:1 onto the arguments of
 * UProceduralMeshComponent::CreateMeshSection_LinearColor. Every generator appends into one of
 * these; the actor then hands each buffer to a mesh section.
 *
 * Vertex-color convention used across RoadForge (read by the auto-generated M_RoadForge_VC):
 *   RGB = base colour,  A = emissive strength (0 = matte, 1 = glowing -> used for lane lines).
 */
struct ROADFORGE_API FRoadMeshBuffers
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UV;
	TArray<FLinearColor> Color;
	TArray<FProcMeshTangent> Tangents;

	int32 Num() const { return Vertices.Num(); }

	void Reset()
	{
		Vertices.Reset();
		Triangles.Reset();
		Normals.Reset();
		UV.Reset();
		Color.Reset();
		Tangents.Reset();
	}
};

namespace RoadForgeMesh
{
	/**
	 * Sweep a flat (horizontal) ribbon along a polyline. The ribbon spans the lateral interval
	 * [LatRight, LatLeft] measured along the per-vertex left-hand normal (centimetres). Mitred at
	 * interior joints so the width stays constant on bends. Normals point +Z.
	 *
	 * If ZPerVertex is supplied (one entry per point), each vertex sits at ZPerVertex[i] + Z, which
	 * lets a road follow a sloped elevation profile (e.g. a bridge ramping up). Otherwise Z is flat.
	 */
	ROADFORGE_API void AppendFlatRibbon(const TArray<FVector2D>& Pts, double LatLeft, double LatRight,
		double Z, const FLinearColor& Color, FRoadMeshBuffers& Out, const TArray<double>* ZPerVertex = nullptr);

	/**
	 * Like AppendFlatRibbon, but the ribbon is broken into dashes of DashLen cm separated by GapLen cm
	 * gaps (arc-length parametrised, continuous across vertices). Used for dashed centre lane lines.
	 * HalfWidth is the half-width of the painted stripe (cm). Faces point +Z. ZPerVertex as above.
	 */
	ROADFORGE_API void AppendDashedRibbon(const TArray<FVector2D>& Pts, double HalfWidth, double Z,
		double DashLen, double GapLen, const FLinearColor& Color, FRoadMeshBuffers& Out, const TArray<double>* ZPerVertex = nullptr);

	/**
	 * Sweep a vertical strip (a low wall) along a polyline at a constant lateral offset, from BaseZ
	 * up to TopZ. NormalSign (+1 / -1) selects which horizontal direction the face normal points.
	 * If ZPerVertex is supplied, BaseZ/TopZ are offsets added on top of the per-vertex elevation.
	 */
	ROADFORGE_API void AppendVerticalStrip(const TArray<FVector2D>& Pts, double Lat,
		double BaseZ, double TopZ, double NormalSign, const FLinearColor& Color, FRoadMeshBuffers& Out,
		const TArray<double>* ZPerVertex = nullptr);

	/** Add a single quad (A->B->C->D) with one shared normal. */
	ROADFORGE_API void AppendQuad(const FVector& A, const FVector& B, const FVector& C, const FVector& D,
		const FVector& Normal, const FLinearColor& Color, FRoadMeshBuffers& Out);

	/** Add a flat horizontal disc (triangle fan) centred at C with the given radius. Used for junctions. */
	ROADFORGE_API void AppendDisc(const FVector2D& C, double Radius, int32 Segments,
		double Z, const FLinearColor& Color, FRoadMeshBuffers& Out);

	/**
	 * Fill a road intersection with the CONVEX HULL of the incident roads' mouth corners (the "RoadPatcher"
	 * idea): each arm contributes two corner points at +/- its lateral half-width, pushed out along the arm
	 * by ArmReach; the hull of those corners is fanned from its centroid. A hull is always a simple, hole-free
	 * polygon, so T/Y/skewed junctions can never produce the self-intersecting "diamond gap" an angle-sorted
	 * fan did, while still hugging the carriageway width (no blobby disc over-inflation).
	 *
	 *   ArmDirs  : unit-ish directions pointing from Centre outward along each incident road.
	 *   ArmHalfW : the real road half-width of each arm (cm) -> lateral spread of that arm's mouth.
	 *   ArmReach : how far along each arm the mouth sits (cm). Set this slightly beyond the radius the road
	 *              ribbons were cut at, so the patch always laps over the road ends with no terrain gap.
	 */
	ROADFORGE_API void AppendIntersectionPolygon(const FVector2D& Centre, const TArray<FVector2D>& ArmDirs,
		const TArray<double>& ArmHalfW, const TArray<double>& ArmReach, double Z,
		const FLinearColor& Color, FRoadMeshBuffers& Out);

	/**
	 * Sweep a box-section column (4 side walls + a top cap) between BaseZ and TopZ centred at Centre, with
	 * the section HalfLong x HalfWide oriented so its long axis follows Dir. Used to drop support pillars
	 * under elevated bridge/overpass decks (the "extend via algorithm" step when OSM has no pier geometry).
	 */
	ROADFORGE_API void AppendBoxColumn(const FVector2D& Centre, const FVector2D& Dir, double HalfLong,
		double HalfWide, double BaseZ, double TopZ, const FLinearColor& Color, FRoadMeshBuffers& Out);

	/**
	 * Produce a copy of a polyline shifted sideways by Lateral cm along the (mitred) left-hand normal.
	 * Positive = left of travel direction. Used to place lane-divider lines at lane boundaries.
	 */
	ROADFORGE_API void OffsetPolyline(const TArray<FVector2D>& In, double Lateral, TArray<FVector2D>& Out);

	/**
	 * Break a polyline into the runs of consecutive indices whose points lie OUTSIDE every given circle.
	 * Used to stop lane markings at junction patches so no line crosses an intersection. Runs shorter than
	 * 2 points are dropped. With no circles, returns a single run covering the whole polyline.
	 */
	ROADFORGE_API void SplitIndexRunsOutsideCircles(const TArray<FVector2D>& In,
		const TArray<FVector2D>& Centers, const TArray<double>& Radii, TArray<TArray<int32>>& OutRuns);

	/**
	 * Extrude a closed polygon footprint into a prism: vertical walls + a flat triangulated roof.
	 * Ring is the footprint in cm (without a duplicated closing vertex). Winding is auto-corrected.
	 */
	ROADFORGE_API void AppendPolygonExtrude(const TArray<FVector2D>& Ring, double BaseZ, double Height,
		const FLinearColor& WallColor, const FLinearColor& RoofColor, FRoadMeshBuffers& Out);

	/**
	 * Ear-clipping triangulation of a simple (non self-intersecting) polygon. Returns index triples
	 * into Poly. Input may be CW or CCW; output triangles are wound CCW (front face up for a roof).
	 */
	ROADFORGE_API bool TriangulatePolygon(const TArray<FVector2D>& Poly, TArray<int32>& OutTriangles);

	/**
	 * Walk a polyline and emit a transform every Spacing centimetres, offset laterally by
	 * LateralOffset (along the left-hand normal), at world height ZHeight, with the given Scale.
	 * Used to scatter street-lamp / prop instances along roads.
	 */
	ROADFORGE_API void SamplePointsAlong(const TArray<FVector2D>& Pts, double Spacing, double LateralOffset,
		double ZHeight, const FVector& Scale, TArray<FTransform>& OutTransforms, const TArray<double>* ZPerVertex = nullptr);
}
