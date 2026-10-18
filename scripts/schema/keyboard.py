# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.

from enum import Enum
from typing import Literal
from pydantic import BaseModel, Field, NonNegativeInt, PositiveFloat, PositiveInt


class KeyboardUSBPort(str, Enum):
    FULL_SPEED = "fs"
    HIGH_SPEED = "hs"


# USB Configuration
class KeyboardUSB(BaseModel):
    # USB Vendor ID
    vid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    # USB Product ID
    pid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    port: KeyboardUSBPort


# Keyboard Configuration
class KeyboardKeyboard(BaseModel):
    num_profiles: int = Field(ge=1, le=8)
    num_layers: int = Field(ge=1, le=8)
    num_keys: int = Field(ge=1, le=256)
    num_advanced_keys: int = Field(ge=1, le=64)
    # Maximum number of Dynamic Keystroke bindings per key. Higher values may require higher storage sizes.
    num_dynamic_keystroke_max_bindings: int = Field(ge=4, le=64, default=4)
    num_macro_nodes: int = Field(ge=1, le=255, default=128)


# Hardware Configuration
class KeyboardHardware(BaseModel):
    # High-speed external oscillator value in Hz
    hse_value: PositiveInt
    # Keyboard driver name
    driver: str


# Raw ADC input configuration
class KeyboardAnalogRaw(BaseModel):
    # Array of raw ADC input channels. If a string is provided, it is used as the GPIO pin name
    input: list[NonNegativeInt | str]
    # Array of key index mappings for the raw ADC inputs
    vector: list[NonNegativeInt]


# Analog Multiplexer ADC input configuration
class KeyboardAnalogMux(BaseModel):
    # Array of GPIO pin names for the multiplexer select lines
    select: list[str]
    # Array of ADC input channels for the multiplexers. If a string is provided, it is used as the GPIO pin name
    input: list[NonNegativeInt | str]
    # Mapping from multiplexers to ADC input channels
    matrix: list[list[NonNegativeInt]]
    # Drive the select lines one at a time (a column scan) instead of as a binary
    # address. The matrix then has one row per select line instead of 2^n rows.
    one_hot: bool = False
    # Selected line is driven low and the others high. Only used with `one_hot`.
    active_low: bool = False


# Analog Configuration
class KeyboardAnalog(BaseModel):
    # ADC resolution for this keyboard. A higher value means higher accuracy but slower matrix scans. Default to the maximum resolution supported by the MCU if not provided.
    adc_resolution: PositiveInt | None = None
    # Set to true if the ADC value is inversely proportional to the travel distance of the keys
    invert_adc: bool = False
    # Delay in microseconds between ADC scans
    delay: int | None = None
    raw: KeyboardAnalogRaw | None = None
    mux: KeyboardAnalogMux | None = None


# Calibration Configuration
class KeyboardCalibration(BaseModel):
    # See `include/lib/eeconfig.h`
    initial_rest_value: NonNegativeInt
    # See `include/lib/eeconfig.h`
    initial_bottom_out_threshold: NonNegativeInt


# Wear leveling Configuration
class KeyboardWearLeveling(BaseModel):
    # Size of the virtual persistent storage in bytes. There must be enough RAM of this size to hold the entire virtual storage.
    virtual_size: int = Field(ge=1, le=16384)
    # Size of the write log in bytes
    write_log_size: int = Field(ge=1, le=65536)


class KeyboardLayoutKey(BaseModel):
    key: NonNegativeInt
    w: PositiveFloat | None = None
    h: PositiveFloat | None = None
    x: float | None = None
    y: float | None = None
    # Option key and value pairs for the corresponding labels
    option: tuple[int, int] | None = None


# Keyboard Layout
class KeyboardLayout(BaseModel):
    # Labels for each layout option. Use a string for a toggle option, or an array for a select option
    labels: list[str | list[str]] | None = None
    # Metadata for how the keyboard should be rendered in the web configurator
    keymap: list[list[KeyboardLayoutKey]]


# Actuation Configuration
class KeyboardKeyActuation(BaseModel):
    key: NonNegativeInt
    actuation_point: int = Field(ge=0, le=255)
    # Rapid Trigger press / release sensitivity (0 = off / same as rt_down)
    rt_down: int = Field(ge=0, le=255, default=0)
    rt_up: int = Field(ge=0, le=255, default=0)
    continuous: bool = False


class KeyboardActuation(BaseModel):
    # Default actuation point
    actuation_point: int = Field(ge=0, le=255)
    # Keys that differ from the default
    keys: list[KeyboardKeyActuation] = []


# keyboard.json Schema
# One step of a default macro
class KeyboardMacroStep(BaseModel):
    action: Literal["press", "release", "tap"]
    # Keycode name, e.g. "KC_LSFT"
    keycode: str
    # Delay in milliseconds after this step
    delay: int = Field(ge=0, le=8191, default=0)


# Default advanced key. Only Tap Dance, Macro and Null Bind keys can be set here.
class KeyboardAdvancedKey(BaseModel):
    type: Literal["tap_dance", "macro", "null_bind"]
    layer: NonNegativeInt
    key: NonNegativeInt
    # Tap Dance actions: a macro name from `macros`, or a keycode name that stays
    # pressed while the key is held. Unset actions are left out.
    tap: str | None = None
    hold: str | None = None
    double_tap: str | None = None
    tap_hold: str | None = None
    # Tap Dance tapping term in milliseconds
    tapping_term: int = Field(ge=1, le=65535, default=200)
    # Macro key: a macro name from `macros`
    macro: str | None = None
    # Null Bind: `key` is the primary key
    secondary_key: NonNegativeInt | None = None
    behavior: Literal["last", "primary", "secondary", "neutral", "distance"] = "last"
    bottom_out_point: int = Field(ge=0, le=255, default=0)


class Keyboard(BaseModel):
    name: str
    manufacturer: str
    maintainer: str
    usb: KeyboardUSB
    keyboard: KeyboardKeyboard
    hardware: KeyboardHardware
    analog: KeyboardAnalog
    calibration: KeyboardCalibration
    wear_leveling: KeyboardWearLeveling | None = None
    layout: KeyboardLayout
    # Default keymap
    keymap: list[list[str]] | None = None
    # Default keymaps for each profile. If not specified, the default keymap will be used for all profiles.
    keymaps: list[list[list[str]]] | None = None
    actuation: KeyboardActuation | None = None
    # Default macros by name, used by `advanced_keys`
    macros: dict[str, list[KeyboardMacroStep]] | None = None
    # Default advanced keys (same for every profile)
    advanced_keys: list[KeyboardAdvancedKey] | None = None
