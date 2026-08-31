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
	Value = 0	UMETA(DisplayName = "Value"),

	/** Classic gradient noise. The default for warp and general detail. */
	Perlin = 1	UMETA(DisplayName = "Perlin"),

	/** Cellular F1, inverted so 1.0 sits at the feature point. Billowy. */
	WorleyF1 = 2	UMETA(DisplayName = "Worley F1"),

	/** Cellular F2-F1. Produces the cell-boundary web rather than blobs. */
	WorleyF2MinusF1 = 3	UMETA(DisplayName = "Worley F2 - F1"),

	/** Perlin remapped by inverted Worley. The standard cloud base signal. */
	PerlinWorley = 4	UMETA(DisplayName = "Perlin-Worley"),
};

/** Standardized tiling periods.
 *
 *  Restricted to powers of two because every other value is dominated. The
 *  Nyquist rule caps octaves at floor(log_L(Resolution / (4 * BasePeriod))) + 1,
 *  which is a step function whose breakpoints land on powers of two: periods 3
 *  and 4 permit the same octave count, as do 5 through 8. Within a bracket the
 *  largest value gives the finest base features for the same budget, so the top
 *  of each bracket, which is always a power of two, is the only one worth
 *  offering.
 *
 *  Note that this is the BAKED period. Sampling the result at an integer UV
 *  multiple in a material is also tiling-safe and gives finer features, but it
 *  shortens the repeat distance by the same factor, so it is a trade rather
 *  than a substitute for baking a higher period. Where material-side scaling
 *  does pay is in multi-lookup composition: two lookups at co-prime scales,
 *  say 3 and 5, have a combined period of 15 tiles. Power-of-two scales would
 *  simply collapse back to the largest. */
UENUM(BlueprintType)
enum class ENoiseBasePeriod : uint8
{
	/** Never selected. Exists only because UnrealHeaderTool requires every
	 *  UENUM to have a zero entry, for the default-initialized case. The
	 *  enumerator values here are the periods themselves, which starts at 1, so
	 *  the zero slot has no meaningful period to name. GetBasePeriod maps it to
	 *  1 rather than returning zero and producing a division by zero downstream. */
	Invalid = 0		UMETA(Hidden),

	P1 = 1		UMETA(DisplayName = "1 (whole tile)"),
	P2 = 2		UMETA(DisplayName = "2"),
	P4 = 4		UMETA(DisplayName = "4"),
	P8 = 8		UMETA(DisplayName = "8"),
	P16 = 16	UMETA(DisplayName = "16"),
	P32 = 32	UMETA(DisplayName = "32"),
	P64 = 64	UMETA(DisplayName = "64"),
};

/** Storage format for the baked volume. */
UENUM(BlueprintType)
enum class ENoiseOutputFormat : uint8
{
	/** Uncompressed BGRA8. 4 bytes per voxel. Signed channels are bias-encoded
	 *  into [0,1] on write, which spends the 256 levels across [-1,1] and leaves
	 *  a quantization step of about 1/127.
	 *
	 *  Fine for density. Marginal for a warp or vector field, because warp error
	 *  is amplified by the gradient of whatever it displaces: a stair-step in the
	 *  offset becomes a stair-step in the result. */
	BGRA8 = 0	UMETA(DisplayName = "BGRA8 (4 bpp)"),

	/** Uncompressed RGBA16F. 8 bytes per voxel. Stores signed values directly
	 *  with no encoding, so the decode is identity and precision is not spent on
	 *  representing the sign.
	 *
	 *  The right choice for curl and warp volumes, which want to be low
	 *  resolution anyway: 64^3 is 2 MB, 128^3 is 16 MB. */
	RGBA16F = 1	UMETA(DisplayName = "RGBA16F (8 bpp)"),
};

