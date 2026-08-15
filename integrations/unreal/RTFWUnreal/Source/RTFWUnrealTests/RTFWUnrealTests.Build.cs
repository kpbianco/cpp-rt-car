using UnrealBuildTool;

public class RTFWUnrealTests : ModuleRules
{
    public RTFWUnrealTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        bEnableExceptions = true;
        bUseRTTI = true;
        PrivateDependencyModuleNames.AddRange(
            new string[] { "Core", "RTFWUnreal" });
    }
}
