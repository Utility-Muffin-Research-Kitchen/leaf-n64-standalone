#!/bin/sh
set -eu

SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

if [ -f "$SELF_DIR/../../launcher/env.sh" ]; then
    . "$SELF_DIR/../../launcher/env.sh"
elif [ -n "${UMRK_ENV_FILE:-}" ] && [ -f "$UMRK_ENV_FILE" ]; then
    . "$UMRK_ENV_FILE"
fi

if [ "$#" -lt 1 ]; then
    echo "usage: launch.sh <rom-path>" >&2
    exit 64
fi

if [ "${MUPEN64PLUS_DEBUG:-0}" = "1" ]; then
    set -x
fi

PLATFORM="${PLATFORM:-mlp1}"
SDCARD_PATH="${SDCARD_PATH:-/mnt/sdcard}"
UMRK_PLATFORM_PATH="${UMRK_PLATFORM_PATH:-${SYSTEM_PATH:-$SDCARD_PATH/.system/leaf/platforms/$PLATFORM}}"
USERDATA_PATH="${USERDATA_PATH:-$SDCARD_PATH/.userdata/$PLATFORM}"
SHARED_USERDATA_PATH="${SHARED_USERDATA_PATH:-$SDCARD_PATH/.userdata/shared}"
SAVES_PATH="${SAVES_PATH:-$SDCARD_PATH/Saves}"
STATES_PATH="${STATES_PATH:-$SDCARD_PATH/States}"
LOGS_PATH="${LOGS_PATH:-$USERDATA_PATH/logs}"
ROMS_PATH="${ROMS_PATH:-$SDCARD_PATH/Roms}"

BIN_DIR="$SELF_DIR/bin"
LIB_DIR="$SELF_DIR/lib"
DEFAULTS_DIR="$SELF_DIR/defaults"
INI="$BIN_DIR/ini"
ROM="$1"
ORIGINAL_ROM="$ROM"
ROM_BASE="$(basename "$ROM")"
ROM_STEM="${ROM_BASE%.*}"

STATE_ROOT="${MUPEN64PLUS_STATE_ROOT:-$USERDATA_PATH/mupen64plus}"
CONFIG_DIR="$STATE_ROOT/config"
CACHE_DIR="$STATE_ROOT/cache"
PER_GAME_DIR="$STATE_ROOT/per-game"
BATTERY_SAVE_DIR="$SAVES_PATH/N64"
STATE_SAVE_DIR="$STATES_PATH/Mupen64Plus Standalone"
SCREENSHOT_DIR="${SCREENSHOTS_PATH:-$STATE_ROOT/screenshots}"

mkdir -p \
    "$CONFIG_DIR" \
    "$CACHE_DIR" \
    "$PER_GAME_DIR" \
    "$BATTERY_SAVE_DIR" \
    "$STATE_SAVE_DIR" \
    "$SCREENSHOT_DIR" \
    "$LOGS_PATH"

exec >>"$LOGS_PATH/mupen64plus.log"
exec 2>&1

cleanup() {
    if [ -n "${ROM_EXTRACT_DIR:-}" ] && [ -d "$ROM_EXTRACT_DIR" ]; then
        rm -rf "$ROM_EXTRACT_DIR"
    fi
    if [ -n "${DEVICE_CFG_BACKUP:-}" ] && [ -f "$DEVICE_CFG_BACKUP" ]; then
        mv "$DEVICE_CFG_BACKUP" "$DEVICE_CFG"
    fi
}
trap cleanup EXIT INT TERM HUP QUIT

DEVICE_CFG="$CONFIG_DIR/mupen64plus.cfg"
if [ ! -f "$DEVICE_CFG" ]; then
    cp "$DEFAULTS_DIR/default.cfg" "$DEVICE_CFG"
fi

INPUT_PROFILE_VERSION="mlp1-loong-sdl-20260627"
INPUT_PROFILE_STAMP="$CONFIG_DIR/.input-profile-$INPUT_PROFILE_VERSION"
if [ "$PLATFORM" = "mlp1" ] && [ ! -f "$INPUT_PROFILE_STAMP" ]; then
    INPUT_PROFILE_CFG="$CONFIG_DIR/mlp1-input-profile.ini"
    cat >"$INPUT_PROFILE_CFG" <<'EOF'
[Input-SDL-Control1]
DPad R = "hat(0 Right)"
DPad L = "hat(0 Left)"
DPad D = "hat(0 Down)"
DPad U = "hat(0 Up)"
Start = "button(9)"
Z Trig = "button(6)"
R Trig = "button(5)"
L Trig = "button(4)"
Select = "button(8)"
Hotkey = "button(10)"
EOF
    if "$INI" merge "$DEVICE_CFG" "$INPUT_PROFILE_CFG"; then
        : >"$INPUT_PROFILE_STAMP"
    fi