/** Optional value redistribution applied after normalization.
 *
 *  Every mode here is a MONOTONE remap of value, which means it preserves every
 *  level set exactly: the set {v > t} simply becomes {v > t'}. Features do not
 *  move, isosurfaces keep their geometry, nothing is reshaped spatially. Only
 *  the labels on the values change.
 *
 *  The problem this solves: normalization fixes the RANGE and says nothing about
 *  the DISTRIBUTION within it, and the archetypes differ enormously. Perlin FBM
 *  is near-Gaussian and clusters tightly around the middle. Worley F1 is heavily
 *  skewed low, because in 3D the volume within radius r grows as r^3, so most of
 *  the space is far from any feature point. Ridged variants pile up at the top,
 *  since folding about the midpoint stacks two halves of the distribution.
 *
 *  So pow(v, 2) nudges Perlin slightly dark and nearly annihilates Worley F1.
 *  Redistribution is what lets one set of multipliers and exponents mean the
 *  same thing on every channel. */
UENUM(BlueprintType)
enum class ENoiseDistributionMode : uint8
{
	/** Leave the distribution alone. */
	None = 0	UMETA(DisplayName = "None"),

	/** Apply v^g with g chosen so the median lands exactly on 0.5. Endpoints
	 *  stay pinned at 0 and 1.
	 *
	 *  Fixes the midpoint but not the spread: Perlin stays tightly clustered and
	 *  Worley stays broad, so an exponent still bites harder on one than the
	 *  other, just from a common centre. Cheap, and it preserves most of each
	 *  basis's character. */
	CenterMedian = 1	UMETA(DisplayName = "Center Median"),

	/** Full histogram equalization: every channel comes out uniformly
	 *  distributed over [0,1], so all statistics match and a given exponent or
	 *  multiplier means the same thing everywhere.
	 *
	 *  The cost is character. Perlin's softness comes precisely from its values
	 *  clustering, and uniformizing it reads harsher even though not one feature
	 *  has moved. Ridged variants change most, since the high-end pile-up is
	 *  exactly what equalization flattens.
	 *
	 *  Recoverable, but not with an exponent: pow has no inflection, so it can
	 *  shift mass up or down but cannot gather it toward the middle. That needs
	 *  an S-curve. smoothstep(0, 1, v) in the material re-clusters toward the
	 *  centre, and applied twice approaches Gaussian. The advantage of doing it
	 *  that way is that the same S-curve then means the same thing on every
	 *  channel. */
	Equalize = 2	UMETA(DisplayName = "Equalize"),
};

namespace NoiseBakeConstants
{
	/** Bins in the distribution probe histogram. Only needs to resolve a median
	 *  and a smooth CDF, both of which are far coarser than this. */
	static constexpr int32 HistogramBins = 1024;

	/** Samples in the per-channel equalization LUT uploaded to the shader.
	 *  A CDF is monotone and smooth, so linear interpolation between 64 points
	 *  is well past sufficient. */
	static constexpr int32 EqualizationLutSize = 64;
}

/** Which derivative of a scalar potential a vector field bake produces.
 *
 *  These are the two pure cases of the Helmholtz decomposition, which says any
 *  vector field splits into a gradient part and a curl part. They are exact
 *  complements: a gradient field has zero curl, a curl field has zero
 *  divergence. Picking the wrong one is not a matter of taste. */
UENUM(BlueprintType)
enum class ENoiseVectorFieldMode : uint8
{
	/** curl P of a three-component vector potential. Divergence-free: pure
	 *  swirl, with nothing created or destroyed anywhere in the field.
	 *
	 *  This is what you want for advection and for a warp that stirs material
	 *  around without clumping it. Costs twelve FBM evaluations per voxel and
	 *  three noise instances, because the three output components must all be
	 *  derivatives of ONE potential for the divergence-free property to hold. */
	Curl = 0	UMETA(DisplayName = "Curl (divergence-free, swirl)"),

	/** grad f of a single scalar field. Curl-free: pure sources and sinks.
	 *
	 *  Points uphill toward maxima, so negate at the sample site for attraction
	 *  toward high values. This is the attractor/repulsor field, and it is the
	 *  right tool for a warp that pulls material into or out of features.
	 *
	 *  Half the cost of curl: six evaluations of one noise instance.
	 *
	 *  The magnitude goes to zero at maxima and minima, which is correct rather
	 *  than a defect -- a peak is a stable fixed point with no pull. */
	Gradient = 1	UMETA(DisplayName = "Gradient (curl-free, attract/repel)"),
};

/** What the alpha channel of a vector field bake carries. */
UENUM(BlueprintType)
enum class ENoiseVectorAlphaMode : uint8
{
	/** An entirely independent scalar field, authored like any packed channel. */
	Independent = 0	UMETA(DisplayName = "Independent Channel"),

