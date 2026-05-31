// Copyright RoadForge Contributors. Licensed under the MIT License.

#include "OSMRoadGenerator.h"

#include "RoadForgeMeshUtils.h"
#include "RoadForgeOSM.h"

#include "ProceduralMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Algo/Reverse.h"

#if WITH_EDITOR
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogRoadForge, Log, All);

namespace
{
	constexpr double kEarthRadiusM = 6378137.0;
	constexpr double kCmPerM = 100.0;

	// Mesh section indices on the single ProceduralMeshComponent.
	enum : int32
	{
		SEC_Roads = 0,
		SEC_Footways = 1,
		SEC_LaneLines = 2,
		SEC_Curbs = 3,
		SEC_Buildings = 4,
		SEC_Ground = 5,
		SEC_Junctions = 6,
		SEC_BuildingDetails = 7,
		SEC_Bridges = 8,
	};

	/** highway tag -> base width (m) + category (0 vehicle, 1 pedestrian). false => skip this way. */
	bool ClassifyHighway(const FString& Hw, double& OutWidthM, int32& OutCategory, int32& OutDefaultLanes)
	{
		OutCategory = 0;
		OutDefaultLanes = 2;

		// Not a drivable/walkable surface (markers, areas, lifecycle tags) -> skip entirely.
		if (Hw == TEXT("construction") || Hw == TEXT("proposed") || Hw == TEXT("razed") || Hw == TEXT("abandoned")
			|| Hw == TEXT("raceway") || Hw == TEXT("bus_guideway") || Hw == TEXT("escape") || Hw == TEXT("corridor")
			|| Hw == TEXT("elevator") || Hw == TEXT("platform") || Hw == TEXT("rest_area") || Hw == TEXT("services")
			|| Hw == TEXT("bus_stop") || Hw == TEXT("traffic_island") || Hw == TEXT("street_lamp")
			|| Hw == TEXT("speed_camera") || Hw == TEXT("turning_circle") || Hw == TEXT("stop") || Hw == TEXT("give_way"))
		{
			return false;
		}

		if (Hw == TEXT("motorway") || Hw == TEXT("trunk")) { OutWidthM = 14.0; OutDefaultLanes = 4; }
		else if (Hw == TEXT("motorway_link") || Hw == TEXT("trunk_link")) { OutWidthM = 7.0; OutDefaultLanes = 1; }
		else if (Hw == TEXT("primary")) { OutWidthM = 10.5; OutDefaultLanes = 3; }
		else if (Hw == TEXT("primary_link")) { OutWidthM = 6.0; OutDefaultLanes = 1; }
		else if (Hw == TEXT("secondary")) { OutWidthM = 8.5; OutDefaultLanes = 2; }
		else if (Hw == TEXT("secondary_link")) { OutWidthM = 5.5; OutDefaultLanes = 1; }
		else if (Hw == TEXT("tertiary") || Hw == TEXT("tertiary_link")) { OutWidthM = 7.0; OutDefaultLanes = 2; }
		else if (Hw == TEXT("residential") || Hw == TEXT("unclassified") || Hw == TEXT("living_street") || Hw == TEXT("road")) { OutWidthM = 6.0; OutDefaultLanes = 2; }
		else if (Hw == TEXT("service")) { OutWidthM = 3.5; OutDefaultLanes = 1; }
		else if (Hw == TEXT("pedestrian")) { OutWidthM = 5.0; OutCategory = 1; }
		else if (Hw == TEXT("footway") || Hw == TEXT("path") || Hw == TEXT("cycleway") || Hw == TEXT("steps") || Hw == TEXT("track") || Hw == TEXT("bridleway")) { OutWidthM = 2.0; OutCategory = 1; }
		else { OutWidthM = 5.0; OutDefaultLanes = 2; }
		return true;
	}

	/** OSM `layer` (and bridge/tunnel implications). Returns the integer layer; 0 = ground level. */
	int32 ParseLayer(const TSharedPtr<FJsonObject>& Tags)
	{
		int32 Layer = 0;
		FString S;
		if (Tags->TryGetStringField(TEXT("layer"), S))
		{
			Layer = FCString::Atoi(*S);
		}
		// A bridge with no explicit layer still sits above the surface; a tunnel sits below.
		if (Layer == 0)
		{
			if (Tags->TryGetStringField(TEXT("bridge"), S) && S != TEXT("no")) { Layer = 1; }
			else if (Tags->TryGetStringField(TEXT("tunnel"), S) && S != TEXT("no")) { Layer = -1; }
		}
		return Layer;
	}

	bool IsOneway(const TSharedPtr<FJsonObject>& Tags)
	{
		FString S;
		if (Tags->TryGetStringField(TEXT("oneway"), S))
		{
			return S == TEXT("yes") || S == TEXT("true") || S == TEXT("1") || S == TEXT("-1");
		}
		return false;
	}

	/** Everything we derive once from a highway way's tags (shared by the junction pre-pass and the build). */
	struct FRoadSpec
	{
		int32 Category = 0;   // 0 vehicle, 1 pedestrian
		int32 NumLanes = 2;
		int32 LanesForward = 0;   // OSM lanes:forward  (0 = unspecified)
		int32 LanesBackward = 0;  // OSM lanes:backward (0 = unspecified)
		bool bOneway = false;
		bool bBridge = false;     // bridge=yes (or implied by layer>0): build deck barriers + pillars
		int32 Layer = 0;
		int32 Rank = 0;           // importance 1 (service) .. 7 (motorway); bigger draws fractionally higher
		double WidthM = 5.0;
		double HalfWCm = 0.0;
	};

	/** Importance rank used to break z-fighting ties between coplanar roads that don't share an OSM node. */
	int32 HighwayRank(const FString& Hw)
	{
		if (Hw == TEXT("motorway") || Hw == TEXT("motorway_link")) { return 7; }
		if (Hw == TEXT("trunk") || Hw == TEXT("trunk_link")) { return 6; }
		if (Hw == TEXT("primary") || Hw == TEXT("primary_link")) { return 5; }
		if (Hw == TEXT("secondary") || Hw == TEXT("secondary_link")) { return 4; }
		if (Hw == TEXT("tertiary") || Hw == TEXT("tertiary_link")) { return 3; }
		if (Hw == TEXT("residential") || Hw == TEXT("unclassified") || Hw == TEXT("living_street") || Hw == TEXT("road")) { return 2; }
		return 1; // service and everything else minor
	}

	bool ComputeRoadSpec(const TSharedPtr<FJsonObject>& Tags, const FString& Hw, double WidthScale, FRoadSpec& Out)
	{
		double WidthM = 5.0; int32 Cat = 0; int32 DefaultLanes = 2;
		if (!ClassifyHighway(Hw, WidthM, Cat, DefaultLanes))
		{
			return false;
		}
		Out.Category = Cat;
		Out.Layer = ParseLayer(Tags);
		Out.bOneway = IsOneway(Tags);
		Out.Rank = HighwayRank(Hw);
		{
			FString S;
			Out.bBridge = (Tags->TryGetStringField(TEXT("bridge"), S) && S != TEXT("no")) || Out.Layer > 0;
			// Many OSM ways mark an overpass as bridge=yes without an explicit layer=1. Treat that as one
			// elevated layer; otherwise the "bridge" road is generated at ground height and visibly intersects
			// the road below.
			if (Out.bBridge && Out.Layer == 0 && Cat == 0)
			{
				Out.Layer = 1;
			}
		}

		int32 NumLanes = DefaultLanes;
		if (Cat == 0)
		{
			FString LanesStr;
			if (Tags->TryGetStringField(TEXT("lanes"), LanesStr))
			{
				const int32 L = FCString::Atoi(*LanesStr);
				if (L > 0) { NumLanes = L; }
			}
			// Per-direction lane counts let us place the centre line at the real split, not the geometry centre.
			FString DirStr;
			if (Tags->TryGetStringField(TEXT("lanes:forward"), DirStr))  { Out.LanesForward  = FMath::Max(0, FCString::Atoi(*DirStr)); }
			if (Tags->TryGetStringField(TEXT("lanes:backward"), DirStr)) { Out.LanesBackward = FMath::Max(0, FCString::Atoi(*DirStr)); }
			// If only one side is tagged, infer the other so the pair stays consistent with the total.
			if (Out.LanesForward > 0 && Out.LanesBackward == 0 && Out.LanesForward < NumLanes)  { Out.LanesBackward = NumLanes - Out.LanesForward; }
			if (Out.LanesBackward > 0 && Out.LanesForward == 0 && Out.LanesBackward < NumLanes) { Out.LanesForward  = NumLanes - Out.LanesBackward; }

			if (Out.bOneway)
			{
				// A oneway way is usually ONE carriageway of a divided road (motorway/trunk/primary are almost
				// always mapped as separate forward/backward ways). Draw it at just its own lanes' width so the
				// two carriageways keep their real median gap instead of each ballooning to the full bidirectional
				// classification width and overlapping/interweaving into one another.
				WidthM = FMath::Max(NumLanes * 3.5, 3.5);
			}
			else
			{
				WidthM = FMath::Max(WidthM, NumLanes * 3.4);
			}
		}
		FString WidthStr;
		if (Tags->TryGetStringField(TEXT("width"), WidthStr))
		{
			const double W = FCString::Atod(*WidthStr);
			if (W > 0.0) { WidthM = W; }
		}

		Out.NumLanes = NumLanes;
		Out.WidthM = WidthM;
		Out.HalfWCm = 0.5 * WidthM * WidthScale * kCmPerM;
		return true;
	}

	double ParseBuildingHeight(const TSharedPtr<FJsonObject>& Tags, double MetersPerLevel, double DefaultHeight)
	{
		FString S;
		if (Tags->TryGetStringField(TEXT("height"), S))
		{
			const double V = FCString::Atod(*S); // tolerant of trailing " m"
			if (V > 0.0) { return V; }
		}
		if (Tags->TryGetStringField(TEXT("building:levels"), S))
		{
			const double L = FCString::Atod(*S);
			if (L > 0.0) { return L * MetersPerLevel; }
		}
		return DefaultHeight;
	}

	void PickBuildingColors(int64 Id, FLinearColor& OutWall, FLinearColor& OutRoof)
	{
		// Brighter, more varied facade palette (sandstone / concrete / glass / brick / beige). The previous
		// tints were so dark that walls read as black slabs in shadow; these mid-high albedos survive shadow
		// and Lumen GI while still looking like real materials.
		static const FLinearColor Palette[] = {
			FLinearColor(0.84f, 0.79f, 0.70f, 0.0f), // warm sandstone
			FLinearColor(0.76f, 0.77f, 0.80f, 0.0f), // light concrete
			FLinearColor(0.88f, 0.86f, 0.82f, 0.0f), // off-white render
			FLinearColor(0.64f, 0.71f, 0.78f, 0.0f), // cool glass-grey
			FLinearColor(0.80f, 0.67f, 0.56f, 0.0f), // terracotta
			FLinearColor(0.72f, 0.75f, 0.72f, 0.0f), // pale green-grey
			FLinearColor(0.67f, 0.73f, 0.83f, 0.0f), // blue glass
			FLinearColor(0.82f, 0.76f, 0.64f, 0.0f), // beige
		};
		const int32 NumPalette = sizeof(Palette) / sizeof(Palette[0]);
		// Knuth multiplicative hash spreads consecutive OSM ids across the palette far better than (id % n).
		const uint32 H = static_cast<uint32>(static_cast<uint64>(Id) * 2654435761u);
		OutWall = Palette[H % NumPalette];
		// Deterministic +/-8% value jitter so two buildings sharing a palette slot still differ slightly.
		const float Jit = 0.92f + 0.16f * (((H >> 9) & 0xFF) / 255.0f);
		OutWall.R = FMath::Clamp(OutWall.R * Jit, 0.0f, 1.0f);
		OutWall.G = FMath::Clamp(OutWall.G * Jit, 0.0f, 1.0f);
		OutWall.B = FMath::Clamp(OutWall.B * Jit, 0.0f, 1.0f);
		OutRoof = OutWall * 0.85f; // roofs a touch darker than walls, but never black
		OutRoof.A = 0.0f;
	}

