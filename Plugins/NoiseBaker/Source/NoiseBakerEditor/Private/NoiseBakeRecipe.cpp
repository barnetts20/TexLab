#include "NoiseBakeRecipe.h"

void UNoiseBakeRecipe::GetChannels(TArray<FNoiseChannelRecipe>& OutChannels) const
{
	OutChannels.Reset(4);
	OutChannels.Add(RedChannel);
	OutChannels.Add(GreenChannel);
	OutChannels.Add(BlueChannel);
	OutChannels.Add(AlphaChannel);
}