	/** |curl P| or |grad f|, the local magnitude of the vector field.
	 *
	 *  This looks redundant, since length(rgb) is one instruction. It is not,
	 *  once mips exist: averaging vectors that point in different directions
	 *  shortens them, and averaging magnitudes does not. So length(mip2.rgb)
	 *  systematically underestimates local strength, while a separately baked
	 *  magnitude that has been mip-filtered preserves it. Same asymmetry that
	 *  motivates storing normal length separately for specular antialiasing.
	 *
	 *  Worth the channel only if consumers actually sample lower mips. */
	VectorMagnitude = 1	UMETA(DisplayName = "Vector Magnitude"),

	/** The scalar potential f itself. Gradient mode only.
	 *
	 *  Usually the best default there: f is already evaluated, so storing it is
	 *  free, and one fetch then gives both the field value and its direction of
	 *  steepest ascent. For a warp that is the useful pairing -- you want to know
	 *  where you are on the field as well as which way it slopes.
	 *
	 *  Meaningless in Curl mode, where the potential has three components and no
	 *  single one of them is "the" potential. Validation rejects it there. */
	ScalarPotential = 2	UMETA(DisplayName = "Scalar Potential (gradient only)"),
};

/** How a channel's raw FBM output gets mapped into [0,1].
 *
 *  Note there is no "symmetric about zero" mode, and there deliberately is not
 *  one yet. Symmetric normalization is a property of the SOURCE, not the
 *  destination: it matters when a basis emits genuinely signed values, so that
 *  raw zero and output zero coincide. Every basis here returns [0,1], so raw
 *  zero is the bottom of the range rather than a meaningful centre, and
 *  normalizing symmetrically against it would leave the data occupying part of
 *  the range and waste the rest.
 *
 *  Bipolar output does not need it. The polarity conversion is the last step,
 *  so a channel normalized to the full [0,1] and then remapped to [-1,1] puts
 *  its zero crossing at the midpoint of the observed range, using every level.
 *  Pair it with CenterMedian and the crossing lands on the median instead,
 *  which is what a displacement field with no net drift actually wants.
 *
 *  When a signed basis arrives -- curl of a vector potential -- a symmetric mode
 *  comes back, scoped to that basis rather than to the output polarity. */
UENUM(BlueprintType)
enum class ENoiseNormalizeMode : uint8
{
	/** Bake a low-resolution probe pass first, take the observed min/max, pad
	 *  slightly, and use that. Correct in almost every case and costs ~1% of the
	 *  bake. This is what you want unless you have a reason otherwise. */
	AutoProbe = 0	UMETA(DisplayName = "Auto (Probe Pass)"),

	/** Use ManualMin/ManualMax. Use this when two channels must share an
	 *  identical range, or when you are re-baking at a new resolution and need
	 *  byte-for-byte comparable output to a previous bake. */
	Manual = 1	UMETA(DisplayName = "Manual Range"),

	/** Emit the raw FBM value clamped to [0,1]. The basis functions already
	 *  return [0,1], so this is only lossy for ridged/multi-octave setups that
	 *  do not reach the full range. */
	None = 2	UMETA(DisplayName = "None"),
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

