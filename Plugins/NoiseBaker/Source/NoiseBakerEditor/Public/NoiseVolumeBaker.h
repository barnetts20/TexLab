#pragma once

#include "CoreMinimal.h"
#include "NoiseBakeTypes.h"

class UNoiseBakeRecipeBase;
class UVolumeTexture;

/** What the shader evaluates per voxel. Mirrors NVB_EVAL_* in NoiseBakeCS.usf. */
enum class ENoiseEvalMode : int32
{
	/** Four independent scalar channels. */
	Packed = 0,

	/** RGB is curl P, where the potential's three components are channel
	 *  recipes 0, 1 and 2 at decorrelating seeds. */
	Curl = 1,

	/** RGB is grad f, where f is channel recipe 0. */
	Gradient = 2,
};

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
	ENoiseEvalMode EvalMode = ENoiseEvalMode::Packed;

	/** Central-difference step for curl, in UVW units. Ignored when packed. */
	float CurlEpsilon = 0.0f;

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
	static bool Bake(UNoiseBakeRecipeBase* Recipe, FString& OutError);

	/** Packs a recipe's channels into GPU parameters. Normalization is left at
	 *  identity; FillNormalization applies the probe result afterwards. */
	static void BuildDispatchParams(const UNoiseBakeRecipeBase& Recipe, FNoiseBakeDispatchParams& OutParams);

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
		const UNoiseBakeRecipeBase& Recipe,
		FNoiseBakeDispatchParams& InOutParams,
		TArray<FNoiseChannelNormalization>& OutNormalization,
		FNoiseVectorFieldStats& OutVectorStats,
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

	static UVolumeTexture* ResolveOrCreateTexture(UNoiseBakeRecipeBase& Recipe, FString& OutError);

	static bool WriteTexture(
		UNoiseBakeRecipeBase& Recipe,
		UVolumeTexture& Texture,
		const TArray<uint8>& Texels,
		const TArray<FNoiseChannelNormalization>& Normalization,
		const FIntVector& BrickDimensions,
		const TArray<float>& BrickMinMax,
		FString& OutError);
};