#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "NoiseBakeTypes.generated.h"

/** Noise basis functions. Values must stay in sync with the PN_BASIS_* defines
 *  in Shaders/Private/PeriodicNoise.ush. */
UENUM(BlueprintType)
enum class ENoiseBasis : uint8
{
	/** Interpolated per-cell random scalar. Soft, cheap, no directional bias. */
	Value				= 0	UMETA(DisplayName = "Value"),

	/** Classic gradient noise. The default for warp and general detail. */
	Perlin				= 1	UMETA(DisplayName = "Perlin"),

	/** Cellular F1, inverted so 1.0 sits at the feature point. Billowy. */
	WorleyF1			= 2	UMETA(DisplayName = "Worley F1"),

	/** Cellular F2-F1. Produces the cell-boundary web rather than blobs. */
	WorleyF2MinusF1		= 3	UMETA(DisplayName = "Worley F2 - F1"),

	/** Perlin remapped by inverted Worley. The standard cloud base signal. */
	PerlinWorley		= 4	UMETA(DisplayName = "Perlin-Worley"),
};

/** How a channel's raw FBM output gets mapped into [0,1] before quantization. */
UENUM(BlueprintType)
enum class ENoiseNormalizeMode : uint8
{
	/** Bake a low-resolution probe pass first, take the observed min/max, pad
	 *  slightly, and use that. Correct in almost every case and costs ~1% of the
	 *  bake. This is what you want unless you have a reason otherwise. */
	AutoProbe	= 0	UMETA(DisplayName = "Auto (Probe Pass)"),

	/** Use ManualMin/ManualMax. Use this when two channels must share an
	 *  identical range, or when you are re-baking at a new resolution and need
	 *  byte-for-byte comparable output to a previous bake. */
	Manual		= 1	UMETA(DisplayName = "Manual Range"),

	/** Emit the raw FBM value clamped to [0,1]. The basis functions already
	 *  return [0,1], so this is only lossy for ridged/multi-octave setups that
	 *  do not reach the full range. */
	None		= 2	UMETA(DisplayName = "None"),
};

/** Per-channel noise definition. Each of the four output channels carries an
 *  entirely independent instance of this: separate basis, frequency, seed and
 *  octave count. Nothing is shared between channels by design. */
USTRUCT(BlueprintType)
struct NOISEBAKER_API FNoiseChannelRecipe
{
	GENERATED_BODY()

	/** Which noise function this channel is built from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis")
	ENoiseBasis Basis = ENoiseBasis::Perlin;

	/** Lattice cells across the tile at octave 0. This is the tiling period:
	 *  the baked texture repeats exactly every BasePeriod cells, so a value of 4
	 *  means the first octave completes 4 times across the volume.
	 *
	 *  Larger values give finer base features and a less obviously repeating
	 *  texture, at the cost of Nyquist headroom for the higher octaves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis", meta = (ClampMin = "1", UIMin = "1", UIMax = "32"))
	int32 BasePeriod = 4;

	/** Number of FBM octaves. Each octave multiplies the period by Lacunarity,
	 *  so octave count is bounded by resolution (see the Nyquist rule in
	 *  ValidateChannel). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis", meta = (ClampMin = "1", ClampMax = "10", UIMin = "1", UIMax = "8"))
	int32 Octaves = 4;

	/** Per-octave frequency multiplier. MUST be an integer: a tiling lattice
	 *  requires every octave's period to be a whole number of cells across the
	 *  tile, and fractional lacunarity breaks that. This is the tradeoff for
	 *  exact tiling; fractional lacunarity is the usual trick for hiding lattice
	 *  alignment and it is not available to us. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis", meta = (ClampMin = "2", ClampMax = "8", UIMin = "2", UIMax = "4"))
	int32 Lacunarity = 2;

	/** Per-octave amplitude multiplier. 0.5 is the classic 1/f falloff. Higher
	 *  values push contrast into the fine octaves and look noisier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Gain = 0.5f;

	/** Hash seed. Channels with the same basis and frequency but different
	 *  seeds produce uncorrelated fields. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis")
	int32 Seed = 1337;

	/** How far feature points are displaced from their cell centre, for the
	 *  Worley bases. 1.0 is fully random within the cell; 0.0 collapses to a
	 *  regular grid. Values above ~1.0 can produce missed-neighbour artefacts
	 *  because the search is only 3x3x3. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WorleyJitter = 1.0f;

	/** Fold each octave around its midpoint before summing, producing sharp
	 *  creases instead of smooth undulation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shaping")
	bool bRidged = false;

	/** Flip the final value. Applied last, after normalization and remap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shaping")
	bool bInvert = false;

	/** Output range the normalized [0,1] signal is rescaled into. Narrowing this
	 *  is how you give a channel headroom or a floor without touching the
	 *  material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shaping", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OutputMin = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shaping", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OutputMax = 1.0f;

	/** See ENoiseNormalizeMode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normalization")
	ENoiseNormalizeMode NormalizeMode = ENoiseNormalizeMode::AutoProbe;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normalization", meta = (EditCondition = "NormalizeMode == ENoiseNormalizeMode::Manual", EditConditionHides))
	float ManualMin = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normalization", meta = (EditCondition = "NormalizeMode == ENoiseNormalizeMode::Manual", EditConditionHides))
	float ManualMax = 1.0f;

	/** Lattice cells across the tile at the finest octave. This is the number
	 *  that governs aliasing. */
	int32 GetFinestPeriod() const
	{
		int32 Period = FMath::Max(BasePeriod, 1);
		for (int32 i = 1; i < FMath::Max(Octaves, 1); ++i)
		{
			Period *= FMath::Max(Lacunarity, 2);
		}
		return Period;
	}
};

