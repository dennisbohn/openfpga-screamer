#!/usr/bin/env python3
"""
dosram.py - read the original game's memory out of a running DOSBox, the way a cheat tool does.

Finds the DOSBox process, looks through its memory for the game's data object (a stretch of
STARTL.EXE's own bytes) and checks the copy it found by the loader's relocations: the words
the LE fixups point into the data object hold the stored value plus the object's linear base,
the same base at every one. Then any address of the data object as openscreamer docs/NOTES.md
and tools/dis.sh write it (0x100000 + ds offset) can be read live, while the game runs or
stands paused (P).

Usage: python -I tools/dosram.py <what> [...]
    race [conf]         starts DOSBox itself and the race (Enter through the menus), then the
                        screenshot on the grid and pause
    cam                 the camera record (ds:0x10a1c)
    shot [s] [delay]    waits (up to s seconds) until the paused game runs again, then Ctrl+F5
    list                the pieces in reach this frame (0x83164's list), with their records
    dump <addr> <n>     n bytes from a flat address, as words, ints and floats
    watch <addr> <n>    the same every 0.2 s until Ctrl+C, changed words marked
The flat images are made by tools/unle.py into build/exe/ (tools/dis.sh does it once).
"""
import ctypes, ctypes.wintypes as wt, struct, subprocess, sys, time, os

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


def key(sc, up):
    """a key by its scan code to the window in front, as the keyboard sends it"""
    i = INPUT(1, KI(0, sc, 8 | (2 if up else 0), 0, 0))
    ctypes.WinDLL("user32").SendInput(1, ctypes.byref(i), ctypes.sizeof(i))


def front(pid):
    """DOSBox's window to the front, so that the keys go there"""
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
    out = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = [x.strip('"') for x in line.split('","')]
        if len(f) > 1 and "dosbox" in f[0].lower():
            return int(f[1])
    sys.exit("no DOSBox running")


class Proc:
    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(0x0410, False, pid)    # QUERY_INFORMATION | VM_READ
        if not self.h:
            sys.exit("cannot open DOSBox (error %d)" % ctypes.get_last_error())

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, addr, buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    def regions(self):
        a, m = 0, MBI()
        while k32.VirtualQueryEx(self.h, a, ctypes.byref(m), ctypes.sizeof(m)):
            if m.State == 0x1000 and m.Protect & 0xEE and not m.Protect & 0x100:
                yield m.BaseAddress or 0, m.RegionSize
            a = (m.BaseAddress or 0) + m.RegionSize


OBJS = {1: (0x10000, 0xEB7A3), 3: (DATA, DATA_END)}     # code, data (unle.py's layout)


def find_obj(p, img, fix, lo, hi):
    """the host address of an object's first byte and its linear base in the guest"""
    sites = [i for i in range(lo, hi - 4) if img[i:i + 4] != fix[i:i + 4]
             and (struct.unpack_from("<I", fix, i)[0] - struct.unpack_from("<I", img, i)[0]) == lo]
    # a signature: 64 bytes of the object that no fixup touches, text if there is some
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
    """the game's memory: flat addresses as in openscreamer docs/NOTES.md, guest pointers"""
    def __init__(self, p):
        img = open(os.path.join(root, "build/exe/startl.bin"), "rb").read()
        fix = open(os.path.join(root, "build/exe/startl_fix.bin"), "rb").read()
        self.p, self.obj = p, {k: find_obj(p, img, fix, lo, hi) for k, (lo, hi) in OBJS.items()}
        self.mem = {h - l for h, l in self.obj.values()}    # guest memory's host address
        if len(self.mem) != 1:
            sys.exit("the objects do not sit in one guest memory (paging?)")
        self.mem = self.mem.pop()

    def flat(self, addr, n):
        for k, (lo, hi) in OBJS.items():
            if lo <= addr < hi:
                return self.p.read(self.obj[k][0] + addr - lo, n)
        sys.exit("0x%x is in no object" % addr)

    def ptr(self, lin, n):          # a guest pointer (linear address)
        return self.p.read(self.mem + lin, n)

    def to_flat(self, lin):         # a guest pointer as a flat address, if in an object
        for k, (lo, hi) in OBJS.items():
            if 0 <= lin - self.obj[k][1] < hi - lo:
                return lin - self.obj[k][1] + lo
        return None


def show(b, addr, mark=None):
    for i in range(0, len(b) - 3, 4):
        u = struct.unpack_from("<I", b, i)[0]
        f = struct.unpack_from("<f", b, i)[0]
        fs = "%.4f" % f if 1e-6 < abs(f) < 1e9 else ""
        print("%s%06x  %08x %12d %14s" % ("*" if mark and i in mark else " ", addr + i, u,
                                          struct.unpack_from("<i", b, i)[0], fs))


def race(conf):
    """DOSBox started on its own straight into STARTL.EXE (conf: an [autoexec] that runs it),
    Enter until the race stands on the grid (the player's car at its start place), then the
    screenshot at once with the animation clock; the race then paused (P)"""
    d = os.environ.get("GAME_DIR", "C:/GOG Games/Screamer")
    if conf is None:
        conf = os.path.join(root, "build", "dosrace.conf")
        open(conf, "w").write('[autoexec]\n@ECHO OFF\nmount C "%s"\nimgmount d "%s/scream.dat" -t iso -fs iso\n'
                              'c:\nSTARTL.EXE\nexit\n' % (d, d))
    subprocess.Popen([d + "/DOSBOX/DOSBox.exe", "-conf", d + "/dosboxScreamer.conf", "-conf", conf,
                      "-noconsole"], cwd=d + "/DOSBOX")
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
            # running (the clock moves), and the camera's swing onto the car over (it stands)
            if (x, z) == (2600, -18500) and g.flat(0x121538, 4) != c0 and g.flat(0x110a1c, 0x3c) == cam0:
                front(g.p.pid)
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


def main():
    a = sys.argv[1:] or ["cam"]
    if a[0] == "race":
        return race(a[1] if len(a) > 1 else None)
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
        # wait until the paused game runs again (its animation clock ds:0x21538 stands still
        # in the pause, the timer does not), then Ctrl+F5 to DOSBox (its screenshot)
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
        # the animation clock (ds:0x21538, 16 bits, a turn in 3.33 s), for our picture to match
        print("Ctrl+F5 sent; animation clock ds:0x21538 %d .. %d"
              % (struct.unpack("<I", c0)[0], struct.unpack("<I", c1)[0]))
    elif a[0] == "list":
        # 0x83164's list of the pieces in reach (0x500f0, count ds:0x82248): dist^2 >> 2 and a
        # 0x40-byte record: +8 x, +0xc height, +0x10 z
        n = struct.unpack("<I", g.flat(0x82248, 4))[0]
        lst = g.flat(0x500f0, 8 * n)
        print("%d pieces in reach" % n)
        for i in range(n):
            d, rec = struct.unpack_from("<Ii", lst, 8 * i)
            r = g.ptr(rec, 0x40)
            w = struct.unpack("<16i", r)
            print("%4d %6d %08x" % (i, int((d * 4) ** 0.5), rec) + " %7d %6d %7d |" % w[2:5]
                  + " ".join("%x" % (x & 0xffffffff) for x in w[5:16]) + " | " + " ".join("%x" % (x & 0xffffffff) for x in w[0:2]))


if __name__ == "__main__":
    main()
