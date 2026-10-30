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

Build the harness (in WSL), then run a script with `tools/tas/run.sh`:

```
runtime/build-harness.sh ~/hotd2/roms/hotd2.zip ~/hotd2/hb
tools/tas/run.sh tools/tas/chapter1.tas      # smoke test: chapter 1 and its boss, ~1 minute
tools/tas/run.sh tools/tas/full_run.tas      # coin-in to the ending, ~6 minutes
```

`run.sh SCRIPT [build dir] [run dir]` runs in `~/hotd2/tas-run` (screenshots and dumps
land there), links `cart.bin`, sets `HOTD2_TRACE=0` (no hardware tracing, needed for
speed: about 280 frames/s), `HOTD2_INTERP=1` (code the static analysis missed runs in
the interpreter) and a frame limit (`HOTD2_FRAMES`, default 120000; reaching it before
the script ends fails the run with exit code 3), and shows only the script's log. Log
lines look like `[tas  12345] text` with the frame number; `HOTD2_TAS_LOG=file` writes
them to a file instead of stderr. By hand:

```
cd run-dir && ln -sf ~/hotd2/hb/cart.bin cart.bin
HOTD2_TAS=/path/to/full_run.tas HOTD2_TRACE=0 HOTD2_INTERP=1 HOTD2_FRAMES=120000 ~/hotd2/hb/harness ~/hotd2/hb/ic22.bin
```

Exit codes: 0 the script ended (or `quit`) with every `check` passing, 1 a `check`
failed, 2 script error, 3 an `until` timed out or the frame limit came first. The
emulation is deterministic: a script gives the same log, frame for frame, every run.

## Scripts

| script | what |
|---|---|
| `tools/tas/full_run.tas` | coin-in to the ending; checkpoints at every chapter, every boss fight, the ending, name entry and ranking |
| `tools/tas/chapter1.tas` | quick test: start a game and clear chapter 1 including its boss |
| `tools/tas/start_game.tas` | boot, two coins, start (skips the opening cutscene) |
| `tools/tas/auto_setup.tas` | auto play settings shared by the scripts |
| `tools/tas/chapterN.inc` | one chapter each: boss fight(s) and the next chapter as checkpoints |

A run cannot start mid-game (the harness has no save states), so a chapter script
plays the chapters before it too; `chapter1.tas` is the short one.

Current result of `full_run.tas` (all checks pass, exit 0):

| checkpoint | frame | lives |
|---|---|---|
| game started | 1489 | 3 |
| chapter 1 boss / cleared | 11970 / 15720 | 4 / 5 |
| chapter 2 boss / cleared | 28144 / 32050 | 5 / 5 |
| chapter 3 boss / cleared | 42450 / 47585 | 5 / 5 |
| chapter 4 boss / cleared | 55460 / 60771 | 4 / 5 |
| chapter 5 bosses 1-4 | 64543, 67958, 71968, 82697 | 5 |
| final boss / beaten | 90941 / 92664 | 5 |
| ending (no continue used) | 95040 | 5 |
| name entry / ranking | 102088 / 103350 | |

Lives go up as well as down: rescuing people gives extra lives.

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
| `auto ignore SHOTS FRAMES` | drop a part for FRAMES after SHOTS shots at it made no progress (default 2, 90) |
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

`auto on` plays like a careful player using the game's own hit-test data (read only):

- Every frame the game rebuilds a list of shootable objects (count at `0C14E100`,
  20-byte entries at `0C146D70`, word 0 = object) and tests the gun ray (when the
  trigger is pulled) against spheres in view space (camera looking down -Z): either
  the object's bounding sphere (centre `+108/+112/+116`, radius `+0x120`) or, when
  flag `0x80` of `+48` is set, one sphere per skeleton part (part `+0x208 + index*0x90`,
  centre `+104/+108/+112`, radius `+120`; skeleton from table `0CB59DD4` indexed by
  `s16 +0x1F0`, children at node `+24`, part index at node `+20`). Objects and parts
  with flag `0x10` are tested against meshes instead and are not modelled. Of all
  spheres the ray passes through, the one with the nearest centre takes the shot
  (`f_0c0293a2` calls `f_0c029540`/`f_0c029740`, ray test `f_0c02b30c`, records sorted
  by `f_0c02a384`, applied in `f_0c029e2e`).
