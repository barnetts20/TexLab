#include "NoiseBakeShader.h"

#include "DataDrivenShaderPlatformInfo.h"

bool FNoiseBakeCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FNoiseBakeCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), FNoiseBakeCS::ThreadGroupSize);
}

IMPLEMENT_GLOBAL_SHADER(FNoiseBakeCS, "/Plugin/NoiseBaker/Private/NoiseBakeCS.usf", "MainCS", SF_Compute);
