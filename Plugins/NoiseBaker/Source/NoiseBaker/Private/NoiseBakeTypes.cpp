#include "NoiseBakeTypes.h"

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

	bool ValidateChannel(const FNoiseChannelRecipe& Channel, int32 Resolution, const TCHAR* ChannelName, FString& OutError)
	{
		if (Channel.BasePeriod < 1)
		{
			OutError = FString::Printf(TEXT("%s: BasePeriod must be at least 1."), ChannelName);
			return false;
		}

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
		const int32 FinestPeriod = Channel.GetFinestPeriod();
		const int32 RequiredResolution = FinestPeriod * MinVoxelsPerFinestCell;

		if (Resolution < RequiredResolution)
		{
			OutError = FString::Printf(
				TEXT("%s: finest octave lays %d cells across the volume (BasePeriod %d x Lacunarity %d ^ %d octaves), ")
				TEXT("which needs at least %d voxels of resolution to resolve. Current resolution is %d. ")
				TEXT("Reduce Octaves, reduce BasePeriod, or raise Resolution."),
				ChannelName, FinestPeriod, Channel.BasePeriod, Channel.Lacunarity, Channel.Octaves - 1,
				RequiredResolution, Resolution);
			return false;
		}

		if (Channel.OutputMax < Channel.OutputMin)
		{
			OutError = FString::Printf(
				TEXT("%s: OutputMax (%.3f) is below OutputMin (%.3f). Use bInvert to flip the channel ")
				TEXT("rather than crossing the output range."),
				ChannelName, Channel.OutputMax, Channel.OutputMin);
			return false;
		}

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
