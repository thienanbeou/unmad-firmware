"""Run the libhmk MAD60 HE build's key handling in Unicorn and check tap dance.

usage: python check_tapdance_emu.py firmware.elf   (raw.bin next to it)

Boots the real firmware up to USB start (ADC skipped), loads a test setup
through the firmware's own raw HID commands (as hmkconf would; see FIXTURE
below), then calls layout_task() once per simulated millisecond with chosen
key states, and records every layout_register / layout_unregister.
Expectations follow the stock (Vial) tap dance: term 190 ms, libhmk holds a
tapped key for tick_rate = 30 ms. Works with any default keymap that has
MO(1) on the stock Fn key.
"""
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmk_emu import Board  # noqa: E402
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1  # noqa: E402

REPO = Path(__file__).resolve().parents[3]       # libhmk checkout
BOARD = Path(__file__).resolve().parents[1]      # keyboards/mad60he
ELF = Path(sys.argv[1])
KEYCODES_H = REPO / "include" / "keycodes.h"
names = {}
for name, val in re.findall(r"^\s*(KC_\w+|SP_\w+)\s*=\s*(0x[0-9A-Fa-f]+)", KEYCODES_H.read_text(), re.M):
    names.setdefault(int(val, 16), name.replace("KC_", ""))
KC = {v: k for k, v in names.items()}

FN, TD0_A, RBRC_S, TD1_F, M0_D, TD2_N = 60, 29, 30, 32, 31, 47
A, D = TD0_A, M0_D   # layer 0: plain A and D, Null Bind pair (deeper key wins)
TERM, TAP = 190, 30

# Test setup, written to profile 0 over raw HID. Macros: name -> steps
# (action, keycode, delay ms); a bare keycode as a tap dance action stays
# pressed while held (the same rule as make.py's defaults).
MACROS = {
    "M0": [("press", "LEFT_SHIFT", 7), ("press", "0", 105), ("release", "0", 8), ("release", "LEFT_SHIFT", 0)],
    "M4": [("tap", "LEFT_BRACKET", 0), ("tap", "RIGHT_BRACKET", 0), ("release", "LEFT_SHIFT", 0),
           ("release", "RIGHT_SHIFT", 0), ("tap", "LEFT", 0)],
    "M5": [("press", "LEFT_SHIFT", 0), ("tap", "9", 0), ("tap", "0", 0), ("release", "LEFT_SHIFT", 0),
           ("tap", "LEFT", 0)],
    "SHIFT+9": [("press", "LEFT_SHIFT", 0), ("press", "9", 0)],
}
GESC_C, REPEAT_V, LLOCK_B = 44, 45, 46   # Fn + C, Fn + V, Fn + B
FIXTURE = [   # (type, layer, key, ...)
    ("tap_dance", 1, TD0_A, "LEFT_BRACKET", None, "M4", None),
    ("macro", 1, M0_D, "M0"),
    ("tap_dance", 1, TD1_F, "SHIFT+9", None, "M5", None),
    ("tap_dance", 1, TD2_N, "HOME", None, "PAGE_UP", None),
    ("null_bind", 0, A, D),
]
# Layer keys (v1.2) on layer 0 G H J K L ; (Q-U are the 7-key rollover test);
# ' is a probe that types F14 / F15 / F16 on layers 1 / 2 / 3; on layer 3, J
# and K lead back (TO(0), DF(0))
OSM_R, OSL_T, TO_Y, DF_U, TL_LOWER_I, TL_UPPER_O, PROBE_P = 33, 34, 35, 36, 37, 38, 39
OSM_LSHIFT, OSL_3, TO_3, DF_3, TO_0, DF_0 = 0xB9, 0xB3, 0xA3, 0xAB, 0xA0, 0xA8
KEYMAP = [(1, RBRC_S, "RIGHT_BRACKET"), (1, GESC_C, "SP_GRAVE_ESCAPE"),
          (1, REPEAT_V, "SP_REPEAT_KEY"), (1, LLOCK_B, "SP_LAYER_LOCK"),
          (0, OSM_R, OSM_LSHIFT), (0, OSL_T, OSL_3), (0, TO_Y, TO_3), (0, DF_U, DF_3),
          (0, TL_LOWER_I, "SP_TRI_LAYER_LOWER"), (0, TL_UPPER_O, "SP_TRI_LAYER_UPPER"),
          (1, PROBE_P, "F14"), (2, PROBE_P, "F15"), (3, PROBE_P, "F16"),
          (3, TO_Y, TO_0), (3, DF_U, DF_0)]   # (layer, key, keycode name or value)
