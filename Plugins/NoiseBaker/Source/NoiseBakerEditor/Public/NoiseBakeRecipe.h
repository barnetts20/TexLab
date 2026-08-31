#pragma once

#include "CoreMinimal.h"
#include "NoiseBakeRecipeBase.h"
#include "NoiseBakeRecipe.generated.h"

/** A packed four-channel volume noise bake.
 *
 *  Four entirely independent scalar fields sharing one texture: separate basis,
 *  frequency, seed, octave count and shaping per channel. Nothing is shared
 *  between them unless you opt in through NormalizationGroup.
 *
 *  This is the general case. Where the four channels are NOT independent -- a
 *  curl bake, where one potential drives three components -- the authoring
 *  surface belongs in its own asset rather than here, because expressing it
 *  through four channel structs would mean three that must be kept identical
 *  and a comment explaining the rule. See UCurlBakeRecipe.
 *
 *  The recipe is the authored artefact; the volume texture is derived output.
 *  Keeping them separate is what makes it possible to re-bake at 4x resolution,
 *  or change one channel, without re-authoring anything. */
UCLASS(BlueprintType)
class NOISEBAKEREDITOR_API UNoiseBakeRecipe : public UNoiseBakeRecipeBase
{
	GENERATED_BODY()

public:
	// One flat category with four struct rows, rather than a "Channels|R"
	// subcategory per channel. The subcategory was an extra nesting level: it
	// creates a category AND a subcategory, and the struct property then adds
	// its own expander on top.
	//
	// ShowOnlyInnerProperties must NOT be added here. With all four channels in
	// one category it would splice all four structs' members into a single
	// undifferentiated list with no indication of which channel any given Seed
	// or Basis belongs to.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channels", meta = (DisplayName = "R"))
	FNoiseChannelRecipe RedChannel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channels", meta = (DisplayName = "G"))
	FNoiseChannelRecipe GreenChannel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channels", meta = (DisplayName = "B"))
	FNoiseChannelRecipe BlueChannel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channels", meta = (DisplayName = "A"))
	FNoiseChannelRecipe AlphaChannel;

	virtual void GetChannels(TArray<FNoiseChannelRecipe>& OutChannels) const override;
};