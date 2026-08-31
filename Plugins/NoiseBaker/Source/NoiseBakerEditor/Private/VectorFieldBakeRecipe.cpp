#include "VectorFieldBakeRecipe.h"

#include "NoiseVolumeBaker.h"

namespace VectorFieldBakeInternal
{
	/** Seed offsets for the three potential components in Curl mode.
	 *
	 *  Deliberately NOT multiples of 0x9E3779B9, which is what PN_FBM adds per
	 *  octave. A component seed landing on another component's octave seed would
	 *  correlate two axes of the potential, and a correlated potential produces
	 *  a curl field with a preferred direction. Subtle, and invisible in a
	 *  preview. */
	static constexpr int32 ComponentSeedOffset[3] = { 0, 0x51ED2701, 0x2545F491 };

	/** Mirrors NVB_VEC_ALPHA_* in NoiseBakeCS.usf. */
	static constexpr int32 AlphaIndependent = 0;
	static constexpr int32 AlphaMagnitude = 1;
	static constexpr int32 AlphaPotential = 2;
}

UVectorFieldBakeRecipe::UVectorFieldBakeRecipe()
{
	// A derivative field is signed, and validation requires a signed storage
	// format for any bipolar channel. Defaulting it here means a new asset is
	// valid on creation rather than failing the first bake on a format error.
	OutputFormat = ENoiseOutputFormat::RGBA16F;

	// See the note on Potential: differentiation flattens the octave spectrum,
	// so the packed recipe's 0.5 produces static here.
	Potential.Gain = 0.3f;
	Potential.Octaves = 3;
}

void UVectorFieldBakeRecipe::GetChannels(TArray<FNoiseChannelRecipe>& OutChannels) const
{
	OutChannels.Reset(4);

	const bool bCurl = (Mode == ENoiseVectorFieldMode::Curl);

	// The three RGB entries describe the OUTPUT components, not the inputs. Their
	// basis settings feed the potential evaluation (all three in Curl mode, only
	// index 0 in Gradient mode), while their normalization and shaping settings
	// are what the probe and the shaper apply to the resulting vector.
	//
	// Presenting them as ordinary channels is what lets the probe, the
	// normalization grouping, the parameter packing and the recorded metadata
	// stay entirely unaware that this is a derivative bake at all.
	for (int32 Component = 0; Component < 3; ++Component)
	{
		FNoiseChannelRecipe C = Potential;

		// Curl needs three DIFFERENT instances: one instance for all three
		// components collapses the cross terms and confines the output to a
		// plane. Gradient needs exactly one, and only index 0 is ever read, so
		// the seed is left alone for the other two.
		if (bCurl)
		{
			C.Seed = Potential.Seed + VectorFieldBakeInternal::ComponentSeedOffset[Component];
		}

		// Forced, not authored, and each for its own reason.

		// One shared scale across the three components. Normalizing them
		// independently rescales each axis by a different factor, which rotates
		// and skews every vector in the field. The result still looks like
		// plausible noise, which is what makes it dangerous.
		C.NormalizationGroup = 1;

		// The sign of a component is its direction along that axis.
		C.bBipolarOutput = true;

		// Redistribution is monotone, so on a scalar it moves nothing. On a
		// VECTOR it is destructive for the same reason independent normalization
		// is: each component would get a different remap, which changes the
		// direction of every vector. There is no per-component redistribution
		// that preserves a vector field.
		C.DistributionMode = ENoiseDistributionMode::None;

		OutChannels.Add(C);
	}

	switch (AlphaMode)
	{
	case ENoiseVectorAlphaMode::VectorMagnitude:
	case ENoiseVectorAlphaMode::ScalarPotential:
	{
		// Both are derived in the shader rather than evaluated from their own
		// basis, so the recipe carried here only supplies normalization and
		// shaping. Its basis fields are copied from the potential so the
		// recorded metadata reports something meaningful rather than defaults.
		FNoiseChannelRecipe Derived = Potential;
		Derived.NormalizationGroup = 0;
		Derived.bBipolarOutput = false;
		Derived.DistributionMode = ENoiseDistributionMode::None;

		OutChannels.Add(Derived);
		break;
	}

	case ENoiseVectorAlphaMode::Independent:
	default:
		OutChannels.Add(AlphaChannel);
		break;
	}
}