/** Per-channel normalization actually applied during a bake, recorded so the
 *  material (or any consumer) can recover the pre-normalization value:
 *
 *      Raw = (Stored - Bias) / Scale
 *
 *  Without this a re-bake at a different resolution silently shifts every
 *  threshold downstream, because the probe pass will observe a slightly
 *  different min/max. */
USTRUCT(BlueprintType)
struct NOISEBAKER_API FNoiseChannelNormalization
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normalization")
	float ObservedMin = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normalization")
	float ObservedMax = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normalization")
	float Scale = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normalization")
	float Bias = 0.0f;
};

/** Conservative per-brick bounds over the baked volume, one min/max pair per
 *  channel per brick, stored as bytes in the same 0-255 space as the texture.
 *
 *  Layout: BrickData[((z * BrickDim.Y + y) * BrickDim.X + x) * 8 + i], where
 *  i in [0,4) is the per-channel min in RGBA order and i in [4,8) is the max.
 *
 *  For a raymarcher the min/max lets you bound the composed field over a brick
 *  without sampling it. Because the large-scale structure here is analytic, the
 *  detail bounds alone are not sufficient to declare a brick empty; they bound
 *  the detail term of the product, which is what you multiply the analytic
 *  bound by to get a conservative interval. */
UCLASS(BlueprintType)
class NOISEBAKER_API UNoiseBakeAssetUserData : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** Identifies which bake produced this texture. Recipe and texture both
	 *  carry it; a consumer that cares about stability can assert on it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	FGuid BakeGuid;

	/** Incremented by the recipe on every successful bake. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	int32 BakeVersion = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	int32 Resolution = 0;

	/** Tiling period per channel, in lattice cells across the volume. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	TArray<int32> ChannelBasePeriods;

	/** Normalization applied per channel, in RGBA order. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	TArray<FNoiseChannelNormalization> ChannelNormalization;

	/** Voxels per brick edge. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Acceleration")
	int32 BrickSize = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Acceleration")
	FIntVector BrickDimensions = FIntVector::ZeroValue;

	/** 8 bytes per brick: RGBA min then RGBA max. */
	UPROPERTY()
	TArray<uint8> BrickMinMax;
};

/** Shared validation, used by both the recipe's editor-time checks and the
 *  baker's pre-dispatch guard. */
namespace NoiseBakeValidation
{
	/** Minimum voxels per lattice cell at the finest octave. Below roughly 4 the
	 *  top octave aliases into hash noise, and it will not survive mip
	 *  generation either. */
	static constexpr int32 MinVoxelsPerFinestCell = 4;

	/** Returns true if the channel is bakeable at the given resolution.
	 *  OutError is filled with a human-readable reason when it is not. */
	NOISEBAKER_API bool ValidateChannel(const FNoiseChannelRecipe& Channel, int32 Resolution, const TCHAR* ChannelName, FString& OutError);

	/** Resolution must be a power of two in [16, 512]. */
	NOISEBAKER_API bool ValidateResolution(int32 Resolution, FString& OutError);
}
