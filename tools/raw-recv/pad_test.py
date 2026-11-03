#!/usr/bin/env python3
"""v0.7.20: raw-view's controller, end to end on the PC.

A fake console on 127.0.0.1:9950 says "send the controller" (header flag 128)
for 2.5 s, then stops saying it; raw-view --pad-test presses SDL's virtual
controller, keyboard and mouse on a script (pad.h). Checks that every press arrives as the
console's HidNpadButton bits, sticks with y up, keepalives while nothing
changes, and nothing at all once the console stops asking.

    python3 tools/raw-recv/pad_test.py      # needs a built raw-view
"""
import os, select, shutil, socket, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ON_SECS, TOTAL = 2.8, 4.3


def run(ini):
    """one raw-view against one fake console; ini = the viewer.ini text it starts with (None: no file)"""
    data = tempfile.mkdtemp(prefix="sft-padtest-")
    if ini is not None:
        d = os.path.join(data, "switch-frame-tap", "viewer")
        os.makedirs(d)
        with open(os.path.join(d, "viewer.ini"), "w") as f:
            f.write(ini)
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 9950))
    ls.listen(1)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy",
               XDG_DATA_HOME=data, XDG_CONFIG_HOME=data, XDG_STATE_HOME=data)
    viewer = subprocess.Popen([os.path.join(HERE, "raw-view"), "--net", "127.0.0.1", "--pad-test", "--software", "--seconds", str(TOTAL - 0.5)],
                              env=env, stdout=subprocess.DEVNULL, stderr=open(os.path.join(data, "log.txt"), "w"))
    ls.settimeout(10)
    c, _ = ls.accept()
    t0 = time.time()
    last_hello, buf, states, late, pads = 0.0, b"", set(), 0, 0
    t_first = None    # the viewer's first message: under load it can start a second or more late
    while time.time() - t0 < TOTAL:
        now = time.time()
        if now - last_hello >= 0.2:
            try:
                c.sendall(struct.pack("<IHHIIIIII", 0x52544653, 2, 128 if now - t0 < ON_SECS else 0, 0, 0, 0, 0, 0, 0))
            except OSError:
                break
            last_hello = now
        if not select.select([c], [], [], 0.05)[0]:
            continue
        try:
            d = c.recv(4096)
        except ConnectionResetError:           # the viewer closed with our hellos unread
            break
        if not d:
            break
        buf += d
        while len(buf) >= 4:
            if buf[:4] == b"SFTI":
                if len(buf) < 32:
                    break
                _, flags, buttons, lx, ly, rx, ry = struct.unpack("<4sIQiiii", buf[:32])
                buf = buf[32:]
                pads += 1
                if t_first is None:
                    t_first = time.time()
                states.add((flags, buttons, lx, ly, rx, ry))
                if time.time() - t0 > ON_SECS + 0.3:
                    late += 1
            elif buf[:3] == b"SFT":
                buf = buf[4:]
            else:
                buf = buf[1:]
    viewer.wait(timeout=10)
    c.close()
    ls.close()
    global last_log
    last_log = open(os.path.join(data, "log.txt")).read()
    shutil.rmtree(data, ignore_errors=True)
    # the keepalive is judged on the span the viewer was sending while asked
    span = (t0 + ON_SECS) - t_first if t_first is not None else 0.0
    return states, pads, late, span


def has(states, buttons, rx_min=None):
    """a state with these buttons (and, for the mouse, the right stick leaning right)"""
    return any(st[1] == buttons and (rx_min is None or st[4] >= rx_min) for st in states)


fail = False
last_log = ""

# 1. the defaults (no viewer.ini; --pad-test turns the keyboard and the mouse on)
states, pads, late, span = run(None)
want = {
    (1, 0x2, 0, 0, 0, 0): "B (the bottom button)",
    (1, 0x0, 32767, 0, 0, -32767): "left stick right, right stick down",
    (1, 0x40000, 0, 0, 0, 0): "HOME",
    (1, 0x200, 0, 0, 0, 0): "ZR (a trigger)",
    (1, 0x1, 23170, 23170, 0, 0): "the keyboard's W + D + Z (up-right on the circle, A)",
}
bad = [name for st, name in want.items() if st not in states]
if not has(states, 0x200, 1000):
    bad.append("the mouse: left button = ZR, moving right leans the right stick")
if bad:
    print("defaults missing:", ", ".join(bad), "- got", sorted(states))
    fail = True
    print(last_log[-1500:])
if span >= 0.5 and pads < 7 * span:
    print("only %d messages in the %.1f s it was asked (from its first) - the 100 ms keepalive is missing" % (pads, span))
    fail = True
if late:
    print("%d messages after the console stopped asking" % late)
    fail = True
ok1 = "%d messages, %d states" % (pads, len(states))

# 2. rebound (what the Controller menu writes): Z -> Y instead of A, the controller's bottom button -> X
#    instead of B, the mouse's left button -> A instead of ZR
ini = ("controller_to_switch=1\nkeyboard_to_switch=1\nmouse_to_switch=1\n"
       "key_a=0,0\nkey_y=29,0\npad_b=-1\npad_x=0\nmouse_btn_left=a\n")
states, pads, late, span = run(ini)
want = {
    (1, 0x4, 0, 0, 0, 0): "X (the bottom button, rebound)",
    (1, 0x8, 23170, 23170, 0, 0): "the keyboard's W + D + Z (Z rebound to Y)",
}
bad = [name for st, name in want.items() if st not in states]
if not has(states, 0x1, 1000):
    bad.append("the mouse: left button rebound to A")
unwanted = [name for st, name in {(1, 0x2, 0, 0, 0, 0): "B (unbound)", (1, 0x1, 23170, 23170, 0, 0): "A from Z (unbound)"}.items() if st in states]
if has(states, 0x200, 1000):
    unwanted.append("ZR from the mouse (rebound)")
if bad or unwanted:
    if bad:
        print("rebound missing:", ", ".join(bad))
    if unwanted:
        print("rebound still sends:", ", ".join(unwanted))
    print("got", sorted(states))
    fail = True
if fail:
    sys.exit(1)
print("OK: defaults %s; rebound %d messages" % (ok1, pads))
