
## Repo layout

| Path | Contents |
|---|---|
| `StackResizerSplitFix.uplugin` | Plugin descriptor: version, SMR metadata, SML dependency |
| `Config/Alpakit.ini` | Package targets — Windows **and both server targets** |
| `Config/AccessTransformers.ini` | Access widening for game classes (empty on purpose — see the file) |
| `Config/PluginSettings.ini` | Stages `Resources/` into the packaged mod |
| `Resources/Icon128.png` | Mod icon (placeholder — replace before release) |
| `Source/StackResizerSplitFix/` | The C++ module |
| `docs/toolchain.md` | Dev environment requirements |
| `docs/research-notes.md` | Bug hypothesis, investigation plan, relevant game classes |

The C++ module is a plain SML mod module — no Game World Module and no `Content/`, because
this mod adds no content; it only patches behavior. If it later needs assets (a mod config,
or [Widget Blueprint Hooks](https://docs.ficsit.app/satisfactory-modding/latest/Development/ModLoader/WidgetBlueprintHooks.html)),
add a `Content/` folder and a root module — no restructuring required.

## Building and testing

1. Open `FactoryGame.uproject` in the Unreal Editor (Unreal Engine 5.6.1-CSS).
   After any change to C++ class structure, close the editor and rebuild
   `Development Editor | Win64 | FactoryGame` from Visual Studio first.
2. In the editor, open **Alpakit Dev** and find *Stack Resizer Split Fix (StackResizerSplitFix)*.
3. Press **Alpakit!** — it builds Shipping, packages, and copies into the game install.

Because the bug is multiplayer-only, testing needs a real client/server split, not just
Play-In-Editor. `Config/Alpakit.ini` already enables the `WindowsServer` and `LinuxServer`
targets; `LinuxServer` needs the clang cross-compile toolchain installed.

Output goes to `FactoryGame.log` under the `LogStackResizerSplitFix` category. The lines that
matter on a client:

```
StackSizeSync: received 1827 authoritative sizes, corrected 290 on this client
StackSizeSync: re-corrected 290 descriptors that had drifted back
StackSizeSync: maintenance window closed after 5 passes
```

A non-zero `corrected` means this launch lost the registration-order race and the mod fixed
it. A `re-corrected` line means something reset the descriptors *after* the first apply —
that is the mod conflict caught in the act, and it names how many items were affected.

## Key external links

- Beginner's guide: https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/index.html
- Multiplayer / replication: https://docs.ficsit.app/satisfactory-modding/latest/Development/Satisfactory/Multiplayer.html
- Inventories and items: https://docs.ficsit.app/satisfactory-modding/latest/Development/Satisfactory/Inventory.html
- Starter project: https://github.com/satisfactorymodding/SatisfactoryModLoader
- Mod repository (SMR): https://ficsit.app
- Community Discord (main support channel): linked from https://docs.ficsit.app
