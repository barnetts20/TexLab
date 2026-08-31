using UnrealBuildTool;

public class NoiseBaker : ModuleRules
{
	public NoiseBaker(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"RHI",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",	// IPluginManager, for the shader source directory mapping
			"Renderer",
		});
	}
}
