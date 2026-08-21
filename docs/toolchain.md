# Toolchain requirements (as of 2026-08, docs.ficsit.app "latest")

Source: https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/dependencies.html

## Platform

- **Windows** is the primary supported platform. **Linux** has a separate guide:
  https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/LinuxSetup.html
- **macOS is not supported.** The custom engine editor is distributed as Win64
  installers only, and the game itself is Windows-only. Editing source/docs on the
  Mac and building on a Windows/Linux box via this git repo is the workable split.

## Required software (Windows path)

1. **Satisfactory** itself (Steam or Epic), launched at least once.
2. **Visual Studio 2022** (specifically 2022, *not* 2026) with:
   - Workloads: .NET Desktop Development, Desktop development with C++, Game development with C++
   - Components: MSVC v143 x64/x86 build tools (v14.38-17.8), .NET 8.0 Runtime (LTS), .NET Framework 4.8.1 SDK
   - The docs provide an importable `.vsconfig` to select these automatically.
   - Rider is a popular alternative IDE but VS + MSVC packages must still be installed.
3. **Custom Unreal Engine 5.6.1-CSS** (Coffee Stain's modified engine + community changes):
   - Requires a GitHub account linked to Epic Games (for base UE repo access), then
     linked to the modding org via https://linker.ficsit.app/link
   - Download installer (`UnrealEngine-CSS-Editor-Win64.exe` + 3 `.bin` files) from
     https://github.com/satisfactorymodding/UnrealEngine/releases/latest
   - Engine version must match the game version being modded.
4. **Clang cross-compile toolchain** (v25, clang-18.1.0-based) — only needed to build
   for Linux dedicated servers.
5. **Wwise** audio SDK — integrated into the starter project (see Project Setup page).
6. **Starter project**: clone https://github.com/satisfactorymodding/SatisfactoryModLoader
   (Option A, recommended) — mods are developed as plugins inside it.

## Sizing

- 30+ GB disk for the environment; budget ~100 GB to be safe.
- A real GPU is needed for the Unreal editor (rules out most non-GPU cloud VMs).

## Setup helper

Community tool **SMEH** (SirDigby) automates much of the setup on standard systems —
linked from the dependencies page.

## Current state (2026-08-20, Windows machine)

Done:

- Starter project cloned at the repo root two levels up (`SatisfactoryModLoader/`,
  branch `master`, headers at CL502094).
- This repo moved to `SatisfactoryModLoader/Mods/StackResizerSplitFix/` and is now the
  mod's Unreal plugin folder — see the README for the layout and rationale.
- Plugin scaffold created: `.uplugin`, `Config/`, `Resources/`, and the C++ module
  `Source/StackResizerSplitFix/`.

Still required on this machine before the mod can be built:

1. Visual Studio 2022 with the workloads/components listed above.
2. Custom Unreal Engine 5.6.1-CSS installed — `FactoryGame.uproject` has
   `"EngineAssociation": "5.6.1-CSS"`.
3. Wwise 2023.1.14.8770 integrated into the starter project, and soundbanks generated.
4. Generate Visual Studio project files, then build `Development Editor | Win64 | FactoryGame`.
5. Clang cross-compile toolchain (v25, clang-18.1.0) — **required, not optional** here,
   because `Config/Alpakit.ini` builds the `LinuxServer` target and the bug only
   reproduces in multiplayer / on dedicated servers.

References:

- https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/project_setup.html
- https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/StarterProjectStructure.html
- https://docs.ficsit.app/satisfactory-modding/latest/Development/Cpp/setup.html
