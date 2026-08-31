#include "NoiseBakeRecipeBase.h"

#include "NoiseVolumeBaker.h"
#include "Misc/MessageDialog.h"

#define LOCTEXT_NAMESPACE "NoiseBakeRecipeBase"

DEFINE_LOG_CATEGORY_STATIC(LogNoiseBakeRecipe, Log, All);

bool UNoiseBakeRecipeBase::Validate(FString& OutError) const
{
	if (!NoiseBakeValidation::ValidateResolution(Resolution, OutError))
	{
		return false;
	}

	static const TCHAR* ChannelNames[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

	TArray<FNoiseChannelRecipe> Channels;
	GetChannels(Channels);

	if (Channels.Num() != 4)
	{
		OutError = FString::Printf(
			TEXT("GetChannels returned %d entries; the pipeline requires exactly 4 in RGBA order."),
			Channels.Num());
		return false;
	}

	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (!NoiseBakeValidation::ValidateChannel(Channels[Index], Resolution, ChannelNames[Index], OutError))
		{
			return false;
		}
	}

	// Bipolar output requires a signed storage format.
	//
	// BGRA8 is a UNORM format: the texture unit converts byte/255 to [0,1] in
	// fixed-function hardware before the value reaches any shader, and filtering
	// happens in that space too. A signed channel therefore has to be
	// bias-encoded back into [0,1] on write, which means the texture viewer
	// shows it as unsigned, a raymarch samples it as unsigned, and the field
	// only reads correctly if every consumer remembers to apply the recorded
	// decode. Nothing about the asset signals that requirement, so the failure
	// is silent and looks like a bake problem rather than a sampling one.
	if (OutputFormat != ENoiseOutputFormat::RGBA16F)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (Channels[Index].bBipolarOutput)
			{
				OutError = FString::Printf(
					TEXT("Channel %s has bBipolarOutput set, but OutputFormat is BGRA8. BGRA8 is an ")
					TEXT("unsigned format, so the signed value would be bias-encoded back into [0,1] and ")
					TEXT("would read as unsigned everywhere until a consumer applied the recorded decode. ")
					TEXT("Set OutputFormat to RGBA16F, or clear bBipolarOutput and do the [-1,1] conversion ")
					TEXT("at the sample site."),
					ChannelNames[Index]);
				return false;
			}
		}
	}

	// Group members must agree on normalize mode. Mixing modes within a group
	// produces one shared range that is then interpreted two different ways,
	// which defeats the point of grouping.
	for (int32 Group = 1; Group <= 3; ++Group)
	{
		ENoiseNormalizeMode GroupMode = ENoiseNormalizeMode::None;
		int32 FirstMember = INDEX_NONE;

		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (Channels[Index].NormalizationGroup != Group)
			{
				continue;
			}

			if (FirstMember == INDEX_NONE)
			{
				FirstMember = Index;
				GroupMode = Channels[Index].NormalizeMode;
				continue;
			}

			if (Channels[Index].NormalizeMode != GroupMode)
			{
				OutError = FString::Printf(
					TEXT("Normalization group %d mixes normalize modes (channel %s differs from %s). ")
					TEXT("Members of a group share one range, so they must agree on how to interpret it."),
					Group, ChannelNames[Index], ChannelNames[FirstMember]);
				return false;
			}
		}
	}

	if (bBakeBrickBounds)
	{
		if (BrickSize < 2 || !FMath::IsPowerOfTwo(BrickSize) || Resolution % BrickSize != 0)
		{
			OutError = FString::Printf(
				TEXT("BrickSize %d must be a power of two that divides Resolution %d."),
				BrickSize, Resolution);
			return false;
		}
	}

	if (TargetTexture.IsNull() && TargetPackagePath.IsEmpty())
	{
		OutError = TEXT("No TargetTexture assigned and TargetPackagePath is empty; nowhere to write the result.");
		return false;
	}

	return ValidateDerived(OutError);
}

void UNoiseBakeRecipeBase::ValidateOnly()
{
	FString Error;
	if (Validate(Error))
	{
		LastBakeStatus = TEXT("Validation passed.");
		UE_LOG(LogNoiseBakeRecipe, Log, TEXT("[%s] validation passed."), *GetName());
	}
	else
	{
		LastBakeStatus = FString::Printf(TEXT("Validation failed: %s"), *Error);
		UE_LOG(LogNoiseBakeRecipe, Error, TEXT("[%s] %s"), *GetName(), *Error);
	}
}

void UNoiseBakeRecipeBase::Bake()
{
	FString Error;

	if (!FNoiseVolumeBaker::Bake(this, Error))
	{
		LastBakeStatus = FString::Printf(TEXT("FAILED: %s"), *Error);
		UE_LOG(LogNoiseBakeRecipe, Error, TEXT("[%s] bake failed: %s"), *GetName(), *Error);

		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
			FString::Printf(TEXT("Noise bake failed.\n\n%s"), *Error)));
		return;
	}

	MarkPackageDirty();
	UE_LOG(LogNoiseBakeRecipe, Log, TEXT("[%s] %s"), *GetName(), *LastBakeStatus);
}

#undef LOCTEXT_NAMESPACE
