// Copyright RoadForge Contributors. Licensed under the MIT License.

#include "RoadForgeMeshUtils.h"

#include "Algo/Reverse.h"

namespace
{
	/** Tangent frame of a polyline at vertex i: travel direction, left-hand normal, and mitre scale. */
	void ComputeFrame(const TArray<FVector2D>& Pts, int32 i, FVector2D& OutDir, FVector2D& OutPerp, double& OutMiter)
	{
		const int32 N = Pts.Num();
		FVector2D Fwd(0, 0), Bwd(0, 0);
		if (i < N - 1) { Fwd = (Pts[i + 1] - Pts[i]).GetSafeNormal(); }
		if (i > 0)     { Bwd = (Pts[i] - Pts[i - 1]).GetSafeNormal(); }

		if (i == 0)          { OutDir = Fwd; }
		else if (i == N - 1) { OutDir = Bwd; }
		else                 { OutDir = (Fwd + Bwd).GetSafeNormal(); }

		if (OutDir.IsNearlyZero()) { OutDir = Fwd.IsNearlyZero() ? Bwd : Fwd; }
		if (OutDir.IsNearlyZero()) { OutDir = FVector2D(1, 0); }

		OutPerp = FVector2D(-OutDir.Y, OutDir.X); // left-hand normal

		OutMiter = 1.0;
		if (i > 0 && i < N - 1 && !Fwd.IsNearlyZero())
		{
			const FVector2D SegPerp(-Fwd.Y, Fwd.X);
			const double Cos = FVector2D::DotProduct(OutPerp, SegPerp);
			if (Cos > KINDA_SMALL_NUMBER)
			{
				OutMiter = FMath::Clamp(1.0 / Cos, 1.0, 3.0);
			}
		}
	}

	double SignedArea(const TArray<FVector2D>& P)
	{
		double A = 0.0;
		const int32 N = P.Num();
		for (int32 i = 0; i < N; ++i)
		{
			const FVector2D& a = P[i];
			const FVector2D& b = P[(i + 1) % N];
			A += (a.X * b.Y - b.X * a.Y);
		}
		return 0.5 * A;
	}

	bool PointInTriangle(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		// Barycentric sign test.
		const double d1 = (P.X - B.X) * (A.Y - B.Y) - (A.X - B.X) * (P.Y - B.Y);
		const double d2 = (P.X - C.X) * (B.Y - C.Y) - (B.X - C.X) * (P.Y - C.Y);
		const double d3 = (P.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (P.Y - A.Y);
		const bool HasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
		const bool HasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
		return !(HasNeg && HasPos);
	}

	/** Andrew's monotone-chain 2D convex hull. Output is the hull boundary wound CCW, no duplicated end. */
	void BuildConvexHull2D(const TArray<FVector2D>& In, TArray<FVector2D>& Out)
	{
		Out.Reset();
		const int32 N = In.Num();
		if (N < 3) { Out = In; return; }

		TArray<FVector2D> P = In;
		P.Sort([](const FVector2D& A, const FVector2D& B)
		{
			return (A.X < B.X) || (A.X == B.X && A.Y < B.Y);
		});

		auto Cross = [](const FVector2D& O, const FVector2D& A, const FVector2D& B) -> double
		{
			return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X);
		};

		TArray<FVector2D> H;
		H.SetNum(2 * N);
		int32 k = 0;
		for (int32 i = 0; i < N; ++i) // lower hull
		{
			while (k >= 2 && Cross(H[k - 2], H[k - 1], P[i]) <= 0.0) { --k; }
			H[k++] = P[i];
		}
		for (int32 i = N - 2, t = k + 1; i >= 0; --i) // upper hull
		{
			while (k >= t && Cross(H[k - 2], H[k - 1], P[i]) <= 0.0) { --k; }
			H[k++] = P[i];
		}
		H.SetNum(k > 0 ? k - 1 : 0); // drop the repeated start point
		Out = MoveTemp(H);
	}
}

