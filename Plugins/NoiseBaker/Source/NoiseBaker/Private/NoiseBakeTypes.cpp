#include "NoiseBakeTypes.h"

int32 FNoiseChannelRecipe::GetEffectiveOctaves(int32 Resolution) const
{
	const int32 MaxOctaves = NoiseBakeValidation::GetMaxOctaves(
		Resolution, GetBasePeriod(), Lacunarity);

	if (MaxOctaves == 0)
	{
		return 0;
	}

	return FMath::Clamp(Octaves, 1, MaxOctaves);
}

int32 FNoiseChannelRecipe::GetFinestPeriod(int32 Resolution) const
{
	int32 Period = GetBasePeriod();

	for (int32 i = 1; i < FMath::Max(GetEffectiveOctaves(Resolution), 1); ++i)
	{
		Period *= FMath::Max(Lacunarity, 2);
	}

	return Period;
}

namespace NoiseBakeValidation
{
	bool ValidateResolution(int32 Resolution, FString& OutError)
	{
		if (Resolution < 16 || Resolution > 512)
		{
			OutError = FString::Printf(
				TEXT("Resolution %d is out of range. Supported range is 16 to 512."), Resolution);
			return false;
		}

		if (!FMath::IsPowerOfTwo(Resolution))
		{
			OutError = FString::Printf(
				TEXT("Resolution %d is not a power of two. Non-power-of-two volumes cannot generate ")
				TEXT("mips cleanly, and a box reduction of a tiling volume only stays tiling when each ")
				TEXT("mip halves exactly."), Resolution);
			return false;
		}

		return true;
	}

	int32 GetMaxOctaves(int32 Resolution, int32 BasePeriod, int32 Lacunarity)
	{
		BasePeriod = FMath::Max(BasePeriod, 1);
		Lacunarity = FMath::Max(Lacunarity, 2);

		// The finest octave must still get MinVoxelsPerFinestCell voxels per
		// cell. Counted by repeated multiplication rather than a log, so the
		// answer cannot disagree with GetFinestPeriod by a rounding step.
		if (BasePeriod * MinVoxelsPerFinestCell > Resolution)
		{
			return 0;
		}

		int32 Octaves = 1;
		int32 Period = BasePeriod;

		while ((int64)Period * Lacunarity * MinVoxelsPerFinestCell <= (int64)Resolution)
		{
			Period *= Lacunarity;
			Octaves++;
		}

		return Octaves;
	}

	bool ValidateChannel(const FNoiseChannelRecipe& Channel, int32 Resolution, const TCHAR* ChannelName, FString& OutError)
	{
		const int32 BasePeriod = Channel.GetBasePeriod();

		if (Channel.Octaves < 1)
		{
			OutError = FString::Printf(TEXT("%s: Octaves must be at least 1."), ChannelName);
			return false;
		}

		if (Channel.Lacunarity < 2)
		{
			OutError = FString::Printf(
				TEXT("%s: Lacunarity is %d. It must be an integer of at least 2. Exact tiling requires ")
				TEXT("every octave to complete a whole number of lattice cells across the volume, which ")
				TEXT("fractional lacunarity cannot satisfy."), ChannelName, Channel.Lacunarity);
			return false;
		}

		// Nyquist. The finest octave lays FinestPeriod cells across the volume;
		// below a few voxels per cell it degenerates into uncorrelated hash noise
		// that also destroys the mip chain.
		const int32 MaxOctaves = GetMaxOctaves(Resolution, BasePeriod, Channel.Lacunarity);

		if (MaxOctaves == 0)
		{
			OutError = FString::Printf(
				TEXT("%s: base period %d needs at least %d voxels of resolution on its own, but the ")
				TEXT("volume is %d^3. Drop the base period or raise the resolution."),
				ChannelName, BasePeriod, BasePeriod * MinVoxelsPerFinestCell, Resolution);
			return false;
		}

		// Deliberately NOT an error when the request exceeds MaxOctaves.
		//
		// The octave count is a request for detail, and the honest response to
		// asking for more than fits is to give as much as fits. Rejecting made
		// the count a value that had to be recomputed by hand every time the
		// resolution changed, which meant a recipe that baked at 256 would fail
		// at 128 for a reason that has nothing to do with what was authored.
		//
		// The clamp happens in GetEffectiveOctaves and is logged once per bake
		// where the value is consumed, so it is visible without being an
		// obstacle. The zero case above stays fatal because no octave count
		// rescues a base period that will not fit on its own.

		if (Channel.NormalizeMode == ENoiseNormalizeMode::Manual &&
			Channel.ManualMax - Channel.ManualMin < KINDA_SMALL_NUMBER)
		{
			OutError = FString::Printf(
				TEXT("%s: manual normalization range is degenerate (%.6f to %.6f)."),
				ChannelName, Channel.ManualMin, Channel.ManualMax);
			return false;
		}

		return true;
	}
}