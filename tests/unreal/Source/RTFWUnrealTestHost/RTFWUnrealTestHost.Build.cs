using UnrealBuildTool;

public class RTFWUnrealTestHost : ModuleRules
{
    public RTFWUnrealTestHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.Add("Core");
    }
}
