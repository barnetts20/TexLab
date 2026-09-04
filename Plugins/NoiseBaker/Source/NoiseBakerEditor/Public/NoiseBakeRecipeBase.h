#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NoiseBakeTypes.h"
#include "NoiseBakeRecipeBase.generated.h"

class UVolumeTexture;
struct FNoiseBakeDispatchParams;

/** Everything a bake needs that is not the description of the field itself.
 *
 *  The split exists because the pipeline downstream of evaluation is genuinely
 *  identical for every kind of bake: slab dispatch and its TDR chunking, the
 *  range and distribution probes, normalization grouping, quantization,
 *  brick bounds, the tiling self-test, texture creation and the bake stamp.
 *  Only the description of what to evaluate differs.
 *
 *  Subclasses supply that description through two hooks. GetChannels returns
 *  the effective RGBA view, which is what the probe, the normalization pass and
 *  the recorded metadata all operate on. ConfigureEvaluation sets the
 *  evaluation mode and any mode-specific parameters on the dispatch block.
 *
 *  A subclass whose authored parameters do not map one-to-one onto four
 *  independent channels -- a curl bake has ONE potential driving three
 *  components -- still presents four channels here. That keeps the entire
 *  pipeline unaware of the distinction while letting the authoring surface tell
 *  the truth about its own shape. */
UCLASS(Abstract, BlueprintType)
class NOISEBAKEREDITOR_API UNoiseBakeRecipeBase : public UDataAsset
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

	/** Storage format. BGRA8 for density and detail; RGBA16F for anything with
	 *  a bipolar channel, which validation requires. See ENoiseOutputFormat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	ENoiseOutputFormat OutputFormat = ENoiseOutputFormat::BGRA8;

	/** Adds sub-LSB triangular dither before quantizing to 8 bits. Ignored for
	 *  RGBA16F, which has no uniform quantization step to dither against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bDither = true;

	/** Generate a mip chain. Safe for tiling volumes: a 2x2x2 box reduction
	 *  never reads outside the volume, so each mip stays exactly periodic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bGenerateMips = true;

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
	 *  This tests the evaluator rather than the output, which is the only way to
	 *  catch a wrap bug reliably; a visual 2x2 tiled preview will not show a
	 *  subtly wrong modulo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	bool bValidateTiling = true;

	/** Max permitted absolute difference in the tiling self-test. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bValidateTiling"))
	float TilingTolerance = 1e-4f;

	// -- Target -------------------------------------------------------------

	/** Existing texture to overwrite. If null, one is created at
	 *  TargetPackagePath / TargetAssetName and assigned here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	TSoftObjectPtr<UVolumeTexture> TargetTexture;

	/** Content path for a newly created texture, e.g. /VolumeNoiseLib. */
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

	/** Cross-channel vector facts from the most recent bake. Empty of meaning
	 *  for a packed recipe; see FNoiseVectorFieldStats. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake Record")
	FNoiseVectorFieldStats LastBakeVectorStats;

	// -- Actions ------------------------------------------------------------

	UFUNCTION(CallInEditor, Category = "Bake", meta = (DisplayName = "Bake Volume Texture"))
	void Bake();

	UFUNCTION(CallInEditor, Category = "Bake", meta = (DisplayName = "Validate Only"))
	void ValidateOnly();

	// -- Subclass hooks -----------------------------------------------------

	/** The effective four channels in RGBA order.
	 *
	 *  Every consumer downstream of authoring reads the field through this: the
	 *  probe, the normalization grouping, the shader parameter packing and the
	 *  recorded metadata. A subclass whose authored form is not four independent
	 *  channels synthesises the view here. */
	virtual void GetChannels(TArray<FNoiseChannelRecipe>& OutChannels) const
		PURE_VIRTUAL(UNoiseBakeRecipeBase::GetChannels, );

	/** Sets the evaluation mode and any mode-specific dispatch parameters.
	 *  Called after the shared per-channel packing, so it can override. */
	virtual void ConfigureEvaluation(FNoiseBakeDispatchParams& InOutParams) const {}

	/** True when the RAW output of this channel can be negative before any
	 *  shaping. Curl components can; every scalar basis cannot.
	 *
	 *  This is what selects symmetric normalization, and it is deliberately a
	 *  property of the source rather than of the output polarity. Normalizing a
	 *  signed source about the midpoint of its observed range would put the
	 *  field's zero wherever the probe happened to land; normalizing an unsigned
	 *  source symmetrically would strand the data in part of the range and waste
	 *  the rest. Only the source knows which case it is. */
	virtual bool IsChannelSourceSigned(int32 ChannelIndex) const { return false; }

	/** True when RGB is one vector rather than three independent scalars.
	 *
	 *  Asked rather than inferred from the normalization groups, because a
	 *  packed recipe is free to group three channels for reasons of its own
	 *  without those channels being a vector, and the difference decides
	 *  whether a magnitude statistic means anything. */
	virtual bool IsVectorField() const { return false; }

	/** True when A holds the scalar potential whose gradient is in RGB, from
	 *  the same evaluation. Lets the probe record the conversion between the
	 *  two normalizations; see FNoiseVectorFieldStats. */
	virtual bool IsAlphaGradientPotential() const { return false; }

	/** Runs every shared validation rule, then ValidateDerived.
	 *  Returns false and fills OutError on the first failure. */
	bool Validate(FString& OutError) const;

protected:
	/** Subclass-specific rules. The shared rules have already passed. */
	virtual bool ValidateDerived(FString& OutError) const { return true; }
};