SET_KEYMAP, SET_ADVANCED_KEYS, SET_MACROS = 129, 133, 141
ACTION = {"press": 1, "tap": 2, "release": 3}
MACRO_NONE, AK_TAP_DANCE, AK_MACRO, AK_NULL_BIND, NB_DISTANCE = 0xFF, 6, 5, 1, 4
bindings = json.loads((BOARD / "keyboard.json").read_text())["keyboard"].get(
    "num_dynamic_keystroke_max_bindings", 4)
AK_SIZE = 3 + 2 * bindings + 1      # advanced_key_t: layer, key, type, union (Dynamic Keystroke is largest)

b = Board(ELF)
S = b.syms
b.skip |= {S["analog_init"], S["matrix_init"], S["hid_send_reports"]}   # key states are set directly
b.stop |= {S["tud_rhport_init"]}
b.run(count=30_000_000)
assert b.reg(UC_ARM_REG_PC) == S["tud_rhport_init"], hex(b.reg(UC_ARM_REG_PC))
b.stop.discard(S["tud_rhport_init"])

CMD_BUF = 0x20018000


def command(*payload):
    """One raw HID command through command_enqueue / command_task; must succeed."""
    b.uc.mem_write(CMD_BUF, bytes(payload).ljust(64, b"\0"))
    b.run(S["command_enqueue"], count=100_000, args=(CMD_BUF, 64))
    b.run(S["command_task"], count=3_000_000)
    reply = b.uc.mem_read(S["out_buf"], 1)[0]
    b.uc.mem_write(S["command_response_pending"], b"\0")   # no USB here to send the reply
    assert reply == payload[0], f"command {payload[0]} refused"


nodes, heads = [], {}


def head(name):
    if name is None:
        return MACRO_NONE
    if name not in heads:
        heads[name] = len(nodes)
        steps = MACROS.get(name) or [("press", name, 0)]
        for i, (action, kc, delay) in enumerate(steps):
            nxt = len(nodes) + 1 if i + 1 < len(steps) else MACRO_NONE
            nodes.append(struct.pack("<BHB", KC[kc], ACTION[action] | delay << 3, nxt))
    return heads[name]


aks = []
for t, layer, key, *rest in FIXTURE:
    if t == "tap_dance":
        body = bytes([AK_TAP_DANCE, *(head(a) for a in rest)]) + struct.pack("<H", TERM)
    elif t == "macro":
        body = bytes([AK_MACRO, head(rest[0])])
    else:
        body = bytes([AK_NULL_BIND, rest[0], NB_DISTANCE, 0])
    aks.append((bytes([layer, key]) + body).ljust(AK_SIZE, b"\0"))
for i, node in enumerate(nodes):
    command(SET_MACROS, 0, *struct.pack("<H", i * 4), 4, *node)
for i, ak in enumerate(aks):
    command(SET_ADVANCED_KEYS, 0, *struct.pack("<H", i * AK_SIZE), AK_SIZE, *ak)
for layer, key, kc in KEYMAP:
    command(SET_KEYMAP, 0, layer, key, 1, KC[kc] if isinstance(kc, str) else kc)