	/** Lattice cells across the tile at octave 0, and therefore the tiling
	 *  period: the baked texture repeats exactly every BasePeriod cells.
	 *
	 *  Larger values give finer base features and more unique content before the
	 *  repeat becomes readable, at the cost of Nyquist headroom for the higher
	 *  octaves. See GetMaxOctaves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis")
	ENoiseBasePeriod BasePeriod = ENoiseBasePeriod::P4;

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

	/** Replaces Worley's strict nearest-point minimum with a weighted blend of
	 *  all 27 candidates. Worley bases only; 0 is the classic behaviour.
	 *
	 *  Strict min is C0 but not C1: the derivative jumps wherever the nearest
	 *  feature point switches, which is exactly on the cell boundaries. In a
	 *  density field that is invisible. In a DERIVATIVE field it is ruinous,
	 *  because a discontinuity in the potential's slope becomes a discontinuity
	 *  in the output value of a curl or gradient bake, and it reads as hard cell
	 *  walls.
	 *
	 *  The soft form has no argmin to be discontinuous: every neighbour
	 *  contributes always, with weights that vary smoothly with distance. It is
	 *  free, since the 27 distances are already computed either way.
	 *
	 *  This is a character control, not a fix to leave at maximum. Raising it
	 *  rounds cells toward blobs, and far enough up the field starts to resemble
	 *  Value noise, at which point Perlin is cheaper. 0.3 to 0.6 is the useful
	 *  band for a potential.
	 *
	 *  Only F1 is affected. F2 needs an actual ranking, so F2-F1 stays C0-only
	 *  and remains a density-only basis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Basis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WorleySmoothness = 0.0f;

	/** Fold each octave around its midpoint before summing, producing sharp
	 *  creases instead of smooth undulation.
	 *
	 *  Baked because it applies INSIDE the octave loop. Once the octaves are
	 *  summed there is no way to recover it, which is the test for whether an
	 *  operation belongs here at all: anything applied per-octave must be baked,
	 *  anything applied to the finished value can be deferred to the consumer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shaping")
	bool bRidged = false;

	/** Redistribution applied after normalization. See ENoiseDistributionMode.
	 *
	 *  Baked despite operating on the finished value, because Equalize needs the
	 *  CDF of the entire volume and a consumer sampling one texel does not have
	 *  it. This is the exception that proves the rule. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shaping")
	ENoiseDistributionMode DistributionMode = ENoiseDistributionMode::None;

	/** Emit [-1,1] instead of [0,1], applied last as v * 2 - 1.
	 *
	 *  The only range control. Channels always occupy the full unit interval
	 *  otherwise, because a narrowed output range would spend storage levels on
	 *  something a multiply at the sample site does for free: baking [0, 0.5]
	 *  into BGRA8 uses 128 of 256 levels and throws away a bit for no gain.
	 *
	 *  This one earns its place because it changes the DECODE CONTRACT rather
	 *  than just the value. The stored bytes on BGRA8 are identical either way;
	 *  what differs is the DecodeScale/DecodeBias recorded for the consumer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shaping")
	bool bBipolarOutput = false;

	/** True when the final value can go below zero, which is what triggers bias
	 *  encoding on an unsigned storage format. */
	bool IsSigned() const
	{
		return bBipolarOutput;
	}

	/** See ENoiseNormalizeMode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normalization")
	ENoiseNormalizeMode NormalizeMode = ENoiseNormalizeMode::AutoProbe;

	/** Channels sharing a non-zero group id are normalized together: the probe
	 *  ranges are merged and one scale and bias is applied across all of them.
	 *  0 means normalize independently.
	 *
	 *  This exists for vector-valued output. Normalizing the components of a
	 *  vector independently rescales each axis by a different factor, which
	 *  rotates and skews every vector in the field. The result still looks like
	 *  plausible noise, which is what makes it dangerous: nothing about the
	 *  preview says the directions are wrong.
	 *
	 *  For a curl or warp field, put R, G and B in group 1 and leave A at 0.
	 *  Group members should also share a normalize mode; one shared range
	 *  interpreted two different ways defeats the purpose. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normalization", meta = (ClampMin = "0", ClampMax = "3"))
	int32 NormalizationGroup = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normalization", meta = (EditCondition = "NormalizeMode == ENoiseNormalizeMode::Manual", EditConditionHides))
	float ManualMin = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normalization", meta = (EditCondition = "NormalizeMode == ENoiseNormalizeMode::Manual", EditConditionHides))
	float ManualMax = 1.0f;

	/** BasePeriod as a plain integer.
	 *
	 *  The clamp is what makes the mandatory Invalid = 0 enumerator harmless: a
	 *  default-initialized struct falls through to period 1 rather than zero,
	 *  which would divide by zero in the lattice mapping. */
	int32 GetBasePeriod() const
	{
		return FMath::Max((int32)BasePeriod, 1);
	}

	/** Lattice cells across the tile at the finest octave. This is the number
	 *  that governs aliasing. */
	int32 GetFinestPeriod() const
	{
		int32 Period = GetBasePeriod();
		for (int32 i = 1; i < FMath::Max(Octaves, 1); ++i)
		{
			Period *= FMath::Max(Lacunarity, 2);
		}
		return Period;
	}
};

