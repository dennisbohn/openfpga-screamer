#!/usr/bin/env python3
"""
dosram.py - reads the original game's memory out of a running DOSBox, the way a cheat tool does.

Windows only: the script calls kernel32 and user32 through ctypes.
It finds the DOSBox process and looks through its memory for the game's data object, a stretch of
STARTL.EXE's own bytes. It checks the copy it found with the loader's relocations: the words that
the LE fixups point into the data object hold the stored value plus the object's linear base, the
same base at every one. After that, any address of the data object can be read live, written as
openscreamer docs/NOTES.md and tools/dis.sh write it (0x100000 + the ds offset), while the game
runs or stands paused (P).

Safety: only a DOSBox that this script started (race, shots, walk) or that the process DOSBOX_PID
names is ever read or keyed. The first DOSBox it finds is never taken, and keys stop at once when
the window in front is not that DOSBox.

Usage: python -I tools/dosram.py <command> [...]
    race [conf]         starts DOSBox itself and the race (Enter through the menus), then takes the
                        screenshot on the grid and pauses. conf is a DOSBox configuration file; by default
                        the script writes build/dosrace.conf. VIEW_KEY in the environment is the scan
                        code of a view key (F1..F4 = 0x3b..0x3e) that is pressed on the grid first.
    shots               as race, then the grid's picture in the game's four views (F1 .. F4). For each
                        view it prints the camera as tools/render.sh's CAM and takes a raw screenshot
                        into build/dosbox. The game's car stands.
    walk n [secs ...]   as race, then the race goes on for secs seconds with the gas held, is paused
                        and read (the camera, the cell window, the pieces in reach, the
                        level-of-detail constants), and again for the next secs. The result is
                        build/dosbox/walk<n>.json. DOSBox is closed at the end.
    cam                 prints the camera record (ds:0x10a1c)
    shot [s] [delay]    waits (up to s seconds) until the paused game runs again, then sends Ctrl+F5
    list                prints the pieces in reach this frame (the list of original: STARTL.EXE
                        0x83164), with their records
    dump <addr> <n>     prints n bytes from a flat address, as words, ints and floats
    watch <addr> <n>    the same every 0.2 s until Ctrl+C, with the changed words marked
Needs: the game's folder ($GAME_DIR, default "C:/GOG Games/Screamer") with its DOSBox, and the flat
images of STARTL.EXE in build/exe/ (tools/unle.py makes them; tools/dis.sh does it once).
"""
import ctypes, ctypes.wintypes as wt, json, struct, subprocess, sys, time, os

sys.stdout.reconfigure(encoding="utf-8")
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA, DATA_END = 0x100000, 0x1472C0

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


class KI(ctypes.Structure):
    _fields_ = [("vk", wt.WORD), ("sc", wt.WORD), ("fl", wt.DWORD), ("time", wt.DWORD),
                ("ex", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("k", KI), ("pad", ctypes.c_byte * 8)]


MY_PID = None       # the pid of the DOSBox this script started


def key(sc, up, ext=False):
    """Sends a key (press, or release if up) by its scan code to the window in front, as the keyboard does.

    The script stops without sending if the window in front is not our DOSBox."""
    u = ctypes.WinDLL("user32")
    p = wt.DWORD()
    u.GetWindowThreadProcessId(u.GetForegroundWindow(), ctypes.byref(p))
    if MY_PID and p.value != MY_PID:
        sys.exit("the window in front is not our DOSBox (pid %d): no key sent" % MY_PID)
    i = INPUT(1, KI(0, sc, 8 | (2 if up else 0) | (1 if ext else 0), 0, 0))
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(i))


def front(pid):
    """Brings the window of the DOSBox with this pid to the front, so that the keys go there."""
    u = ctypes.WinDLL("user32")
    found = []
    cb = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def each(h, _):
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(h):
            found.append(h)
        return True
    u.EnumWindows(cb(each), 0)
    for h in found:
        u.keybd_event(0x12, 0, 0, 0)        # Alt, which lets a background program take the front
        u.SetForegroundWindow(h)
        u.keybd_event(0x12, 0, 2, 0)
    time.sleep(0.2)