now = [0]
events: list[tuple[int, str, str]] = []
b.calls[S["layout_register"]] = lambda bd: events.append(
    (now[0], "+", names.get(bd.reg(UC_ARM_REG_R1), hex(bd.reg(UC_ARM_REG_R1)))))
b.calls[S["layout_unregister"]] = lambda bd: events.append(
    (now[0], "-", names.get(bd.reg(UC_ARM_REG_R1), hex(bd.reg(UC_ARM_REG_R1)))))
snap = b.snapshot()
T0 = 5000


def set_options(bits):
    """Write eeconfig->options (wear-leveling cache in RAM, offset 132)."""
    b.uc.mem_write(b.r32(S["eeconfig"]) + 132, bits.to_bytes(2, "little"))


def get_options():
    return int.from_bytes(bytes(b.uc.mem_read(b.r32(S["eeconfig"]) + 132, 2)), "little")


def play(keys: dict[int, list[tuple[int, int]]], ms: int, fn=True, depth=None, options=None, probe=None) -> list[tuple[int, str, str]]:
    """keys: key -> [(down_ms, up_ms)], returns key events (layer key MO(1) left out).
    depth: key -> f(ms) giving the travel distance (0-255) while pressed (default 200)."""
    b.restore(snap)
    events.clear()
    if options is not None:
        set_options(options)
    if fn:
        keys = {FN: [(0, ms)], **keys}
    for t in range(ms):
        now[0] = t
        b.w32(S["uwTick"], T0 + t)
        for k in range(61):
            down = any(a <= t < z for a, z in keys.get(k, []))
            b.uc.mem_write(S["key_matrix"] + k * 10 + 9, bytes([down]))
            d = (depth or {}).get(k, lambda _t: 200)(t) if down else 0
            b.uc.mem_write(S["key_matrix"] + k * 10 + 6, bytes([d]))
        b.run(S["layout_task"], count=400_000, reset_sp=False)
        if probe:
            probe(t)
    return [e for e in events if e[2] not in ("0xc1",)]


def show(ev):
    return " ".join(f"{t}{s}{n}" for t, s, n in ev)


def near(t, want, slack=3):
    return want - 1 <= t <= want + slack


ok = True


def check(name, ev, cond):
    global ok
    print(("ok   " if cond else "FAIL ") + f"{name}: {show(ev)}")
    ok &= bool(cond)


# 1. single tap: "[" after the tapping term, released one tap later
ev = play({TD0_A: [(20, 60)]}, 400)
check("1 single tap -> [", ev,
      [(s, n) for _, s, n in ev] == [("+", "LEFT_BRACKET"), ("-", "LEFT_BRACKET")]
      and near(ev[0][0], 20 + TERM) and near(ev[1][0], 20 + TERM + TAP))

# 2. held past the term, no hold action: tap action held until release (Vial)
ev = play({TD0_A: [(20, 500)]}, 600)
check("2 held -> [ held until release", ev,
      [(s, n) for _, s, n in ev] == [("+", "LEFT_BRACKET"), ("-", "LEFT_BRACKET")]
      and near(ev[0][0], 20 + TERM) and near(ev[1][0], 500))

# 3. double tap: macro M4 = tap [, tap ], release shifts, tap Left
ev = play({TD0_A: [(20, 50), (90, 120)]}, 700)
keys = [(s, n) for _, s, n in ev if n in ("LEFT_BRACKET", "RIGHT_BRACKET", "LEFT")]
check("3 double tap -> M4 []<-", ev,
      keys == [("+", "LEFT_BRACKET"), ("-", "LEFT_BRACKET"), ("+", "RIGHT_BRACKET"),
               ("-", "RIGHT_BRACKET"), ("+", "LEFT"), ("-", "LEFT")]
      and near(ev[0][0], 90 + TERM))

