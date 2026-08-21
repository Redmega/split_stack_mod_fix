using UnrealBuildTool;

public class StackResizerSplitFix : ModuleRules
{
	public StackResizerSplitFix(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// Trimmed from the starter template's catch-all list to what this module's include
		// chain actually reaches. If an include is ever added that needs more, UBT names the
		// missing module in the error - re-add it here rather than restoring the whole list.
		//
		//   Core / CoreUObject / Engine  reflection, AActor, UWorld, timers
		//   NetCore                      pulled in by Net/UnrealNetwork.h for DOREPLIFETIME
		//   GameplayTags                 FGItemDescriptor.h includes GameplayTagContainer.h
		//   DummyHeaders                 the starter project's header stubs
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine",
			"NetCore",
			"GameplayTags",
			"DummyHeaders",
			"FactoryGame", "SML"
		});
	}
}
