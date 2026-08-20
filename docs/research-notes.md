# Research notes: fix stack-splitting with resized stacks

## The actual problem

Vanilla Satisfactory already supports stack splitting (right-click = split, right-click-hold = pick amount). The bug: with **Stack Resizer** (https://ficsit.app/mod/StackResizer) installed and stack sizes increased, **splitting fails on any stack whose count exceeds the vanilla stack size**. Reproduced in-game (2026-08). Bug reports already exist against Stack Resizer with no movement — so this project builds a **companion patch mod**; upstream fix is not expected.

## Stack Resizer facts (from SMR API, 2026-08-20)

- `mod_reference`: `StackResizer`, author **Acxd**, actively maintained
  (v1.4.3 for game 1.2, June 2026, `sml_version ^3.12.0`).
- **No `source_url`** — closed source. No direct PR possible.
- v1.4.0 changelog: "All BP functions ported to C++"; machine-buffer resizing split
  out into a separate upcoming mod ("Machine Buffer Resizer").
- Related open-source reference the author adapted: LFOB (Large Fluid Output Buffers,
  https://ficsit.app/mod/LargeFluidOutputBuffers) by Joanna Hammond.

## Bug hypothesis (to verify once toolchain is available)

The symptom — splitting works up to the *vanilla* stack size but not beyond — strongly
suggests two different sources of truth for "max stack size":

- `UFGInventoryComponent` supports **arbitrary/override slot sizes** (see
  `FGInventoryComponent.h`); if Stack Resizer raises capacity via slot-size overrides
  or a runtime patch rather than editing every `UFGItemDescriptor`'s stack size, then
  any code that reads `UFGItemDescriptor::GetStackSize()` (likely the split
  quantity-picker widget and/or the server-side split validation) still sees the
  vanilla value and clamps/rejects amounts above it.
- Alternative: the descriptor *is* updated, but the split path caches the stack size
  (CDO read at startup) or validates against `FItemAmount`/enum-based stack size
  (`EStackSize`) rather than the effective slot capacity.

Either way, the fix shape is the same: hook the split flow and replace the "max"
lookup with the *effective* slot capacity (slot's actual item count / inventory
component's slot size), not the descriptor's nominal stack size.

## Where to look (investigation plan, needs Windows/Linux + toolchain)

1. **Characterize the failure mode** (already reproduced; pin down which layer):
   - Right-click-hold amount picker: is its max capped at the vanilla stack size, or
     can you enter a larger amount that then no-ops?
   - Plain right-click (split in half) on an oversized stack: does it do nothing, or
     split into vanilla-sized halves?
   - UI capped → clamp is in the quantity-picker widget (Blueprint hook fix).
     Accepted-but-no-op → clamp/reject is in the inventory/server code (C++ hook fix).
2. **Find the vanilla split implementation**:
   - Extract game UI assets (docs: "Extracting Game Files") — locate the inventory
     slot widget and the split/quantity-picker widget, follow what function performs
     the split and what it clamps against.
   - Check `FGInventoryComponent.h` / `FGInventoryLibrary.h` headers in the starter
     project for split/take-amount functions and their validation.
3. **Inspect Stack Resizer itself**: download its `.pak`/plugin from SMR and open with
   FModel/UAssetGUI to see how it applies sizes (descriptor edit vs slot override).
   This confirms which hypothesis is right and whether the bug is theirs or vanilla's.
4. **Upstream**: existing bug reports have had no movement — don't block on the
   author. Once diagnosed, optionally post the root cause on the mod's SMR chat /
   Discord thread as a courtesy, but ship the companion mod regardless.

## Fix-mod approach (primary plan)

- Small C++ SML mod with **`StackResizer` as a required dependency** (declared in the
  `.uplugin`) — the mod only exists to patch Stack Resizer's interaction with the
  split flow, and depending on it guarantees load order and surfaces the relationship
  in the mod manager.
- Use SML **hooking** (https://docs.ficsit.app/satisfactory-modding/latest/Development/Cpp/hooking.html)
  to hook the split/take function (likely in `UFGInventoryComponent` or the widget's
  handler) and lift the clamp to the effective slot capacity.
- If the clamp lives in a Blueprint widget instead: **Blueprint hooks** /
  **Widget Blueprint Hooks** (https://docs.ficsit.app/satisfactory-modding/latest/Development/ModLoader/WidgetBlueprintHooks.html).
- Multiplayer: if validation is server-side, the hook must apply on the server too —
  test in MP per the docs' multiplayer-testing page.

## Relevant game classes (inventory system)

Source: https://docs.ficsit.app/satisfactory-modding/latest/Development/Satisfactory/Inventory.html
Deeper reference: comments in `FGInventoryComponent.h` and `FGInventoryLibrary.h`.

- **`UFGItemDescriptor`** — item properties incl. nominal stack size.
- **`FInventoryItem`** — one item instance + item state.
- **`FItemAmount`** — descriptor + count, *no state* (recipes/costs).
- **`FInventoryStack`** — stack of items in an inventory; keeps state.
  ⚠ `FItemAmount` ↔ `FInventoryStack` conversion can lose item state.
- **`UFGInventoryComponent`** — container of `FInventoryStack`s; supports per-slot
  size overrides — the likely mismatch behind the bug.

## Next steps

1. (No toolchain needed) Record the exact failure mode per step 1 of the
   investigation plan — it determines whether the fix hooks a widget or C++ code.
2. Set up the toolchain per `docs/toolchain.md`.
3. Diagnose per the plan, then build the companion mod.
