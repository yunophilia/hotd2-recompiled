# TAS scripts (test automation)

A TAS script plays player 1 in the native harness (`runtime/harness.c`) using only
real input: gun position, trigger, reload (gun pointed off-screen), start and coins,
all sent through the emulated JVS I/O board exactly as a cabinet would. Scripts read
guest RAM to decide what to do (where enemies are, when a chapter ends) but never
write it: no freezes, no health hacks. The runner is `runtime/tas.c`; with `HOTD2_TAS`
unset the harness behaves as before.

`tools/tas/full_run.tas` plays from coin-in to the ending and checks progress on the
way, so it doubles as a regression test of the recompiled game: a broken hit test,
gun calibration, scene script or renderer path shows up as an `until` timeout (exit 3)
or a failed `check` (exit 1).

## Running

Build the harness (in WSL) and run it from a directory with `cart.bin`:

```
runtime/build-harness.sh ~/hotd2/roms/hotd2.zip ~/hotd2/hb
cd ~/hotd2/run && ln -sf ../hb/cart.bin cart.bin
HOTD2_TAS=/path/to/tools/tas/full_run.tas HOTD2_TRACE=0 HOTD2_INTERP=1 HOTD2_FRAMES=200000 ~/hotd2/hb/harness ~/hotd2/hb/ic22.bin
```

`HOTD2_TRACE=0` turns off hardware tracing (needed for speed, roughly 250 frames/s),
`HOTD2_INTERP=1` lets code the static analysis missed run in the interpreter, and
`HOTD2_FRAMES` is a safety limit (reaching it before the script ends fails the run
with exit code 3). The script's log goes to stderr, or to `HOTD2_TAS_LOG=file`; lines
look like `[tas  12345] text` with the frame number. `tools/tas/run.sh` wraps all of
this.

Exit codes: 0 the script ended (or `quit`) with every `check` passing, 1 a `check`
failed, 2 script error, 3 an `until` timed out or the frame limit came first. The
emulation is deterministic, so a script gives the same result every run.

## Script language

One command per line; `#` starts a comment. Times are seconds (`1.5`) or frames
(`90f`); the game runs at 60 frames per second. Commands run in order and input
commands take time; everything else is instant.

| command | effect |
|---|---|
| `wait T` | no new input for T (auto play continues) |
| `aim X Y` | gun position in screen pixels (0..639, 0..479); stays until changed |
| `fire [T]` | pull the trigger for T (default 3 frames), then release for as long |
| `reload [T]` | point the gun off-screen for T (default 6 frames), then back |
| `start [T]` | press start |
| `coin` | insert a coin (slot 1); the game needs two credits per player |
| `tap INPUT [T]` | press INPUT for T (default 3 frames), release for T |
| `hold T INPUT` | hold INPUT for T |
| `mash INPUT T [P]` | press INPUT repeatedly for T, period P (default 12 frames) |
| `set INPUT` / `set` | background input added to everything that follows / clear it |
| `until COND [and/or COND...] [mash INPUT] [timeout T]` | wait until the condition holds (default timeout 60 s; a timeout fails the run with exit code 3); `mash` presses INPUT while waiting |
| `check COND [msg TEXT]` | log PASS or FAIL without waiting; any FAIL makes the exit code 1 |
| `auto on` / `auto off` | auto play: aim at and shoot enemies, reload when empty (see below) |
| `auto avoid TASK [DEPTH]` | objects whose task function is TASK (hex) are not shot, or only once nearer than DEPTH |
| `auto ignore SHOTS FRAMES` | deprioritise a target for FRAMES after SHOTS counted shots added no hit (default 2, 90) |
| `auto debug N` | log auto play's view every N frames; `-1`: one line per shot and per life lost |
| `calibrate CX CY KX KY` | gun calibration (see below); the default fits the game's factory setting |
| `log TEXT` | log a line; `{TYPE ADDR}` inside TEXT is replaced by a value, `{frame}` by the frame |
| `shot NAME` | write the next rendered frame to NAME.ppm (`tools/ppm2png.py` converts) |
| `dump NAME` | write main RAM (32 MB) to NAME.bin |
| `repeat [N]` ... `end` | loop N times (no N: until an `until` times out or `quit`) |
| `include FILE` | include another script (path relative to the including script) |
| `quit [CODE]` | stop now with CODE (default 0, or 1 if a check failed) |