	double SignedArea2D(const TArray<FVector2D>& P)
	{
		double A = 0.0;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const FVector2D& a = P[i];
			const FVector2D& b = P[(i + 1) % P.Num()];
			A += a.X * b.Y - b.X * a.Y;
		}
		return 0.5 * A;
	}

	bool HasMeaningfulCycleway(const TSharedPtr<FJsonObject>& Tags)
	{
		static const TCHAR* Keys[] = {
			TEXT("cycleway"), TEXT("cycleway:left"), TEXT("cycleway:right"), TEXT("cycleway:both")
		};
		for (const TCHAR* Key : Keys)
		{
			FString Value;
			if (Tags->TryGetStringField(Key, Value)
				&& !Value.IsEmpty()
				&& Value != TEXT("no")
				&& Value != TEXT("none")
				&& Value != TEXT("opposite"))
			{
				return true;
			}
		}
		return false;
	}

	bool ShouldDrawDirectionArrows(const TSharedPtr<FJsonObject>& Tags, const FString& Highway, const FRoadSpec& Spec)
	{
		if (Spec.Category != 0 || Spec.WidthM < 5.0)
		{
			return false;
		}
		if (Spec.bOneway || Highway.EndsWith(TEXT("_link")))
		{
			return true;
		}
		// Arterials still benefit from sparse directional arrows even when two-way.
		return Highway == TEXT("motorway") || Highway == TEXT("trunk") || Highway == TEXT("primary") || Highway == TEXT("secondary");
	}

	// --- Polyline cleanup -------------------------------------------------------------------------------
	// Raw OSM way geometry has GPS noise, near-duplicate nodes and the odd back-tracking "spike" node. When
	// such a polyline is swept into a ribbon (and especially when run through a Catmull-Rom spline) those
	// defects fold the ribbon over itself into thin self-intersecting "diamonds". Cleaning the polyline so it
	// is as straight as the data allows removes the cause rather than the symptom.

	// Drop consecutive points closer than MinSegCm, always keeping the true first/last vertex.
	void DedupPolyline(TArray<FVector2D>& P, double MinSegCm)
	{
		const int32 N = P.Num();
		if (N < 3) { return; }
		const double MinSq = MinSegCm * MinSegCm;
		TArray<FVector2D> Out;
		Out.Reserve(N);
		Out.Add(P[0]);
		for (int32 i = 1; i < N - 1; ++i)
		{
			if (FVector2D::DistSquared(P[i], Out.Last()) >= MinSq) { Out.Add(P[i]); }
		}
		if (Out.Num() >= 2 && FVector2D::DistSquared(P[N - 1], Out.Last()) < MinSq) { Out.Last() = P[N - 1]; }
		else { Out.Add(P[N - 1]); }
		P = MoveTemp(Out);
	}

	// Ramer-Douglas-Peucker: keep only vertices that deviate more than Eps from the simplified chord. This
	// straightens noisy-but-straight roads so the spline through them does not wiggle.
	void RDPKeep(const TArray<FVector2D>& P, int32 First, int32 Last, double EpsSq, TArray<int32>& Keep)
	{
		double MaxD = 0.0; int32 Idx = -1;
		const FVector2D A = P[First], B = P[Last];
		const FVector2D AB = B - A;
		const double ABLen2 = AB.SizeSquared();
		for (int32 i = First + 1; i < Last; ++i)
		{
			double D2;
			if (ABLen2 < KINDA_SMALL_NUMBER) { D2 = FVector2D::DistSquared(P[i], A); }
			else
			{
				const double T = FMath::Clamp(FVector2D::DotProduct(P[i] - A, AB) / ABLen2, 0.0, 1.0);
				D2 = FVector2D::DistSquared(P[i], A + AB * T);
			}
			if (D2 > MaxD) { MaxD = D2; Idx = i; }
		}
		if (MaxD > EpsSq && Idx > 0)
		{
			RDPKeep(P, First, Idx, EpsSq, Keep);
			RDPKeep(P, Idx, Last, EpsSq, Keep);
		}
		else
		{
			Keep.Add(Last);
		}
	}

	// Remove interior points where the path reverses more sharply than MaxTurnDot (dot of in/out headings).
	// e.g. -0.5 drops turns sharper than 120 deg, which in road data are almost always artefacts.
	void RemoveSpikes(TArray<FVector2D>& P, double MaxTurnDot)
	{
		for (int32 Pass = 0; Pass < 2 && P.Num() >= 3; ++Pass)
		{
			TArray<FVector2D> Out;
			Out.Reserve(P.Num());
			Out.Add(P[0]);
			for (int32 i = 1; i < P.Num() - 1; ++i)
			{
				const FVector2D In = (P[i] - Out.Last()).GetSafeNormal();
				const FVector2D Og = (P[i + 1] - P[i]).GetSafeNormal();
				if (!In.IsNearlyZero() && !Og.IsNearlyZero() && FVector2D::DotProduct(In, Og) < MaxTurnDot)
				{
					continue; // skip the spike vertex
				}
				Out.Add(P[i]);
			}
			Out.Add(P.Last());
			if (Out.Num() == P.Num()) { P = MoveTemp(Out); break; } // converged
			P = MoveTemp(Out);
		}
	}

	void CleanPolyline(TArray<FVector2D>& P)
	{
		if (P.Num() < 3) { return; }
		DedupPolyline(P, 40.0);
		if (P.Num() >= 3)
		{
			TArray<int32> Keep; Keep.Reserve(P.Num()); Keep.Add(0);
			RDPKeep(P, 0, P.Num() - 1, 35.0 * 35.0, Keep);
			TArray<FVector2D> S; S.Reserve(Keep.Num());
			for (int32 Idx : Keep) { S.Add(P[Idx]); }
			P = MoveTemp(S);
		}
		RemoveSpikes(P, -0.5);
	}

	// Squared distance from point P to segment [A,B].
	double DistSqPointSeg(const FVector2D& Pt, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const double L2 = AB.SizeSquared();
		if (L2 < KINDA_SMALL_NUMBER) { return FVector2D::DistSquared(Pt, A); }
		const double T = FMath::Clamp(FVector2D::DotProduct(Pt - A, AB) / L2, 0.0, 1.0);
		return FVector2D::DistSquared(Pt, A + AB * T);
	}

	void AppendPaintedBox(const FVector2D& Centre, const FVector2D& Dir, double Length, double Width,
		double Z, const FLinearColor& Color, FRoadMeshBuffers& Out)
	{
		const FVector2D D = Dir.GetSafeNormal();
		if (D.IsNearlyZero()) { return; }
		const FVector2D P(-D.Y, D.X);
		const FVector2D A = Centre - D * (Length * 0.5) - P * (Width * 0.5);
		const FVector2D B = Centre - D * (Length * 0.5) + P * (Width * 0.5);
		const FVector2D C = Centre + D * (Length * 0.5) + P * (Width * 0.5);
		const FVector2D E = Centre + D * (Length * 0.5) - P * (Width * 0.5);
		RoadForgeMesh::AppendQuad(
			FVector(A.X, A.Y, Z), FVector(B.X, B.Y, Z), FVector(C.X, C.Y, Z), FVector(E.X, E.Y, Z),
			FVector::UpVector, Color, Out);
	}

	void AppendDirectionArrow(const FVector2D& Centre, const FVector2D& Dir, double Z,
		const FLinearColor& Color, FRoadMeshBuffers& Out)
	{
		const FVector2D D = Dir.GetSafeNormal();
		if (D.IsNearlyZero()) { return; }
		const FVector2D P(-D.Y, D.X);

		// Minimal decal-like arrow made from three painted quads: one shaft and a V-shaped arrow head.
		AppendPaintedBox(Centre - D * 45.0, D, 160.0, 28.0, Z, Color, Out);
		AppendPaintedBox(Centre + D * 55.0 + P * 28.0, (D - P * 0.75).GetSafeNormal(), 105.0, 28.0, Z, Color, Out);
		AppendPaintedBox(Centre + D * 55.0 - P * 28.0, (D + P * 0.75).GetSafeNormal(), 105.0, 28.0, Z, Color, Out);
	}

	void AppendDirectionArrowsAlong(const TArray<FVector2D>& Pts, const TArray<double>& ZPerVertex,
		double LateralOffset, double SpacingCm, const FLinearColor& Color, FRoadMeshBuffers& Out)
	{
		if (Pts.Num() < 2 || SpacingCm <= 1.0) { return; }
		double NextS = SpacingCm * 0.5;
		double Accum = 0.0;
		for (int32 i = 0; i < Pts.Num() - 1; ++i)
		{
			const FVector2D Seg = Pts[i + 1] - Pts[i];
			const double SegLen = Seg.Size();
			if (SegLen < KINDA_SMALL_NUMBER) { continue; }
			const FVector2D Dir = Seg / SegLen;
			const FVector2D Perp(-Dir.Y, Dir.X);
			while (NextS <= Accum + SegLen)
			{
				const double T = (NextS - Accum) / SegLen;
				const FVector2D Pos = Pts[i] + Seg * T + Perp * LateralOffset;
				const double Z = FMath::Lerp(ZPerVertex[i], ZPerVertex[i + 1], T) + 1.2;
				AppendDirectionArrow(Pos, Dir, Z, Color, Out);
				NextS += SpacingCm;
			}
			Accum += SegLen;
		}
	}

	void AppendBuildingFacadeDetails(const TArray<FVector2D>& InputRing, double BaseZ, double Height,
		double MetersPerLevel, double WindowSpacingMeters, FRoadMeshBuffers& Out)
	{
		if (InputRing.Num() < 3 || Height < 320.0) { return; }
		TArray<FVector2D> Ring = InputRing;
		if (SignedArea2D(Ring) < 0.0)
		{
			Algo::Reverse(Ring);
		}

		const double FloorH = FMath::Max(MetersPerLevel * kCmPerM, 260.0);
		const int32 Floors = FMath::Clamp(FMath::FloorToInt(Height / FloorH), 1, 48);
		const double WindowSpacing = FMath::Max(WindowSpacingMeters * kCmPerM, 180.0);
		// Glass that reads as glass (cool blue-grey), not a black hole; warmer/brighter lit windows that
		// actually glow at dusk; a lighter parapet so the roofline doesn't go black.
		const FLinearColor WindowLit(1.0f, 0.82f, 0.50f, 0.65f);   // A = emissive strength
		const FLinearColor WindowGlass(0.34f, 0.42f, 0.52f, 0.0f); // unlit reflective glass
		const FLinearColor Parapet(0.50f, 0.50f, 0.49f, 0.0f);

		for (int32 e = 0; e < Ring.Num(); ++e)
		{
			const FVector2D A2 = Ring[e];
			const FVector2D B2 = Ring[(e + 1) % Ring.Num()];
			const FVector2D Edge = B2 - A2;
			const double Len = Edge.Size();
			if (Len < 280.0) { continue; }
			const FVector2D Dir = Edge / Len;
			const FVector2D Outward(Dir.Y, -Dir.X);

			// Roof parapet: small wall at the top silhouette, inspired by the modular roof trim in the references.
			const FVector2D PA = A2 + Outward * 4.0;
			const FVector2D PB = B2 + Outward * 4.0;
			RoadForgeMesh::AppendQuad(
				FVector(PA.X, PA.Y, BaseZ + Height), FVector(PB.X, PB.Y, BaseZ + Height),
				FVector(PB.X, PB.Y, BaseZ + Height + 65.0), FVector(PA.X, PA.Y, BaseZ + Height + 65.0),
				FVector(Outward.X, Outward.Y, 0.0), Parapet, Out);

			const int32 Columns = FMath::Clamp(FMath::FloorToInt(Len / WindowSpacing), 1, 64);
			for (int32 f = 0; f < Floors; ++f)
			{
				const double ZC = BaseZ + 120.0 + f * FloorH + FloorH * 0.38;
				if (ZC + 70.0 > BaseZ + Height - 45.0) { continue; }
				for (int32 c = 0; c < Columns; ++c)
				{
					const double T = (c + 0.5) / Columns;
					const FVector2D C = A2 + Edge * T + Outward * 5.0;
					const double WW = FMath::Clamp(Len / Columns * 0.34, 55.0, 120.0);
					const double WH = FMath::Clamp(FloorH * 0.34, 70.0, 130.0);
					const FVector2D L = C - Dir * (WW * 0.5);
					const FVector2D R = C + Dir * (WW * 0.5);
					// ~1/3 of windows lit, scattered (not a regular stripe) for a lived-in look.
					const FLinearColor Col = (((c * 7 + f * 3 + e * 5) % 3) == 0) ? WindowLit : WindowGlass;
					RoadForgeMesh::AppendQuad(
						FVector(L.X, L.Y, ZC - WH * 0.5), FVector(R.X, R.Y, ZC - WH * 0.5),
						FVector(R.X, R.Y, ZC + WH * 0.5), FVector(L.X, L.Y, ZC + WH * 0.5),
						FVector(Outward.X, Outward.Y, 0.0), Col, Out);
				}
			}
		}
	}
}