def dosbox_pid():
    """The pid of our DOSBox (the one that race started) or DOSBOX_PID's, if that process is a DOSBox.

    No other process is ever taken."""
    pid = MY_PID or int(os.environ.get("DOSBOX_PID", "0"))
    if not pid:
        sys.exit("no DOSBox of ours: start one with race or walk, or name the process in DOSBOX_PID")
    out = subprocess.run(["tasklist", "/fo", "csv", "/nh", "/fi", "PID eq %d" % pid],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = [x.strip('"') for x in line.split('","')]
        if len(f) > 1 and "dosbox" in f[0].lower() and int(f[1]) == pid:
            return pid
    sys.exit("process %d is not a running DOSBox" % pid)


class Proc:
    """A process opened for reading its memory."""
    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(0x0410, False, pid)    # QUERY_INFORMATION | VM_READ
        if not self.h:
            sys.exit("cannot open DOSBox (error %d)" % ctypes.get_last_error())

    def read(self, addr, n):
        """n bytes at the host address addr, or None if they cannot be read."""
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, addr, buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    def regions(self):
        """Yields (address, size) of the readable memory regions of the process."""
        a, m = 0, MBI()
        while k32.VirtualQueryEx(self.h, a, ctypes.byref(m), ctypes.sizeof(m)):
            if m.State == 0x1000 and m.Protect & 0xEE and not m.Protect & 0x100:
                yield m.BaseAddress or 0, m.RegionSize
            a = (m.BaseAddress or 0) + m.RegionSize


# The flat address ranges of the two objects, code (1) and data (3), as tools/unle.py lays them out.
OBJS = {1: (0x10000, 0xEB7A3), 3: (DATA, DATA_END)}


def find_obj(p, img, fix, lo, hi):
    """Finds an object of the game in DOSBox's memory.

    Returns the host address of the object's first byte and its linear base in the guest. img is the
    flat image of STARTL.EXE and fix the same with the fixups applied; lo and hi are the object's
    flat range."""
    sites = [i for i in range(lo, hi - 4) if img[i:i + 4] != fix[i:i + 4]
             and (struct.unpack_from("<I", fix, i)[0] - struct.unpack_from("<I", img, i)[0]) == lo]
    # The signature to search for: 64 bytes of the object that no fixup touches, text if there is some.
    ok = lambda i: not any(i - 4 < s < i + 64 for s in sites)
    sig_at = next((i for i in range(lo, hi - 64, 16)
                   if sum(32 <= c < 127 for c in img[i:i + 64]) > 56 and ok(i)), None)
    if sig_at is None:
        sig_at = next(i for i in range(lo + 0x1000, hi - 64, 16)
                      if len(set(img[i:i + 64])) > 40 and ok(i))
    sig = img[sig_at:sig_at + 64]
    for base, size in p.regions():
        for off in range(0, size, 1 << 24):
            chunk = p.read(base + off, min(1 << 24, size - off) + 64)
            if not chunk:
                continue
            j = chunk.find(sig)
            while j >= 0:
                host = base + off + j - (sig_at - lo)
                lin = set()
                for s in sites[::max(1, len(sites) // 12)]:
                    w = p.read(host + s - lo, 4)
                    if w:
                        lin.add(struct.unpack("<I", w)[0] - struct.unpack_from("<I", img, s)[0])
                if len(lin) == 1:
                    return host, lin.pop()
                j = chunk.find(sig, j + 1)
    sys.exit("the game's object at 0x%x is not in DOSBox's memory (is the game running?)" % lo)


class Game:
    """The game's memory in DOSBox: reads by flat address (openscreamer docs/NOTES.md) or by guest pointer."""
    def __init__(self, p):
        img = open(os.path.join(root, "build/exe/startl.bin"), "rb").read()
        fix = open(os.path.join(root, "build/exe/startl_fix.bin"), "rb").read()
        self.p, self.obj = p, {k: find_obj(p, img, fix, lo, hi) for k, (lo, hi) in OBJS.items()}
        self.mem = {h - l for h, l in self.obj.values()}    # the host address of the guest memory
        if len(self.mem) != 1:
            sys.exit("the objects do not sit in one guest memory (paging?)")
        self.mem = self.mem.pop()

    def flat(self, addr, n):
        """n bytes at a flat address of the code or data object."""
        for k, (lo, hi) in OBJS.items():
            if lo <= addr < hi:
                return self.p.read(self.obj[k][0] + addr - lo, n)
        sys.exit("0x%x is in no object" % addr)

    def ptr(self, lin, n):
        """n bytes at a guest pointer (a linear address)."""
        return self.p.read(self.mem + lin, n)

    def to_flat(self, lin):
        """A guest pointer as a flat address, or None if it points into neither object."""
        for k, (lo, hi) in OBJS.items():
            if 0 <= lin - self.obj[k][1] < hi - lo:
                return lin - self.obj[k][1] + lo
        return None


def show(b, addr, mark=None):
    """Prints the bytes b, which start at flat address addr, as words, ints and floats.

    The rows at the byte offsets in mark are marked with a star."""
    for i in range(0, len(b) - 3, 4):
        u = struct.unpack_from("<I", b, i)[0]
        f = struct.unpack_from("<f", b, i)[0]
        fs = "%.4f" % f if 1e-6 < abs(f) < 1e9 else ""
        print("%s%06x  %08x %12d %14s" % ("*" if mark and i in mark else " ", addr + i, u,
                                          struct.unpack_from("<i", b, i)[0], fs))


def race(conf, view=None):
    """Starts our DOSBox and brings the race to the grid.

    DOSBox starts on its own straight into STARTL.EXE (conf is a configuration with an [autoexec]
    that runs it). The script presses Enter until the race stands on the grid, with the player's car
    at its start place. Then it takes the screenshot at once, together with the animation clock, and
    the race is paused (P). view is the scan code of a view key (F1..F4 = 0x3b..0x3e, the game's four
    driving views) that is pressed on the grid first; the picture is then taken once the camera's
    fly-across has died away."""
    pressed = view is None
    d = os.environ.get("GAME_DIR", "C:/GOG Games/Screamer")
    cap = os.path.join(root, "build", "dosbox")         # DOSBox runs here; screenshots stay in build/
    os.makedirs(cap, exist_ok=True)
    if conf is None:
        conf = os.path.join(root, "build", "dosrace.conf")
        open(conf, "w").write('[sdl]\nfullscreen = false\nwindowresolution = 640x400\n[dosbox]\n'
                              'captures = %s\n'
                              '[capture]\ncapture_dir = %s\ndefault_image_capture_formats = raw\n'
                              '[autoexec]\n@ECHO OFF\nmount C "%s"\n'
                              'imgmount d "%s/scream.dat" -t iso -fs iso\n'
                              'c:\nSTARTL.EXE\nexit\n' % (cap, cap, d, d))
    global MY_PID
    MY_PID = subprocess.Popen([d + "/DOSBOX/DOSBox.exe", "-conf", d + "/dosboxScreamer.conf", "-conf", conf,
                               "-noconsole"], cwd=cap).pid
    end = time.time() + 120
    while time.time() < end:
        time.sleep(1.5)
        try:
            g = Game(Proc(dosbox_pid()))
        except SystemExit:
            continue                        # not loaded yet
        while time.time() < end:
            x, z = struct.unpack("<ii", g.flat(0x10fdc8, 4) + g.flat(0x10fdd0, 4))
            c0, cam0 = g.flat(0x121538, 4), g.flat(0x110a1c, 0x3c)
            time.sleep(0.2)
            # The race runs (the clock moves) and the camera's swing onto the car is over (it stands).
            if (x, z) == (2600, -18500) and g.flat(0x121538, 4) != c0 and g.flat(0x110a1c, 0x3c) == cam0:
                front(g.p.pid)
                cx, ch, cz = struct.unpack_from("<3i", cam0, 0)
                cpitch, cyaw, croll = struct.unpack_from("<3f", cam0, 0x30)
                print('CAM="%d %d %d %r %r %r"' % (cx, cz, ch, cyaw, cpitch, croll))  # for CAM=
                if not pressed:
                    key(view, 0); time.sleep(0.1); key(view, 1)
                    pressed = True
                    time.sleep(0.5)
                    continue
                for sc, up in ((0x1D, 0), (0x3F, 0), (0x3F, 1), (0x1D, 1)):
                    key(sc, up)
                    time.sleep(0.05)
                c1 = g.flat(0x121538, 4)
                time.sleep(0.3)
                key(0x19, 0); time.sleep(0.1); key(0x19, 1)
                print("on the grid: Ctrl+F5, then P; animation clock %d .. %d"
                      % (struct.unpack("<I", c0)[0], struct.unpack("<I", c1)[0]))
                return
            if (x, z) == (2600, -18500):
                continue                    # on the grid, the camera still swinging in
            front(g.p.pid)
            key(0x1C, 0); time.sleep(0.1); key(0x1C, 1)     # Enter
            time.sleep(1.0)
    sys.exit("no race within 2 minutes")


def dump(g, label):
    """Reads what the standing frame (the game paused) walked and drew, as a dictionary for the json.

    It holds:
    the camera, as the view structure V (at 0x11099c) that the game renders with and as the camera
    record C; the cell window (original: STARTL.EXE 0x82e88): the window before and after the camera
    cell's view box, and the box itself, all in the map's coordinates + 2^20 where the game adds it;
    the columns and rows of cells that it visits; the pieces in reach (the list of original: STARTL.EXE
    0x83164, then the moving objects); the level-of-detail constants (0x8a504, 0x8a508, 0x8a550,
    0x8a554); the reach and the far plane as they stand in the code; and the DETAIL."""
    dw = lambda a: struct.unpack("<i", g.flat(a, 4))[0]
    # pitch, heading, roll (2048 = a turn), x, height, z
    v = struct.unpack("<3i3i", g.flat(0x11099c, 0x18))
    c = g.flat(0x110a1c, 0x40)
    out = {"label": label, "view": list(v), "cam": list(struct.unpack_from("<3i", c, 0)),
           "cam_angles": list(struct.unpack_from("<3f", c, 0x30)),
           # x0, z0, x1, z1 (map coordinates)
           "window": [dw(0x82b54), dw(0x82b5c), dw(0x82b60), dw(0x82b68)],
           # the same cut to the box, + 2^20
           "rect": [dw(0x82b9c), dw(0x82ba4), dw(0x82ba8), dw(0x82bb0)],
           # the camera cell's box + 2^20 (+-2e9: none)
           "box": [dw(0x82b6c), dw(0x82b74), dw(0x82b78), dw(0x82b80)],
           "col0x4": dw(0x83137), "ncol": dw(0x83141),
           "lod": [dw(0x8a504), dw(0x8a508), dw(0x8a550), dw(0x8a554)],
           # dist^2 >> 2 < K in the code: K = 676000000 = 0x284af100 each
           "reach_immediates": [dw(0x831cc), dw(0x83230), dw(0x8a70f)],
           # the far plane that the faces are cut at (-43808 at HI)
           "far": dw(0xd7a20),
           "detail": struct.unpack("<i", g.ptr(struct.unpack("<I", g.flat(0x102de8, 4))[0] + 8, 4))[0]}
    n = dw(0x82248)
    lst = g.flat(0x500f0, 8 * n)
    pieces = []
    for i in range(n):
        d, rec = struct.unpack_from("<Ii", lst, 8 * i)
        w = struct.unpack("<16i", g.ptr(rec & 0x3fffffff, 0x40))
        pieces.append({"moving": bool(rec & 0x40000000), "d2q": d, "obj": w[0],
                       "x": w[2], "h": w[3], "z": w[4], "t": w[6]})
    out["reach"] = pieces
    return out


def set_pause(g, want):
    """Puts the game into the paused (want true) or running state, whatever state it is in.

    P pauses (the CD player's screen); Esc leaves it (the game's ESC key, original: STARTL.EXE 0x211d0).
    The animation clock (0x121538) stands still in the pause, the timer does not. That tells the
    two states apart."""
    for _ in range(4):
        c = g.flat(0x121538, 4)
        time.sleep(0.4)
        stands = g.flat(0x121538, 4) == c
        if stands == want:
            return
        front(MY_PID)
        sc = 0x19 if want else 0x01
        key(sc, 0); time.sleep(0.1); key(sc, 1)
        time.sleep(0.5)
    sys.exit("the game would not " + ("pause" if want else "run"))


def walk(n, secs):
    """Brings the race to the grid, then pauses (P) and dumps after each of secs seconds of gas.

    The dumps go to build/dosbox/walk<n>.json."""
    dumps = []
    ext = os.environ.get("GAS_EXT", "1") == "1"
    try:
        race(None)
        g = Game(Proc(dosbox_pid()))
        set_pause(g, True)
        dumps.append(dump(g, "grid"))
        for k, s in enumerate(secs):
            set_pause(g, False)
            front(MY_PID)
            key(0x48, 0, ext)                                       # Up: the gas
            end = time.time() + s
            while time.time() < end:                                # a line every two seconds: the car goes
                time.sleep(2.0)
                v = struct.unpack("<3i", g.flat(0x1109a8, 12))
                print("  clock %d  camera %s" % (struct.unpack("<I", g.flat(0x121538, 4))[0], v), flush=True)
            front(MY_PID)
            key(0x48, 1, ext)
            set_pause(g, True)
            dumps.append(dump(g, "after %.0f s" % sum(secs[:k + 1])))
            print("dump %d: camera %s, %d pieces in reach"
                  % (k + 1, dumps[-1]["view"][3:], len(dumps[-1]["reach"])), flush=True)
    finally:
        if MY_PID:                                                  # our DOSBox, nothing else
            subprocess.run(["taskkill", "/PID", str(MY_PID), "/T", "/F"], capture_output=True)
    os.makedirs(os.path.join(root, "build", "dosbox"), exist_ok=True)
    path = os.path.join(root, "build", "dosbox", "walk%d.json" % n)
    json.dump(dumps, open(path, "w"))
    print("written", path)


def shots():
    """Takes the grid's picture in the game's four views.

    First race (the default view, screenshot, paused). Then the game runs on with the car standing
    (no gas) and each of F1..F4 is pressed. For each view the script waits until the camera stands,
    prints the camera record as tools/render.sh's CAM and takes the screenshot (Ctrl+F5, raw 320 x 200
    in build/dosbox)."""
    try:
        race(None)
        g = Game(Proc(dosbox_pid()))
        set_pause(g, False)
        for sc in (0x3b, 0x3c, 0x3d, 0x3e):
            front(MY_PID)
            key(sc, 0); time.sleep(0.1); key(sc, 1)
            prev, still = None, 0
            end = time.time() + 20
            while still < 3 and time.time() < end:
                time.sleep(0.4)
                cur = g.flat(0x110a1c, 0x3c)
                still = still + 1 if cur == prev else 0
                prev = cur
            cx, ch, cz = struct.unpack_from("<3i", cur, 0)
            cpitch, cyaw, croll = struct.unpack_from("<3f", cur, 0x30)
            print('F%d CAM="%d %d %d %r %r %r"' % (sc - 0x3a, cx, cz, ch, cyaw, cpitch, croll), flush=True)
            front(MY_PID)
            for s, up in ((0x1D, 0), (0x3F, 0), (0x3F, 1), (0x1D, 1)):
                key(s, up)
                time.sleep(0.05)
            time.sleep(0.5)
    finally:
        if MY_PID:
            subprocess.run(["taskkill", "/PID", str(MY_PID), "/T", "/F"], capture_output=True)


def main():
    """Runs the command named by the first argument (see the header of this file)."""
    a = sys.argv[1:] or ["cam"]
    if a[0] == "shots":
        return shots()
    if a[0] == "walk":
        return walk(int(a[1]), [float(x) for x in a[2:]] or [14.0, 6.0, 6.0, 6.0])
    if a[0] == "race":
        view = os.environ.get("VIEW_KEY")
        return race(a[1] if len(a) > 1 else None, int(view, 0) if view else None)
    g = Game(Proc(dosbox_pid()))
    print("code at linear 0x%x, data at 0x%x" % (g.obj[1][1], g.obj[3][1]))
    if a[0] == "cam":
        print("camera record ds:0x10a1c"); show(g.flat(0x110a1c, 0x90), 0x110a1c)
    elif a[0] in ("dump", "watch"):
        addr, n = int(a[1], 0), int(a[2], 0)
        old = g.flat(addr, n); show(old, addr)
        while a[0] == "watch":
            time.sleep(0.2)
            b = g.flat(addr, n)
            ch = {i for i in range(0, n - 3, 4) if b[i:i + 4] != old[i:i + 4]}
            if ch:
                print("--"); show(b, addr, ch); old = b
    elif a[0] == "shot":
        # Wait until the paused game runs again (its animation clock ds:0x21538 stands still in the
        # pause, the timer does not), then send Ctrl+F5 to DOSBox (its screenshot).
        c = g.flat(0x121538, 4)
        print("armed: press P in DOSBox", flush=True)
        end = time.time() + float(a[1] if len(a) > 1 else 300)
        while g.flat(0x121538, 4) == c:
            if time.time() > end:
                sys.exit("the game did not run")
            time.sleep(0.005)
        time.sleep(float(a[2]) if len(a) > 2 else 0.1)
        front(g.p.pid)
        c0 = g.flat(0x121538, 4)
        for sc, up in ((0x1D, 0), (0x3F, 0), (0x3F, 1), (0x1D, 1)):
            key(sc, up)
            time.sleep(0.05)
        c1 = g.flat(0x121538, 4)
        # The animation clock (ds:0x21538, 16 bits, a turn in 3.33 s) is printed, so that our picture
        # can match it.
        print("Ctrl+F5 sent; animation clock ds:0x21538 %d .. %d"
              % (struct.unpack("<I", c0)[0], struct.unpack("<I", c1)[0]))
    elif a[0] == "list":
        # The list of the pieces in reach (original: STARTL.EXE 0x83164) is at 0x500f0, its count at
        # ds:0x82248. An entry is dist^2 >> 2 and a pointer to a 0x40-byte record: +8 x, +0xc height, +0x10 z.
        n = struct.unpack("<I", g.flat(0x82248, 4))[0]
        lst = g.flat(0x500f0, 8 * n)
        print("%d pieces in reach" % n)
        for i in range(n):
            d, rec = struct.unpack_from("<Ii", lst, 8 * i)
            r = g.ptr(rec, 0x40)
            w = struct.unpack("<16i", r)
            print("%4d %6d %08x" % (i, int((d * 4) ** 0.5), rec) + " %7d %6d %7d |" % w[2:5]
                  + " ".join("%x" % (x & 0xffffffff) for x in w[5:16]) + " | "
                  + " ".join("%x" % (x & 0xffffffff) for x in w[0:2]))


if __name__ == "__main__":
    main()
