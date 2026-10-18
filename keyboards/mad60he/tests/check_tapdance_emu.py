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
FIXTURE = [   # (type, layer, key, ...)
    ("tap_dance", 1, TD0_A, "LEFT_BRACKET", None, "M4", None),
    ("macro", 1, M0_D, "M0"),
    ("tap_dance", 1, TD1_F, "SHIFT+9", None, "M5", None),
    ("tap_dance", 1, TD2_N, "HOME", None, "PAGE_UP", None),
    ("null_bind", 0, A, D),
]
KEYMAP = [(1, RBRC_S, "RIGHT_BRACKET")]   # (layer, key, keycode)
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
    command(SET_KEYMAP, 0, layer, key, 1, KC[kc])

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

print("all ok" if ok else "FAILED")
sys.exit(0 if ok else 1)
