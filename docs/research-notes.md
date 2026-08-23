# Research notes: fix stack-splitting with resized stacks

## The actual problem

Vanilla Satisfactory already supports stack splitting (right-click = split, right-click-hold = pick amount). The bug: with **Stack Resizer** (https://ficsit.app/mod/StackResizer) installed and stack sizes increased, **splitting fails on any stack whose count exceeds the vanilla stack size**. Reproduced in-game (2026-08). Bug reports already exist against Stack Resizer with no movement — so this project builds a **companion patch mod** and hopefully might help provide some help for a fix upstream.

## Stack Resizer facts (from SMR API, 2026-08-20)

- `mod_reference`: `StackResizer`, author **Acxd**, actively maintained
  (v1.4.3 for game 1.2, June 2026, `sml_version ^3.12.0`).
- **No `source_url`** — closed source. No direct PR possible.

## Bug hypothesis (to verify once toolchain is available)

**Key facts from the modding Discord (2026-08-20):**

- **The bug only occurs in multiplayer / dedicated-server games** — singleplayer
  splitting works fine with oversized stacks (per MadCat256). This points at
  **replication / netcode**, not a simple UI clamp.
- The split flow: a `CanSplitStack`-style check gates the action (it also decides
  whether the UI shows "open in codex" vs. split); `SplitStack` is called when it
  returns true.
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

## Where to look

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

## Confirmed from the starter project headers (2026-08-20, CL502094)

First look at the real signatures, which narrows the earlier guesses:

- `UFGInventoryComponent::CanSplitStackAtIdx( int32 idx )` → `bool`, and
  `UFGInventoryComponent::SplitStackAtIdx( int32 idx, int32 numItemsToMove )` → `void`
  both exist at `Source/FactoryGame/Public/FGInventoryComponent.h:514` and `:518`.
  The Discord names were right.
- **Both are plain `BlueprintPure`/`BlueprintCallable` UFUNCTIONs — neither is a `Server`
  RPC.** So the client→server hop is *not* in these two functions; something else
  (an RCO, or a higher-level inventory-move path) carries the split across the wire.
  Finding that caller is the next real question.
- Both are `public`, so hooking them needs no Access Transformer / friend declaration.
- `FInventoryStack` (`:95`) is `{ FInventoryItem Item; int32 NumItems; int32
  CachedMaxStackSize = INDEX_NONE; }`. **`CachedMaxStackSize` is a new suspect** — a
  per-stack cached copy of the max size is exactly the kind of thing that can be
  populated correctly on one side of the network and staled/defaulted on the other.
- `GetSlotSize( index, itemDesc )` (`:493`) returns the per-slot override if set,
  otherwise the descriptor's stack size — the two-source lookup the notes above suspected.

Per the Multiplayer docs, client→server calls in Satisfactory mods (and in the game
itself) go through **Remote Call Objects**, which are `Server, WithValidation, Reliable`
UFUNCTIONs. A `_Validate` implementation that rejects an out-of-range count would produce
exactly this symptom: fine in SP (no validation boundary), silently dropped in MP.
Worth checking the RCO that handles inventory interaction before anything else.

## CONFIRMED in-game: the gate fails client-side (2026-08-21, dedicated server)

First real diagnostic run. Client on `G:/Steam/steamapps/common/Satisfactory`, Linux
dedicated server on `\\192.168.68.75\satisfactory\gamefiles`.

```
server  CanSplitStackAtIdx [DedicatedServer]: slot=16 item=Desc_IronScrew_C   numItems=8   slotSize=2000 cachedMaxStackSize=-1  -> true
server  SplitStackAtIdx    [DedicatedServer]: slot=16 numItemsToMove=4  (after: numItems=4)
client  CanSplitStackAtIdx [Client]:          slot=16 item=Desc_IronScrew_C   numItems=8   slotSize=500  cachedMaxStackSize=-1  -> true
client  CanSplitStackAtIdx [Client]:          slot=6  item=Desc_CopperIngot_C numItems=839 slotSize=100  cachedMaxStackSize=-1  -> false
```

What this settles:

1. **The failing check runs on the client, and the server never hears about it.** Slot 6
   returns false client-side and produces *no* server-side log line at all. The RCO
   `_Validate` hypothesis is dead - nothing is being rejected server-side, the request is
   never sent. **The fix has to run client-side.**
2. **The split path itself is fine in multiplayer.** Slot 16 held 8 screws, which fits
   under both sides' idea of the slot size, and split cleanly client -> server -> back.
   So the bug is *only* the size comparison, not netcode plumbing.
3. **Client and server disagree about slot capacity.** 500 vs 2000 for screws; the client's
   numbers (500 screws, 100 ingots) are exactly vanilla. Stack Resizer's sizing is reaching
   the server and not the client. This was hypothesis 3 of the investigation plan, with the
   failing side being the *client*, not the server as originally guessed.
4. **`CachedMaxStackSize` is `INDEX_NONE` on both sides**, so the per-stack cache suspected
   above is not involved. Drop it as a suspect.

Ruled out as causes of the mismatch (verified from the two installs and their logs):

- Not a missing install: `StackResizer` **1.4.3** is present on both sides, as a
  **Game Feature plugin** at `FactoryGame/Mods/GameFeatures/StackResizer/`.
- Not a version mismatch: both `.uplugin` files are byte-identical.
- Not a failed activation: both logs show
  `Game feature 'StackResizer' transitioned successfully. Ending state: Active [Active, Active]`.
- Not a config divergence *file*: there is no `StackResizer.cfg` in either side's
  `FactoryGame/Configs/`, so whatever drives the sizes is not an SML config file.
  (Where the server's non-vanilla values come from is therefore still open - the save,
  or an in-game/GameFeature-data source.)

### Remaining question at the time, and the probe for it (both candidates turned out wrong)

`GetSlotSize()` has two sources: the per-slot override, else the descriptor's stack size.
Two candidates left, distinguished by comparing `slotSize` to the descriptor's own value:

- **Per-slot override.** `UFGInventoryComponent::mArbitrarySlotSizes` is
  `UPROPERTY( SaveGame, meta = ( FGReplicatedUsing = OnRep_ArbitrarySlotSizes ) )`
  (`FGInventoryComponent.h:656`) - i.e. *conditionally* replicated, gated on the
  `mDoRepArbitrarySlotSizes` flag (`:641`). Set server-side without that flag ever being
  raised, it would persist in the save and never reach clients. Fits the evidence exactly.
- **Global table / descriptor.** `UFGResourceSettings::mStackSizes`
  (`FGResourceSettings.h:84`, read via `GetStackSizeFromEnum`) is a plain `Config` CDO
  property, not replicated. If Stack Resizer patches it only under `HasAuthority()`, the
  client keeps vanilla values.

The hooks now also log `descStackSize` (`UFGItemDescriptor::GetStackSize`) per slot and
dump the whole `EStackSize` -> count table once per process (`StackSizeTable:` lines).
Read them as: `slotSize != descStackSize` -> per-slot override, unreplicated;
`slotSize == descStackSize` on both sides but the two sides differ -> global table patched
authority-only.

## Second run: the bug did NOT reproduce, and the mechanism is now known (2026-08-21, 03:37)

Same session, same two machines, client relaunched at 03:36:48. Every split attempt was
allowed, on both sides, and the two sides agreed on every number:

```
client  slot=32 Desc_Cement_C      numItems=758  slotSize=2000 descStackSize=2000  -> true
client  slot=6  Desc_CopperIngot_C numItems=839  slotSize=1500 descStackSize=1500  -> true
client  slot=19 Desc_Wire_C        numItems=2000 slotSize=2000 descStackSize=2000  -> true
client  slot=9  Desc_IronRod_C     numItems=1581 slotSize=1750 descStackSize=1750  -> true
server  (same slots, same sizes, all true; splits executed and halved correctly)
```

Note slot 19: a *completely full* 2000/2000 stack still returns true, so "stack is at
capacity" is not a blocker either.

**Both remaining hypotheses are dead:**

- `slotSize == descStackSize` on every line on both sides -> **no per-slot overrides are in
  play anywhere**. `mArbitrarySlotSizes` / `mDoRepArbitrarySlotSizes` is not involved. Drop it.
- The `StackSizeTable:` dump is **identical and vanilla on both sides**:
  `1 / 50 / 100 / 200 / 500 / 50000`. So Stack Resizer does **not** patch
  `UFGResourceSettings::mStackSizes`. Drop that too.

**What it does instead:** the per-item values (1500 ingots, 1750 rods, 2000 cement/wire)
cannot be derived from that tier table, so Stack Resizer must be writing each
`UFGItemDescriptor` CDO's cached stack size **individually, per item**.

That reframes the whole bug. A CDO write is **local, unreplicated, and not authority-gated** -
the client is supposed to do exactly the same work the server does. So the 03:13 failure was
never "the server has sizing the client can't see". It was **the client's own descriptor
patch missing or being undone**, while the identical code on the server survived. That makes
this an **intermittent ordering race on the client**, which also explains why the existing
upstream bug reports have gone nowhere: it does not reproduce on demand.

### Prime suspect

`UFGItemDescriptor::UpdateCachedStackSize()` (`FGItemDescriptor.h:367`) recomputes
`mCachedStackSize` from the vanilla `EStackSize` table. Anything that re-runs it on the client
*after* Stack Resizer's patch resets that item to its vanilla size - which is precisely the
state the client was in at 03:13 (500 screws, 100 ingots: exact vanilla values).

It is `protected` (access specifier at `:353`), so hooking it needs an Access Transformer.
`Config/AccessTransformers.ini` now friends `FStackResizerSplitFixModule` into
`UFGItemDescriptor`; the address is taken inside `RegisterHooks()`, which is a member of that
class, so the friend is sufficient.

## Third/fourth runs: it is a subsystem registration-order race (2026-08-21, 12:27 and 12:30)

Two **full process restarts** three minutes apart, identical build and mod set, opposite results:

- 12:26:57 launch (`FactoryGame-backup-2026.08.21-12.28.27.log`) - client **correct**:
  IronScrew 2000, CopperIngot 1500, Biofuel 1750, Cement 2000. All splits allowed.
- 12:30:02 launch (`FactoryGame.log`) - client **vanilla**: IronRod 200, Wire 500,
  CopperIngot 100, Coal 100. Splits denied. Server stayed correct throughout (HighSpeedWire
  2000, IronPlateReinforced 1500), so only the client side flips.

**How Stack Resizer applies its sizes:** `BP_StackResizerSubsystem_C`, a mod subsystem that
spawns with the world. Not a startup CDO patch - a per-session one.

**The differentiator.** Both runs register the same 23 SML subsystems, but *in a different
order*, and exactly one subsystem moved:

| subsystem | good run | bad run |
| --- | --- | --- |
| `MarcioCommonLibs.CommonInfoSubsystem` | index 4 | index 22 (last) |
| `BP_StackResizerSubsystem_C` | index 22 (last) | index 20 |
| `Subsystem_WeaponUpgrades_C` | index 21 (before SR) | index 21 (after SR) |

`CommonInfoSubsystem` is the mover; Stack Resizer's shift is the knock-on. **In the good run
nothing registers after Stack Resizer. In the bad run two things do.** That is the whole
difference between working and broken.

Working theory: a subsystem that initialises *after* `BP_StackResizerSubsystem_C` resets the
descriptors' cached stack sizes back to the vanilla `EStackSize` values, undoing the patch.
Candidates are `CommonInfoSubsystem` (MarcioCommonLibs, pulled in by EfficiencyCheckerMod)
and `Subsystem_WeaponUpgrades_C` - both run after Stack Resizer only in the bad run.

If that holds, this is a **mod-conflict race**, not a Stack Resizer bug in isolation, which
would explain why the upstream reports never went anywhere.

### The UpdateCachedStackSize watchdog is a dead end

It hooked cleanly - `LogNativeHookManager: Successfully hooked function
UFGItemDescriptor::UpdateCachedStackSize` - and then **never fired once**, across two full
launches including initial asset load. The out-of-line body exists and got hooked, but every
real call is inlined at its call sites in Shipping. Drop the hook.

Leave the `Friend=` line in `Config/AccessTransformers.ini` alone, though: access transformers
are injected into FactoryGame's own headers at build time, so changing that file rebuilds the
entire FactoryGame module (~329 actions). An unused friend declaration costs nothing at
runtime; removing it costs a full rebuild.

## Outcome: fixed and verified (2026-08-21, 12:53 and 13:14)

`AStackSizeSyncSubsystem` (`Public/StackSizeSyncSubsystem.h`,
`Private/StackSizeSyncSubsystem.cpp`) replicates the server's effective stack size for every
descriptor and applies it client-side. Both verification runs happened to lose the
registration-order race, which made them ideal tests:

```
received 1827 authoritative sizes, corrected 290 on this client
[client pass 1/15]: corrected 290 of 1827
re-corrected 290 descriptors that had drifted back
[client pass 2/15]: corrected 0 of 1827      (... through 15/15)
maintenance window closed after 15 passes
```

Three things this settles:

- **The fix works.** After the sync, zero `-> false` results in the whole session; IronRod
  1581/1750, IronPlate 968/1750, a full Wire 2000/2000 all split, server-side included.
- **The clobber lands ~2s *after* the first apply**, and hits exactly the 290 descriptors
  Stack Resizer had resized. A one-shot apply-on-join would have left the session broken -
  the re-check window is load-bearing, not defensive padding. It is a single event, though;
  passes 2-15 are always clean.
- **`GetStackSize()` reads the per-descriptor cached int**, so the reflection write is the
  right lever. If it were not, every pass would report 290 rather than only the first.

Server side is quiet: `authority gathered 1827`, then 15 passes of `0 entries new or changed`.
No descriptors loaded late in these runs, though the re-gather stays for runs where they do.

### Window sizing, measured (2026-08-21)

Per-pass counts across seven sessions:

| run | on arrival | pass 1 | passes 2+ |
| --- | --- | --- | --- |
| 13:44 | 290 | **290** | all 0 |
| 14:02 | 290 | 0 | all 0 |
| 14:10 | 290 | 0 | all 0 |
| 14:15 | 290 | 0 | all 0 (crashed at 12) |
| 14:17 | 290 | 0 | all 0 |
| current | 290 | **290** | all 0 |

The clobber is a **single event landing within ~2s of the sizes arriving** - twice caught by
pass 1, otherwise already finished before the sizes even landed (`corrected 290` on arrival,
then nothing). Across roughly 95 passes after the first, nothing has ever drifted again.

The original 15 passes x 2s was therefore about 5x longer than the evidence supports. Now
5 x 1s: still ~2.5x the observed worst case, repairs in half the time, and 5 log lines instead
of 15. Note this also shrinks - though does not by itself fix - the window in which the
lifetime bug below could bite.

### Crash: timer outliving the actor (found 2026-08-21 14:15, fixed)

Leaving a session while the maintenance window was still open crashed the game:

```
StackSizeSync[client pass 12/15]: corrected 0 of 1827
LogWindows: Error: [Callstack] AStackSizeSyncSubsystem::ScheduleMaintenance'::`2'::<lambda_1>
  ::operator()() [StackSizeSyncSubsystem.cpp:226]
```

Cause: `SetTimer` was called with a lambda capturing `this`. That overload stores a bare
`TFunction` with **no UObject lifetime tracking**, unlike the member-function overload, which
holds a weak reference and skips the call once the object is gone. Leaving the session
destroyed the subsystem actor with a pass still pending, and the timer then fired on a dangling
pointer.

Fixed by binding the timer to the object -
`SetTimer( Handle, this, &AStackSizeSyncSubsystem::RunMaintenancePass, ... )` - and clearing it
in `EndPlay` rather than trusting teardown ordering. Worth remembering as a general rule: a
lambda timer on an actor is only safe if the actor cannot die before it fires.

## Remaining work

1. **Identify the clobberer.** Suspects are `MarcioCommonLibs.CommonInfoSubsystem` (pulled in
   by EfficiencyCheckerMod) and `Subsystem_WeaponUpgrades_C`; both register after Stack
   Resizer only in the failing runs. Disable each in turn over several launches - one launch
   proves nothing when the outcome is a coin flip - and watch for the `re-corrected` line,
   which is now a clean pass/fail signal. Not needed to ship, since the fix does not depend on
   which mod is at fault, but worth reporting upstream once known.
2. Add the `StackResizer` dependency to the `.uplugin` (see the TODO list in the README).

### Why splitting is the only visible symptom

Worth being precise about, because an earlier version of these notes got it wrong. It claimed
a stale client descriptor would also break fill bars, drag limits and stack merging. It does
not - those were observed working normally throughout the broken sessions.

The reason is that **a wrong client-side capacity only produces a visible bug when it gates an
action that never round-trips to the server.** Everything else self-corrects:

- Moving, dropping and merging items send a request; the server performs the operation using
  its own (correct) sizes and replicates the result back.
- Slot displays read the replicated `FInventoryStack::NumItems`. Showing "839" needs no
  knowledge of the maximum, so a stale maximum cannot corrupt it.
- `CanSplitStackAtIdx` is different in kind: it is a `BlueprintPure` predicate the widget
  consults to decide whether to offer the split *at all*. When it returns false the request is
  never sent, so there is no server round-trip to correct it. It fails silently and locally.

Scanning `FGInventoryComponent.h`, it is the only `BlueprintPure` predicate whose answer
depends on stack capacity. `HasEnoughSpaceForStack(s)`/`HasEnoughSpaceForItem` (`:425-435`)
are also capacity-dependent and `BlueprintCallable`.

### Probe result: the client does consult capacity elsewhere (2026-08-21, 14:00)

Temporary log-only hooks on both `HasEnoughSpaceForStack(s)`, filtered to clients, answered
this. They are called - not never, as assumed:

```
CapacityProbe HasEnoughSpaceForStacks [Client]: item=Desc_Sulfur_C     numItems=1 descStackSize=1500 -> true
CapacityProbe HasEnoughSpaceForStack  [Client]: item=Desc_Sulfur_C     numItems=1 descStackSize=1500 -> true
CapacityProbe HasEnoughSpaceForStacks [Client]: item=Desc_OreBauxite_C numItems=1 descStackSize=1500 -> true
CapacityProbe HasEnoughSpaceForStacks [Client]: item=Desc_Leaves_C     numItems=1 descStackSize=2000 -> true
```

Six events, each firing both functions (`...Stacks` forwards to `...Stack`).

**What this proves.** The stack transfers the player was performing at the time - moving whole
stacks from a storage container into the player inventory - produced **zero** client-side
capacity calls. So that path really is server-side, exactly as argued, and the original
observation that dragging and merging behave normally is explained.

**What this does not prove.** Something else is issuing single-item capacity checks on the
client, and the probe does not say what. The first reading of this log guessed "world pickups"
from the item names - ore, berries, leaves, wood - but the player had picked nothing up, so
that was pattern-matching, not evidence. All that is actually known: `numItems=1`, six
different raw resources, spaced one to two seconds apart, all returning true.

**Second run with `owner=` (14:09), player filling inventory, splitting, and shift-clicking
stacks out of a storage container:**

```
CapacityProbe HasEnoughSpaceForStack [Client]: owner=Build_StorageIntegrated_C_2147471359
  (Build_StorageIntegrated_C) inventory=StorageInventory item=Desc_RawQuartz_C numItems=1
  descStackSize=1500 -> true
```

Four events, eight lines, and every one names **the same single storage actor's
`StorageInventory`** - not the player's inventory. So the checks are about a container, which
rules out the pickup theory and rules out the player inventory too.

They are also still `numItems=1`, never the stack sizes actually being moved. The shift-click
transfers themselves therefore do not go through this check on the client; the pattern - one
event per interaction, always a single item, on the container being interacted with - looks
more like a per-hover or per-interaction affordance test than the transfer itself. Not
identified conclusively, and worth resisting the urge to name it without better evidence.

Note every observation is post-sync, so `descStackSize` is always correct and the result is
always true. This shows the dependency exists; it does not show it misbehaving. Catching that
would need an observation inside the stale window before the sync lands, which would mean
deliberately delaying the sync behind a flag - not worth a build cycle for a path that has
returned true in every sample.

The probe has since been removed, having answered what it usefully could. To restore it: hook
`HasEnoughSpaceForStack` and `...Stacks` with `SUBSCRIBE_METHOD_AFTER`, filter to
`NM_Client`, and log the inventory's owner actor, the item, `NumItems` and `GetStackSize()`.

**Consequence for the design, unchanged.** Whatever the trigger is, a client-side consumer of
stack capacity exists outside the split gate. So a repair-on-demand hook on
`CanSplitStackAtIdx` would not cover it, and cannot replace the proactive passes. That
conclusion holds regardless of what the six events turn out to be.

### Considered and not done

Reacting to the clobber instead of polling for it: repair inside a `SUBSCRIBE_METHOD` (not
`_AFTER`) hook on `CanSplitStackAtIdx`, so the original recomputes on corrected data and
returns true without needing `scope.Override()`. Event-driven, no timer, and **unbounded in
time** - which is a genuine advantage, since the current 30s window would miss a clobber that
happened later in a session.

Not adopted, but the honest reason is weaker than the one first written here: the maintenance
passes repair *everything* proactively, which defends against client-side capacity checks we
cannot enumerate from header stubs, and their `re-corrected` line is the pass/fail signal for
the still-open task of identifying the clobberer. Given the current design is verified working,
that was judged not worth trading away. Adding the hook *alongside* the passes, as an unbounded
safety net, remains a reasonable future change.

Hooking the clobber directly is not possible: the obvious target,
`UFGItemDescriptor::UpdateCachedStackSize`, is inlined at its call sites in the shipped game
binary, and nothing done at this mod's build time changes that.
