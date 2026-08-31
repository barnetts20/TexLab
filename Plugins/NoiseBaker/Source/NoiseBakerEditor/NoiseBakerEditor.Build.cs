using UnrealBuildTool;

public class NoiseBakerEditor : ModuleRules
{
	public NoiseBakerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"NoiseBaker",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"RenderCore",
			"RHI",
			"Renderer",
			"AssetRegistry",
			"Slate",
			"SlateCore",
		});
	}
}
