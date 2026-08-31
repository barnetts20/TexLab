#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/** Volume noise bake compute shader.
 *
 *  Parameter names and packing must stay in sync with
 *  Shaders/Private/NoiseBakeCS.usf. The four per-channel arrays are packed
 *  rather than expressed as a nested struct array because HLSL constant buffer
 *  packing of nested structs differs enough between RHIs to be a liability, and
 *  four float4/int4 arrays index identically everywhere. */
class NOISEBAKER_API FNoiseBakeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNoiseBakeCS);
	SHADER_USE_PARAMETER_STRUCT(FNoiseBakeCS, FGlobalShader);

	/** Cube thread group. 4^3 = 64 threads, which is a full wave on AMD and two
	 *  on NVIDIA, and keeps the 3D group count sane for small volumes. */
	static constexpr int32 ThreadGroupSize = 4;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, VolumeResolution)
		SHADER_PARAMETER(int32, SliceOffset)
		SHADER_PARAMETER(int32, SliceCount)
		SHADER_PARAMETER(int32, Supersample)
		SHADER_PARAMETER(int32, bApplyNormalize)
		SHADER_PARAMETER(FVector3f, DomainOffset)

		/** x: Gain, y: OutputMin, z: OutputMax, w: WorleyJitter */
		SHADER_PARAMETER_ARRAY(FVector4f, ChannelParamsA, [4])

		/** x: Basis, y: BasePeriod, z: Octaves, w: Lacunarity */
		SHADER_PARAMETER_ARRAY(FIntVector4, ChannelParamsB, [4])

		/** x: Seed, y: Flags (PN_FLAG_*), z/w: unused */
		SHADER_PARAMETER_ARRAY(FIntVector4, ChannelParamsC, [4])

		/** x: NormalizeScale, y: NormalizeBias, z/w: unused */
		SHADER_PARAMETER_ARRAY(FVector4f, ChannelNorm, [4])

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutVolume)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};
