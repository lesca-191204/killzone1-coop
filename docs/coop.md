# Online co-op campaign: how it works

Technical notes for the co-op layer added on top of KillzoneRecomp. Addresses are for the **Spanish disc
(SCES-52004)** unless noted. Status: experimental prototype.

## 1. Model: deterministic lockstep

Killzone has no co-op campaign and its online mode (`NETWORK/ONLINE.REL`) is not recompiled, so co-op does not
replicate game objects. Instead **every PC runs the same full game** and only inputs travel:

- `src/kz_netplay.cpp`: UDP session (default port 47474). HELLO/WELCOME handshake with a protocol version (bumped
  whenever the simulation changes), host-chosen frame rate, input delay (3 frames), per-frame pad + mouse state of
  both players, a digest of the game state every 600 frames (desync detector), disconnect handling (the other
  player keeps playing alone). The host's memory card is sent to the guest at connection time, so both start
  from the same save.
- `src/kz_lockstep.cpp`: fixed game step. Each game frame waits for both players' input, then advances the EE a
  fixed number of cycles (`294912000 * step / rate` per frame), so the result never depends on host speed.

For this to work the emulated PS2 must be **bit-for-bit deterministic**:

- `PS2X_DETERMINISTIC=1`, `PS2X_WALLCLOCK=0`, `PS2X_NO_PACING=1`: no host time anywhere in the simulation.
- IOP on an ideal clock (exactly 1.000x; the sync slices used to overshoot and never repay it).
- Scheduler idle jumps compare vblank and timer deadlines by cycles while menus or blocking movies run.
- Video thread (`PS2X_VIF1_THREAD=1`) in *instant completion* mode: the guest sees every VIF1/GIF DMA finish at
  once (D_STAT/CHCR/DMAC cause applied like the synchronous path), VIF1 register writes are queued on the worker
  and GS CSR reads wait for it. The simulation is identical to the synchronous one while drawing runs in
  parallel (~2x faster with two players). Back-pressure: at most 2 vblanks of drawing in flight.
- Settings that change the simulation (FOV, draw distance, LOD, post effects, frame rate) are forced equal.

Runtime changes live in the fork [lesca-191204/PS2Recomp](https://github.com/lesca-191204/PS2Recomp/tree/killzone-coop),
branch `killzone-coop`.

## 2. Player 2: the game's own way ("native P2")

The game's *Battlefields* mode supports two local human players. The level loader (`FUN_001becc0`) runs, in
order: mission + game-mode scripts, `OnIntro` (level movie), `OnLoad` (loads the 1st/3rd person models of every
local human slot), a per-slot pass (control type `'Human'` → player setup; more than one human sets split mode),
the mission's `OnInitialize` (the level's `AddPlayer(spawn)` for slot 0) and the level-start checkpoint.

`src/kz_coop.cpp`:

- Before the level loads, slot 1 is configured like the Battlefields menu does: 2 players, joystick, character,
  creation function and **control type `'Human'`** (slot +0x70).
- The engine calls script functions by name through `FUN_002e3890`. Right after the mission's `OnInitialize`
  returns, the same Lua state runs `AddPlayer(spawn, 1)`: player 2 is born with player 1, before the level-start
  checkpoint, so checkpoints, deaths and level changes are handled by the game itself.
- Death: player 2 respawns after 5 s next to player 1 (Battlefields' respawn path).
- The game's precompiled Lua 4.0 scripts (with Guerrilla's extra opcode 13, `DECLGLOBAL`) were dumped at runtime
  and decompiled to study all of this.

## 3. Fixes found through co-op

- **Level cutscenes** never played (solo too): the runtime's HLE `sceMpegCreate` did not zero the work area; the
  game reads libmpeg's end flag directly from it (`0x3CE3A8`), found garbage and stopped the movie at once.
- **Sound after level changes:** the sound driver's level-unload RPC was cut off (upstream fix, v0.1.7).
- **Level unload crash (v0.3.1 guard):** after a full level, unloading destroyed a `PathResource` (vtable
  `0x52CCE8`, destructor `FUN_0048FFC0`, "Samples" = `PathElement[n]` of 0x30 bytes) whose sample array had been
  reused by other data during play. The destructor now leaves a reused array alone, and a watch on the game's
  `malloc`/`free` (`FUN_001727D0` / `FUN_00172DA8`) logs `[kz] path: WATCH` with the stack when memory inside a
  live path array changes owner. Upstream disables the video thread on the European disc because of a worker
  race that corrupts memory; this is the main suspect for the level-change problems and is being investigated.

## 4. Useful environment variables

| Variable | Effect |
|---|---|
| `KZ_NET=host` / `join`, `KZ_NET_ADDR`, `KZ_NET_FPS` | Session (the launcher sets them) |
| `KZ_LUA="<frame>:<code>;;..."` | Runs Lua at given game frames (tests), e.g. `gGamePlayInfo:SetGameStatus(STATUS_WON) gIsFinished = TRUE` |
| `KZ_PATH_GUARD=0`, `KZ_PATH_WATCH=0`, `KZ_PATH_TEST=1` | PathResource guard off / watch off / force the corruption (repro) |
| `KZ_CRASH_TRACE=1` | Host-side crash: faulting function and stack in the log (on in sessions) |
| `KZ_COOP_LEGACY=1` | Old player-2 creation (frame-30 add), for comparison |
| `PS2X_VIF1_THREAD=0` | Synchronous video (slower, used to A/B the level-change problem); launcher.json `"video_thread": false` |
