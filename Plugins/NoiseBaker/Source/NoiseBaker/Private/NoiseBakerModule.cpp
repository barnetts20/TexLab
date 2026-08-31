#include "NoiseBakerModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FNoiseBakerModule"

DEFINE_LOG_CATEGORY_STATIC(LogNoiseBaker, Log, All);

void FNoiseBakerModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NoiseBaker"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogNoiseBaker, Error,
			TEXT("NoiseBaker plugin not found by IPluginManager; shader directory was not mapped. ")
			TEXT("The plugin folder name must be 'NoiseBaker' to match the .uplugin."));
		return;
	}

	const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/NoiseBaker"), ShaderDir);

	UE_LOG(LogNoiseBaker, Log, TEXT("Mapped /Plugin/NoiseBaker -> %s"), *ShaderDir);
}

void FNoiseBakerModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNoiseBakerModule, NoiseBaker)