# 4. another key interrupts: "[" right away, then the other key ("]" on Fn1)
ev = play({TD0_A: [(20, 60)], RBRC_S: [(80, 140)]}, 400)
seq = [(s, n) for _, s, n in ev]
check("4 interrupt -> [ before ]", ev,
      seq == [("+", "LEFT_BRACKET"), ("-", "LEFT_BRACKET"), ("+", "RIGHT_BRACKET"),
              ("-", "RIGHT_BRACKET")] and near(ev[0][0], 80))

# 5. a quick key press entirely inside the busy time is not lost
ev = play({TD0_A: [(20, 60)], RBRC_S: [(70, 75)]}, 400)
seq = [(s, n) for _, s, n in ev]
check("5 short press while busy is kept", ev,
      seq == [("+", "LEFT_BRACKET"), ("-", "LEFT_BRACKET"), ("+", "RIGHT_BRACKET"),
              ("-", "RIGHT_BRACKET")])

# 6. triple tap: tap three times
ev = play({TD0_A: [(20, 40), (60, 80), (100, 120)]}, 600)
check("6 triple tap -> [[[", ev,
      [(s, n) for _, s, n in ev] == [("+", "LEFT_BRACKET"), ("-", "LEFT_BRACKET")] * 3
      and near(ev[0][0], 100 + TERM)
      # a release and the next press must not share a tick (one USB report)
      and all(ev[i][0] < ev[i + 1][0] for i in range(len(ev) - 1)))

# 7. TD2 on N: double tap -> Page Up
ev = play({TD2_N: [(20, 50), (90, 120)]}, 500)
check("7 N double tap -> PgUp", ev,
      [(s, n) for _, s, n in ev] == [("+", "PAGE_UP"), ("-", "PAGE_UP")]
      and near(ev[0][0], 90 + TERM))

# 8. TD1 on F: single tap -> "(" = Shift + 9 pressed together, then released
ev = play({TD1_F: [(20, 60)]}, 400)
check("8 F tap -> Shift+9", ev,
      [(s, n) for _, s, n in ev[:2]] == [("+", "LEFT_SHIFT"), ("+", "9")]
      and sorted(n for _, s, n in ev[2:]) == ["9", "LEFT_SHIFT"] and all(s == "-" for _, s, _ in ev[2:])
      and near(ev[0][0], 20 + TERM))

# 9. macro key M0 on D: ")" with the stock timing (Shift, +7 ms 0, +105 ms up, +8 ms Shift up)
ev = play({M0_D: [(20, 400)]}, 450)
check("9 D -> M0 )", ev,
      [(s, n) for _, s, n in ev] == [("+", "LEFT_SHIFT"), ("+", "0"), ("-", "0"), ("-", "LEFT_SHIFT")]
      and near(ev[0][0], 20) and near(ev[1][0], 27) and near(ev[2][0], 132) and near(ev[3][0], 140))

# 10. without Fn the key is a plain "a"
ev = play({TD0_A: [(20, 60)]}, 200, fn=False)
check("10 no Fn -> a at once", ev,
      [(s, n) for _, s, n in ev] == [("+", "A"), ("-", "A")] and ev[0][0] == 20)

# 11. A + D null bind: the deeper key wins, and it switches when D goes deeper
ev = play({A: [(20, 300)], D: [(100, 300)]}, 350, fn=False,
          depth={A: lambda t: 100, D: lambda t: 50 if t < 200 else 150})
check("11 A+D deeper wins, switches", ev,
      [(s, n) for _, s, n in ev] == [("+", "A"), ("-", "A"), ("+", "D"), ("-", "D")]
      and ev[0][0] == 20 and near(ev[1][0], 200) and near(ev[2][0], 200) and near(ev[3][0], 300))

# 12. D pressed deeper while A is held: D takes over at once, A back after D lets go
ev = play({A: [(20, 300)], D: [(100, 200)]}, 350, fn=False,
          depth={A: lambda t: 80, D: lambda t: 200})