AOSMRoadGenerator::AOSMRoadGenerator()
{
	PrimaryActorTick.bCanEverTick = false;

	GeneratedMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GeneratedMesh"));
	GeneratedMesh->bUseAsyncCooking = true;
	SetRootComponent(GeneratedMesh);

	LampInstances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("LampInstances"));
	LampInstances->SetupAttachment(GeneratedMesh);
	LampInstances->SetMobility(EComponentMobility::Movable);
}

void AOSMRoadGenerator::BeginPlay()
{
	Super::BeginPlay();

	if (bGenerateOnBeginPlay)
	{
		if (!LocalOSMJsonPath.IsEmpty()) { GenerateFromLocalFile(); }
		else { FetchAndGenerate(); }
	}
}

FVector2D AOSMRoadGenerator::ProjectLatLon(double Lat, double Lon) const
{
	const double dLat = FMath::DegreesToRadians(Lat - OriginLatitude);
	const double dLon = FMath::DegreesToRadians(Lon - OriginLongitude);
	const double North = dLat * kEarthRadiusM;
	const double East = dLon * kEarthRadiusM * FMath::Cos(FMath::DegreesToRadians(OriginLatitude));
	return FVector2D(North * kCmPerM, East * kCmPerM); // X=North, Y=East
}

UMaterialInterface* AOSMRoadGenerator::ResolveMaterial() const
{
	if (OverrideMaterial)
	{
		return OverrideMaterial;
	}
	return LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadForge/M_RoadForge_VC.M_RoadForge_VC"));
}

FString AOSMRoadGenerator::BuildOverpassQuery() const
{
	// `out geom;` returns each way with its node coordinates inline, so no node resolution needed.
	const FString Query = FString::Printf(
		TEXT("[out:json][timeout:90];(way[\"highway\"](%.6f,%.6f,%.6f,%.6f);way[\"building\"](%.6f,%.6f,%.6f,%.6f););out geom;"),
		MinLatitude, MinLongitude, MaxLatitude, MaxLongitude,
		MinLatitude, MinLongitude, MaxLatitude, MaxLongitude);

	return OverpassEndpoint + TEXT("?data=") + FGenericPlatformHttp::UrlEncode(Query);
}

void AOSMRoadGenerator::FetchAndGenerate()
{
	if (bRequestInFlight)
	{
		UE_LOG(LogRoadForge, Warning, TEXT("A request is already in flight."));
		return;
	}
	if (MinLatitude >= MaxLatitude || MinLongitude >= MaxLongitude)
	{
		UE_LOG(LogRoadForge, Error, TEXT("Invalid bbox: min must be < max (lat %f..%f, lon %f..%f)."),
			MinLatitude, MaxLatitude, MinLongitude, MaxLongitude);
		return;
	}

	const FString Url = BuildOverpassQuery();
	UE_LOG(LogRoadForge, Log, TEXT("Requesting OSM data: %s"), *Url);

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("RoadForge-UE/1.0 (procedural city generator)"));
	Request->OnProcessRequestComplete().BindUObject(this, &AOSMRoadGenerator::OnOverpassResponse);

	bRequestInFlight = true;
	Request->ProcessRequest();
}

void AOSMRoadGenerator::OnOverpassResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	bRequestInFlight = false;

	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		UE_LOG(LogRoadForge, Error, TEXT("Overpass request failed (no connection / no response)."));
		return;
	}
	const int32 Code = Response->GetResponseCode();
	if (Code != 200)
	{
		UE_LOG(LogRoadForge, Error, TEXT("Overpass HTTP %d. Body: %s"), Code, *Response->GetContentAsString().Left(512));
		return;
	}

	const int32 N = BuildFromJson(Response->GetContentAsString());
	UE_LOG(LogRoadForge, Log, TEXT("RoadForge: built %d elements from Overpass."), N);
}

void AOSMRoadGenerator::GenerateFromLocalFile()
{
	if (LocalOSMJsonPath.IsEmpty())
	{
		UE_LOG(LogRoadForge, Error, TEXT("LocalOSMJsonPath is empty."));
		return;
	}
	// Loads either a raw .osm XML export or an Overpass .json response (auto-detected) and normalises both to
	// the Overpass "elements" JSON the rest of the pipeline already consumes (keeps BuildFromJson unchanged).
	FString JsonText, Error;
	if (!RoadForgeOSM::LoadOSMText(LocalOSMJsonPath, JsonText, Error))
	{
		UE_LOG(LogRoadForge, Error, TEXT("RoadForge: %s"), *Error);
		return;
	}
	const int32 N = BuildFromJson(JsonText);
	UE_LOG(LogRoadForge, Log, TEXT("RoadForge: built %d elements from '%s'."), N, *FPaths::GetCleanFilename(LocalOSMJsonPath));
}

void AOSMRoadGenerator::ClearAll()
{
	if (GeneratedMesh) { GeneratedMesh->ClearAllMeshSections(); }
	if (LampInstances) { LampInstances->ClearInstances(); }
	ClearSplineComponents();
}

void AOSMRoadGenerator::ApplyReferenceQualityPreset()
{
	bGenerateRoads = true;
	bGenerateFootways = true;
	bGenerateLaneLines = true;
	bGenerateEdgeLines = true;
	bGenerateBikeLanes = false;       // edge-offset strip self-intersects into green diamonds on curves/ramps
	bGenerateDirectionArrows = false; // arrows clutter dense junctions; leave off
	bGenerateIntersections = true;
	bGenerateCurbs = true;
	bGenerateBridges = true;
	bGenerateBuildings = true;
	bGenerateBuildingDetails = true;
	bGenerateStreetLamps = true;
	bGenerateGroundPlane = false;

	bUseSplines = true;
	bSmoothSplines = true;
	bShowSplineComponents = true; // editable per-road path points in the level (issue: editability)
	SplineSampleStepCm = 80.0;
	IntersectionRadiusScale = 1.2;
	RampLengthCm = 6000.0; // longer, gentler on-ramps (60 m) for a smoother climb onto overpasses
	ArrowSpacingMeters = 45.0;
	WindowSpacingMeters = 3.0;
	LampSpacingMeters = 28.0;

	UE_LOG(LogRoadForge, Log, TEXT("RoadForge: applied reference-quality preset. Regenerate to see changes."));
}

void AOSMRoadGenerator::GenerateTaipeiSample()
{
	LocalOSMJsonPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("SampleData/taipei_osm.json"));
	GenerateFromLocalFile();
}

void AOSMRoadGenerator::GenerateBarcelonaSample()
{
	LocalOSMJsonPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("SampleData/barcelona_osm.json"));
	GenerateFromLocalFile();
}

void AOSMRoadGenerator::ImportOSMFile()
{
#if WITH_EDITOR
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		UE_LOG(LogRoadForge, Error, TEXT("ImportOSMFile: desktop platform unavailable."));
		return;
	}

	const FString DefaultPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("SampleData"));
	TArray<FString> OutFiles;
	const bool bPicked = DesktopPlatform->OpenFileDialog(
		nullptr,
		TEXT("Select an OSM / Overpass JSON file"),
		DefaultPath,
		TEXT(""),
		TEXT("OSM JSON (*.json;*.geojson)|*.json;*.geojson|All files (*.*)|*.*"),
		EFileDialogFlags::None,
		OutFiles);

	if (!bPicked || OutFiles.Num() == 0)
	{
		UE_LOG(LogRoadForge, Log, TEXT("ImportOSMFile: cancelled."));
		return;
	}

	LocalOSMJsonPath = OutFiles[0];
	UE_LOG(LogRoadForge, Log, TEXT("ImportOSMFile: %s"), *LocalOSMJsonPath);
	GenerateFromLocalFile();
#else
	UE_LOG(LogRoadForge, Warning, TEXT("ImportOSMFile is editor-only. Set LocalOSMJsonPath and call GenerateFromLocalFile instead."));
#endif
}

void AOSMRoadGenerator::RebuildFromSplines()
{
	if (!bShowSplineComponents)
	{
		UE_LOG(LogRoadForge, Warning, TEXT("RebuildFromSplines needs 'Show Spline Components' enabled on a previous build."));
		return;
	}
	if (RoadSplineComponents.Num() == 0 || CachedOSMJson.IsEmpty())
	{
		UE_LOG(LogRoadForge, Warning, TEXT("RebuildFromSplines: nothing to rebuild (generate once with Show Spline Components first)."));
		return;
	}

	// Snapshot the current (possibly user-dragged) spline control points per OSM way id, in local cm.
	EditedSplinePoints.Reset();
	const int32 Num = FMath::Min(RoadSplineComponents.Num(), RoadSplineWayIds.Num());
	for (int32 i = 0; i < Num; ++i)
	{
		const USplineComponent* Spline = RoadSplineComponents[i];
		if (!Spline) { continue; }
		const int32 NumPts = Spline->GetNumberOfSplinePoints();
		if (NumPts < 2) { continue; }
		TArray<FVector2D> Pts;
		Pts.Reserve(NumPts);
		for (int32 p = 0; p < NumPts; ++p)
		{
			const FVector L = Spline->GetLocationAtSplinePoint(p, ESplineCoordinateSpace::Local);
			Pts.Add(FVector2D(L.X, L.Y));
		}
		EditedSplinePoints.Add(RoadSplineWayIds[i], MoveTemp(Pts));
	}

	TGuardValue<bool> Guard(bApplyEditedSplines, true);
	const int32 N = BuildFromJson(CachedOSMJson);
	EditedSplinePoints.Reset();
	UE_LOG(LogRoadForge, Log, TEXT("RoadForge: rebuilt %d elements from edited splines."), N);
}

USplineComponent* AOSMRoadGenerator::AcquireSamplingSpline()
{
	if (!SamplingSpline)
	{
		SamplingSpline = NewObject<USplineComponent>(this, TEXT("RoadForge_SamplingSpline"), RF_Transient);
	}
	return SamplingSpline;
}

USplineComponent* AOSMRoadGenerator::CreatePersistentRoadSpline(int64 WayId)
{
	USplineComponent* Spline = NewObject<USplineComponent>(this);
	if (!Spline)
	{
		return nullptr;
	}
	Spline->SetMobility(EComponentMobility::Movable);
	Spline->SetupAttachment(GeneratedMesh);
	Spline->RegisterComponent();
	AddInstanceComponent(Spline);
	RoadSplineComponents.Add(Spline);
	RoadSplineWayIds.Add(WayId);
	return Spline;
}

void AOSMRoadGenerator::ClearSplineComponents()
{
	for (const TObjectPtr<USplineComponent>& Spline : RoadSplineComponents)
	{
		if (Spline)
		{
			Spline->DestroyComponent();
		}
	}
	RoadSplineComponents.Reset();
	RoadSplineWayIds.Reset();
}