INPUT: `FIRE` (or `TRIGGER`), `RELOAD`, `START`, `SERVICE`, `TEST`, joined with `+`.

COND: `TYPE ADDR OP VALUE` with TYPE `u8 s8 u16 s16 u32 s32 f32` and OP
`== != < <= > >=`, or `frame OP N`. ADDR is a hex address (`0C3D0430`), a name from
the table below, `[ADDR]` to read a pointer, with `+N`/`-N` decimal offsets (`0x` for
hex), e.g. `u32 [0C3D1284]+48 == 0`. Several conditions join left to right with
`and` / `or`.

## Auto play

`auto on` plays like a careful player using the game's own hit-test data:

- Every frame the game rebuilds a list of shootable objects (count at `0C14E100`,
  20-byte entries at `0C146D70`, word 0 = object). For each one it tests the gun ray
  against spheres in view space (camera looking down −Z): either the object's
  bounding sphere (centre `+108/+112/+116`, radius `+0x120`) or, when flag `0x80` of
  `+48` is set, one sphere per skeleton part (parts `+0x208 + index*0x90`, centre
  `+104/+108/+112`, radius `+120`, skeleton from table `0CB59DD4` indexed by
  `s16 +0x1F0`). Parts and objects with flag `0x10` are tested against meshes
  instead and are not targeted.
- The gun ray goes through `(sx, sy, −F)` with F = `f32 0C403438` (640.2), so a
  sphere centre (X, Y, Z) is hit at `sx = −F·X/Z`, `sy = −F·Y/Z`. The game's current
  gun offset is `s16 0C3D17D0/17D2` (±300 by ±220 at the screen edges).
- Target choice: the nearest object (by its nearest sphere) the gun can reach,
  staying on the current one while it lasts; on it, the topmost sphere (for people,
  the head). Objects flagged `0x100` (dying) and bit-31 objects already shot (thrown
  weapons and props take one hit) are skipped; innocents (task `0C092DE6`) are never
  shot. A target whose counted shots stop adding hits is deprioritised for a while.
- Fire: trigger 2 frames on, 2 off (the game counts at most one shot per press);
  reload when the magazine (`u32 0C3D0444`) is empty, or when nothing is in reach and
  it is not full. Explicit `fire`/`reload` commands override auto play for that frame.
- Each life lost is logged with what auto play was aiming at.

Gun calibration: the harness sends the gun as 16-bit analog values; with the factory
calibration the game's offset is `(px − 320) · 300/320` across and
`−(py − 240) · 220/240` down (measured with a sweep, exact to the game's 1/256 gun
steps), with 3 frames from input to the hit test.

## Known addresses

| name | address | what |
|---|---|---|
| `lives` | `u8 0C3D0430` | player 1 lives |
| `shots` | `u32 0C3D0454` | shots fired (reset per credit) |
| `hits` | `u32 0C3D0458` | shots that hit and counted |
| `ammo` | `u32 0C3D0444` | bullets left in the gun (player struct `0C3D0420` + 36) |
| `player` | `0C3D0420` | player 1 struct (256 bytes per player) |
| `mode` | `u32 0C3D07B4` | main game mode |
| `submode` | `u32 0C3D07B8` | sub-state of the mode |
| `bosshp` | `f32 0C3D11FC` | boss health fraction |
| `gunx`, `guny` | `s16 0C3D17CC/CE` | gun in game units (0..255) |
| `aimx`, `aimy` | `s16 0C3D17D0/D2` | gun offset from the screen centre (ray units) |
| `targets` | `u32 0C14E100` | shootable objects tested last frame |
| `focal` | `f32 0C403438` | projection distance of the gun ray |

Object task functions seen so far: `0C05BC1C` zombies, `0C092DE6` innocents,
`0C076BEC`/`0C07A938` small flying objects.
