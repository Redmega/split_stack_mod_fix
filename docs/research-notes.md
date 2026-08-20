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

**Key facts from the modding Discord (2026-08-20):**

- **The bug only occurs in multiplayer / dedicated-server games** — singleplayer
  splitting works fine with oversized stacks (per MadCat256). This points at
  **replication / netcode**, not a simple UI clamp.
- The split flow: a `CanSplitStack`-style check gates the action (it also decides
  whether the UI shows "open in codex" vs. split); `SplitStack` is called when it
  returns true (per word).
- SirDigby (has seen the actual game source): "it isn't that simple" — i.e. not just
  an inlined stack-size enum comparison. Don't assume the naive clamp theory.

**Working hypothesis:** the client→server path for the split (RPC arguments, or the
inventory-state serialization it round-trips through) validates or quantizes the item
count against the descriptor's *vanilla* stack size — e.g. server-side validation in
the split RPC, or `NetSerialize` of `FInventoryStack`/`FInventoryItem` clamping counts
to nominal stack size. In SP there's no serialization/validation boundary, which is
why it works there.

Secondary suspects (weaker, given the MP-only symptom): stack-size lookups via
`UFGItemDescriptor::GetStackSize()` vs. `UFGInventoryComponent` per-slot overrides
disagreeing only on the server side (does Stack Resizer's resize logic even run on
dedicated servers?).

## Where to look (investigation plan, needs Windows/Linux + toolchain)

1. **Characterize the failure mode across net modes** (bug is MP/dedi-only):
   - Confirm: SP works, listen-server host works?, remote client fails, dedi client
     fails. If the *host* of a listen server also fails, the clamp is in shared
     server code; if only remote clients fail, it's in the RPC/replication boundary.
   - On failure: does the amount picker cap at vanilla max, or accept the amount and
     no-op? Does plain right-click (split-half) also fail?
2. **Find the vanilla split implementation** (starter project headers + extracted
   assets):
   - Locate the `CanSplitStack` / `SplitStack` flow (names per Discord) — likely in
     `FGInventoryComponent.h`, `FGInventoryLibrary.h`, or the slot widget code — and
     trace the client→server RPC it goes through in MP.
   - Inspect `NetSerialize`/replication of `FInventoryStack` / `FInventoryItem` and
     any server-side validation of the split RPC for stack-size-based clamps.
   - Extract game UI assets (docs: "Extracting Game Files") for the slot and
     quantity-picker widgets to see what the client sends.
3. **Inspect Stack Resizer itself**: download its `.pak`/plugin from SMR and open with
   FModel/UAssetGUI to see how it applies sizes (descriptor edit vs slot override),
   and whether its resize logic runs on dedicated servers at all.
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
- **The fix almost certainly must run server-side** (bug is MP/dedi-only): build and
  test the mod for the dedicated-server target too — this makes the Linux
  cross-compile toolchain (see `docs/toolchain.md`) required, not optional. Test in
  MP per the docs' multiplayer-testing page, including a real dedicated server.

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