namespace RoadForgeMesh
{
	void AppendFlatRibbon(const TArray<FVector2D>& Pts, double LatLeft, double LatRight,
		double Z, const FLinearColor& Color, FRoadMeshBuffers& Out, const TArray<double>* ZPerVertex)
	{
		const int32 N = Pts.Num();
		if (N < 2)
		{
			return;
		}
		const bool bUseZ = ZPerVertex && ZPerVertex->Num() == N;

		const int32 Base = Out.Num();
		const double Tile = FMath::Max(FMath::Abs(LatLeft - LatRight), 1.0);
		double Accum = 0.0;

		for (int32 i = 0; i < N; ++i)
		{
			FVector2D Dir, Perp; double Miter;
			ComputeFrame(Pts, i, Dir, Perp, Miter);

			const FVector2D L = Pts[i] + Perp * (LatLeft * Miter);
			const FVector2D R = Pts[i] + Perp * (LatRight * Miter);
			if (i > 0) { Accum += (Pts[i] - Pts[i - 1]).Size(); }
			const double V = Accum / Tile;
			const double VZ = (bUseZ ? (*ZPerVertex)[i] : 0.0) + Z;

			Out.Vertices.Add(FVector(L.X, L.Y, VZ));
			Out.Vertices.Add(FVector(R.X, R.Y, VZ));
			Out.Normals.Add(FVector::UpVector);
			Out.Normals.Add(FVector::UpVector);
			Out.UV.Add(FVector2D(0.0, V));
			Out.UV.Add(FVector2D(1.0, V));
			Out.Color.Add(Color);
			Out.Color.Add(Color);
			const FProcMeshTangent Tan(FVector(Dir.X, Dir.Y, 0.0), false);
			Out.Tangents.Add(Tan);
			Out.Tangents.Add(Tan);
		}

		for (int32 i = 0; i < N - 1; ++i)
		{
			const int32 L0 = Base + 2 * i, R0 = L0 + 1, L1 = L0 + 2, R1 = L0 + 3;
			Out.Triangles.Add(L0); Out.Triangles.Add(L1); Out.Triangles.Add(R0);
			Out.Triangles.Add(R0); Out.Triangles.Add(L1); Out.Triangles.Add(R1);
		}
	}