check("12 A held, D deeper -> D, then A", ev,
      # A comes back in the same tick D goes (one USB report), in either order
      [(s, n) for _, s, n in ev[:3]] == [("+", "A"), ("-", "A"), ("+", "D")]
      and sorted((s, n) for _, s, n in ev[3:5]) == [("+", "A"), ("-", "D")] and ev[3][0] == ev[4][0]
      and [(s, n) for _, s, n in ev[5:]] == [("-", "A")]
      and near(ev[1][0], 100) and near(ev[3][0], 200) and near(ev[5][0], 300))

# 13-16. Stock switches (MAD60 HE fork): option bits 3-5 and their keys
W, LGUI_KEY = 16, 54
OPT_WASD, OPT_GUI, OPT_6KRO = 1 << 3, 1 << 4, 1 << 5
base = None


def report_at(t_want, store):
    def probe(t):
        if t == t_want:
            store.append(bytes(b.uc.mem_read(S["kb_report"], 28)))
    return probe


b.restore(snap)
base = get_options()
ev = play({W: [(20, 60)]}, 100, fn=False, options=base | OPT_WASD)
check("13 WASD swap: W types Up", ev, [(s, n) for _, s, n in ev] == [("+", "UP"), ("-", "UP")])
ev = play({W: [(20, 60)]}, 100, fn=False, options=base)
check("13b WASD swap off: W types W", ev, [(s, n) for _, s, n in ev] == [("+", "W"), ("-", "W")])

reports = []
ev = play({LGUI_KEY: [(20, 60)]}, 100, fn=False, options=base, probe=report_at(30, reports))
ev2 = play({LGUI_KEY: [(20, 60)]}, 100, fn=False, options=base | OPT_GUI, probe=report_at(30, reports))
check("14 Win lock: GUI modifier sent only when unlocked", ev + ev2,
      reports[0][0] == 0x08 and reports[1][0] == 0x00)

