# Satisfactory Split Stack Mod

**Status: planning / research.** No mod code yet — the Satisfactory modding toolchain
(custom Unreal Engine 5.6.1 build, Visual Studio 2022, Wwise) runs only on Windows or
Linux, so actual development is deferred until a suitable machine is available.

## Goal

Fix stack splitting when using [Stack Resizer](https://ficsit.app/mod/StackResizer):
with increased stack sizes, vanilla split (right-click / right-click-hold) fails on
stacks whose count exceeds the vanilla stack size. Reproduced in-game; existing bug
reports against Stack Resizer (closed source) have gone nowhere, so the plan is a
small companion patch mod that lifts the vanilla-stack-size clamp in the split flow.

## Repo layout

| Path | Contents |
|---|---|
| `docs/toolchain.md` | What's needed to set up the dev environment (Windows/Linux) |
| `docs/research-notes.md` | Game/SML API notes relevant to inventory + UI hooking |

## Key external links

- Beginner's guide: https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/index.html
- Starter project (becomes this repo's base when dev starts): https://github.com/satisfactorymodding/SatisfactoryModLoader
- Mod repository (SMR): https://ficsit.app
- Community Discord (main support channel): linked from https://docs.ficsit.app
