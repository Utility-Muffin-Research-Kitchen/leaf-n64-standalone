# N64 Standalone for Leaf / Miniloong Pocket 1

Standalone Mupen64Plus for Leaf on the Miniloong Pocket 1. Leaf stages it under:

```text
.system/leaf/platforms/mlp1/emulators/mupen64plus/
```

Primary commands:

```sh
make build-mlp1
make package-mlp1
make clean
```

`make package-mlp1` writes `output/mlp1/mupen64plus/`, including `launch.sh`,
Mupen64Plus binaries/plugins, defaults, bundled runtime libraries, and
`manifest.json`. The launcher sources Leaf's `launcher/env.sh`, uses the
canonical `Roms/N64` library, stores battery saves under `Saves/N64`, and stores
standalone state/config under the shared Leaf runtime-path contract.

## Controls

Miniloong Pocket 1 controls use the Loong Gamepad SDL joystick layout captured
by Catastrophe:

| Input | N64 action |
|---|---|
| D-pad | N64 d-pad |
| Left stick | N64 analog stick |
| A | A Button |
| B | B Button |
| Start | Start |
| L1 | L Trigger |
| R1 | R Trigger |
| L2 | Z Trigger |
| R2 + X | C-Up |
| R2 + B | C-Down |
| R2 + Y | C-Left |
| R2 + A | C-Right |
| Menu | Open/close quick menu |
| Menu + Select | Leaf game switcher |

Raw button indices are `B=0`, `A=1`, `X=2`, `Y=3`, `L1=4`, `R1=5`, `L2=6`,
`R2=7`, `Select=8`, `Start=9`, and `Menu=10`. L2/R2 are buttons, not trigger
axes; the d-pad is SDL hat 0; the analog stick is axes 0/1.

### Input Mode (d-pad)

The physical d-pad can either stay the N64 d-pad or steer the N64 analog stick,
per game. Change it while playing with **Options → Controls → Input Mode**, or
bind **Options → Shortcuts → Toggle Input Mode** to a button. The change applies
immediately and is stored with the game.

| Mode | Behaviour |
|---|---|
| **D-Pad** (default) | The d-pad sends N64 d-pad directions. The analog stick still controls the N64 analog stick. |
| **Joystick** | The d-pad sends N64 analog-stick values, for games that only read the stick. |

### Using the d-pad as C buttons (or any action)

The N64 d-pad is permanently the physical d-pad, so it is not listed as a
remappable row in **Options → Controls**. To make it send the C buttons:

1. Open **Options → Controls**, select `C-Up`, press **A**.
2. Press the d-pad direction you want (for example **d-pad Up**).
3. Repeat for `C-Down`, `C-Left`, `C-Right`.

Behaviour:

- **D-Pad** mode - a direction with an action bound to it sends that action
  *instead of* the N64 d-pad direction. Directions with no binding keep their
  normal N64 d-pad behaviour. The action's original binding (for example
  **R2 + X** for C-Up) still works, because d-pad bindings are additive.
- **Joystick** mode - the d-pad steers the N64 analog stick, so it does not
  trigger those C bindings. The normal C bindings (R2 + ABXY) still work.

## Quick Menu

Press **Menu** while playing to open the quick menu.

| Action | Button |
|---|---|
| Open / close menu | Menu |
| Navigate | D-pad or left stick |
| Confirm | A |
| Back / resume | B |
| Page left | L1 |
| Page right | R1 |

The quick menu provides Resume, Save State, Load State, Options, and Quit. On
the Quit row, Left/Right toggles between `Save & Quit` and plain `Quit`.

## Save Data

- Battery saves: `Saves/N64`
- Manual save states: `States/Mupen64Plus Standalone`
- Leaf resume state: reserved slot `99`
- Emulator config/cache: `.userdata/mlp1/mupen64plus`

Generated `.buttons` files are runtime caches. Persistent user remaps live in
the console or per-game `.cfg` files.

## Options

The overlay reads the language from Leaf/Jawaka's environment at startup.
`EMU_LANGUAGE` takes precedence, followed by `UMRK_LANGUAGE` and then
`JAWAKA_LANGUAGE`. The selected value is used directly as the language
filename, so `JAWAKA_LANGUAGE=zh_CN` loads `language/zh_CN.txt`.

The embedded options menu can save settings globally or per game. Video plugin
changes require restarting the game; most input and shortcut changes apply
during the current session.

Two video plugins are packaged:

- **Rice**: default, faster first-pass option.
- **GLideN64**: more accurate, heavier on the GPU.

## Logs

Runtime logs are written to:

```text
$SDCARD_PATH/.userdata/$PLATFORM/logs/mupen64plus.log
```

The overlay logs the selected SDL joystick and should report the Loong Gamepad
as joystick 0 with 14 buttons, 2 axes, and 1 hat on Miniloong Pocket 1.

## Technical Documentation

For build internals, patch details, and data paths, see
[TECHNICAL.md](TECHNICAL.md).