void UVectorFieldBakeRecipe::ConfigureEvaluation(FNoiseBakeDispatchParams& InOutParams) const
{
	InOutParams.EvalMode = (Mode == ENoiseVectorFieldMode::Curl)
		? ENoiseEvalMode::Curl
		: ENoiseEvalMode::Gradient;

	// Voxels to UVW. Resolution is a power of two in [16, 512] by validation, so
	// this cannot divide by zero.
	InOutParams.CurlEpsilon = EpsilonVoxels / (float)FMath::Max(Resolution, 1);

	int32 PackedAlphaMode = VectorFieldBakeInternal::AlphaIndependent;
	if (AlphaMode == ENoiseVectorAlphaMode::VectorMagnitude)
	{
		PackedAlphaMode = VectorFieldBakeInternal::AlphaMagnitude;
	}
	else if (AlphaMode == ENoiseVectorAlphaMode::ScalarPotential)
	{
		PackedAlphaMode = VectorFieldBakeInternal::AlphaPotential;
	}

	// Rides in the unused w slot of the alpha channel's int block.
	InOutParams.ChannelParamsC[3].W = PackedAlphaMode;
}

bool UVectorFieldBakeRecipe::IsChannelSourceSigned(int32 ChannelIndex) const
{
	// RGB only. A derivative of a scalar field is genuinely signed before any
	// shaping, so it normalizes about zero rather than about the midpoint of the
	// observed range. Otherwise the field's zero -- which for a gradient is
	// every maximum and minimum, and for a curl is every stagnation point --
	// would sit wherever the probe happened to land.
	//
	// The alpha channel is unsigned in every mode: an independent basis returns
	// [0,1], a magnitude is non-negative, and the scalar potential is a raw FBM
	// value in [0,1].
	return ChannelIndex >= 0 && ChannelIndex <= 2;
}

bool UVectorFieldBakeRecipe::ValidateDerived(FString& OutError) const
{
	if (Mode == ENoiseVectorFieldMode::Curl && AlphaMode == ENoiseVectorAlphaMode::ScalarPotential)
	{
		OutError = TEXT(
			"AlphaMode is Scalar Potential, but Mode is Curl. A curl bake's potential has three "
			"components and no single one of them is 'the' potential, so the setting has no meaning "
			"here. Use Vector Magnitude or an Independent channel, or switch Mode to Gradient.");
		return false;
	}

	if (EpsilonVoxels < 0.05f)
	{
		OutError = FString::Printf(
			TEXT("EpsilonVoxels is %.4f. Below about 0.05 the central difference subtracts two nearly "
				 "equal FBM values and the result is dominated by float cancellation rather than by the "
				 "field."),
			EpsilonVoxels);
		return false;
	}

	// The epsilon has to resolve the finest octave. That octave lays
	// FinestPeriod cells across the tile, so one cell is 1/FinestPeriod in UVW
	// and the step is EpsilonVoxels/Resolution. Stepping across a whole cell
	// differences unrelated parts of the field rather than measuring a slope.
	const float EpsilonUVW = EpsilonVoxels / (float)FMath::Max(Resolution, 1);
	const float FinestCellUVW = 1.0f / (float)FMath::Max(Potential.GetFinestPeriod(), 1);

	if (EpsilonUVW > FinestCellUVW * 0.5f)
	{
		OutError = FString::Printf(
			TEXT("EpsilonVoxels %.4f is %.1f%% of the finest octave's cell size. The central difference "
				 "would span a significant fraction of a lattice cell and smooth away the detail the "
				 "octave count is paying for. Reduce EpsilonVoxels, reduce Octaves, or raise Resolution."),
			EpsilonVoxels, 100.0f * EpsilonUVW / FinestCellUVW);
		return false;
	}

	// Not fatal, but the single most common way a derivative bake disappoints.
	const float EffectiveFalloff = Potential.Gain * (float)FMath::Max(Potential.Lacunarity, 2);
	if (EffectiveFalloff >= 1.0f && Potential.Octaves > 1)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("VectorFieldBakeRecipe '%s': Gain %.2f x Lacunarity %d = %.2f. Differentiation "
				 "multiplies each octave by its frequency, so the output's octave amplitudes fall as "
				 "(Gain x Lacunarity)^i. At 1.0 or above the finest octave dominates and the field reads "
				 "as static. Try Gain %.2f."),
			*GetName(), Potential.Gain, Potential.Lacunarity, EffectiveFalloff,
			0.5f / (float)FMath::Max(Potential.Lacunarity, 2));
	}

	return true;
}
