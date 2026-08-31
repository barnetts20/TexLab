#pragma once

#include "CoreMinimal.h"
#include "NoiseBakeTypes.h"

class UNoiseBakeRecipe;
class UVolumeTexture;

/** Stages of the shaping chain to run. Mirrors NVB_STAGE_* in NoiseBakeCS.usf. */
enum class ENoiseShapeStage : int32
{
	Raw = 0,
	Normalized = 1,
	Full = 2,
};

/** Flat parameter block handed to the render thread. Deliberately plain so it
 *  can be captured by value into a render command without touching UObjects
 *  from a non-game thread. */
struct FNoiseBakeDispatchParams
{
	int32 Resolution = 0;
	int32 Supersample = 1;
	ENoiseShapeStage ShapeStage = ENoiseShapeStage::Full;
	FVector3f DomainOffset = FVector3f::ZeroVector;

	FVector4f ChannelParamsA[4];
	FIntVector4 ChannelParamsB[4];
	FIntVector4 ChannelParamsC[4];
	FVector4f ChannelNorm[4];
	FVector4f ChannelShaping[4];
	FVector4f ChannelEqLut[64];
};

/** Drives a full bake: validate, probe, dispatch, read back, quantize, build
 *  bricks, write the texture asset. Everything is synchronous and blocking by
 *  design; this is an offline tool and a predictable single-threaded control
 *  flow is worth far more here than overlap. */
class NOISEBAKEREDITOR_API FNoiseVolumeBaker
{
public:
	/** Runs the whole pipeline. Returns false with a reason on failure.
	 *  Must be called from the game thread. */
	static bool Bake(UNoiseBakeRecipe* Recipe, FString& OutError);

	/** Packs a recipe's channels into GPU parameters. Normalization is left at
	 *  identity; FillNormalization applies the probe result afterwards. */
	static void BuildDispatchParams(const UNoiseBakeRecipe& Recipe, FNoiseBakeDispatchParams& OutParams);

	/** Dispatches one Z slab and blocks until the readback completes.
	 *  OutSlab is sized to Resolution * Resolution * SliceCount. */
	static bool DispatchSlab(
		const FNoiseBakeDispatchParams& Params,
		int32 SliceOffset,
		int32 SliceCount,
		TArray<FVector4f>& OutSlab,
		FString& OutError);

private:
	static bool RunProbePass(
		const UNoiseBakeRecipe& Recipe,
		FNoiseBakeDispatchParams& InOutParams,
		TArray<FNoiseChannelNormalization>& OutNormalization,
		FString& OutError);

	static bool RunTilingSelfTest(
		const FNoiseBakeDispatchParams& Params,
		int32 ProbeResolution,
		float Tolerance,
		FString& OutError);

	static void QuantizeSlab(
		const TArray<FVector4f>& Slab,
		int32 Resolution,
		int32 SliceOffset,
		int32 SliceCount,
		ENoiseOutputFormat Format,
		bool bDither,
		TArray<uint8>& OutTexels);

	static void BuildBrickBounds(
		const TArray<uint8>& Texels,
		int32 Resolution,
		int32 BrickSize,
		ENoiseOutputFormat Format,
		const TArray<FNoiseChannelNormalization>& Normalization,
		FIntVector& OutDimensions,
		TArray<float>& OutBrickMinMax);

	static UVolumeTexture* ResolveOrCreateTexture(UNoiseBakeRecipe& Recipe, FString& OutError);

	static bool WriteTexture(
		UNoiseBakeRecipe& Recipe,
		UVolumeTexture& Texture,
		const TArray<uint8>& Texels,
		const TArray<FNoiseChannelNormalization>& Normalization,
		const FIntVector& BrickDimensions,
		const TArray<float>& BrickMinMax,
		FString& OutError);
};