/** Per-channel normalization and encoding actually applied during a bake.
 *
 *  Two separate transforms are recorded, and the distinction matters.
 *
 *  Normalization maps the raw FBM output into [0,1] before the output range is
 *  applied. Recording it means a re-bake at a different resolution, which will
 *  observe a slightly different probe range, does not silently shift every
 *  threshold downstream.
 *
 *  Encoding is what makes a signed value fit an unsigned storage format. The
 *  consumer recovers the field value with a single uniform rule regardless of
 *  format or sign:
 *
 *      Value = Stored * DecodeScale + DecodeBias
 *
 *  which is (1, 0) for unsigned BGRA8 and for all RGBA16F, and (2, -1) for a
 *  signed channel in BGRA8. */
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

	/** Applied on write: Stored = Value * EncodeScale + EncodeBias. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Encoding")
	float EncodeScale = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Encoding")
	float EncodeBias = 0.0f;

	/** Inverse of the above, for the material. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Encoding")
	float DecodeScale = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Encoding")
	float DecodeBias = 0.0f;

	/** Redistribution actually applied. Recorded because it is baked in and
	 *  irreversible from the texture alone: two visually similar textures can
	 *  respond very differently to the same exponent if one was equalized. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Distribution")
	ENoiseDistributionMode DistributionMode = ENoiseDistributionMode::None;

	/** Exponent used by CenterMedian, or 1 otherwise. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Distribution")
	float DistributionGamma = 1.0f;

	/** Median of the normalized distribution as measured by the probe, before
	 *  any redistribution. 0.5 means the channel was already centred. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Distribution")
	float ObservedMedian = 0.5f;

	/** True when the channel emits [-1,1]. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Encoding")
	bool bBipolar = false;

	/** Group this channel was normalized with, echoed from the recipe. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normalization")
	int32 NormalizationGroup = 0;
};

/** Conservative per-brick bounds over the baked volume, one min/max pair per
 *  channel per brick.
 *
 *  Stored as floats in DECODED field space, not raw storage bytes. That means
 *  the bounds carry sign for a signed channel, and they stay meaningful when
 *  the output format changes. Layout:
 *
 *      BrickMinMax[(((z * Dim.Y + y) * Dim.X + x) * 8) + i]
 *
 *  with i in [0,4) the per-channel minimum in RGBA order and i in [4,8) the
 *  maximum.
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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	ENoiseOutputFormat OutputFormat = ENoiseOutputFormat::BGRA8;

	/** Tiling period per channel, in lattice cells across the volume. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	TArray<int32> ChannelBasePeriods;

	/** Normalization and encoding applied per channel, in RGBA order. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	TArray<FNoiseChannelNormalization> ChannelNormalization;

	/** Voxels per brick edge. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Acceleration")
	int32 BrickSize = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Acceleration")
	FIntVector BrickDimensions = FIntVector::ZeroValue;

	/** 8 floats per brick: RGBA min then RGBA max, in decoded field space. */
	UPROPERTY()
	TArray<float> BrickMinMax;
};

/** Shared validation, used by both the recipe's editor-time checks and the
 *  baker's pre-dispatch guard. */
namespace NoiseBakeValidation
{
	/** Minimum voxels per lattice cell at the finest octave. Below roughly 4 the
	 *  top octave aliases into hash noise, and it will not survive mip
	 *  generation either. */
	static constexpr int32 MinVoxelsPerFinestCell = 4;

	/** Highest octave count that satisfies the Nyquist rule for the given
	 *  resolution, base period and lacunarity. Returns 0 when the base period
	 *  alone already violates it, in which case no octave count validates. */
	NOISEBAKER_API int32 GetMaxOctaves(int32 Resolution, int32 BasePeriod, int32 Lacunarity);

	/** Returns true if the channel is bakeable at the given resolution.
	 *  OutError is filled with a human-readable reason when it is not. */
	NOISEBAKER_API bool ValidateChannel(const FNoiseChannelRecipe& Channel, int32 Resolution, const TCHAR* ChannelName, FString& OutError);

	/** Resolution must be a power of two in [16, 512]. */
	NOISEBAKER_API bool ValidateResolution(int32 Resolution, FString& OutError);
}