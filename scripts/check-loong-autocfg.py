#!/usr/bin/env python3
"""Keep the packaged Loong autoconfig profile in step with the shipped defaults.

The N64 controller sections run in full-auto mode so the input plugin can map
each pad by its SDL name -- necessary once a wireless controller can take the
first roster slot and the built-in pad is no longer SDL joystick 0. Auto mode
reads its mapping from InputAutoCfg.ini, so the calibrated Loong layout now
lives in two places:

  config/shared/default.cfg          [Input-SDL-Control1]
  config/shared/loong-autocfg.ini    [Loong Gamepad]

If they drift, the built-in pad behaves differently depending on whether a
section was auto-configured or came from the shipped defaults, which is
maddening to debug from the symptom. Compare them here instead.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CFG = ROOT / "config" / "shared" / "default.cfg"
AUTOCFG = ROOT / "config" / "shared" / "loong-autocfg.ini"

DEFAULT_SECTION = "Input-SDL-Control1"
AUTOCFG_SECTION = "Loong Gamepad"

# The mapping itself. Keys outside this set legitimately differ: default.cfg
# carries section bookkeeping (mode, device, name, plugin, version) that has no
# meaning in an autoconfig profile.
COMPARED_KEYS = (
    "AnalogDeadzone",
    "AnalogPeak",
    "DPad R",
    "DPad L",
    "DPad D",
    "DPad U",
    "Start",
    "Z Trig",
    "B Button",
    "A Button",
    "C Button R",
    "C Button L",
    "C Button D",
    "C Button U",
    "R Trig",
    "L Trig",
    "X Axis",
    "Y Axis",
)


def parse_ini(path: Path) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    current = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped[1:-1]
            sections.setdefault(current, {})
        elif "=" in stripped and not stripped.startswith(("#", ";")):
            key, _, value = stripped.partition("=")
            sections.setdefault(current, {})[key.strip()] = value.strip()
    return sections


def normalize(value: str) -> str:
    """default.cfg quotes its values and autoconfig profiles do not; an unset
    button is "" in one and empty in the other."""
    return value.strip().strip('"').strip()


def main() -> int:
    for path in (DEFAULT_CFG, AUTOCFG):
        if not path.exists():
            print(f"error: missing {path}", file=sys.stderr)
            return 1

    shipped = parse_ini(DEFAULT_CFG).get(DEFAULT_SECTION)
    profile = parse_ini(AUTOCFG).get(AUTOCFG_SECTION)
    if shipped is None:
        print(f"error: [{DEFAULT_SECTION}] not found in {DEFAULT_CFG}", file=sys.stderr)
        return 1
    if profile is None:
        print(f"error: [{AUTOCFG_SECTION}] not found in {AUTOCFG}", file=sys.stderr)
        return 1

    problems: list[str] = []
    for key in COMPARED_KEYS:
        if key not in shipped:
            problems.append(f"{key}: missing from [{DEFAULT_SECTION}]")
            continue
        if key not in profile:
            problems.append(f"{key}: missing from [{AUTOCFG_SECTION}]")
            continue
        want = normalize(shipped[key])
        got = normalize(profile[key])
        if want != got:
            problems.append(
                f"{key}: default.cfg has {want!r}, loong-autocfg.ini has {got!r}"
            )

    if problems:
        print(
            "Loong autoconfig profile has drifted from the shipped controller "
            "defaults:",
            file=sys.stderr,
        )
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1

    print(f"loong autoconfig matches [{DEFAULT_SECTION}]: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
