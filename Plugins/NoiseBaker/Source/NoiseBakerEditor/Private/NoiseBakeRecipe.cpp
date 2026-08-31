#include "NoiseBakeRecipe.h"

#include "NoiseVolumeBaker.h"
#include "Misc/MessageDialog.h"

#define LOCTEXT_NAMESPACE "NoiseBakeRecipe"

DEFINE_LOG_CATEGORY_STATIC(LogNoiseBakeRecipe, Log, All);

void UNoiseBakeRecipe::GetChannels(TArray<FNoiseChannelRecipe>& OutChannels) const
{
	OutChannels.Reset(4);
	OutChannels.Add(RedChannel);
	OutChannels.Add(GreenChannel);
	OutChannels.Add(BlueChannel);
	OutChannels.Add(AlphaChannel);
}

bool UNoiseBakeRecipe::Validate(FString& OutError) const
{
	if (!NoiseBakeValidation::ValidateResolution(Resolution, OutError))
	{
		return false;
	}

	static const TCHAR* ChannelNames[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

	TArray<FNoiseChannelRecipe> Channels;
	GetChannels(Channels);

	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (!NoiseBakeValidation::ValidateChannel(Channels[Index], Resolution, ChannelNames[Index], OutError))
		{
			return false;
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

	return true;
}

void UNoiseBakeRecipe::ValidateOnly()
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

void UNoiseBakeRecipe::Bake()
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
