# Stack Resizer Split Fix

Mod reference: **`StackResizerSplitFix`**

A companion patch mod for [Stack Resizer](https://ficsit.app/mod/StackResizer).
With increased stack sizes, splitting a stack (right-click / right-click-hold) fails on
stacks whose count exceeds the vanilla stack size — but **only in multiplayer and on
dedicated servers**; singleplayer seemed to work fine. Stack Resizer is closed source and existing
bug reports have been hard to track down, so this mod patches the interaction from the outside.

The root cause is not in the split flow at all. Stack Resizer applies its sizes by writing
each item descriptor's cached stack size — a purely local, unreplicated write that each side
has to perform for itself. On the client that write is fragile: SML subsystem registration
order varies between launches, and when another mod's subsystem initialises after Stack
Resizer's, the client's descriptors get reset to vanilla sizes while the server keeps the
resized ones. The client then refuses to split anything larger than the vanilla size, because
`CanSplitStackAtIdx` compares against the descriptor it can see.

Rather than compete for a position in that ordering, this mod treats the server as the
authority and repairs the client: `AStackSizeSyncSubsystem` sends the server's effective
stack size for every descriptor on join (in batches, through a per-player remote call object),
applies it client-side, and re-checks for a short
window afterwards — the clobber has been observed landing *two seconds after* the first
apply, so a single pass is not enough. See [`docs/research-notes.md`](docs/research-notes.md)
for the full investigation and the log evidence.

## TODO

- [ ] Identify which mod actually clobbers the descriptors. Strongly suspected to be
      `MarcioCommonLibs.CommonInfoSubsystem` or `Subsystem_WeaponUpgrades_C` — both register
      after Stack Resizer only in the failing runs. Disabling each in turn over several
      launches should settle it; the `re-corrected` log line is a clean pass/fail signal.

`RequiredOnRemote` stays at its default (`true`): the mod is needed on both ends. The client
does the repair, but only the server can supply the values to repair against — it spawns the
subsystem (`SpawnOnServer_Replicate`) and gathers the snapshot.
