// Copyright RoadForge Contributors. Licensed under the MIT License.

#include "PCGRoadForgeScatter.h"

#include "PCGContext.h"
#include "PCGPin.h"
#include "PCGData.h"
#include "PCGPoint.h"
#include "Data/PCGSplineData.h"
#include "Data/PCGBasePointData.h"
#include "Components/SplineComponent.h" // ESplineCoordinateSpace

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGRoadForgeScatter)

namespace
{
	const FName GRoadForgeSplinesPin(TEXT("Splines"));
}

TArray<FPCGPinProperties> UPCGRoadForgeScatterSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Props;
	FPCGPinProperties& Pin = Props.Emplace_GetRef(GRoadForgeSplinesPin, EPCGDataType::PolyLine,
		/*bAllowMultipleConnections=*/true, /*bAllowMultipleData=*/true);
	Pin.SetRequiredPin();
	return Props;
}

FPCGElementPtr UPCGRoadForgeScatterSettings::CreateElement() const
{
	return MakeShared<FPCGRoadForgeScatterElement>();
}

bool FPCGRoadForgeScatterElement::ExecuteInternal(FPCGContext* Context) const
{
	check(Context);
	const UPCGRoadForgeScatterSettings* Settings = Context->GetInputSettings<UPCGRoadForgeScatterSettings>();
	check(Settings);

	const double Step = FMath::Max(static_cast<double>(Settings->SpacingMeters) * 100.0, 50.0);
	const double Lat  = static_cast<double>(Settings->LateralOffsetMeters) * 100.0;
	const double Hz   = static_cast<double>(Settings->HeightOffsetCm);
	const int32 Sides = Settings->bBothSides ? 2 : 1;

	// Walk every input road spline and drop oriented points along each kerb at SpacingMeters intervals.
	TArray<FPCGPoint> Pts;
	const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(GRoadForgeSplinesPin);
	for (const FPCGTaggedData& In : Inputs)
	{
		const UPCGSplineData* SplineData = Cast<UPCGSplineData>(In.Data);
		if (!SplineData) { continue; }

		const FPCGSplineStruct& Spline = SplineData->SplineStruct;
		const double Len = Spline.GetSplineLength();
		if (Len <= 1.0) { continue; }

		const int32 Count = FMath::Max(1, FMath::FloorToInt(Len / Step));
		for (int32 i = 0; i <= Count; ++i)
		{
			const double D = FMath::Min(static_cast<double>(i) * Step, Len);
			const FTransform T = Spline.GetTransformAtDistanceAlongSpline(D, ESplineCoordinateSpace::World, /*bUseScale=*/false);
			const FVector Loc = T.GetLocation();
			const FVector Right = T.GetRotation().GetRightVector();
			const FVector Fwd = T.GetRotation().GetForwardVector();

			for (int32 s = 0; s < Sides; ++s)
			{
				const double Sign = (s == 0) ? 1.0 : -1.0;
				const FVector P = Loc + Right * (Lat * Sign) + FVector(0.0, 0.0, Hz);
				FRotator Rot = Fwd.Rotation();
				if (Settings->bFaceRoad && s == 1) { Rot.Yaw += 180.0; }

				FPCGPoint& Pt = Pts.Emplace_GetRef();
				Pt.Transform = FTransform(Rot.Quaternion(), P);
				Pt.Density = 1.0f;
				Pt.Steepness = 1.0f;
				Pt.Seed = static_cast<int32>(GetTypeHash(P));
			}
		}
	}

	UPCGBasePointData* PointData = FPCGContext::NewPointData_AnyThread(Context);
	check(PointData);
	PointData->SetNumPoints(Pts.Num(), /*bInitializeValues=*/false);
	PointData->AllocateProperties(EPCGPointNativeProperties::All);

	FPCGPointValueRanges Ranges(PointData, /*bAllocate=*/false);
	for (int32 i = 0; i < Pts.Num(); ++i)
	{
		Ranges.SetFromPoint(i, Pts[i]);
	}

	FPCGTaggedData& Output = Context->OutputData.TaggedData.Emplace_GetRef();
	Output.Data = PointData;
	return true;
}