- The gun ray goes through `(sx, sy, -F)` with F = `f32 0C403438` (640.2), so a sphere
  centre (X, Y, Z) is hit at `sx = -F*X/Z`, `sy = -F*Y/Z`. The game's current gun
  offset is `s16 0C3D17D0/17D2` (+-300 by +-220 at the screen edges).
- Aim point: the sphere's centre, or a point further out on it, that the gun can reach
  and that no nearer sphere covers (so a boss's weapon or a crate in front does not
  swallow the shot).
- Target choice: the nearest object that can be hit, staying a little longer on the
  current one; on it, the part that made progress most often, else the topmost part
  (for people, the head). Skipped: objects flagged `0x100` (hits no longer count:
  dying), bit-31 objects already shot (thrown weapons and props take one hit), and
  objects listed with `auto avoid` (always: innocents, task `0C092DE6`; shooting one
  costs a life and shows HOLD YOUR FIRE!).
- Progress learning: a shot is seen as the magazine (`u32 0C3D0444`) dropping by one
  (the shot counter stops during boss fights). It made progress when, within 3
  frames, the hit counter went up or the boss health (`f32 0C3D11FC`) went down. After
  `SHOTS` shots without progress (`auto ignore`, default 2) the part is dropped for
  `FRAMES` (default 90); an object whose every part is dropped is only shot when
  nothing else is in reach. This is how bosses' weak points are found without
  per-boss knowledge, and how dying enemies stop eating bullets.
- Fire: trigger 2 frames on, 2 off (the game counts one shot per press); reload when
  the magazine is empty, or when nothing is in reach and it is not full. Explicit
  `fire`/`reload` commands override auto play for that frame.
- Each life lost is logged with what auto play was aiming at (with `auto debug` also a
  screenshot `life_FRAME.ppm`); `auto debug -1` logs every shot with the target, its
  part and flags, and the game's own nearest hit record (`0C14A538 + 60 * index`).

Gun calibration: the harness sends the gun as 16-bit analog values; with the factory
calibration the game's offset is `(px - 320) * 300/320` across and
`-(py - 240) * 220/240` down (measured with a sweep, exact to the game's 1/256 gun
steps), and it takes 3 frames from input to the hit test.

Why `auto_setup.tas` holds fire on small flying objects (tasks `0C076BEC`,
`0C07A938`) until they are close: in chapter 1, shooting a whole swarm of them in the
distance as fast as possible spawns an effect per hit, the display list grows from
about 530 KB to 855 KB within 100 frames, and the Kamui SDK's vertex buffer overflows
(`f_0c0d0080` calls the error callback with code 2; the game's handler `f_0c0b33e0`
sets the border colour and executes `trapa #48`, which the harness reports as
unimplemented `C330 at 0C0B3460`). The SDK's buffer setup matches Flycast's, so this
is the game's own limit rather than a port bug.

## Known addresses

| name | address | what |
|---|---|---|
| `lives` | `u8 0C3D0430` | player 1 lives |
| `shots` | `u32 0C3D0454` | shots fired (reset per credit) |
| `hits` | `u32 0C3D0458` | shots that hit and counted |
| `ammo` | `u32 0C3D0444` | bullets left in the gun (player struct `0C3D0420` + 36) |
| `player` | `0C3D0420` | player 1 struct (256 bytes per player) |
| `chapter` | `u32 0C3D0530` | chapter, 0-based (0..4) |
| `mode` | `u32 0C3D07B4` | main game mode: 5 in game, 6 continue, others attract and ranking |
| `submode` | `u32 0C3D07B8` | sub-state of the mode; in game (mode 5): 2 playing, 8 ending, 10 name entry |
| `bosshp` | `f32 0C3D11FC` | boss health, 1 to 0 during a boss fight, 0 otherwise |
| `gunx`, `guny` | `s16 0C3D17CC/CE` | gun in game units (0..255) |
| `aimx`, `aimy` | `s16 0C3D17D0/D2` | gun offset from the screen centre (ray units) |
| `targets` | `u32 0C14E100` | shootable objects tested last frame |
| `focal` | `f32 0C403438` | projection distance of the gun ray |

Objects start with their task function (word 0); seen so far: `0C05BC1C` most
enemies, `0C092DE6` innocents (their task takes a life from whoever shot them, via
`f_0c0972a0`), `0C076BEC`/`0C07A938` small flying objects, `0C07E064` the chapter 2
boss. Flags at `+48`: `0x08` shot this frame, `0x02`/`0x04` by player 1/2, `0x10` mesh
hit test, `0x80` per-part spheres, `0x100` hits not counted.