reports = []
HID_A = 0x04
# The host reads only the NKRO bitmap (bytes 8-27; the descriptor declares the
# 6-key list as padding), so the key must be in the bitmap in both modes
play({TD0_A: [(20, 60)]}, 100, fn=False, options=base, probe=report_at(30, reports))
play({TD0_A: [(20, 60)]}, 100, fn=False, options=base | OPT_6KRO, probe=report_at(30, reports))
nkro, sixkro = reports
bit = lambda r, h: r[8 + h // 8] >> (h & 7) & 1
check("15 6KRO: a single key reaches the host (NKRO bitmap) in both modes", [],
      HID_A in nkro[2:8] and bit(nkro, HID_A) and HID_A in sixkro[2:8] and bit(sixkro, HID_A))

# Seven keys held (Q W E R T Y U, pressed 5 ms apart): NKRO sends all seven,
# 6KRO the first six and ignores U
ROW = range(15, 22)
HID_ROW = [0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18]
reports = []
held = {k: [(20 + 5 * i, 90)] for i, k in enumerate(ROW)}
play(held, 120, fn=False, options=base, probe=report_at(70, reports))
play(held, 120, fn=False, options=base | OPT_6KRO, probe=report_at(70, reports))
play(held, 120, fn=False, options=base | OPT_6KRO, probe=report_at(100, reports))
nkro7, six7, after = reports
check("15b 7 keys: NKRO bitmap has 7, 6KRO bitmap has the first 6 (U ignored), all released after",
      [], [bit(nkro7, h) for h in HID_ROW] == [1] * 7
      and [bit(six7, h) for h in HID_ROW] == [1] * 6 + [0]
      and sum(bin(x).count("1") for x in six7[8:]) == 6
      and not any(after[2:]))   # 6-key list cleared too (BIOS boot protocol)

b.restore(snap)
set_options(base)
seen = []
for kc, bit in ((0xF0, OPT_WASD), (0xF1, OPT_GUI), (0xF2, OPT_6KRO)):
    b.run(S["layout_register"], count=2_000_000, args=(0, kc), reset_sp=True)
    on = get_options() & bit
    b.run(S["layout_register"], count=2_000_000, args=(0, kc), reset_sp=True)
    seen.append((on == bit, get_options() & bit == 0))
check("16 SP_WASD_SWAP / SP_GUI_LOCK / SP_NKRO_TOGGLE toggle their bit on and off", [],
      all(a and b_ for a, b_ in seen) and get_options() == base)

# 17-20. v1.1 (QMK free wins): Grave Escape, Repeat Key, Mac mode, TG via Toggle.
# Checked on the keyboard report the host reads (modifiers byte 0, bitmap 8-27).
L_SHIFT, L_CTRL, L_WIN, L_ALT, KEY_1, KEY_S = 41, 53, 54, 55, 1, 30
OPT_MAC = 1 << 6
HID_ESC, HID_GRAVE, HID_A, HID_1, HID_S, HID_RBRC = 0x29, 0x35, 0x04, 0x1E, 0x16, 0x30


def report(keys, at, fn=True, options=None):
    store = []
    play(keys, max(at) + 60, fn=fn, options=options,
         probe=lambda t: store.append(bytes(b.uc.mem_read(S["kb_report"], 28))) if t in at else None)
    return store


def keys_in(r):
    return {h for h in range(160) if bit(r, h)}


bit = lambda r, h: r[8 + h // 8] >> (h & 7) & 1   # noqa: E731
# Grave Escape (Fn + C): Esc; ` with Shift or Win; Esc again with Ctrl + Shift
alone, shifted, ctrl_shift, win = (
    report({GESC_C: [(20, 60)]}, [40, 100]),
    report({L_SHIFT: [(10, 70)], GESC_C: [(20, 60)]}, [40, 100]),
    report({L_CTRL: [(5, 70)], L_SHIFT: [(10, 70)], GESC_C: [(20, 60)]}, [40]),
    # Win before Fn: with Fn held, the stock Fn layer makes that key Win lock
    report({L_WIN: [(5, 70)], FN: [(10, 70)], GESC_C: [(20, 60)]}, [40], fn=False))
check("17 Grave Escape: Esc, ` with Shift or Win, Esc with Ctrl+Shift, released after", [],
      keys_in(alone[0]) == {HID_ESC} and keys_in(alone[1]) == set()
      and keys_in(shifted[0]) == {HID_GRAVE} and keys_in(shifted[1]) == set()
      and keys_in(ctrl_shift[0]) == {HID_ESC} and keys_in(win[0]) == {HID_GRAVE})

# Repeat Key (Fn + V): the last key again; Shift + 1 repeats as Shift + 1 after
# Shift is let go, and both are released with the Repeat key
plain = report({A: [(20, 40)], FN: [(60, 120)], REPEAT_V: [(80, 100)]}, [90, 110], fn=False)
shift1 = report({L_SHIFT: [(10, 50)], KEY_1: [(20, 40)], FN: [(60, 120)], REPEAT_V: [(80, 100)]},
                [70, 90, 110], fn=False)
check("18 Repeat Key: repeats A; repeats Shift+1 with Shift; all released after", [],
      keys_in(plain[0]) == {HID_A} and keys_in(plain[1]) == set()
      and shift1[0][0] == 0 and shift1[1][0] == 0x02 and keys_in(shift1[1]) == {HID_1}
      and shift1[2][0] == 0 and keys_in(shift1[2]) == set())

# Mac mode (option bit 6): Left Alt sends GUI, Left Win sends Alt
mac_alt = report({L_ALT: [(20, 60)]}, [40], fn=False, options=base | OPT_MAC)
mac_win = report({L_WIN: [(20, 60)]}, [40], fn=False, options=base | OPT_MAC)
win_alt = report({L_ALT: [(20, 60)]}, [40], fn=False, options=base)
b.restore(snap)
set_options(base)
b.run(S["layout_register"], count=2_000_000, args=(0, KC["SP_MAC_MODE"]), reset_sp=True)
mac_on = get_options() & OPT_MAC
b.run(S["layout_register"], count=2_000_000, args=(0, KC["SP_MAC_MODE"]), reset_sp=True)
check("19 Mac mode: Alt -> GUI and Win -> Alt; normal without it; SP_MAC_MODE toggles bit 6", [],
      mac_alt[0][0] == 0x08 and mac_win[0][0] == 0x04 and win_alt[0][0] == 0x04
      and mac_on == OPT_MAC and get_options() & OPT_MAC == 0)

# QMK's TG(1) here: Layer Lock (upstream libhmk). Fn + Layer Lock keeps layer 1
# on after Fn is released (S types layer 1's ]); Fn + Layer Lock again ends it.
# (A Toggle advanced key holding MO(1) cannot do this: advanced keys do not
# fall through to lower layers, so on layer 1 the key is layer 1's keycode.)
ll = report({FN: [(10, 50), (300, 350)], LLOCK_B: [(20, 40), (310, 330)], KEY_S: [(100, 140), (400, 440)]},
            [120, 420], fn=False)
plain_s = report({KEY_S: [(100, 140)]}, [120], fn=False)
check("20 TG via Layer Lock: layer 1 stays on after Fn is released, off after the next lock", [],
      keys_in(ll[0]) == {HID_RBRC} and keys_in(ll[1]) == {HID_S} and keys_in(plain_s[0]) == {HID_S})

# Layer keys (v1.2). ' types F14 / F15 / F16 on layers 1 / 2 / 3, ' on 0.
def probes(ev):
    return [n for _, s, n in ev if s == "+" and n in ("QUOTE", "F14", "F15", "F16")]


# One Shot Shift (G): tapped, Shift waits for the next key and goes with its
# release; held while 1 is pressed, it is a plain Shift; then nothing is armed
HID_SHIFT = 0x02
tap = report({OSM_R: [(20, 40)], KEY_1: [(80, 120)]}, [60, 100, 140], fn=False)
held = report({OSM_R: [(20, 120)], KEY_1: [(50, 80)], PROBE_P: [(150, 170)]}, [60, 140, 160], fn=False)
check("22 One Shot Shift: tap arms it for the next key, released with it; held = plain Shift", [],
      tap[0][0] == HID_SHIFT and tap[1][0] == HID_SHIFT and keys_in(tap[1]) == {HID_1}
      and tap[2][0] == 0 and keys_in(tap[2]) == set()
      and held[0][0] == HID_SHIFT and keys_in(held[0]) == {HID_1} and held[1][0] == 0
      and held[2][0] == 0)

# One Shot layer 3 (H): tapped, only the next key uses layer 3; held, every
# key pressed meanwhile does, and the layer goes with the key
osl_tap = play({OSL_T: [(20, 40)], PROBE_P: [(80, 100), (130, 150)]}, 200, fn=False)
osl_held = play({OSL_T: [(20, 150)], PROBE_P: [(50, 70), (100, 120), (170, 190)]}, 230, fn=False)
check("23 One Shot layer: tap -> next key on layer 3 only; held -> like MO(3)", osl_tap + osl_held,
      probes(osl_tap) == ["F16", "QUOTE"] and probes(osl_held) == ["F16", "F16", "QUOTE"])

# TO(3) (J) stays on layer 3 until TO(0) (layer 3's J); DF(3) (K) makes layer
# 3 the default until DF(0)
to = play({TO_Y: [(20, 40), (140, 160)], PROBE_P: [(60, 80), (100, 120), (180, 200)]}, 240, fn=False)
df = play({DF_U: [(20, 40), (140, 160)], PROBE_P: [(60, 80), (100, 120), (180, 200)]}, 240, fn=False)
check("24 TO / DF: layer 3 stays until TO(0) / DF(0)", to + df,
      probes(to) == ["F16", "F16", "QUOTE"] and probes(df) == ["F16", "F16", "QUOTE"])

# Tri Layer: Lower (L) = layer 1, Upper (;) = layer 2, both = layer 3
tri = play({TL_LOWER_I: [(20, 200)], TL_UPPER_O: [(60, 140), (230, 280)],
            PROBE_P: [(30, 50), (90, 110), (160, 180), (240, 260), (300, 320)]}, 340, fn=False)
check("25 Tri Layer: Lower -> 1, Lower + Upper -> 3, Upper -> 2, none -> 0", tri,
      probes(tri) == ["F14", "F16", "F14", "F15", "QUOTE"])

# Raw HID hands every lighting command to rgb_command (commands.c lists them
# one by one; white balance once answered "unknown" on the board for that)
for c in (0xE0, 0xE2, 0xE7, 0xE9, 0xEB):
    command(c)
command(0xEA, 255, 255, 255)
command(0xEC, 170, 255, 210)
check("21 raw HID routes the lighting commands (GET_RGB, key colours, Caps, white balance, layer colour)", [], True)


# Profiles and layers in use (v1.4): 4 and 6 at first, the configurator adds
# more up to 8; out of range refused; fewer profiles than the current one's
# index go back to profile 0 (current_profile sits at offset 134)
def reply(*payload):
    b.uc.mem_write(CMD_BUF, bytes(payload).ljust(64, b"\0"))
    b.run(S["command_enqueue"], count=100_000, args=(CMD_BUF, 64))
    b.run(S["command_task"], count=3_000_000)
    b.uc.mem_write(S["command_response_pending"], b"\0")
    return bytes(b.uc.mem_read(S["out_buf"], 3))


first = reply(0xED)
refused = [reply(0xEE, *v)[0] for v in ((0, 6), (4, 0), (9, 6), (4, 9))]
grown = reply(0xEE, 8, 8)[0] == 0xEE and reply(0xED)[1:] == bytes([8, 8])


def keycode(profile, layer, key):
    return reply(128, profile, layer, key)[1]


def ak_slot(profile, slot):
    out = reply(132, profile, (slot * 12) & 0xFF, (slot * 12) >> 8)
    return bytes(b.uc.mem_read(S["out_buf"] + 2, 12))


# Something to clear: a key on layer 7, a toggle key on layer 7 (last slot),
# a key on profile 5; and something to keep on layer 0 / profile 0
default_p5 = keycode(5, 0, 0)
command(129, 0, 7, 0, 1, KC["A"])
command(129, 0, 0, 1, 1, KC["B"])
command(129, 5, 0, 0, 1, KC["C"])
command(133, 0, (39 * 12) & 0xFF, (39 * 12) >> 8, 12, 7, 5, 4, KC["D"], 200, 0, 0, 0, 0, 0, 0, 0)
staged = ak_slot(0, 39)[:3] == bytes([7, 5, 4]) and keycode(0, 7, 0) == KC["A"]
b.uc.mem_write(b.r32(S["eeconfig"]) + 134, b"\x05")
shrunk = reply(0xEE, 4, 6)[0] == 0xEE and reply(0xED)[1:] == bytes([4, 6])
profile = b.uc.mem_read(b.r32(S["eeconfig"]) + 134, 1)[0]
cleared = (keycode(0, 7, 0) == 1 and ak_slot(0, 39) == bytes(12) and keycode(5, 0, 0) == default_p5
           and keycode(0, 0, 1) == KC["B"])
check(f"26 in use: first {list(first[1:])}, out of range refused, 8 x 8 set, back to 4 x 6 -> profile {profile}, "
      f"layer 7 key / advanced key and profile 5 cleared, layer 0 kept", [],
      first[1:] == bytes([4, 6]) and refused == [0xFF] * 4 and grown and staged and shrunk and profile == 0
      and cleared)

print("all ok" if ok else "FAILED")
sys.exit(0 if ok else 1)
