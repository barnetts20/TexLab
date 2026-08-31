#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NoiseBakeTypes.h"
#include "NoiseBakeRecipe.generated.h"

class UVolumeTexture;

/** A reproducible description of one packed volume noise bake.
 *
 *  The recipe is the authored artefact; the volume texture is derived output.
 *  Keeping them separate is what makes it possible to re-bake at 4x resolution,
 *  or to change one channel, without re-authoring anything. It also means the
 *  bake parameters are diffable in source control, which a raw texture is not.
 *
 *  Every successful bake stamps a GUID and increments BakeVersion, and both are
 *  written into the texture's asset user data. Once anything downstream depends
 *  on the field, that stamp is how you detect that the texture in front of you
 *  is not the one that content was authored against. */
UCLASS(BlueprintType)
class NOISEBAKEREDITOR_API UNoiseBakeRecipe : public UDataAsset
{
	GENERATED_BODY()

public:
	// -- Output -------------------------------------------------------------

	/** Voxels per axis. Power of two, 16 to 512.
	 *
	 *  128 is a good working resolution; 256 is a good shipping resolution for
	 *  a detail volume. 512^3 BGRA8 is 512MB uncompressed, which is almost
	 *  certainly not what you want for a detail texture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (ClampMin = "16", ClampMax = "512"))
	int32 Resolution = 128;

	/** Jittered samples per voxel edge; total cost scales by the cube. 1 is
	 *  fine while iterating. 2 (8 samples) noticeably cleans up high-octave
	 *  content and is cheap enough for a final bake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (ClampMin = "1", ClampMax = "4"))
	int32 Supersample = 1;

	/** Storage format. BGRA8 for density and detail; RGBA16F when any channel
	 *  is signed and precision matters, which is the usual case for curl and
	 *  warp volumes. See ENoiseOutputFormat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	ENoiseOutputFormat OutputFormat = ENoiseOutputFormat::BGRA8;

	/** Adds sub-LSB triangular dither before quantizing to 8 bits. Ignored for
	 *  RGBA16F, which has no uniform quantization step to dither against.
	 *
	 *  Worth having on for anything a raymarcher integrates through, where
	 *  8-bit banding shows up as visible shells. Turn it off if a consumer
	 *  thresholds the channel and you want quantization to be deterministic
	 *  rather than dithered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bDither = true;

	/** Generate a mip chain. Safe for tiling volumes: a 2x2x2 box reduction
	 *  never reads outside the volume, so each mip stays exactly periodic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bGenerateMips = true;

	// -- Channels -----------------------------------------------------------
	//
	// One flat category with four struct rows, rather than a "Channels|R"
	// subcategory per channel. The subcategory was the extra level: it creates a
	// category AND a subcategory, and the struct property then adds its own
	// expander on top, so the panel nested three deep before reaching a
	// parameter. ShowOnlyInnerProperties removes the struct's expander but not
	// the two category levels, which is why it did not help here.
	//
	// Note that ShowOnlyInnerProperties must NOT come back now. With all four
	// channels in one category it would splice all four structs' members into a
	// single undifferentiated list with no indication of which channel any given
	// Seed or Basis belongs to.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channels", meta = (DisplayName = "R"))
	FNoiseChannelRecipe RedChannel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channels", meta = (DisplayName = "G"))
	FNoiseChannelRecipe GreenChannel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channels", meta = (DisplayName = "B"))
	FNoiseChannelRecipe BlueChannel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channels", meta = (DisplayName = "A"))
	FNoiseChannelRecipe AlphaChannel;

	// -- Acceleration -------------------------------------------------------

	/** Bake a per-brick min/max pyramid into the texture's asset user data.
	 *  Costs a fraction of a percent of bake time and a few hundred KB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acceleration")
	bool bBakeBrickBounds = true;

	/** Voxels per brick edge. 8 gives 16^3 bricks at 128^3. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acceleration", meta = (ClampMin = "2", ClampMax = "32", EditCondition = "bBakeBrickBounds"))
	int32 BrickSize = 8;

	// -- Validation ---------------------------------------------------------

	/** Verify periodicity on the GPU before committing the bake.
	 *
	 *  Evaluates a probe grid at UVW and again at UVW + 1 along each axis. The
	 *  field is periodic with period exactly 1.0 in UVW, so the two must agree.
	 *  This tests the evaluator rather than the output, which is the only way
	 *  to catch a wrap bug reliably; a visual 2x2 tiled preview will not show a
	 *  subtly wrong modulo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	bool bValidateTiling = true;

	/** Max permitted absolute difference in the tiling self-test. Differences
	 *  at this magnitude are float precision from the larger UVW magnitude, not
	 *  a real seam. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bValidateTiling"))
	float TilingTolerance = 1e-4f;

	// -- Target -------------------------------------------------------------

	/** Existing texture to overwrite. If null, one is created at
	 *  TargetPackagePath / TargetAssetName and assigned here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	TSoftObjectPtr<UVolumeTexture> TargetTexture;

	/** Content path for a newly created texture, e.g. /Game/Noise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	FString TargetPackagePath = TEXT("/Game/Noise");

	/** Asset name for a newly created texture. Empty means "<RecipeName>_VT". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	FString TargetAssetName;

	/** Save the texture package immediately after baking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	bool bSaveAfterBake = true;

	// -- Bake record --------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake Record")
	FGuid LastBakeGuid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake Record")
	int32 BakeVersion = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake Record")
	FString LastBakeStatus;

	/** Observed normalization per channel from the most recent bake, RGBA
	 *  order. Useful for pinning a channel to Manual once you like the range. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake Record")
	TArray<FNoiseChannelNormalization> LastBakeNormalization;

	// -- Actions ------------------------------------------------------------

	UFUNCTION(CallInEditor, Category = "Bake", meta = (DisplayName = "Bake Volume Texture"))
	void Bake();

	UFUNCTION(CallInEditor, Category = "Bake", meta = (DisplayName = "Validate Only"))
	void ValidateOnly();

	/** Returns the four channels in RGBA order. */
	void GetChannels(TArray<FNoiseChannelRecipe>& OutChannels) const;

	/** Runs every validation rule. Returns false and fills OutError on the
	 *  first failure. */
	bool Validate(FString& OutError) const;
};