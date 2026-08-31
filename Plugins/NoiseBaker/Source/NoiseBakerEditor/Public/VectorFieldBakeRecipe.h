#pragma once

#include "CoreMinimal.h"
#include "NoiseBakeRecipeBase.h"
#include "VectorFieldBakeRecipe.generated.h"

/** A vector field baked into RGB as a derivative of a scalar potential, with A
 *  on its own rule.
 *
 *  Separate from UNoiseBakeRecipe because the parameter surface genuinely
 *  differs: this has ONE basis, period, octave count and seed driving a single
 *  potential, not four independent ones. Expressing that through the packed
 *  recipe would mean three channel structs that must be kept identical plus a
 *  comment telling you to set the normalization group ids correctly, which is a
 *  data model lying about its own shape.
 *
 *  Curl and Gradient share this asset for the same reason, applied the other
 *  way: their parameter surfaces are IDENTICAL. One authored scalar recipe, an
 *  epsilon, an alpha rule. The difference is which derivative combination gets
 *  taken, which is one enum value rather than a different set of knobs. They are
 *  also exact complements -- the two pure cases of the Helmholtz decomposition,
 *  which says any vector field splits into a curl-free part and a
 *  divergence-free part -- so having them adjacent is the honest arrangement.
 *
 *  Everything downstream of evaluation is shared with the packed path: slab
 *  dispatch, both probes, quantization, brick bounds, the tiling self-test and
 *  the bake stamp all live on UNoiseBakeRecipeBase.
 *
 *  Tiling survives differentiation, since a finite difference of a periodic
 *  function is periodic with the same period. Mips survive it too: divergence
 *  and curl are linear operators and so is a box filter, so a downsampled
 *  divergence-free field stays approximately divergence-free. */
UCLASS(BlueprintType)
class NOISEBAKEREDITOR_API UVectorFieldBakeRecipe : public UNoiseBakeRecipeBase
{
	GENERATED_BODY()

public:
	UVectorFieldBakeRecipe();

	/** Which derivative to take. See ENoiseVectorFieldMode; the two are not
	 *  interchangeable, and picking the wrong one is not a matter of taste. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vector Field")
	ENoiseVectorFieldMode Mode = ENoiseVectorFieldMode::Curl;

	/** The scalar potential.
	 *
	 *  In Gradient mode this is f, evaluated once. In Curl mode it is evaluated
	 *  three times at decorrelating seed offsets to form P = (Px, Py, Pz), and
	 *  the output is curl P.
	 *
	 *  Note the Gain default is lower than the packed recipe's 0.5, and that is
	 *  not arbitrary. Differentiation amplifies high frequencies: each octave's
	 *  contribution to the derivative scales with its frequency, so where the
	 *  potential's octave amplitudes fall as Gain^i, the derivative's fall as
	 *  (Gain * Lacunarity)^i. At Gain 0.5 and Lacunarity 2 that is FLAT, the
	 *  fine octaves dominate, and the result looks like static. Derivatives want
	 *  roughly 0.25 to 0.35, or fewer octaves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vector Field", meta = (DisplayName = "Potential"))
	FNoiseChannelRecipe Potential;

	/** Central-difference step, in voxels.
	 *
	 *  A quarter voxel is the useful default. Smaller and the difference of two
	 *  nearly equal FBM values loses precision to float cancellation; larger and
	 *  it smooths away the finest octave you paid to bake. Expressed in voxels
	 *  rather than UVW so it tracks resolution automatically. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vector Field", meta = (ClampMin = "0.05", ClampMax = "2.0"))
	float EpsilonVoxels = 0.25f;

	// -- Alpha --------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alpha")
	ENoiseVectorAlphaMode AlphaMode = ENoiseVectorAlphaMode::VectorMagnitude;

	/** Used when AlphaMode is Independent. Ignored otherwise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alpha", meta = (DisplayName = "A", EditCondition = "AlphaMode == ENoiseVectorAlphaMode::Independent", EditConditionHides))
	FNoiseChannelRecipe AlphaChannel;

	// -- Base hooks ---------------------------------------------------------

	virtual void GetChannels(TArray<FNoiseChannelRecipe>& OutChannels) const override;
	virtual void ConfigureEvaluation(FNoiseBakeDispatchParams& InOutParams) const override;
	virtual bool IsChannelSourceSigned(int32 ChannelIndex) const override;

protected:
	virtual bool ValidateDerived(FString& OutError) const override;
};
