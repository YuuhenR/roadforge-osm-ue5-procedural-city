// Copyright RoadForge Contributors. Licensed under the MIT License.

#pragma once

#include "PCGSettings.h"
#include "PCGElement.h"

#include "PCGRoadForgeScatter.generated.h"

/**
 * RoadForge "scatter along roads" PCG node.
 *
 * Input : road centre-line splines (PolyLine pin) — feed it the RoadForge generator's editable road splines
 *         via a "Get Spline Data" node, or any spline.
 * Output: points (Point pin) placed at a regular spacing along each road, offset laterally to one or both
 *         kerb lines and oriented along the road, ready for a downstream Static Mesh Spawner (street lamps,
 *         trees, props, parked cars...). This is the "PCG along splines" decoration layer of the pipeline.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural))
class ROADFORGE_API UPCGRoadForgeScatterSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("RoadForgeScatter")); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCGRoadForge", "ScatterTitle", "RoadForge Scatter Along Roads"); }
	virtual FText GetNodeTooltipText() const override { return NSLOCTEXT("PCGRoadForge", "ScatterTooltip", "Place points along road splines (offset to the kerb, oriented along the road) for a downstream Static Mesh Spawner."); }
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Sampler; }
#endif

	/** Distance between placements along each road, metres. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, ClampMin = "1.0"))
	float SpacingMeters = 22.0f;

	/** Lateral offset from the road centre-line to the placement, metres (≈ half road width + a little). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, ClampMin = "0.0"))
	float LateralOffsetMeters = 4.5f;

	/** Place on both kerbs (true) or just the left side (false). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	bool bBothSides = true;

	/** Vertical offset added to every placement, cm. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	float HeightOffsetCm = 0.0f;

	/** Alternate the yaw by 180° on the right side so props face the road from both kerbs. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	bool bFaceRoad = true;

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override { return DefaultPointOutputPinProperties(); }
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGRoadForgeScatterElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