void AOSMRoadGenerator::BuildSplineSamples(const TArray<FVector2D>& In, TArray<FVector2D>& Out, USplineComponent* Spline) const
{
	Out.Reset();
	if (!Spline || In.Num() < 2)
	{
		Out = In;
		return;
	}

	TArray<FVector> Pts3D;
	Pts3D.Reserve(In.Num());
	for (const FVector2D& P : In)
	{
		Pts3D.Add(FVector(P.X, P.Y, 0.0));
	}

	Spline->SetSplinePoints(Pts3D, ESplineCoordinateSpace::Local, /*bUpdateSpline*/ false);
	// CurveClamped (not plain Curve) clamps the auto tangents so the Catmull-Rom interpolation cannot
	// overshoot between points. Plain Curve overshoots at bends and is a second source of the looping
	// "diamond" artefacts; clamped tangents keep the road hugging its cleaned polyline.
	const ESplinePointType::Type PointType = bSmoothSplines ? ESplinePointType::CurveClamped : ESplinePointType::Linear;
	for (int32 i = 0; i < Pts3D.Num(); ++i)
	{
		Spline->SetSplinePointType(i, PointType, /*bUpdateSpline*/ false);
	}
	Spline->UpdateSpline();

	const double Length = Spline->GetSplineLength();
	if (Length <= KINDA_SMALL_NUMBER)
	{
		Out = In;
		return;
	}

	const double Step = FMath::Max(SplineSampleStepCm, 20.0);
	const int32 Count = FMath::Max(1, FMath::CeilToInt(Length / Step));
	Out.Reserve(Count + 1);
	for (int32 i = 0; i <= Count; ++i)
	{
		const double Distance = FMath::Min(Length, i * Step);
		const FVector L = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::Local);
		Out.Add(FVector2D(L.X, L.Y));
	}
}

