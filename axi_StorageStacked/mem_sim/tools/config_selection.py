"""Canonical CLI selections used by validation and post-processing tools.

Keeping these paths in one module ensures every tool selects the same one of the four
validation cfg files instead of silently rebuilding a different common parameter surface.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

REFERENCE_PRESETS = {
    "hbm3": (ROOT / "configs/validation/hbm3.cfg", "ramulator2_reference_1ch"),
    "hbm4": (ROOT / "configs/validation/hbm4.cfg", "ramulator2_reference_1ch"),
    "lpddr5": (ROOT / "configs/validation/lpddr5.cfg", "ramulator2_reference_1ch"),
    "lpddr6": (ROOT / "configs/validation/lpddr6.cfg", "ramulator2_reference_1ch"),
}

HBM4_NATIVE = (ROOT / "configs/validation/hbm4.cfg", "hbm4", "validation_native_1ch")
DRAMSIM3_HBM2 = (ROOT / "configs/validation/hbm3.cfg", "hbm3", "dramsim3_hbm2_common")


def selection_args(standard: str, preset: str | None = None) -> list[str]:
    config, default_preset = REFERENCE_PRESETS[standard]
    return ["--config", str(config), "--standard", standard,
            "--preset", preset or default_preset]


def explicit_selection_args(selection: tuple[Path, str, str]) -> list[str]:
    config, standard, preset = selection
    return ["--config", str(config), "--standard", standard, "--preset", preset]