fi

PER_GAME_CFG="$PER_GAME_DIR/$ROM_BASE.cfg"
if [ -f "$PER_GAME_CFG" ]; then
    DEVICE_CFG_BACKUP="$DEVICE_CFG.console-backup"
    cp "$DEVICE_CFG" "$DEVICE_CFG_BACKUP"
    export EMU_CONSOLE_CFG="$DEVICE_CFG_BACKUP"
    "$INI" merge "$DEVICE_CFG" "$PER_GAME_CFG" || true
fi

resolve_font() {
    if [ -n "${CAT_FONT_PATH:-}" ]; then
        case "$CAT_FONT_PATH" in
            /*)
                [ -f "$CAT_FONT_PATH" ] && printf '%s\n' "$CAT_FONT_PATH" && return 0
                ;;
            *)
                if [ -n "${CAT_FONTS_DIR:-}" ] && [ -f "$CAT_FONTS_DIR/$CAT_FONT_PATH" ]; then
                    printf '%s\n' "$CAT_FONTS_DIR/$CAT_FONT_PATH"
                    return 0
                fi
                ;;
        esac
    fi
    for candidate in \
        "${UMRK_LAUNCHER_PATH:-$UMRK_PLATFORM_PATH/launcher}/res/font.ttf" \
        "${UMRK_LAUNCHER_PATH:-$UMRK_PLATFORM_PATH/launcher}/res/fonts/SpaceGrotesk/SpaceGrotesk-Regular.ttf"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

FONT_FILE="$(resolve_font || true)"

case "$ROM" in
    *.zip|*.ZIP|*.7z|*.7Z)
        ROM_EXTRACT_DIR="$(mktemp -d /tmp/m64p_extracted.XXXXXX)"
        "$LIB_DIR/7zzs" e "$ROM" -o"$ROM_EXTRACT_DIR" -y >/dev/null
        ROM_INNER=""
        for f in "$ROM_EXTRACT_DIR"/*.z64 "$ROM_EXTRACT_DIR"/*.n64 "$ROM_EXTRACT_DIR"/*.v64 "$ROM_EXTRACT_DIR"/*.rom; do
            if [ -f "$f" ]; then
                ROM_INNER="$f"
                break
            fi
        done
        if [ -z "$ROM_INNER" ]; then
            ROM_INNER="$(find "$ROM_EXTRACT_DIR" -maxdepth 1 -type f -print | sort | head -n 1)"
        fi
        if [ -z "$ROM_INNER" ] || [ ! -f "$ROM_INNER" ]; then
            echo "[mupen64plus] no ROM file found inside archive: $ORIGINAL_ROM" >&2
            exit 1
        fi
        ROM_RENAMED="$ROM_EXTRACT_DIR/$ROM_STEM.z64"
        if [ "$ROM_INNER" != "$ROM_RENAMED" ]; then
            mv "$ROM_INNER" "$ROM_RENAMED"
        fi
        ROM="$ROM_RENAMED"
        ;;
esac

VIDEO_PLUGIN_VALUE="$("$INI" get "$DEVICE_CFG" "NextUI" "VideoPlugin" 2>/dev/null || true)"
case "$VIDEO_PLUGIN_VALUE" in
    0|gliden64|GLideN64)
        if [ -f "$LIB_DIR/mupen64plus-video-GLideN64.so" ]; then
            GFX_PLUGIN="mupen64plus-video-GLideN64.so"
            export EMU_VIDEO_PLUGIN=gliden64
        else
            GFX_PLUGIN="mupen64plus-video-rice.so"
            export EMU_VIDEO_PLUGIN=rice
        fi
        ;;
    *)
        GFX_PLUGIN="mupen64plus-video-rice.so"
        export EMU_VIDEO_PLUGIN=rice
        ;;
esac

export HOME="$STATE_ROOT/home"
export XDG_CONFIG_HOME="$CONFIG_DIR"
export XDG_DATA_HOME="$STATE_ROOT/data"
export XDG_CACHE_HOME="$CACHE_DIR"
export LD_LIBRARY_PATH="$LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_VIDEODRIVER="${MUPEN64PLUS_SDL_VIDEODRIVER:-kmsdrm}"
export SDL_KMSDRM_REQUIRE_DRM_MASTER="${SDL_KMSDRM_REQUIRE_DRM_MASTER:-0}"
export DISPLAY_ROTATION="${MUPEN64PLUS_DISPLAY_ROTATION:-${DISPLAY_ROTATION:-270}}"
if [ "$EMU_VIDEO_PLUGIN" = "gliden64" ]; then
    export DISPLAY_ROTATION="${MUPEN64PLUS_DISPLAY_ROTATION:-0}"
    export MUPEN64PLUS_GLIDEN64_FINAL_ROTATION="${MUPEN64PLUS_GLIDEN64_FINAL_ROTATION:-90}"
    export MUPEN64PLUS_OVERLAY_ROTATION="${MUPEN64PLUS_OVERLAY_ROTATION:-270}"
fi
export MUPEN64PLUS_C_BUTTON_MOD="${MUPEN64PLUS_C_BUTTON_MOD:-1}"
export MUPEN64PLUS_C_BUTTON_MOD_BUTTON="${MUPEN64PLUS_C_BUTTON_MOD_BUTTON:-7}"
export MUPEN64PLUS_C_BUTTON_A_BUTTON="${MUPEN64PLUS_C_BUTTON_A_BUTTON:-1}"
export MUPEN64PLUS_C_BUTTON_B_BUTTON="${MUPEN64PLUS_C_BUTTON_B_BUTTON:-0}"
export MUPEN64PLUS_C_BUTTON_X_BUTTON="${MUPEN64PLUS_C_BUTTON_X_BUTTON:-2}"
export MUPEN64PLUS_C_BUTTON_Y_BUTTON="${MUPEN64PLUS_C_BUTTON_Y_BUTTON:-3}"
export EMU_OVERLAY_JOYSTICK_INDEX="${EMU_OVERLAY_JOYSTICK_INDEX:-0}"
export EMU_MENU_BUTTON="${EMU_MENU_BUTTON:-10}"
export EMU_SELECT_BUTTON="${EMU_SELECT_BUTTON:-8}"
export EMU_START_BUTTON="${EMU_START_BUTTON:-9}"
export EMU_L1_BUTTON="${EMU_L1_BUTTON:-4}"
export EMU_R1_BUTTON="${EMU_R1_BUTTON:-5}"
export EMU_L2_AXIS="${EMU_L2_AXIS:-4}"
export EMU_R2_AXIS="${EMU_R2_AXIS:-5}"
export EMU_ROM_PATH="$ORIGINAL_ROM"
export EMU_OVERLAY_JSON="$DEFAULTS_DIR/overlay_settings.json"
export EMU_OVERLAY_INI="$DEVICE_CFG"
export EMU_OVERLAY_GAME="$ROM_STEM"
export EMU_OVERLAY_CONSOLE="${EMU_OVERLAY_CONSOLE:-Nintendo 64}"
export EMU_DEFAULT_CFG="$DEFAULTS_DIR/default.cfg"
export EMU_OVERLAY_FONT="$FONT_FILE"
export EMU_OVERLAY_SCREENSHOT_DIR="$STATE_SAVE_DIR"
export EMU_OVERLAY_ROMFILE="$ROM_BASE"
export EMU_OVERLAY_STATE_DIR="$STATE_SAVE_DIR"
export EMU_OVERLAY_STATE_STEM="$ROM_STEM"
export EMU_DISABLE_NEXTUI_MARKERS="${EMU_DISABLE_NEXTUI_MARKERS:-1}"
export EMU_PER_GAME_CFG="$PER_GAME_CFG"
export EMU_BUTTON_MAP_FILE="$PER_GAME_DIR/$ROM_BASE.buttons"

mkdir -p "$HOME" "$XDG_DATA_HOME"

RESOLUTION="${MUPEN64PLUS_RESOLUTION:-${CAT_WINDOW_WIDTH:-960}x${CAT_WINDOW_HEIGHT:-720}}"
SCREEN_W="${RESOLUTION%x*}"
SCREEN_H="${RESOLUTION#*x}"

cd "$SELF_DIR"
set +e
"$BIN_DIR/mupen64plus" \
    --fullscreen \
    --resolution "$RESOLUTION" \
    --configdir "$CONFIG_DIR" \
    --datadir "$DEFAULTS_DIR" \
    --plugindir "$LIB_DIR" \
    --sshotdir "$SCREENSHOT_DIR" \
    --cachedir "$CACHE_DIR" \
    --set "Video-General[ScreenWidth]=$SCREEN_W" \
    --set "Video-General[ScreenHeight]=$SCREEN_H" \
    --set "Core[SaveSRAMPath]=$BATTERY_SAVE_DIR/" \
    --set "Core[SaveStatePath]=$STATE_SAVE_DIR/" \
    --set "Video-GLideN64[txPath]=$ROMS_PATH/N64/.hires_texture" \
    --set "Video-GLideN64[txCachePath]=$CACHE_DIR/gliden64-texture-cache" \
    --set "Video-GLideN64[fontName]=$FONT_FILE" \
    --gfx "$LIB_DIR/$GFX_PLUGIN" \
    --audio "$LIB_DIR/mupen64plus-audio-sdl.so" \
    --input "$LIB_DIR/mupen64plus-input-sdl.so" \
    --rsp "$LIB_DIR/mupen64plus-rsp-hle.so" \
    "$ROM"
rc=$?
set -e
exit "$rc"