int32 AOSMRoadGenerator::BuildFromJson(const FString& JsonText)
{
	OriginLatitude = 0.5 * (MinLatitude + MaxLatitude);
	OriginLongitude = 0.5 * (MinLongitude + MaxLongitude);

	// Cache the source so Rebuild From Splines (and edited-spline rebuilds) can reuse it without re-fetching.
	if (!bApplyEditedSplines)
	{
		CachedOSMJson = JsonText;
	}

	// Persistent road splines from a previous build are rebuilt from scratch.
	ClearSplineComponents();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogRoadForge, Error, TEXT("Failed to parse JSON (%d bytes)."), JsonText.Len());
		return 0;
	}

	const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
	if (!Root->TryGetArrayField(TEXT("elements"), Elements) || !Elements)
	{
		UE_LOG(LogRoadForge, Error, TEXT("No 'elements' array in response."));
		return 0;
	}

	// Auto-centre the projection on the data's OWN bounds, so any imported OSM file (raw .osm XML or an
	// Overpass response for a different city) lands at the actor origin instead of hundreds of km away when
	// its coordinates don't match the Area bbox. Falls back to the bbox centre (set above) if no geometry.
	{
		double MinLa = 90.0, MinLo = 180.0, MaxLa = -90.0, MaxLo = -180.0;
		bool bAny = false;
		for (const TSharedPtr<FJsonValue>& EV : *Elements)
		{
			const TSharedPtr<FJsonObject> E = EV->AsObject();
			if (!E.IsValid()) { continue; }
			const TArray<TSharedPtr<FJsonValue>>* Geom = nullptr;
			if (!E->TryGetArrayField(TEXT("geometry"), Geom) || !Geom) { continue; }
			for (const TSharedPtr<FJsonValue>& GV : *Geom)
			{
				const TSharedPtr<FJsonObject> G = GV->AsObject();
				if (!G.IsValid()) { continue; }
				double La = 0.0, Lo = 0.0;
				if (G->TryGetNumberField(TEXT("lat"), La) && G->TryGetNumberField(TEXT("lon"), Lo))
				{
					MinLa = FMath::Min(MinLa, La); MaxLa = FMath::Max(MaxLa, La);
					MinLo = FMath::Min(MinLo, Lo); MaxLo = FMath::Max(MaxLo, Lo);
					bAny = true;
				}
			}
		}
		if (bAny)
		{
			OriginLatitude = 0.5 * (MinLa + MaxLa);
			OriginLongitude = 0.5 * (MinLo + MaxLo);
		}
	}

	FRoadMeshBuffers Roads, Footways, LaneLines, Curbs, Buildings, BuildingDetails, Ground, Junctions, Bridges;
	TArray<FTransform> LampTransforms;

	// --- Pre-pass: node graph (mirrors Quick Brush's road-network/RoadPatcher topology step). ---
	//   NodeMap      : ground vehicle-road junctions + each incident road's "arm" (exit dir + half-width),
	//                  used to fill intersections with an arm-shaped polygon instead of a crude disc.
	//   NodeMinLayer : the lowest OSM layer touching each node, so a bridge way only ramps down to ground
	//                  at the ends where it actually meets a surface road (chained overpasses stay up).
	struct FJunctionInfo
	{
		FVector2D Pos = FVector2D::ZeroVector;
		double MaxHalfW = 0.0;
		int32 WayCount = 0;
		int32 MaxRank = 0;
		TArray<FVector2D> ArmDirs;
		TArray<double> ArmHalfW;
		// Filled by the post-pass below: arms merged so split OSM ways count once, plus the real-junction gate.
		TArray<FVector2D> MergedDirs;
		TArray<double> MergedHalfW;
		bool bIsJunction = false;
		int32 Layer = 0;
	};
	TMap<int64, FJunctionInfo> NodeMap;
	TMap<int64, FJunctionInfo> ElevatedNodeMap;
	TMap<int64, int32> NodeMinLayer;
	// Ground-level road segments (centreline A->B + half width), used so bridge pillars never land on a road.
	TArray<FVector2D> GroundSegA, GroundSegB;
	TArray<double> GroundSegHalfW;
	// Widest drivable road touching each node + that road's heading, so an on-ramp (_link) can be slid to
	// align its edge with the through road instead of connecting on its centreline.
	TMap<int64, double> NodeMaxHalfW;
	TMap<int64, FVector2D> NodeWideDir;
	for (const TSharedPtr<FJsonValue>& ElemValue : *Elements)
	{
		const TSharedPtr<FJsonObject> Elem = ElemValue->AsObject();
		if (!Elem.IsValid()) { continue; }
		FString Type;
		if (!Elem->TryGetStringField(TEXT("type"), Type) || Type != TEXT("way")) { continue; }

		const TSharedPtr<FJsonObject>* TagsPtr = nullptr;
		if (!Elem->TryGetObjectField(TEXT("tags"), TagsPtr) || !TagsPtr) { continue; }
		FString Hw;
		if (!(*TagsPtr)->TryGetStringField(TEXT("highway"), Hw)) { continue; }

		FRoadSpec Spec;
		if (!ComputeRoadSpec(*TagsPtr, Hw, WidthScale, Spec)) { continue; }

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Geom = nullptr;
		if (!Elem->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes) { continue; }
		if (!Elem->TryGetArrayField(TEXT("geometry"), Geom) || !Geom) { continue; }
		const int32 Count = FMath::Min(Nodes->Num(), Geom->Num());
		if (Count < 2) { continue; }

		// Project this way's node ids + positions once.
		TArray<int64> NodeIds;   NodeIds.SetNumUninitialized(Count);
		TArray<FVector2D> NodePos; NodePos.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			double NodeIdD = 0.0; (*Nodes)[i]->TryGetNumber(NodeIdD);
			NodeIds[i] = static_cast<int64>(NodeIdD);
			double Lat = 0.0, Lon = 0.0;
			if (const TSharedPtr<FJsonObject> GNode = (*Geom)[i]->AsObject())
			{
				GNode->TryGetNumberField(TEXT("lat"), Lat);
				GNode->TryGetNumberField(TEXT("lon"), Lon);
			}
			NodePos[i] = ProjectLatLon(Lat, Lon);
		}

		// Lowest layer touching each node (all drivable ways), for the elevation ramps.
		for (int32 i = 0; i < Count; ++i)
		{
			int32& ML = NodeMinLayer.FindOrAdd(NodeIds[i], Spec.Layer);
			ML = FMath::Min(ML, Spec.Layer);
		}

		// Record ground-level vehicle-road segments so bridge pillars and footway crossings can steer clear
		// of carriageways underneath. Do not include footways here, or they would immediately cut themselves.
		if (Spec.Layer == 0 && Spec.Category == 0)
		{
			for (int32 i = 0; i < Count - 1; ++i)
			{
				GroundSegA.Add(NodePos[i]);
				GroundSegB.Add(NodePos[i + 1]);
				GroundSegHalfW.Add(Spec.HalfWCm);
			}
		}

		// Track the widest vehicle road (+ its heading) at every node, for on-ramp edge alignment below.
		if (Spec.Category == 0)
		{
			for (int32 i = 0; i < Count; ++i)
			{
				const int64 Nid = NodeIds[i];
				double& MH = NodeMaxHalfW.FindOrAdd(Nid, 0.0);
				if (Spec.HalfWCm > MH)
				{
					MH = Spec.HalfWCm;
					const FVector2D Seg = (i + 1 < Count) ? (NodePos[i + 1] - NodePos[i]) : (NodePos[i] - NodePos[i - 1]);
					NodeWideDir.Add(Nid, Seg.GetSafeNormal());
				}
			}
		}

		// Junctions + arms: vehicle roads only. Ground and elevated layers are stored separately so an
		// overpass merge gets its own patch without being mixed with the road below.
		if (!bGenerateIntersections || Spec.Category != 0 || Spec.Layer < 0) { continue; }

		// Stable arm heading at a node: walk along the way until ~6 m of road has accumulated, so dense
		// near-junction OSM nodes can't give a noisy direction. This is what keeps crosswalks square to the
		// road instead of skewed.
		auto ArmHeading = [&](int32 From, int32 Step) -> FVector2D
		{
			const double BaselineCm = 600.0;
			double Dist = 0.0;
			int32 j = From;
			while (true)
			{
				const int32 nj = j + Step;
				if (nj < 0 || nj >= Count) { break; }
				Dist += FVector2D::Distance(NodePos[j], NodePos[nj]);
				j = nj;
				if (Dist >= BaselineCm) { break; }
			}
			return (j != From) ? (NodePos[j] - NodePos[From]).GetSafeNormal() : FVector2D::ZeroVector;
		};

		TMap<int64, FJunctionInfo>* JunctionMap = (Spec.Layer == 0) ? &NodeMap : &ElevatedNodeMap;
		TSet<int64> SeenThisWay;
		for (int32 i = 0; i < Count; ++i)
		{
			const int64 NodeId = NodeIds[i];
			FJunctionInfo& Info = JunctionMap->FindOrAdd(NodeId);
			Info.Pos = NodePos[i];
			Info.Layer = Spec.Layer;
			Info.MaxHalfW = FMath::Max(Info.MaxHalfW, Spec.HalfWCm);
			Info.MaxRank = FMath::Max(Info.MaxRank, Spec.Rank);
			if (!SeenThisWay.Contains(NodeId)) { Info.WayCount += 1; SeenThisWay.Add(NodeId); }
			if (i > 0)         { const FVector2D D = ArmHeading(i, -1); if (!D.IsNearlyZero()) { Info.ArmDirs.Add(D); Info.ArmHalfW.Add(Spec.HalfWCm); } }
			if (i < Count - 1) { const FVector2D D = ArmHeading(i, +1); if (!D.IsNearlyZero()) { Info.ArmDirs.Add(D); Info.ArmHalfW.Add(Spec.HalfWCm); } }
		}
	}

	// --- Junction post-pass: merge near-parallel arms (so a road that OSM split into several ways counts as
	// ONE arm, not many), then decide which nodes are REAL junctions. A node is only a junction when it has
	// >= 3 distinct arm directions (T / Y / cross). Two near-opposite arms = a road just passing through /
	// split point -> NOT a junction, so we no longer drop an asphalt blob + crosswalks on straight roads. ---
	int32 NumDegenerateJunctions = 0; // >=3 arms but all clustered in a narrow cone -> OSM split artefact
	int32 NumClampedJunctions = 0;    // implausible width tag -> clamped so the patch can't balloon
	auto PostProcessJunctionMap = [&](TMap<int64, FJunctionInfo>& Map, bool bAllowTwoArmMerge)
	{
		for (TPair<int64, FJunctionInfo>& Pair : Map)
		{
			FJunctionInfo& Info = Pair.Value;
			const int32 ArmCount = FMath::Min(Info.ArmDirs.Num(), Info.ArmHalfW.Num());
			for (int32 Arm = 0; Arm < ArmCount; ++Arm)
			{
				const FVector2D Dir = Info.ArmDirs[Arm].GetSafeNormal();
				if (Dir.IsNearlyZero()) { continue; }
				bool bMerged = false;
				for (int32 Existing = 0; Existing < Info.MergedDirs.Num(); ++Existing)
				{
					if (FVector2D::DotProduct(Dir, Info.MergedDirs[Existing]) > 0.94) // ~20 degrees
					{
						Info.MergedHalfW[Existing] = FMath::Max(Info.MergedHalfW[Existing], Info.ArmHalfW[Arm]);
						bMerged = true;
						break;
					}
				}
				if (!bMerged)
				{
					Info.MergedDirs.Add(Dir);
					Info.MergedHalfW.Add(Info.ArmHalfW[Arm]);
				}
			}

			// Sanity-clamp implausible widths (bad OSM width/lanes tags) so one bad way can't spawn a giant blob.
			if (Info.MaxHalfW > 2500.0) { Info.MaxHalfW = 2500.0; ++NumClampedJunctions; }

			// Ground road junctions still require >=3 arms. Elevated merge nodes may be 2 arms (main + ramp);
			// patch those if they are not just a straight split/continuation.
			const int32 MD = Info.MergedDirs.Num();
			double MinDot = 1.0;
			double MaxAbsDot = 0.0;
			for (int32 a = 0; a < MD; ++a)
			{
				for (int32 b = a + 1; b < MD; ++b)
				{
					const double Dot = FVector2D::DotProduct(Info.MergedDirs[a], Info.MergedDirs[b]);
					MinDot = FMath::Min(MinDot, Dot);
					MaxAbsDot = FMath::Max(MaxAbsDot, FMath::Abs(Dot));
				}
			}
			const bool bThreeArmSpread = (MD >= 3) && (MinDot < 0.766); // arms span more than ~40 degrees overall
			const bool bTwoArmMerge = bAllowTwoArmMerge && (MD == 2) && (MaxAbsDot < 0.90); // not parallel/opposite
			if (MD >= 3 && !bThreeArmSpread) { ++NumDegenerateJunctions; }
			Info.bIsJunction = (Info.WayCount >= 2) && (Info.MaxHalfW > 0.0) && (bThreeArmSpread || bTwoArmMerge);
		}
	};
	PostProcessJunctionMap(NodeMap, false);
	PostProcessJunctionMap(ElevatedNodeMap, true);
	if (NumDegenerateJunctions > 0 || NumClampedJunctions > 0)
	{
		UE_LOG(LogRoadForge, Warning,
			TEXT("RoadForge OSM check: skipped %d degenerate junction node(s) (arms clustered / split ways) and clamped %d over-wide junction(s). These are usually OSM data quirks, not generator bugs."),
			NumDegenerateJunctions, NumClampedJunctions);
	}

	// Colours (RGB = base, A = emissive strength). Asphalt is a realistic mid-grey (~0.10 albedo) rather than
	// near-black, so roads and junctions read as solid grey even in building shadow instead of going pure black.
	const FLinearColor AsphaltColor(0.100f, 0.100f, 0.105f, 0.0f);
	const FLinearColor SidewalkColor(0.40f, 0.39f, 0.37f, 0.0f);
	const FLinearColor YellowLineColor(0.88f, 0.74f, 0.22f, 0.6f);  // centre line (two-way)
	const FLinearColor WhiteLineColor(0.92f, 0.92f, 0.88f, 0.5f);   // lane dividers / single-direction
	const FLinearColor BikeLaneColor(0.10f, 0.34f, 0.22f, 0.0f);    // subtle green surface, reference-style role lane
	const FLinearColor CurbColor(0.60f, 0.60f, 0.58f, 0.0f);
	// Intersection patches reuse AsphaltColor directly (see the junction loop) so they are visually identical
	// to the carriageway — same albedo, same world-planar texture, same emissive (none).

	// Junction circles (centre + patch radius), built once from the node graph. Lane markings are cut where
	// these will be covered by an asphalt patch, and the patch loop below reuses the exact same circles.
	TArray<FVector2D> JunctionPts;
	TArray<double> JunctionR;
	TArray<FVector2D> ElevatedJunctionPts;
	TArray<double> ElevatedJunctionR;
	TArray<int32> ElevatedJunctionLayer;
	if (bGenerateIntersections)
	{
		JunctionPts.Reserve(NodeMap.Num());
		JunctionR.Reserve(NodeMap.Num());
		for (const TPair<int64, FJunctionInfo>& Pair : NodeMap)
		{
			const FJunctionInfo& Info = Pair.Value;
			if (!Info.bIsJunction) { continue; }
			JunctionPts.Add(Info.Pos);
			JunctionR.Add(Info.MaxHalfW * IntersectionRadiusScale);
		}
		ElevatedJunctionPts.Reserve(ElevatedNodeMap.Num());
		ElevatedJunctionR.Reserve(ElevatedNodeMap.Num());
		ElevatedJunctionLayer.Reserve(ElevatedNodeMap.Num());
		for (const TPair<int64, FJunctionInfo>& Pair : ElevatedNodeMap)
		{
			const FJunctionInfo& Info = Pair.Value;
			if (!Info.bIsJunction) { continue; }
			ElevatedJunctionPts.Add(Info.Pos);
			ElevatedJunctionR.Add(Info.MaxHalfW * FMath::Max(IntersectionRadiusScale, 1.05));
			ElevatedJunctionLayer.Add(Info.Layer);
		}
	}

	int32 Built = 0;

	for (const TSharedPtr<FJsonValue>& ElemValue : *Elements)
	{
		const TSharedPtr<FJsonObject> Elem = ElemValue->AsObject();
		if (!Elem.IsValid()) { continue; }

		FString Type;
		if (!Elem->TryGetStringField(TEXT("type"), Type) || Type != TEXT("way")) { continue; }

		const TSharedPtr<FJsonObject>* TagsPtr = nullptr;
		if (!Elem->TryGetObjectField(TEXT("tags"), TagsPtr) || !TagsPtr) { continue; }
		const TSharedPtr<FJsonObject>& WayTags = *TagsPtr;

		// Collect inline geometry, projected to local cm.
		const TArray<TSharedPtr<FJsonValue>>* Geometry = nullptr;
		if (!Elem->TryGetArrayField(TEXT("geometry"), Geometry) || !Geometry || Geometry->Num() < 2) { continue; }

		TArray<FVector2D> Points;
		Points.Reserve(Geometry->Num());
		for (const TSharedPtr<FJsonValue>& GeomValue : *Geometry)
		{
			const TSharedPtr<FJsonObject> Node = GeomValue->AsObject();
			if (!Node.IsValid()) { continue; }
			double Lat = 0.0, Lon = 0.0;
			if (Node->TryGetNumberField(TEXT("lat"), Lat) && Node->TryGetNumberField(TEXT("lon"), Lon))
			{
				Points.Add(ProjectLatLon(Lat, Lon));
			}
		}
		if (Points.Num() < 2) { continue; }

		FString Highway;
		FString BuildingTag;

		// OSM element id (used to match edited splines back to their road way, and to colour buildings).
		double ElemIdD = 0.0;
		Elem->TryGetNumberField(TEXT("id"), ElemIdD);
		const int64 ElemId = static_cast<int64>(ElemIdD);

		if (WayTags->TryGetStringField(TEXT("highway"), Highway))
		{
			FRoadSpec Spec;
			if (!ComputeRoadSpec(WayTags, Highway, WidthScale, Spec))
			{
				continue; // not a real road surface (construction / platform / marker / ...)
			}
			if (Spec.Layer < 0 && bSkipTunnels)
			{
				continue;
			}

			// Clean the raw OSM polyline (dedupe near-duplicate nodes, RDP-straighten GPS noise, drop
			// back-tracking spikes) BEFORE it is splined. Kinks/spikes here are what fold the swept ribbon
			// into thin self-intersecting "diamonds", so straightening the source removes them at the root.
			CleanPolyline(Points);
			if (Points.Num() < 2) { continue; }

			// If the user has dragged this road's editable spline, replace the OSM polyline with the edited
			// path points so the rebuilt mesh follows the new alignment (the "editable path points" feature).
			if (bApplyEditedSplines)
			{
				if (const TArray<FVector2D>* Edited = EditedSplinePoints.Find(ElemId))
				{
					if (Edited->Num() >= 2)
					{
						Points = *Edited;
					}
				}
			}

			const double HalfW = Spec.HalfWCm;
			const double WidthM = Spec.WidthM;
			const double LayerOffset = Spec.Layer * LayerHeightCm;
			const bool bIsRampLink = Highway.EndsWith(TEXT("_link"));
			const bool bElevatedRampLink = bIsRampLink && !FMath::IsNearlyZero(LayerOffset);

				// A raised way ramps down to ground ONLY at ends that actually touch a lower layer (a surface
				// road). Ends that join another same/higher-layer way stay up, so chained overpasses no longer
				// dip at every segment join (the multi-layer "stacked/weird" problem). Needs OSM node ids.
				bool bRampStart = true, bRampEnd = true;
				if (Spec.Layer != 0 && NodeMinLayer.Num() > 0)
				{
					const TArray<TSharedPtr<FJsonValue>>* WayNodes = nullptr;
					if (Elem->TryGetArrayField(TEXT("nodes"), WayNodes) && WayNodes && WayNodes->Num() >= 2)
					{
						double N0 = 0.0, N1 = 0.0;
						(*WayNodes)[0]->TryGetNumber(N0);
						(*WayNodes)[WayNodes->Num() - 1]->TryGetNumber(N1);
						if (const int32* L0 = NodeMinLayer.Find(static_cast<int64>(N0))) { bRampStart = (*L0 < Spec.Layer); }
						if (const int32* L1 = NodeMinLayer.Find(static_cast<int64>(N1))) { bRampEnd   = (*L1 < Spec.Layer); }
					}
				}

			// Route the projected polyline through a spline (smoothing + uniform resampling) when enabled.
			// This is the "OSM -> Spline" step of the pipeline; the mesh is then built from RoadPts.
			TArray<FVector2D> Smoothed;
			const TArray<FVector2D>* PtsToUse = &Points;
			if (bUseSplines)
			{
				USplineComponent* Spline = bShowSplineComponents ? CreatePersistentRoadSpline(ElemId) : AcquireSamplingSpline();
				if (Spline)
				{
					BuildSplineSamples(Points, Smoothed, Spline);
					if (Smoothed.Num() >= 2) { PtsToUse = &Smoothed; }
				}
			}
			// --- On-ramp separation: an elevated "_link" ramp is drawn in OSM down the parent road's
			// CENTRELINE, so it ends up sitting ON TOP of the through lanes where it joins. A real interchange
			// leaves space for the ramp coming up alongside. Slide the ramp sideways so it runs flush BESIDE
			// the parent (its inner edge just outside the parent's outer edge, plus a small gap) at the
			// connecting node, tapering the shift to zero toward the far end so it merges in smoothly. ---
			TArray<FVector2D> RampAligned;
			const TArray<FVector2D>* AlignedPtr = PtsToUse;
			if (bElevatedRampLink
				&& PtsToUse->Num() >= 2 && NodeMaxHalfW.Num() > 0)
			{
				const TArray<TSharedPtr<FJsonValue>>* WN = nullptr;
				int64 NA = 0, NB = 0;
				if (Elem->TryGetArrayField(TEXT("nodes"), WN) && WN && WN->Num() >= 2)
				{
					double a = 0.0, b = 0.0;
					(*WN)[0]->TryGetNumber(a);
					(*WN)[WN->Num() - 1]->TryGetNumber(b);
					NA = static_cast<int64>(a);
					NB = static_cast<int64>(b);
				}

				const TArray<FVector2D>& Src = *PtsToUse;
				const int32 NP = Src.Num();
				TArray<double> Arc; Arc.SetNumUninitialized(NP); Arc[0] = 0.0;
				for (int32 i = 1; i < NP; ++i) { Arc[i] = Arc[i - 1] + FVector2D::Distance(Src[i - 1], Src[i]); }
				const double Total = Arc[NP - 1];

				// Lateral offset contributed by an end node: push the ramp to the side it actually lies on
				// (judged by its NET displacement from the node, robust to a near-parallel first segment) so
				// it sits beside the wider parent road with a small gap, never overlapping it.
				auto EndOffset = [&](int64 Nid, const FVector2D& NodePt, const FVector2D& FarPt) -> FVector2D
				{
					if (Nid == 0) { return FVector2D::ZeroVector; }
					if (const FJunctionInfo* EJ = ElevatedNodeMap.Find(Nid))
					{
						if (EJ->bIsJunction && EJ->Layer == Spec.Layer)
						{
							return FVector2D::ZeroVector; // elevated patch owns this merge; do not kick the endpoint sideways
						}
					}
					const double* PH = NodeMaxHalfW.Find(Nid);
					const FVector2D* PD = NodeWideDir.Find(Nid);
					if (!PH || !PD) { return FVector2D::ZeroVector; }
					if (*PH <= HalfW + 80.0) { return FVector2D::ZeroVector; } // no meaningfully wider parent here
					const FVector2D ParentPerp(-PD->Y, PD->X);
					const double Perp = FVector2D::DotProduct(FarPt - NodePt, ParentPerp);
					const double AbsPerp = FMath::Abs(Perp);
					if (AbsPerp < 150.0) { return FVector2D::ZeroVector; } // ~collinear: a continuation, not a side ramp
					const double Side = (Perp >= 0.0) ? 1.0 : -1.0;
					// Target the ramp centerline beside the parent road, but only add the MISSING lateral
					// separation. Many OSM links are already drawn offset from the parent; pushing the full
					// amount again bends them across the deck (the "高架依然没做好" screenshot).
					const double DesiredAbs = *PH + HalfW + 60.0;
					const double Missing = DesiredAbs - AbsPerp;
					if (Missing <= 50.0) { return FVector2D::ZeroVector; } // already sufficiently beside it
					return ParentPerp * (Side * FMath::Min(Missing, HalfW + 180.0));
				};

				const FVector2D OffStart = EndOffset(NA, Src[0], Src.Last());
				const FVector2D OffEnd   = EndOffset(NB, Src.Last(), Src[0]);

				if (!OffStart.IsNearlyZero() || !OffEnd.IsNearlyZero())
				{
					const double Taper = FMath::Min(Total * 0.5, 4000.0); // shift fades out over <=40 m
					RampAligned.SetNumUninitialized(NP);
					for (int32 i = 0; i < NP; ++i)
					{
						double ws = 0.0, we = 0.0;
						if (Taper > KINDA_SMALL_NUMBER)
						{
							const double ts = FMath::Clamp(1.0 - Arc[i] / Taper, 0.0, 1.0);
							const double te = FMath::Clamp(1.0 - (Total - Arc[i]) / Taper, 0.0, 1.0);
							ws = ts * ts * ts * (ts * (ts * 6.0 - 15.0) + 10.0); // smootherstep
							we = te * te * te * (te * (te * 6.0 - 15.0) + 10.0);
						}
						RampAligned[i] = Src[i] + OffStart * ws + OffEnd * we;
					}
					AlignedPtr = &RampAligned;
				}
			}
			const TArray<FVector2D>& RoadPts = *AlignedPtr;

			// Per-vertex elevation: flat at RoadZOffset, but bridges/overpasses (layer>0) ramp up from
			// ground at each end so they connect smoothly to the surface roads instead of jumping.
			// A tiny rank-based bias lifts more important roads a few cm so that where two SURFACE roads
			// physically overlap (wide ways that don't share an OSM node) they no longer z-fight/flicker.
			// A sub-centimetre per-way jitter breaks ties between SAME-rank overlaps (e.g. two elevated
			// carriageways / a ramp running beside the deck) so coplanar ribbons stop flickering too.
			const double IdJitter = static_cast<double>((static_cast<uint64>(ElemId) * 2654435761u) % 4u) * 0.6;
			const double RankBias = OverlapZBiasCm * FMath::Max(0, Spec.Rank - 1) + IdJitter;
			TArray<double> RoadZ;
			{
				const int32 NP = RoadPts.Num();
				RoadZ.SetNumUninitialized(NP);
				if (FMath::IsNearlyZero(LayerOffset))
				{
					for (int32 i = 0; i < NP; ++i) { RoadZ[i] = RoadZOffset + RankBias; }
				}
				else
				{
					TArray<double> Arc; Arc.SetNumUninitialized(NP);
					Arc[0] = 0.0;
					for (int32 i = 1; i < NP; ++i) { Arc[i] = Arc[i - 1] + FVector2D::Distance(RoadPts[i - 1], RoadPts[i]); }
					const double Total = Arc[NP - 1];
					const double Ramp = FMath::Clamp(Total * 0.45, 1.0, RampLengthCm);
					for (int32 i = 0; i < NP; ++i)
					{
						// Only ramp toward an end that meets a lower layer; a non-ramping end stays full height.
						const double DStart = bRampStart ? Arc[i] : Ramp;
						const double DEnd   = bRampEnd   ? (Total - Arc[i]) : Ramp;
						const double EndDist = FMath::Min(DStart, DEnd);
						const double t = FMath::Clamp(EndDist / Ramp, 0.0, 1.0);
						// Quintic "smootherstep": zero slope AND zero curvature at both ends, so the on-ramp
						// eases out of the ground and into the flat deck with no visible kink (better 上桥 feel).
						const double f = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
						RoadZ[i] = RoadZOffset + RankBias + LayerOffset * f;
					}
				}
			}

			// Roads are physically cut out around same-layer junction patches; the patch owns that area.
			// This prevents road ribbons/curbs/markings from really crossing each other. Ground uses the
			// ground RoadPatcher, elevated ramps/main spans use the elevated merge patch.
			TArray<FVector2D> CutPts;
			TArray<double> CutR;
			if (Spec.Category == 0)
			{
				if (Spec.Layer == 0)
				{
					CutPts = JunctionPts;
					CutR = JunctionR;
				}
				else
				{
					for (int32 c = 0; c < ElevatedJunctionPts.Num(); ++c)
					{
						if (ElevatedJunctionLayer.IsValidIndex(c) && ElevatedJunctionLayer[c] == Spec.Layer)
						{
							CutPts.Add(ElevatedJunctionPts[c]);
							CutR.Add(ElevatedJunctionR[c]);
						}
					}
				}
			}
			TArray<TArray<int32>> RoadRuns;
			const bool bCutAtJunctions = (CutPts.Num() > 0);
			if (bCutAtJunctions)
			{
				RoadForgeMesh::SplitIndexRunsOutsideCircles(RoadPts, CutPts, CutR, RoadRuns);
			}
			else
			{
				TArray<int32> WholeRun;
				WholeRun.Reserve(RoadPts.Num());
				for (int32 i = 0; i < RoadPts.Num(); ++i) { WholeRun.Add(i); }
				if (WholeRun.Num() >= 2) { RoadRuns.Add(MoveTemp(WholeRun)); }
			}

			auto BuildRunArrays = [&](const TArray<int32>& Run, TArray<FVector2D>& OutPts, TArray<double>& OutZ)
			{
				OutPts.Reset(); OutZ.Reset();
				OutPts.Reserve(Run.Num()); OutZ.Reserve(Run.Num());
				for (const int32 Idx : Run)
				{
					OutPts.Add(RoadPts[Idx]);
					OutZ.Add(RoadZ[Idx]);
				}
			};

			if (Spec.Category == 1)
			{
				if (bGenerateFootways)
				{
					// Footways/cycleways in OSM often include "crossing" stubs through carriageways. Since the
					// user asked to remove crosswalk-style intersection content, do not let those pale strips
					// visibly run across roads or under bridge piers. Keep normal sidewalks just outside the
					// carriageway: only cut points that fall inside a vehicle-road half-width.
					auto InsideVehicleRoad = [&](const FVector2D& P) -> bool
					{
						for (int32 s = 0; s < GroundSegA.Num(); ++s)
						{
							const double Clear = GroundSegHalfW[s] + 20.0;
							if (DistSqPointSeg(P, GroundSegA[s], GroundSegB[s]) < Clear * Clear) { return true; }
						}
						return false;
					};
					auto EmitFootwaySegment = [&](const TArray<FVector2D>& SegPts, const TArray<double>& SegZ)
					{
						if (SegPts.Num() >= 2)
						{
							RoadForgeMesh::AppendFlatRibbon(SegPts, +HalfW, -HalfW, 1.0, SidewalkColor, Footways, &SegZ);
						}
					};
					for (const TArray<int32>& Run : RoadRuns)
					{
						TArray<FVector2D> SegPts; TArray<double> SegZ;
						BuildRunArrays(Run, SegPts, SegZ);
						TArray<FVector2D> KeptPts;
						TArray<double> KeptZ;
						for (int32 i = 0; i < SegPts.Num(); ++i)
						{
							const bool bBlockedPoint = InsideVehicleRoad(SegPts[i]);
							const bool bBlockedMid = (i > 0) && InsideVehicleRoad((SegPts[i - 1] + SegPts[i]) * 0.5);
							if (bBlockedPoint || bBlockedMid)
							{
								EmitFootwaySegment(KeptPts, KeptZ);
								KeptPts.Reset();
								KeptZ.Reset();
								continue;
							}
							KeptPts.Add(SegPts[i]);
							KeptZ.Add(SegZ[i]);
						}
						EmitFootwaySegment(KeptPts, KeptZ);
					}
					++Built;
				}
				continue;
			}

			// Vehicle road surface.
			if (bGenerateRoads)
			{
				for (const TArray<int32>& Run : RoadRuns)
				{
					TArray<FVector2D> SegPts; TArray<double> SegZ;
					BuildRunArrays(Run, SegPts, SegZ);
					RoadForgeMesh::AppendFlatRibbon(SegPts, +HalfW, -HalfW, 0.0, AsphaltColor, Roads, &SegZ);
				}
			}

			// Shared painted-line emitter: offsets a copy of the road polyline sideways, then (on ground roads)
			// cuts it around every junction circle so NO stripe is drawn across an intersection. DGap>0 = dashed.
			const double ZLine = 0.6;
			const bool bSuppressLanes = (CutPts.Num() > 0);
			auto EmitLine = [&](double Offset, double Half, double DLen, double DGap, const FLinearColor& Col)
			{
				TArray<FVector2D> Line;
				RoadForgeMesh::OffsetPolyline(RoadPts, Offset, Line);
				const bool bDashed = (DGap > 0.0);
				auto Paint = [&](const TArray<FVector2D>& P, const TArray<double>* Z)
				{
					if (P.Num() < 2) { return; }
					if (bDashed) { RoadForgeMesh::AppendDashedRibbon(P, Half, ZLine, DLen, DGap, Col, LaneLines, Z); }
					else         { RoadForgeMesh::AppendFlatRibbon(P, +Half, -Half, ZLine, Col, LaneLines, Z); }
				};
				if (!bSuppressLanes) { Paint(Line, &RoadZ); return; }
				TArray<TArray<int32>> Runs;
				RoadForgeMesh::SplitIndexRunsOutsideCircles(Line, CutPts, CutR, Runs);
				for (const TArray<int32>& Run : Runs)
				{
					TArray<FVector2D> SegPts; TArray<double> SegZ;
					SegPts.Reserve(Run.Num()); SegZ.Reserve(Run.Num());
					for (int32 idx : Run) { SegPts.Add(Line[idx]); SegZ.Add(RoadZ[idx]); }
					Paint(SegPts, &SegZ);
				}
			};

			// Role lane surfaces: if OSM says the road has cycle lanes, tint both edge lanes green like the
			// reference's separate bikelane deck material. Kept as a flat strip so it still follows ramps.
			if (!bElevatedRampLink && bGenerateBikeLanes && HasMeaningfulCycleway(WayTags) && WidthM >= 7.0)
			{
				const double BikeHalf = 55.0;   // ~1.1 m lane stripe
				const double BikeInset = 95.0;
				if (HalfW - BikeInset > BikeHalf)
				{
					EmitLine(+(HalfW - BikeInset), BikeHalf, 0.0, 0.0, BikeLaneColor);
					EmitLine(-(HalfW - BikeInset), BikeHalf, 0.0, 0.0, BikeLaneColor);
				}
			}

			// Lane markings (real-world convention): white dashed lines split SAME-direction lanes; one solid
			// yellow centre line splits OPPOSING traffic. The centre sits at the actual directional split, and
			// that boundary is never also painted white -> no two stripes ever share an offset (fixes overlap).
			if (!bElevatedRampLink && bGenerateLaneLines && WidthM >= 4.0)
			{
				const double LineHalf = 8.0;  // ~16 cm painted stripe
				const int32 Lanes = FMath::Max(1, Spec.NumLanes);
				const double LaneW = (2.0 * HalfW) / Lanes;
				const bool bTwoWay = !Spec.bOneway && Lanes >= 2;
				const double DashGap = bDashedCenterLine ? DashGapCm : 0.0; // 0 => paint a solid stripe instead

				// Lanes per direction -> which interior boundary carries the centre line. Trust OSM
				// lanes:backward/forward; otherwise split evenly (an odd extra lane goes to the forward side).
				int32 LanesBwd = 0;
				if (bTwoWay)
				{
					LanesBwd = Spec.LanesBackward;
					if (LanesBwd <= 0 || LanesBwd >= Lanes) { LanesBwd = Lanes / 2; }
				}

				// If the lanes tag implies implausibly narrow lanes (< 2.2 m) for this width, skip the interior
				// dividers (they'd clutter/overlap) and just keep the centre line at the geometric middle.
				const bool bDrawDividers = LaneW >= 220.0;
				const int32 CentreBoundary = (bTwoWay && bDrawDividers) ? LanesBwd : -1;

				if (bDrawDividers)
				{
					for (int32 li = 1; li < Lanes; ++li)
					{
						if (li == CentreBoundary) { continue; } // becomes the solid yellow centre, below
						EmitLine(-HalfW + li * LaneW, LineHalf, DashLengthCm, DashGap, WhiteLineColor);
					}
				}

				if (bTwoWay)
				{
					const double CentreOff = (CentreBoundary >= 0) ? (-HalfW + CentreBoundary * LaneW) : 0.0;
					// Wider two-way roads get a double-solid centre; minor roads a single solid stripe.
					if (WidthM >= 9.0)
					{
						const double Sep = 11.0; // cm between the two yellow stripes
						EmitLine(CentreOff - Sep, LineHalf, 0.0, 0.0, YellowLineColor);
						EmitLine(CentreOff + Sep, LineHalf, 0.0, 0.0, YellowLineColor);
					}
					else
					{
						EmitLine(CentreOff, LineHalf, 0.0, 0.0, YellowLineColor);
					}
				}
			}

			// Solid white edge lines just inside the carriageway (extra styling for wider roads).
			if (!bElevatedRampLink && bGenerateEdgeLines && WidthM >= 7.0)
			{
				const double EdgeInset = 25.0;
				const double EdgeHalf = 6.0;
				if (HalfW - EdgeInset > EdgeHalf)
				{
					EmitLine(+(HalfW - EdgeInset), EdgeHalf, 0.0, 0.0, WhiteLineColor);
					EmitLine(-(HalfW - EdgeInset), EdgeHalf, 0.0, 0.0, WhiteLineColor);
				}
			}

			if (!bElevatedRampLink && bGenerateDirectionArrows && ShouldDrawDirectionArrows(WayTags, Highway, Spec))
			{
				const double SpacingCm = ArrowSpacingMeters * kCmPerM;
				for (const TArray<int32>& Run : RoadRuns)
				{
					TArray<FVector2D> SegPts; TArray<double> SegZ;
					BuildRunArrays(Run, SegPts, SegZ);
					if (Spec.bOneway)
					{
						AppendDirectionArrowsAlong(SegPts, SegZ, 0.0, SpacingCm, WhiteLineColor, LaneLines);
					}
					else if (WidthM >= 7.0)
					{
						AppendDirectionArrowsAlong(SegPts, SegZ, -HalfW * 0.32, SpacingCm, WhiteLineColor, LaneLines);

						TArray<FVector2D> ReversePts = SegPts;
						TArray<double> ReverseZ = SegZ;
						Algo::Reverse(ReversePts);
						Algo::Reverse(ReverseZ);
						AppendDirectionArrowsAlong(ReversePts, ReverseZ, -HalfW * 0.32, SpacingCm, WhiteLineColor, LaneLines);
					}
				}
			}

			if (!bElevatedRampLink && bGenerateCurbs)
			{
				for (const TArray<int32>& Run : RoadRuns)
				{
					TArray<FVector2D> SegPts; TArray<double> SegZ;
					BuildRunArrays(Run, SegPts, SegZ);
					RoadForgeMesh::AppendFlatRibbon(SegPts, HalfW + CurbWidthCm, HalfW, CurbHeightCm, CurbColor, Curbs, &SegZ);
					RoadForgeMesh::AppendVerticalStrip(SegPts, HalfW, 0.0, CurbHeightCm, -1.0, CurbColor, Curbs, &SegZ);
					RoadForgeMesh::AppendFlatRibbon(SegPts, -HalfW, -(HalfW + CurbWidthCm), CurbHeightCm, CurbColor, Curbs, &SegZ);
					RoadForgeMesh::AppendVerticalStrip(SegPts, -HalfW, 0.0, CurbHeightCm, +1.0, CurbColor, Curbs, &SegZ);
				}
			}
			if (!bElevatedRampLink && bGenerateStreetLamps && WidthM >= 6.0)
			{
				const double LateralOffset = HalfW + CurbWidthCm + 40.0; // just outside the curb
				const double SpacingCm = LampSpacingMeters * kCmPerM;
				for (const TArray<int32>& Run : RoadRuns)
				{
					TArray<FVector2D> SegPts; TArray<double> SegZ;
					BuildRunArrays(Run, SegPts, SegZ);
					RoadForgeMesh::SamplePointsAlong(SegPts, SpacingCm, LateralOffset,
						250.0, FVector(0.12, 0.12, 5.0), LampTransforms, &SegZ);
				}
			}

			// --- Bridges / overpasses: a raised deck needs a real cross-section (box-girder slab + solid
			// parapets) and a piered substructure (cap-beam bents on columns over footings), otherwise it
			// reads as a floating ribbon. We synthesise all of it from the deck polyline (the "extend via
			// algorithm" step the brief asks for, since OSM rarely models any of this). ---
			if (bGenerateBridges && Spec.bBridge && !FMath::IsNearlyZero(LayerOffset))
			{
				const FLinearColor DeckColor(0.42f, 0.42f, 0.44f, 0.0f);   // concrete deck / soffit
				const FLinearColor BarrierColor(0.70f, 0.70f, 0.68f, 0.0f); // lighter parapet wall

				// Cross-section (relative to the deck top at z=0, all swept along the deck so they follow
				// the curve): a thin structural slab with a small cantilever overhang past the carriageway,
				// a central box girder hanging below for visual depth, and a solid parapet on each overhang.
				const double DeckThickness = bIsRampLink ? FMath::Max(CurbHeightCm * 1.4, 28.0) : FMath::Max(CurbHeightCm * 2.0, 40.0);
				const double DeckOverhang  = bIsRampLink ? 16.0 : 36.0;           // ramps stay visually light
				const double DeckHalf      = HalfW + DeckOverhang;
				const double GirderHalf    = FMath::Min(FMath::Max(HalfW * 0.6, 150.0), HalfW); // within the carriageway
				const double GirderDepth   = bIsRampLink
					? FMath::Clamp(FMath::Abs(LayerOffset) * 0.07, 24.0, 70.0)
					: FMath::Clamp(FMath::Abs(LayerOffset) * 0.14, 50.0, 120.0);
				const double SoffitRel     = -(DeckThickness + GirderDepth);     // deck-relative girder bottom
				const double ParapetInner  = HalfW + 4.0;                        // parapet stands on the overhang
				const double ParapetH      = bIsRampLink ? BridgeBarrierHeightCm * 0.72 : BridgeBarrierHeightCm;

				// Structural slab: underside + edge fascia + the two overhang top strips that close the gap
				// between the carriageway edge and the deck edge (so you can't see through to the soffit).
				RoadForgeMesh::AppendFlatRibbon(RoadPts, +DeckHalf, -DeckHalf, -DeckThickness, DeckColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendVerticalStrip(RoadPts, +DeckHalf, -DeckThickness, 0.0, +1.0, DeckColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendVerticalStrip(RoadPts, -DeckHalf, -DeckThickness, 0.0, -1.0, DeckColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendFlatRibbon(RoadPts, +DeckHalf, +HalfW, 0.0, DeckColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendFlatRibbon(RoadPts, -HalfW, -DeckHalf, 0.0, DeckColor, Bridges, &RoadZ);

				// Central box girder hanging below the slab -> the deck reads as a real box section from the side.
				RoadForgeMesh::AppendFlatRibbon(RoadPts, +GirderHalf, -GirderHalf, SoffitRel, DeckColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendVerticalStrip(RoadPts, +GirderHalf, SoffitRel, -DeckThickness, +1.0, DeckColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendVerticalStrip(RoadPts, -GirderHalf, SoffitRel, -DeckThickness, -1.0, DeckColor, Bridges, &RoadZ);

				// Solid parapets (inner face + outer face + top cap) on each cantilever overhang.
				RoadForgeMesh::AppendVerticalStrip(RoadPts, +DeckHalf, 0.0, ParapetH, +1.0, BarrierColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendVerticalStrip(RoadPts, +ParapetInner, 0.0, ParapetH, -1.0, BarrierColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendFlatRibbon(RoadPts, +DeckHalf, +ParapetInner, ParapetH, BarrierColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendVerticalStrip(RoadPts, -DeckHalf, 0.0, ParapetH, -1.0, BarrierColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendVerticalStrip(RoadPts, -ParapetInner, 0.0, ParapetH, +1.0, BarrierColor, Bridges, &RoadZ);
				RoadForgeMesh::AppendFlatRibbon(RoadPts, -ParapetInner, -DeckHalf, ParapetH, BarrierColor, Bridges, &RoadZ);

				// Piered substructure (the standard procedural-bridge recipe: deck -> girder soffit ->
				// transverse CAP BEAM at each span node -> COLUMNS at lateral offsets -> FOOTINGS). OSM
				// almost never models piers, so we synthesise a regular run of bents along the deck. Each
				// bent is placed only where the girder soffit is high enough off the ground to leave a real
				// column, and never over a ground-level road (a column would stab through it) — blocked
				// candidates are nudged along the deck before being skipped.
				const FLinearColor PierColor(0.55f, 0.55f, 0.56f, 0.0f);
				const double PierSpacing  = FMath::Max(BridgePillarSpacingMeters * kCmPerM, 900.0);
				const double CapHeight    = FMath::Clamp(FMath::Abs(LayerOffset) * 0.08, 32.0, 64.0);
				const double CapHalfWide  = 70.0;                                // cap-beam thickness along the road
				const double CapHalfLong  = DeckHalf * 0.82;                     // cap-beam reaches across the deck
				const double FootHeight   = 24.0;
				const double GroundZ      = 0.0;
				const double ColHalf      = FMath::Clamp(HalfW * 0.13, 45.0, 95.0); // square-ish column footprint
				const double FootHalf     = ColHalf + 35.0;
				const bool   bTwinColumns = (DeckHalf > 850.0);                  // only very wide decks get a two-column bent
				const double ColLateral   = HalfW * 0.55;                        // twin-column offset from centre
				const double MinColumn    = 120.0;                               // need >=1.2 m of clear column to build

				const double TotalLen = [&]() { double L = 0.0; for (int32 i = 0; i + 1 < RoadPts.Num(); ++i) { L += FVector2D::Distance(RoadPts[i], RoadPts[i + 1]); } return L; }();
				auto SampleAtS = [&](double S, FVector2D& OutPos, FVector2D& OutDir, double& OutDeckZ) -> bool
				{
					double Accum = 0.0;
					for (int32 i = 0; i + 1 < RoadPts.Num(); ++i)
					{
						const FVector2D Seg = RoadPts[i + 1] - RoadPts[i];
						const double SegLen = Seg.Size();
						if (SegLen < KINDA_SMALL_NUMBER) { continue; }
						if (S <= Accum + SegLen)
						{
							const double T = (S - Accum) / SegLen;
							OutPos = RoadPts[i] + Seg * T;
							OutDir = Seg / SegLen;
							OutDeckZ = FMath::Lerp(RoadZ[i], RoadZ[i + 1], T);
							return true;
						}
						Accum += SegLen;
					}
					return false;
				};

				// True if a footing centred at P (half-extent FootHalf) would overlap any ground road.
				auto BlockedByGroundRoad = [&](const FVector2D& P) -> bool
				{
					for (int32 s = 0; s < GroundSegA.Num(); ++s)
					{
						const double Clear = GroundSegHalfW[s] + FootHalf + 100.0;
						if (DistSqPointSeg(P, GroundSegA[s], GroundSegB[s]) < Clear * Clear) { return true; }
					}
					return false;
				};

				// Emit one column + its footing at a leg centre between FootTopZ and the cap-beam bottom.
				auto BuildLeg = [&](const FVector2D& C, const FVector2D& Dir, double ColTopZ)
				{
					const double FootTopZ = GroundZ + FootHeight;
					RoadForgeMesh::AppendBoxColumn(C, Dir, ColHalf, ColHalf, FootTopZ, ColTopZ, PierColor, Bridges);
					RoadForgeMesh::AppendBoxColumn(C, Dir, FootHalf, FootHalf, GroundZ, FootTopZ, PierColor, Bridges);
				};

				// Link ramps are short, sloped merge pieces; full pier bents on them create the black blocks
				// and sidewalk-like slabs seen in close-up. Keep their lightweight deck only, and place bents
				// on the main bridge/viaduct spans.
				if (!bIsRampLink)
				{
					for (double S = PierSpacing * 0.5; S < TotalLen; S += PierSpacing)
					{
						static const double Nudges[] = { 0.0, 180.0, -180.0, 360.0, -360.0 };
						for (double Nudge : Nudges)
						{
							const double SS = S + Nudge;
							if (SS <= 0.0 || SS >= TotalLen) { continue; }
							FVector2D Pos, Dir; double DeckZ = 0.0;
							if (!SampleAtS(SS, Pos, Dir, DeckZ)) { continue; }

							const FVector2D Perp(-Dir.Y, Dir.X);
							const double SoffitBottomZ = DeckZ + SoffitRel;          // bottom of the box girder here
							const double ColTopZ       = SoffitBottomZ - CapHeight;  // columns stop under the cap beam
							const double FootTopZ      = GroundZ + FootHeight;
							if ((ColTopZ - FootTopZ) < MinColumn) { continue; }      // deck too low (ramp tail) -> no bent

							// Reject if any leg (or the cap) would sit over a ground road; nudge instead.
							const FVector2D L0 = bTwinColumns ? (Pos + Perp * ColLateral) : Pos;
							const FVector2D L1 = bTwinColumns ? (Pos - Perp * ColLateral) : Pos;
							if (BlockedByGroundRoad(L0) || BlockedByGroundRoad(L1)) { continue; }

							// Transverse cap beam (crosshead) tucked just under the girder soffit.
							RoadForgeMesh::AppendBoxColumn(Pos, Perp, CapHalfLong, CapHalfWide, ColTopZ, SoffitBottomZ, PierColor, Bridges);
							if (bTwinColumns) { BuildLeg(L0, Dir, ColTopZ); BuildLeg(L1, Dir, ColTopZ); }
							else              { BuildLeg(Pos, Dir, ColTopZ); }
							break;
						}
					}
				}
			}
			++Built;
		}
		else if (bGenerateBuildings && WayTags->TryGetStringField(TEXT("building"), BuildingTag))
		{
			// Footprint ring (drop the duplicated closing vertex if present).
			TArray<FVector2D> Ring = Points;
			if (Ring.Num() >= 2 && FVector2D::Distance(Ring[0], Ring.Last()) < 1.0)
			{
				Ring.Pop();
			}
			if (Ring.Num() < 3) { continue; }

			FLinearColor Wall, Roof;
			PickBuildingColors(ElemId, Wall, Roof);

			const double HeightCm = ParseBuildingHeight(WayTags, MetersPerLevel, DefaultBuildingHeight) * kCmPerM;
			RoadForgeMesh::AppendPolygonExtrude(Ring, RoadZOffset, HeightCm, Wall, Roof, Buildings);
			if (bGenerateBuildingDetails)
			{
				AppendBuildingFacadeDetails(Ring, RoadZOffset, HeightCm, MetersPerLevel, WindowSpacingMeters, BuildingDetails);
			}
			++Built;
		}
	}

	// Optional ground plane covering the bbox (+ margin).
	if (bGenerateGroundPlane)
	{
		const FVector2D C0 = ProjectLatLon(MinLatitude, MinLongitude);
		const FVector2D C1 = ProjectLatLon(MinLatitude, MaxLongitude);
		const FVector2D C2 = ProjectLatLon(MaxLatitude, MaxLongitude);
		const FVector2D C3 = ProjectLatLon(MaxLatitude, MinLongitude);
		const double Margin = 2000.0;
		const double MinX = FMath::Min(FMath::Min(C0.X, C1.X), FMath::Min(C2.X, C3.X)) - Margin;
		const double MaxX = FMath::Max(FMath::Max(C0.X, C1.X), FMath::Max(C2.X, C3.X)) + Margin;
		const double MinY = FMath::Min(FMath::Min(C0.Y, C1.Y), FMath::Min(C2.Y, C3.Y)) - Margin;
		const double MaxY = FMath::Max(FMath::Max(C0.Y, C1.Y), FMath::Max(C2.Y, C3.Y)) + Margin;
		const FLinearColor GroundColor(0.055f, 0.065f, 0.05f, 0.0f);
		RoadForgeMesh::AppendQuad(
			FVector(MinX, MinY, 0.0), FVector(MaxX, MinY, 0.0),
			FVector(MaxX, MaxY, 0.0), FVector(MinX, MaxY, 0.0),
			FVector::UpVector, GroundColor, Ground);
	}

	// --- Junction patches (Quick Brush's RoadPatcher idea): fill each REAL intersection (>=3 distinct arms)
	// with a polygon shaped to its incident roads. Sits ~1.5 cm above the road so it hides the overlapping
	// ribbons where ways cross (and any ground showing through); lane markings were already cut to this same
	// area -> one tidy patch. Pass-through / split nodes are skipped so straight roads get no stray blob. ---
	int32 NumJunctions = 0;
	if (bGenerateIntersections)
	{
		const double JunctionZ = RoadZOffset + 1.5;
		for (const TPair<int64, FJunctionInfo>& Pair : NodeMap)
		{
			const FJunctionInfo& Info = Pair.Value;
			if (!Info.bIsJunction) { continue; }

			// Arms were already merged (near-parallel split ways collapsed) in the post-pass.
			const TArray<FVector2D>& UniqueDirs = Info.MergedDirs;
			const TArray<double>& UniqueHalfW = Info.MergedHalfW;

			// Patch reach along each arm = the radius the road ribbons were cut at, PLUS a margin so the patch
			// always laps over the cut road ends (the last kept road vertex can sit up to one sample step
			// beyond the cut circle). This guarantees no terrain gap ring between road and junction. The
			// lateral spread stays at the real road half-width so the hull still hugs the carriageway.
			const double CutR = Info.MaxHalfW * IntersectionRadiusScale;
			const double Reach = CutR + FMath::Max(SplineSampleStepCm, 130.0);
			TArray<double> PatchReach;
			PatchReach.Init(Reach, UniqueDirs.Num());

			// Patch uses the SAME asphalt colour as the road so there is no tonal seam at all (the user wants
			// the junction to read as the same surface as the carriageway). Crosswalk striping was removed by
			// request — junctions are now just the clean asphalt patch.
			RoadForgeMesh::AppendIntersectionPolygon(Info.Pos, UniqueDirs, UniqueHalfW, PatchReach, JunctionZ, AsphaltColor, Junctions);
			++NumJunctions;
		}
		for (const TPair<int64, FJunctionInfo>& Pair : ElevatedNodeMap)
		{
			const FJunctionInfo& Info = Pair.Value;
			if (!Info.bIsJunction) { continue; }

			const TArray<FVector2D>& UniqueDirs = Info.MergedDirs;
			const TArray<double>& UniqueHalfW = Info.MergedHalfW;
			if (UniqueDirs.Num() < 2 || UniqueHalfW.Num() < 2) { continue; }

			const double CutR = Info.MaxHalfW * FMath::Max(IntersectionRadiusScale, 1.05);
			const double Reach = CutR + FMath::Max(SplineSampleStepCm, 130.0);
			TArray<double> PatchReach;
			PatchReach.Init(Reach, UniqueDirs.Num());
			const double ElevatedZ = RoadZOffset + Info.Layer * LayerHeightCm
				+ OverlapZBiasCm * FMath::Max(0, Info.MaxRank - 1) + 3.0;

			// Same-layer elevated merge patch: owns the small area where an on-ramp/main deck meet so their
			// ribbons do not physically cross. It is asphalt-coloured like the carriageway and sits above
			// bridge concrete/soffit, while the road runs and lane markings were cut to the same radius.
			RoadForgeMesh::AppendIntersectionPolygon(Info.Pos, UniqueDirs, UniqueHalfW, PatchReach, ElevatedZ, AsphaltColor, Junctions);
			++NumJunctions;
		}
	}

	// --- Commit geometry ---
	GeneratedMesh->ClearAllMeshSections();
	UMaterialInterface* Mat = ResolveMaterial();
	if (!Mat)
	{
		// Never leave a section on the null/checkerboard material: fall back to an always-present engine one.
		Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		UE_LOG(LogRoadForge, Warning, TEXT("RoadForge material missing; using engine fallback. Run Tools->RoadForge->Regenerate RoadForge Material."));
	}

	auto MakeSection = [&](int32 Index, FRoadMeshBuffers& Buf, bool bCollision)
	{
		if (Buf.Num() == 0) { return; }
		GeneratedMesh->CreateMeshSection_LinearColor(Index, Buf.Vertices, Buf.Triangles,
			Buf.Normals, Buf.UV, Buf.Color, Buf.Tangents, bCollision);
		GeneratedMesh->SetMaterial(Index, Mat);
	};

	MakeSection(SEC_Ground, Ground, bGenerateCollision);
	MakeSection(SEC_Roads, Roads, bGenerateCollision);
	MakeSection(SEC_Footways, Footways, false);
	MakeSection(SEC_LaneLines, LaneLines, false);
	MakeSection(SEC_Junctions, Junctions, false);
	MakeSection(SEC_Curbs, Curbs, false);
	MakeSection(SEC_Bridges, Bridges, bGenerateCollision);
	MakeSection(SEC_Buildings, Buildings, false);
	MakeSection(SEC_BuildingDetails, BuildingDetails, false);

	// --- Street lamps (instanced) ---
	LampInstances->ClearInstances();
	if (bGenerateStreetLamps && LampTransforms.Num() > 0)
	{
		UStaticMesh* Mesh = LampMesh ? LampMesh.Get()
			: LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		if (Mesh)
		{
			LampInstances->SetStaticMesh(Mesh);
			LampInstances->AddInstances(LampTransforms, /*bShouldReturnIndices*/ false);
		}
	}

	UE_LOG(LogRoadForge, Log,
		TEXT("RoadForge: %d ways | verts road=%d foot=%d lane=%d curb=%d bridge=%d bld=%d bldDetail=%d | lamps=%d | junctions=%d | splines=%d | origin (%.5f, %.5f)"),
		Built, Roads.Num(), Footways.Num(), LaneLines.Num(), Curbs.Num(), Bridges.Num(), Buildings.Num(), BuildingDetails.Num(),
		LampTransforms.Num(), NumJunctions, RoadSplineComponents.Num(), OriginLatitude, OriginLongitude);

	return Built;
}
