"""Check the libhmk eeconfig migration between two MAD60 HE builds in Unicorn.

usage: python check_migration_emu.py OLD.elf NEW.elf   (raw.bin next to each)

Boots the old build on blank flash (it writes its default configuration to
sector 11), copies sector 11 into the new build and boots it. The new build
must migrate instead of resetting: global settings and every profile byte kept,
version bumped to v1.9, the lighting block filled in (the old block kept or the
config.h defaults, `hue_range` 255, the Caps Lock colour and the white balance
appended), magic numbers intact, and the result written back to sector 11. OLD
may be a v1.5, v1.6, v1.7 or v1.8 build.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmk_emu import Board  # noqa: E402

REPO = Path(__file__).resolve().parents[3]       # libhmk checkout
BOARD = Path(__file__).resolve().parents[1]      # keyboards/mad60he
OLD, NEW = Path(sys.argv[1]), Path(sys.argv[2])
SECTOR11, SECTOR_SIZE = 0x080E0000, 0x20000
MAGIC_END = struct.pack("<I", 0x0A4B4D48)          # EECONFIG_MAGIC_END
CONFIG_H = (BOARD / "config.h").read_text()
NUM_KEYS = 61


def boot(elf, sector11=None):
    b = Board(elf)
    if sector11 is not None:
        b.uc.mem_write(SECTOR11, sector11)
    b.run(None, 6_000_000)
    return b


def config(b):
    """The live configuration (eeconfig points into the wear-leveling cache)."""
    return bytes(b.uc.mem_read(b.r32(b.syms["eeconfig"]), 0x4000))


old = boot(OLD)
sector = bytes(old.uc.mem_read(SECTOR11, SECTOR_SIZE))
old_cfg = config(old)
magic_start = old_cfg[:4]
old_version = struct.unpack_from("<H", old_cfg, 4)[0]
assert old_version in (0x0105, 0x0106, 0x0107, 0x0108), f"old build stores version {old_version:#x}"
old_end = old_cfg.index(MAGIC_END)

new = boot(NEW, sector)
new_cfg = config(new)
assert new_cfg[:4] == magic_start, "magic_start"
new_version = struct.unpack_from("<H", new_cfg, 4)[0]
assert new_version == 0x0109, f"new build did not migrate (version {new_version:#x})"
new_end = new_cfg.index(MAGIC_END)

GLOBAL_V15 = 14 + NUM_KEYS * 2                    # MIGRATION_V1_5_GLOBAL_CONFIG_SIZE
RGB_V16 = 5 + NUM_KEYS * 3
RGB_V17 = RGB_V16 + 1                              # + hue_range (v1.7)
RGB_V18 = RGB_V17 + 3                              # + Caps Lock hue, sat, val (v1.8)
RGB_SIZE = RGB_V18 + 3                             # + white balance R, G, B (v1.9)
OLD_RGB = {0x0105: 0, 0x0106: RGB_V16, 0x0107: RGB_V17, 0x0108: RGB_V18}[old_version]
assert new_end - old_end == RGB_SIZE - OLD_RGB, f"size grew by {new_end - old_end}"
# Global block (magic, version aside), then the profiles, byte for byte
assert new_cfg[6:GLOBAL_V15] == old_cfg[6:GLOBAL_V15], "global settings changed"
assert new_cfg[GLOBAL_V15 + RGB_SIZE:new_end] == old_cfg[GLOBAL_V15 + OLD_RGB:old_end], "profiles changed"
cfg = dict(re.findall(r"#define (RGB_DEFAULT_\w+) (\S+)", CONFIG_H))
rgb = new_cfg[GLOBAL_V15:GLOBAL_V15 + RGB_SIZE]
want = [13, int(cfg["RGB_DEFAULT_HUE"]), int(cfg["RGB_DEFAULT_SAT"]),
        int(cfg["RGB_DEFAULT_VAL"]), int(cfg["RGB_DEFAULT_SPEED"])]
assert cfg["RGB_DEFAULT_EFFECT"] == "RGB_EFFECT_RAINBOW_PINWHEELS"   # = 13
if OLD_RGB:
    assert rgb[:OLD_RGB] == old_cfg[GLOBAL_V15:GLOBAL_V15 + OLD_RGB], "old lighting changed"
else:
    assert list(rgb[:5]) == want and rgb[5:RGB_V16] == bytes(NUM_KEYS * 3), f"lighting block {list(rgb[:5])}"
if old_version < 0x0107:
    assert rgb[RGB_V16] == 255, f"hue_range {rgb[RGB_V16]}"
caps = [0, 0, int(cfg["RGB_DEFAULT_VAL"])]
if old_version < 0x0108:
    assert list(rgb[RGB_V17:RGB_V18]) == caps, f"Caps Lock colour {list(rgb[RGB_V17:RGB_V18])}"
assert list(rgb[RGB_V18:]) == [255, 255, 255], f"white balance {list(rgb[RGB_V18:])}"
writes = [a for a in new.flash_writes if SECTOR11 <= a < SECTOR11 + SECTOR_SIZE]
assert writes, "migration not written to flash"
assert all(a >= SECTOR11 for a in new.flash_writes), "flash write below sector 11"
print(f"migration v{old_version >> 8}.{old_version & 255} -> v1.9: {old_end} -> {new_end} bytes, global settings and "
      f"{new_end - GLOBAL_V15 - RGB_SIZE} profile bytes unchanged, lighting "
      f"{'kept' if OLD_RGB else want} + Caps Lock colour + white balance 255, {len(writes)} flash writes in sector 11")

# Booting the migrated flash again must not migrate or reset a second time
again = boot(NEW, bytes(new.uc.mem_read(SECTOR11, SECTOR_SIZE)))
again_cfg = config(again)
assert again_cfg[:new_end + 4] == new_cfg[:new_end + 4], "second boot changed the configuration"
print("second boot: configuration stable")
print("all ok")