	void AppendDashedRibbon(const TArray<FVector2D>& Pts, double HalfWidth, double Z,
		double DashLen, double GapLen, const FLinearColor& Color, FRoadMeshBuffers& Out, const TArray<double>* ZPerVertex)
	{
		const int32 N = Pts.Num();
		if (N < 2 || HalfWidth <= 0.0 || DashLen <= 1.0)
		{
			return;
		}
		const bool bUseZ = ZPerVertex && ZPerVertex->Num() == N;

		const double Period = DashLen + FMath::Max(GapLen, 0.0);
		double Accum = 0.0; // arc length consumed up to the current segment start

		for (int32 i = 0; i < N - 1; ++i)
		{
			const FVector2D Seg = Pts[i + 1] - Pts[i];
			const double SegLen = Seg.Size();
			if (SegLen < KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const FVector2D Dir = Seg / SegLen;
			const FVector2D Perp(-Dir.Y, Dir.X);
			const double Z0 = (bUseZ ? (*ZPerVertex)[i] : 0.0) + Z;
			const double Z1 = (bUseZ ? (*ZPerVertex)[i + 1] : 0.0) + Z;

			double s = 0.0; // position along this segment
			while (s < SegLen - KINDA_SMALL_NUMBER)
			{
				const double Phase = FMath::Fmod(Accum + s, Period);
				if (Phase < DashLen)
				{
					// Inside a dash: emit a quad up to the end of the dash or the segment.
					const double DashRemaining = DashLen - Phase;
					const double DashEnd = FMath::Min(SegLen, s + DashRemaining);

					const FVector2D A = Pts[i] + Dir * s;
					const FVector2D B = Pts[i] + Dir * DashEnd;
					const double ZA = FMath::Lerp(Z0, Z1, s / SegLen);
					const double ZB = FMath::Lerp(Z0, Z1, DashEnd / SegLen);
					AppendQuad(
						FVector(A.X - Perp.X * HalfWidth, A.Y - Perp.Y * HalfWidth, ZA),
						FVector(A.X + Perp.X * HalfWidth, A.Y + Perp.Y * HalfWidth, ZA),
						FVector(B.X + Perp.X * HalfWidth, B.Y + Perp.Y * HalfWidth, ZB),
						FVector(B.X - Perp.X * HalfWidth, B.Y - Perp.Y * HalfWidth, ZB),
						FVector::UpVector, Color, Out);
					s = DashEnd;
				}
				else
				{
					// Inside a gap: skip to the start of the next dash.
					s += (Period - Phase);
				}
			}

			Accum += SegLen;
		}
	}

	void AppendVerticalStrip(const TArray<FVector2D>& Pts, double Lat,
		double BaseZ, double TopZ, double NormalSign, const FLinearColor& Color, FRoadMeshBuffers& Out,
		const TArray<double>* ZPerVertex)
	{
		const int32 N = Pts.Num();
		if (N < 2)
		{
			return;
		}
		const bool bUseZ = ZPerVertex && ZPerVertex->Num() == N;

		const int32 Base = Out.Num();
		double Accum = 0.0;

		for (int32 i = 0; i < N; ++i)
		{
			FVector2D Dir, Perp; double Miter;
			ComputeFrame(Pts, i, Dir, Perp, Miter);

			const FVector2D P = Pts[i] + Perp * (Lat * Miter);
			const FVector Normal = FVector(Perp.X, Perp.Y, 0.0) * NormalSign;
			if (i > 0) { Accum += (Pts[i] - Pts[i - 1]).Size(); }
			const double VB = (bUseZ ? (*ZPerVertex)[i] : 0.0) + BaseZ;
			const double VT = (bUseZ ? (*ZPerVertex)[i] : 0.0) + TopZ;

			Out.Vertices.Add(FVector(P.X, P.Y, VB));
			Out.Vertices.Add(FVector(P.X, P.Y, VT));
			Out.Normals.Add(Normal);
			Out.Normals.Add(Normal);
			Out.UV.Add(FVector2D(Accum / 100.0, 0.0));
			Out.UV.Add(FVector2D(Accum / 100.0, 1.0));
			Out.Color.Add(Color);
			Out.Color.Add(Color);
			const FProcMeshTangent Tan(FVector(Dir.X, Dir.Y, 0.0), false);
			Out.Tangents.Add(Tan);
			Out.Tangents.Add(Tan);
		}

		for (int32 i = 0; i < N - 1; ++i)
		{
			const int32 B0 = Base + 2 * i, T0 = B0 + 1, B1 = B0 + 2, T1 = B0 + 3;
			Out.Triangles.Add(B0); Out.Triangles.Add(T0); Out.Triangles.Add(B1);
			Out.Triangles.Add(B1); Out.Triangles.Add(T0); Out.Triangles.Add(T1);
		}
	}

	void OffsetPolyline(const TArray<FVector2D>& In, double Lateral, TArray<FVector2D>& Out)
	{
		const int32 N = In.Num();
		Out.Reset();
		Out.Reserve(N);
		if (N < 2)
		{
			Out = In;
			return;
		}
		for (int32 i = 0; i < N; ++i)
		{
			FVector2D Dir, Perp; double Miter;
			ComputeFrame(In, i, Dir, Perp, Miter);
			Out.Add(In[i] + Perp * (Lateral * Miter));
		}
	}

	void SplitIndexRunsOutsideCircles(const TArray<FVector2D>& In,
		const TArray<FVector2D>& Centers, const TArray<double>& Radii, TArray<TArray<int32>>& OutRuns)
	{
		OutRuns.Reset();

		auto IsInside = [&](const FVector2D& P) -> bool
		{
			const int32 C = FMath::Min(Centers.Num(), Radii.Num());
			for (int32 c = 0; c < C; ++c)
			{
				if (FVector2D::DistSquared(P, Centers[c]) <= Radii[c] * Radii[c]) { return true; }
			}
			return false;
		};

		TArray<int32> Run;
		for (int32 i = 0; i < In.Num(); ++i)
		{
			if (IsInside(In[i]))
			{
				if (Run.Num() >= 2) { OutRuns.Add(Run); }
				Run.Reset();
			}
			else
			{
				Run.Add(i);
			}
		}
		if (Run.Num() >= 2) { OutRuns.Add(Run); }
	}

	void AppendQuad(const FVector& A, const FVector& B, const FVector& C, const FVector& D,
		const FVector& Normal, const FLinearColor& Color, FRoadMeshBuffers& Out)
	{
		const int32 Base = Out.Num();
		Out.Vertices.Add(A); Out.Vertices.Add(B); Out.Vertices.Add(C); Out.Vertices.Add(D);
		for (int32 k = 0; k < 4; ++k) { Out.Normals.Add(Normal); Out.Color.Add(Color); }
		Out.UV.Add(FVector2D(0, 0)); Out.UV.Add(FVector2D(1, 0));
		Out.UV.Add(FVector2D(1, 1)); Out.UV.Add(FVector2D(0, 1));
		const FProcMeshTangent Tan((B - A).GetSafeNormal(), false);
		for (int32 k = 0; k < 4; ++k) { Out.Tangents.Add(Tan); }
		Out.Triangles.Add(Base + 0); Out.Triangles.Add(Base + 1); Out.Triangles.Add(Base + 2);
		Out.Triangles.Add(Base + 0); Out.Triangles.Add(Base + 2); Out.Triangles.Add(Base + 3);
	}

	void AppendDisc(const FVector2D& C, double Radius, int32 Segments,
		double Z, const FLinearColor& Color, FRoadMeshBuffers& Out)
	{
		if (Radius <= 1.0)
		{
			return;
		}
		Segments = FMath::Clamp(Segments, 6, 64);

		const int32 Centre = Out.Num();
		Out.Vertices.Add(FVector(C.X, C.Y, Z));
		Out.Normals.Add(FVector::UpVector);
		Out.UV.Add(FVector2D(0.5, 0.5));
		Out.Color.Add(Color);
		Out.Tangents.Add(FProcMeshTangent(1, 0, 0));

		for (int32 k = 0; k <= Segments; ++k)
		{
			const double Ang = (2.0 * PI * k) / Segments;
			const double X = C.X + Radius * FMath::Cos(Ang);
			const double Y = C.Y + Radius * FMath::Sin(Ang);
			Out.Vertices.Add(FVector(X, Y, Z));
			Out.Normals.Add(FVector::UpVector);
			Out.UV.Add(FVector2D(0.5 + 0.5 * FMath::Cos(Ang), 0.5 + 0.5 * FMath::Sin(Ang)));
			Out.Color.Add(Color);
			Out.Tangents.Add(FProcMeshTangent(1, 0, 0));
		}

		for (int32 k = 0; k < Segments; ++k)
		{
			Out.Triangles.Add(Centre);
			Out.Triangles.Add(Centre + 1 + k);
			Out.Triangles.Add(Centre + 1 + k + 1);
		}
	}

	void AppendIntersectionPolygon(const FVector2D& Centre, const TArray<FVector2D>& ArmDirs,
		const TArray<double>& ArmHalfW, const TArray<double>& ArmReach, double Z,
		const FLinearColor& Color, FRoadMeshBuffers& Out)
	{
		const int32 NA = FMath::Min3(ArmDirs.Num(), ArmHalfW.Num(), ArmReach.Num());
		if (NA < 1) { return; }

		// Gather the two mouth corners of every incident road: pushed OUT along the arm by ArmReach (so the
		// patch laps over the cut road end) and spread sideways by the arm's REAL half-width. The CONVEX HULL
		// of these corners (+ the node centre) is the junction surface. Unlike the old angle-sorted fan, a
		// hull is always a simple, hole-free polygon, so uneven / T / Y / skewed junctions can no longer
		// produce the self-intersecting "diamond gap" that showed terrain through the middle of a crossing.
		TArray<FVector2D> Corners;
		Corners.Reserve(NA * 2 + 1);
		Corners.Add(Centre);
		for (int32 a = 0; a < NA; ++a)
		{
			const FVector2D Dir = ArmDirs[a].GetSafeNormal();
			if (Dir.IsNearlyZero()) { continue; }
			const FVector2D Perp(-Dir.Y, Dir.X);
			const double HalfW = FMath::Max(ArmHalfW[a], 1.0);
			const double Reach = FMath::Max(ArmReach[a], HalfW);
			const FVector2D Mouth = Centre + Dir * Reach;
			Corners.Add(Mouth + Perp * HalfW);
			Corners.Add(Mouth - Perp * HalfW);
		}
		if (Corners.Num() < 3) { return; }

		TArray<FVector2D> Hull;
		BuildConvexHull2D(Corners, Hull);
		if (Hull.Num() < 3) { return; }
		// Wind the patch the SAME way as AppendFlatRibbon's road triangles (clockwise in XY = negative signed
		// area) so the patch front face points UP. The material is two-sided, so the wrong winding would still
		// render, but from above we'd then see the BACK face -> its +Z vertex normal is flipped to -Z -> the
		// junction is shaded as a downward face and reads dark / glossy. Matching the road winding makes the
		// patch shade identically to the surrounding asphalt.
		if (SignedArea(Hull) > 0.0) { Algo::Reverse(Hull); } // force CW -> front face up (matches road ribbons)

		// Fan from the hull centroid (always interior for a convex polygon) so every triangle is valid.
		FVector2D Cen(0.0, 0.0);
		for (const FVector2D& P : Hull) { Cen += P; }
		Cen /= static_cast<double>(Hull.Num());

		const double UvTile = 600.0; // world-planar UVs keep the asphalt texture continuous across the seam
		const int32 Base = Out.Num();
		Out.Vertices.Add(FVector(Cen.X, Cen.Y, Z));
		Out.Normals.Add(FVector::UpVector);
		Out.UV.Add(FVector2D(Cen.X / UvTile, Cen.Y / UvTile));
		Out.Color.Add(Color);
		Out.Tangents.Add(FProcMeshTangent(1, 0, 0));
		for (const FVector2D& P : Hull)
		{
			Out.Vertices.Add(FVector(P.X, P.Y, Z));
			Out.Normals.Add(FVector::UpVector);
			Out.UV.Add(FVector2D(P.X / UvTile, P.Y / UvTile));
			Out.Color.Add(Color);
			Out.Tangents.Add(FProcMeshTangent(1, 0, 0));
		}

		const int32 H = Hull.Num();
		for (int32 i = 0; i < H; ++i)
		{
			Out.Triangles.Add(Base);
			Out.Triangles.Add(Base + 1 + i);
			Out.Triangles.Add(Base + 1 + ((i + 1) % H));
		}
	}

	void AppendBoxColumn(const FVector2D& Centre, const FVector2D& Dir, double HalfLong,
		double HalfWide, double BaseZ, double TopZ, const FLinearColor& Color, FRoadMeshBuffers& Out)
	{
		if (TopZ - BaseZ < 1.0) { return; }
		FVector2D D = Dir.GetSafeNormal();
		if (D.IsNearlyZero()) { D = FVector2D(1.0, 0.0); }
		const FVector2D P(-D.Y, D.X);
		HalfLong = FMath::Max(HalfLong, 5.0);
		HalfWide = FMath::Max(HalfWide, 5.0);

		// Four base corners (CCW looking down) of the rectangular section.
		const FVector2D C0 = Centre + D * HalfLong + P * HalfWide;
		const FVector2D C1 = Centre - D * HalfLong + P * HalfWide;
		const FVector2D C2 = Centre - D * HalfLong - P * HalfWide;
		const FVector2D C3 = Centre + D * HalfLong - P * HalfWide;
		const FVector2D Ring[4] = { C0, C1, C2, C3 };

		// Side walls: outward-facing quads.
		for (int32 i = 0; i < 4; ++i)
		{
			const FVector2D a = Ring[i];
			const FVector2D b = Ring[(i + 1) % 4];
			const FVector2D Edge = (b - a).GetSafeNormal();
			const FVector Outward(Edge.Y, -Edge.X, 0.0); // right of CCW travel = outward
			AppendQuad(
				FVector(a.X, a.Y, BaseZ), FVector(b.X, b.Y, BaseZ),
				FVector(b.X, b.Y, TopZ), FVector(a.X, a.Y, TopZ),
				Outward, Color, Out);
		}

		// Top cap (the deck sits on it; a flat lid keeps the column closed).
		AppendQuad(
			FVector(C0.X, C0.Y, TopZ), FVector(C1.X, C1.Y, TopZ),
			FVector(C2.X, C2.Y, TopZ), FVector(C3.X, C3.Y, TopZ),
			FVector::UpVector, Color, Out);
	}

	bool TriangulatePolygon(const TArray<FVector2D>& Poly, TArray<int32>& OutTriangles)
	{
		const int32 N = Poly.Num();
		if (N < 3)
		{
			return false;
		}

		// Work on a list of vertex indices; ensure CCW so an ear has positive area.
		TArray<int32> V;
		V.SetNum(N);
		if (SignedArea(Poly) > 0.0)
		{
			for (int32 i = 0; i < N; ++i) { V[i] = i; }
		}
		else
		{
			for (int32 i = 0; i < N; ++i) { V[i] = (N - 1) - i; }
		}

		int32 NV = N;
		int32 Count = 2 * NV; // guard against infinite loops on bad input
		for (int32 m = 0, v = NV - 1; NV > 2; )
		{
			if (Count-- <= 0)
			{
				return false; // probably a non-simple polygon
			}

			int32 u = v;        if (NV <= u) { u = 0; }
			v = u + 1;          if (NV <= v) { v = 0; }
			int32 w = v + 1;    if (NV <= w) { w = 0; }

			const FVector2D& A = Poly[V[u]];
			const FVector2D& B = Poly[V[v]];
			const FVector2D& C = Poly[V[w]];

			// Convex ear test: positive area + no other vertex inside.
			const double Area = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
			bool bEar = (Area > 0.0);
			if (bEar)
			{
				for (int32 p = 0; p < NV; ++p)
				{
					if (p == u || p == v || p == w) { continue; }
					if (PointInTriangle(Poly[V[p]], A, B, C))
					{
						bEar = false;
						break;
					}
				}
			}

			if (bEar)
			{
				OutTriangles.Add(V[u]);
				OutTriangles.Add(V[v]);
				OutTriangles.Add(V[w]);
				for (int32 s = v; s + 1 < NV; ++s) { V[s] = V[s + 1]; }
				--NV;
				Count = 2 * NV;
				++m;
			}
		}

		return OutTriangles.Num() >= 3;
	}

	void AppendPolygonExtrude(const TArray<FVector2D>& Ring, double BaseZ, double Height,
		const FLinearColor& WallColor, const FLinearColor& RoofColor, FRoadMeshBuffers& Out)
	{
		if (Ring.Num() < 3 || Height <= 0.0)
		{
			return;
		}

		// Local copy wound CCW so outward wall normals are consistent.
		TArray<FVector2D> R = Ring;
		if (SignedArea(R) < 0.0)
		{
			Algo::Reverse(R);
		}

		const int32 M = R.Num();
		const double TopZ = BaseZ + Height;

		// Walls: one outward-facing quad per edge.
		for (int32 i = 0; i < M; ++i)
		{
			const FVector2D& a = R[i];
			const FVector2D& b = R[(i + 1) % M];
			const FVector2D EdgeDir = (b - a).GetSafeNormal();
			if (EdgeDir.IsNearlyZero())
			{
				continue;
			}
			const FVector Outward(EdgeDir.Y, -EdgeDir.X, 0.0); // right of CCW travel = outward

			const FVector A(a.X, a.Y, BaseZ);
			const FVector B(b.X, b.Y, BaseZ);
			const FVector C(b.X, b.Y, TopZ);
			const FVector D(a.X, a.Y, TopZ);
			AppendQuad(A, B, C, D, Outward, WallColor, Out);
		}

		// Roof: triangulated cap at TopZ.
		TArray<int32> RoofTris;
		if (TriangulatePolygon(R, RoofTris))
		{
			const int32 Base = Out.Num();
			for (int32 i = 0; i < M; ++i)
			{
				Out.Vertices.Add(FVector(R[i].X, R[i].Y, TopZ));
				Out.Normals.Add(FVector::UpVector);
				Out.UV.Add(FVector2D(R[i].X / 500.0, R[i].Y / 500.0));
				Out.Color.Add(RoofColor);
				Out.Tangents.Add(FProcMeshTangent(1, 0, 0));
			}
			for (const int32 Idx : RoofTris)
			{
				Out.Triangles.Add(Base + Idx);
			}
		}
	}

	void SamplePointsAlong(const TArray<FVector2D>& Pts, double Spacing, double LateralOffset,
		double ZHeight, const FVector& Scale, TArray<FTransform>& OutTransforms, const TArray<double>* ZPerVertex)
	{
		const int32 N = Pts.Num();
		if (N < 2 || Spacing <= 1.0)
		{
			return;
		}
		const bool bUseZ = ZPerVertex && ZPerVertex->Num() == N;

		double NextS = Spacing * 0.5;
		double Accum = 0.0;

		for (int32 i = 0; i < N - 1; ++i)
		{
			const FVector2D Seg = Pts[i + 1] - Pts[i];
			const double SegLen = Seg.Size();
			if (SegLen < KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const FVector2D Dir = Seg / SegLen;
			const FVector2D Perp(-Dir.Y, Dir.X);

			while (NextS <= Accum + SegLen)
			{
				const double T = (NextS - Accum) / SegLen;
				const FVector2D Pos = Pts[i] + Seg * T + Perp * LateralOffset;
				const FRotator Rot = FRotationMatrix::MakeFromX(FVector(Dir.X, Dir.Y, 0.0)).Rotator();
				const double Z = (bUseZ ? FMath::Lerp((*ZPerVertex)[i], (*ZPerVertex)[i + 1], T) : 0.0) + ZHeight;
				OutTransforms.Add(FTransform(Rot, FVector(Pos.X, Pos.Y, Z), Scale));
				NextS += Spacing;
			}
			Accum += SegLen;
		}
	}
}
