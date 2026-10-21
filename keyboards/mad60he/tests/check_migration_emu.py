"""Check the libhmk eeconfig migration between two MAD60 HE builds in Unicorn.

usage: python check_migration_emu.py OLD.elf NEW.elf   (raw.bin next to each)

Boots the old build on blank flash (it writes its default configuration to
sector 11), adds a tap dance and a macro to each old profile through the old
build's own wear-leveling log, copies sector 11 into the new build and boots
it. The new build must migrate instead of resetting: global settings and every
profile byte kept, version bumped to v1.12, the lighting block filled in (the
old block kept or the config.h defaults, `hue_range` 255, the Caps Lock
colour, the white balance and the layer indicator colours appended), the
profiles and layers in use set (4 and 6), each old profile moved to the new
counts (8 layers, 40 advanced keys, 160 macro nodes; new layers transparent,
new slots empty), profiles 4-7 the defaults, magic numbers intact, and the
result written back to sector 11. Builds up to v1.3 keep the settings in an
8 KB wear-leveling store; v1.4 reads that once (WL_LEGACY_VIRTUAL_SIZE).
OLD may be a v1.5 to v1.11 build.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmk_emu import Board  # noqa: E402

BOARD = Path(__file__).resolve().parents[1]      # keyboards/mad60he
OLD, NEW = Path(sys.argv[1]), Path(sys.argv[2])
SECTOR11, SECTOR_SIZE = 0x080E0000, 0x20000
MAGIC_END = struct.pack("<I", 0x0A4B4D48)          # EECONFIG_MAGIC_END
CONFIG_H = (BOARD / "config.h").read_text()
NUM_KEYS = 61
OLD_PROFILES, OLD_LAYERS, OLD_AKS, OLD_NODES = 4, 4, 32, 128
NEW_PROFILES, NEW_LAYERS, NEW_AKS, NEW_NODES = 8, 8, 40, 160
AK, NODE, TAIL = 12, 4, NUM_KEYS + 9 + 1           # gamepad buttons, options, tick rate
BUF = 0x20010000


def profile_size(layers, aks, nodes):
    return layers * NUM_KEYS + NUM_KEYS * 4 + aks * AK + nodes * NODE + TAIL


OLD_P = profile_size(OLD_LAYERS, OLD_AKS, OLD_NODES)
NEW_P = profile_size(NEW_LAYERS, NEW_AKS, NEW_NODES)


def boot(elf, sector11=None):
    b = Board(elf)
    if sector11 is not None:
        b.uc.mem_write(SECTOR11, sector11)
    b.run(None, 8_000_000)
    return b


def config(b):
    """The live configuration (eeconfig points into the wear-leveling cache)."""
    return bytes(b.uc.mem_read(b.r32(b.syms["eeconfig"]), 0x4000))


old = boot(OLD)
old_cfg = config(old)
old_version = struct.unpack_from("<H", old_cfg, 4)[0]
assert old_version in range(0x0105, 0x010C), f"old build stores version {old_version:#x}"

GLOBAL_V15 = 14 + NUM_KEYS * 2                    # MIGRATION_V1_5_GLOBAL_CONFIG_SIZE
RGB_V16 = 5 + NUM_KEYS * 3
RGB_V17 = RGB_V16 + 1                              # + hue_range (v1.7)
RGB_V18 = RGB_V17 + 3                              # + Caps Lock hue, sat, val (v1.8)
RGB_V19 = RGB_V18 + 3                              # + white balance R, G, B (v1.9)
RGB_V110 = RGB_V19 + 3                             # + layer 1 indicator colour (v1.10)
RGB_V111 = RGB_V110 + 6                            # + layers 2, 3 (v1.11)
RGB_SIZE = RGB_V111 + 12                           # + layers 4-7 (v1.12)
OLD_RGB = {0x0105: 0, 0x0106: RGB_V16, 0x0107: RGB_V17, 0x0108: RGB_V18, 0x0109: RGB_V19,
           0x010A: RGB_V110, 0x010B: RGB_V111}[old_version]
OLD_GLOBAL = GLOBAL_V15 + OLD_RGB
NEW_GLOBAL = GLOBAL_V15 + RGB_SIZE + 2             # + profiles and layers in use

# A tap dance (slot 0) and a macro (slot 1) in every old profile, written
# through the old build's wear-leveling log (so the new build replays the
# legacy log): different per profile, and in the last advanced key / macro
# node slots too, so a shifted copy would show
for p in range(OLD_PROFILES):
    base = OLD_GLOBAL + p * OLD_P
    ak_at = base + OLD_LAYERS * NUM_KEYS + NUM_KEYS * 4
    node_at = ak_at + OLD_AKS * AK
    aks = bytes([1, 29, 6, 0, 1, 0xFF, 0xFF, 200 + p, 0, 0, 0, 0,
                 1, 31, 5, 2, 0, 0, 0, 0, 0, 0, 0, 0])
    nodes = bytes([4 + p, 0x02, 0, 1, 5, 0x02, 0, 0xFF, 6, 0x02, 0, 0xFF])
    last_ak = bytes([2, 40, 3, 4 + p, 5, 200, 0, 1, 0, 0, 0, 0])
    last_node = bytes([7 + p, 0x02, 0, 0xFF])
    for at, data in ((ak_at, aks), (node_at, nodes),
                     (ak_at + (OLD_AKS - 1) * AK, last_ak), (node_at + (OLD_NODES - 1) * NODE, last_node)):
        old.uc.mem_write(BUF, data)
        old.run(old.syms["wear_leveling_write"], 3_000_000, (at, BUF, len(data)))
old_cfg = config(old)
assert old_cfg[OLD_GLOBAL + OLD_LAYERS * NUM_KEYS + NUM_KEYS * 4 + 2] == 6, "old write did not land"
sector = bytes(old.uc.mem_read(SECTOR11, SECTOR_SIZE))
old_end = old_cfg.index(MAGIC_END)
assert old_end == OLD_GLOBAL + OLD_PROFILES * OLD_P, f"old layout {old_end}"

new = boot(NEW, sector)
new_cfg = config(new)
assert new_cfg[:4] == old_cfg[:4], "magic_start"
new_version = struct.unpack_from("<H", new_cfg, 4)[0]
assert new_version == 0x010C, f"new build did not migrate (version {new_version:#x})"
new_end = new_cfg.index(MAGIC_END)
assert new_end == NEW_GLOBAL + NEW_PROFILES * NEW_P, f"new layout {new_end}"

# Global block (magic, version aside) and lighting
assert new_cfg[6:GLOBAL_V15] == old_cfg[6:GLOBAL_V15], "global settings changed"
cfg = dict(re.findall(r"#define (RGB_DEFAULT_\w+) (\S+)", CONFIG_H))
val = int(cfg["RGB_DEFAULT_VAL"])
rgb = new_cfg[GLOBAL_V15:GLOBAL_V15 + RGB_SIZE]
assert cfg["RGB_DEFAULT_EFFECT"] == "RGB_EFFECT_RAINBOW_PINWHEELS"   # = 13
if OLD_RGB:
    assert rgb[:OLD_RGB] == old_cfg[GLOBAL_V15:OLD_GLOBAL], "old lighting changed"
else:
    want = [13, int(cfg["RGB_DEFAULT_HUE"]), int(cfg["RGB_DEFAULT_SAT"]), val, int(cfg["RGB_DEFAULT_SPEED"])]
    assert list(rgb[:5]) == want and rgb[5:RGB_V16] == bytes(NUM_KEYS * 3), f"lighting block {list(rgb[:5])}"
if old_version < 0x0107:
    assert rgb[RGB_V16] == 255, f"hue_range {rgb[RGB_V16]}"
if old_version < 0x0108:
    assert list(rgb[RGB_V17:RGB_V18]) == [0, 0, val], f"Caps Lock colour {list(rgb[RGB_V17:RGB_V18])}"
if old_version < 0x0109:
    assert list(rgb[RGB_V18:RGB_V19]) == [255, 255, 255], f"white balance {list(rgb[RGB_V18:RGB_V19])}"
hues = [170, 85, 0, 43, 128, 213, 21]              # layers 1-7: blue, green, red, yellow, cyan, magenta, orange
layers = [x for h in hues for x in (h, 255, val)]
start = max(OLD_RGB, RGB_V19)
assert list(rgb[start:]) == layers[start - RGB_V19:], f"layer colours {list(rgb[RGB_V19:])}"
in_use = list(new_cfg[GLOBAL_V15 + RGB_SIZE:NEW_GLOBAL])
assert in_use == [4, 6], f"profiles / layers in use {in_use}"

# Profiles 0-3: every old byte, the new layers transparent, the new slots empty
for p in range(OLD_PROFILES):
    o = old_cfg[OLD_GLOBAL + p * OLD_P:OLD_GLOBAL + (p + 1) * OLD_P]
    n = new_cfg[NEW_GLOBAL + p * NEW_P:NEW_GLOBAL + (p + 1) * NEW_P]
    oi = ni = 0

    def take(o_len, n_len, what):
        global oi, ni
        assert n[ni:ni + o_len] == o[oi:oi + o_len], f"profile {p}: {what} changed"
        extra = n[ni + o_len:ni + n_len]
        oi, ni = oi + o_len, ni + n_len
        return extra

    assert take(OLD_LAYERS * NUM_KEYS, NEW_LAYERS * NUM_KEYS, "keymap") == bytes([1]) * (4 * NUM_KEYS), \
        f"profile {p}: new layers not transparent"
    take(NUM_KEYS * 4, NUM_KEYS * 4, "actuation")
    assert take(OLD_AKS * AK, NEW_AKS * AK, "advanced keys") == bytes(8 * AK), f"profile {p}: new slots not empty"
    assert take(OLD_NODES * NODE, NEW_NODES * NODE, "macro nodes") == bytes([0, 0, 0, 0xFF]) * 32, \
        f"profile {p}: new nodes not empty"
    take(TAIL, TAIL, "gamepad / tick rate")
    assert n[NEW_LAYERS * NUM_KEYS + NUM_KEYS * 4 + 2] == 6 and n[NEW_LAYERS * NUM_KEYS + NUM_KEYS * 4 + 7] == 200 + p, \
        f"profile {p}: tap dance not carried"

# Profiles 4-7: the defaults (= a fresh v1.12 boot's)
fresh = config(boot(NEW))
for p in range(OLD_PROFILES, NEW_PROFILES):
    at = NEW_GLOBAL + p * NEW_P
    assert new_cfg[at:at + NEW_P] == fresh[at:at + NEW_P], f"profile {p} not the defaults"
assert list(fresh[GLOBAL_V15 + RGB_SIZE:NEW_GLOBAL]) == [4, 6], "fresh in use"

writes = [a for a in new.flash_writes if SECTOR11 <= a < SECTOR11 + SECTOR_SIZE]
assert writes, "migration not written to flash"
assert all(a >= SECTOR11 for a in new.flash_writes), "flash write below sector 11"
print(f"migration v{old_version >> 8}.{old_version & 255} -> v1.12: {old_end} -> {new_end} bytes "
      f"(8 KB legacy store -> 16 KB), global settings and the {OLD_PROFILES} old profiles kept "
      f"(tap dance / macro / last slots written through the old log), new layers transparent, "
      f"new advanced key / macro slots empty, profiles 4-7 default, lighting "
      f"{'kept' if OLD_RGB else 'defaults'} + layer colours, in use 4 x 6, {len(writes)} flash writes in sector 11")

# Booting the migrated flash again must not migrate or reset a second time
again = boot(NEW, bytes(new.uc.mem_read(SECTOR11, SECTOR_SIZE)))
assert config(again)[:new_end + 4] == new_cfg[:new_end + 4], "second boot changed the configuration"
print("second boot: configuration stable")
print("all ok")
