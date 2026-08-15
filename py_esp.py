import ctypes
import json
import math
import os
import struct
import sys
import time

try:
    import pymem
    import pymem.process
except ImportError:
    sys.exit("pip install pymem pywin32")

import win32api
import win32con
import win32gui

BASE = {
    "dwEntityList": 0x2554050,
    "dwLocalPlayerController": 0x2383DA0,
    "dwViewMatrix": 0x23AE550,
}

OFF = {
    "m_iHealth": 0x34C,
    "m_iTeamNum": 0x3E7,
    "m_lifeState": 0x354,
    "m_pGameSceneNode": 0x330,
    "m_vOldOrigin": 0x13B8,
    "m_modelState": 0x140,
    "m_hPlayerPawn": 0x914,
    "m_sSanitizedPlayerName": 0x868,
    "m_iConnected": 0x6EC,
}

BONE_HEAD = 6
BONE_STRIDE = 32

TEAM_COLOUR = {2: 0x3C46FF, 3: 0xFFB43C}
BOX_COLOUR = 0xFFFFFF
NAME_COLOUR = 0xFFFFFF


def load_dumper_offsets():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "offsets.json")
    if not os.path.isfile(path):
        return False
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    client = data.get("client.dll") or data.get("client_dll") or {}
    found = 0
    for key in BASE:
        if key in client:
            BASE[key] = client[key]
            found += 1
    return found == len(BASE)


class Mem:
    def __init__(self, proc_name="cs2.exe"):
        self.pm = pymem.Pymem(proc_name)
        self.client = pymem.process.module_from_name(
            self.pm.process_handle, "client.dll"
        ).lpBaseOfDll

    def ptr(self, addr):
        try:
            return self.pm.read_ulonglong(addr)
        except Exception:
            return 0

    def i32(self, addr):
        try:
            return self.pm.read_int(addr)
        except Exception:
            return 0

    def u8(self, addr):
        try:
            return self.pm.read_uchar(addr)
        except Exception:
            return 0

    def u32(self, addr):
        try:
            return self.pm.read_uint(addr)
        except Exception:
            return 0

    def f32(self, addr):
        try:
            return self.pm.read_float(addr)
        except Exception:
            return 0.0

    def vec3(self, addr):
        try:
            raw = self.pm.read_bytes(addr, 12)
            return struct.unpack("<fff", raw)
        except Exception:
            return (0.0, 0.0, 0.0)

    def matrix(self, addr):
        try:
            raw = self.pm.read_bytes(addr, 64)
            v = struct.unpack("<16f", raw)
            return [list(v[0:4]), list(v[4:8]), list(v[8:12]), list(v[12:16])]
        except Exception:
            return None

    def cstr(self, addr, size=64):
        try:
            raw = self.pm.read_bytes(addr, size)
            return raw.split(b"\x00")[0].decode("utf-8", "ignore")
        except Exception:
            return ""


def entity_from_handle(mem, entity_list, handle):
    if handle == 0xFFFFFFFF or not handle:
        return 0
    entry = mem.ptr(entity_list + 0x8 * ((handle & 0x7FFF) >> 9) + 16)
    if not entry:
        return 0
    return mem.ptr(entry + 112 * (handle & 0x1FF))


def world_to_screen(m, pos, width, height):
    x, y, z = pos
    w = m[3][0] * x + m[3][1] * y + m[3][2] * z + m[3][3]
    if w < 0.001:
        return None
    px = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3]
    py = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3]
    inv = 1.0 / w
    return (
        (width * 0.5) + (px * inv) * (width * 0.5),
        (height * 0.5) - (py * inv) * (height * 0.5),
    )


def collect_players(mem, local_pawn, local_team, team_check):
    out = []
    entity_list = mem.ptr(mem.client + BASE["dwEntityList"])
    if not entity_list:
        return out

    for i in range(1, 64):
        entry = mem.ptr(entity_list + 0x8 * ((i & 0x7FFF) >> 9) + 16)
        if not entry:
            continue
        ctrl = mem.ptr(entry + 112 * (i & 0x1FF))
        if not ctrl:
            continue
        if mem.i32(ctrl + OFF["m_iConnected"]) != 0:
            continue

        pawn = entity_from_handle(mem, entity_list, mem.u32(ctrl + OFF["m_hPlayerPawn"]))
        if not pawn or pawn == local_pawn:
            continue
        if mem.u8(pawn + OFF["m_lifeState"]) != 0:
            continue

        health = mem.i32(pawn + OFF["m_iHealth"])
        if health <= 0 or health > 100:
            continue

        team = mem.i32(pawn + OFF["m_iTeamNum"])
        if team_check and team == local_team:
            continue

        origin = mem.vec3(pawn + OFF["m_vOldOrigin"])

        head = (origin[0], origin[1], origin[2] + 72.0)
        scene = mem.ptr(pawn + OFF["m_pGameSceneNode"])
        if scene:
            bones = mem.ptr(scene + OFF["m_modelState"] + 0x80)
            if bones:
                b = mem.vec3(bones + BONE_HEAD * BONE_STRIDE)
                if any(b) and all(math.isfinite(c) for c in b):
                    head = (b[0], b[1], b[2] + 10.0)

        name_ptr = mem.ptr(ctrl + OFF["m_sSanitizedPlayerName"])
        name = mem.cstr(name_ptr, 32) if name_ptr else ""

        out.append(
            {
                "health": health,
                "team": team,
                "origin": origin,
                "head": head,
                "name": name or "player",
            }
        )
    return out


class Overlay:
    def __init__(self):
        self.hwnd = None
        self.width = win32api.GetSystemMetrics(win32con.SM_CXSCREEN)
        self.height = win32api.GetSystemMetrics(win32con.SM_CYSCREEN)
        self._make_window()

    def _make_window(self):
        cls = win32gui.WNDCLASS()
        cls.lpfnWndProc = {win32con.WM_DESTROY: lambda *a: win32gui.PostQuitMessage(0)}
        cls.lpszClassName = "PyEspOverlay"
        cls.hbrBackground = win32gui.GetStockObject(win32con.BLACK_BRUSH)
        win32gui.RegisterClass(cls)

        self.hwnd = win32gui.CreateWindowEx(
            win32con.WS_EX_LAYERED
            | win32con.WS_EX_TRANSPARENT
            | win32con.WS_EX_TOPMOST
            | win32con.WS_EX_TOOLWINDOW
            | win32con.WS_EX_NOACTIVATE,
            "PyEspOverlay",
            None,
            win32con.WS_POPUP,
            0,
            0,
            self.width,
            self.height,
            0,
            0,
            0,
            None,
        )
        win32gui.SetLayeredWindowAttributes(
            self.hwnd, 0x000000, 255, win32con.LWA_COLORKEY
        )
        win32gui.ShowWindow(self.hwnd, win32con.SW_SHOW)

    def begin(self):
        self.hdc = win32gui.GetDC(self.hwnd)
        self.mem_dc = win32gui.CreateCompatibleDC(self.hdc)
        self.bitmap = win32gui.CreateCompatibleBitmap(self.hdc, self.width, self.height)
        win32gui.SelectObject(self.mem_dc, self.bitmap)
        win32gui.FillRect(
            self.mem_dc,
            (0, 0, self.width, self.height),
            win32gui.GetStockObject(win32con.BLACK_BRUSH),
        )

    def end(self):
        win32gui.BitBlt(
            self.hdc, 0, 0, self.width, self.height, self.mem_dc, 0, 0, win32con.SRCCOPY
        )
        win32gui.DeleteObject(self.bitmap)
        win32gui.DeleteDC(self.mem_dc)
        win32gui.ReleaseDC(self.hwnd, self.hdc)

    def rect(self, x, y, w, h, colour, thickness=1):
        pen = win32gui.CreatePen(win32con.PS_SOLID, thickness, colour)
        old_pen = win32gui.SelectObject(self.mem_dc, pen)
        old_brush = win32gui.SelectObject(
            self.mem_dc, win32gui.GetStockObject(win32con.NULL_BRUSH)
        )
        win32gui.Rectangle(self.mem_dc, int(x), int(y), int(x + w), int(y + h))
        win32gui.SelectObject(self.mem_dc, old_brush)
        win32gui.SelectObject(self.mem_dc, old_pen)
        win32gui.DeleteObject(pen)

    def fill(self, x, y, w, h, colour):
        brush = win32gui.CreateSolidBrush(colour)
        win32gui.FillRect(
            self.mem_dc, (int(x), int(y), int(x + w), int(y + h)), brush
        )
        win32gui.DeleteObject(brush)

    def text(self, x, y, s, colour):
        win32gui.SetTextColor(self.mem_dc, colour)
        win32gui.SetBkMode(self.mem_dc, win32con.TRANSPARENT)
        win32gui.DrawText(
            self.mem_dc,
            s,
            -1,
            (int(x), int(y), int(x) + 400, int(y) + 20),
            win32con.DT_LEFT | win32con.DT_TOP | win32con.DT_SINGLELINE,
        )

    def pump(self):
        win32gui.PumpWaitingMessages()


def main():
    if load_dumper_offsets():
        print("offsets: loaded from offsets.json")
    else:
        print("offsets: using built-in defaults from auth_client.h")
    if not all(BASE.values()):
        sys.exit("Module offsets are zero — fill BASE or supply offsets.json.")

    try:
        mem = Mem()
    except Exception as exc:
        sys.exit(f"could not attach to cs2.exe: {exc}")

    overlay = Overlay()
    print(f"attached, client.dll at 0x{mem.client:X} — END to quit")

    while True:
        if win32api.GetAsyncKeyState(win32con.VK_END) & 0x8000:
            break
        overlay.pump()

        vm = mem.matrix(mem.client + BASE["dwViewMatrix"])
        local_ctrl = mem.ptr(mem.client + BASE["dwLocalPlayerController"])
        if vm is None or not local_ctrl:
            time.sleep(0.05)
            continue

        entity_list = mem.ptr(mem.client + BASE["dwEntityList"])
        local_pawn = entity_from_handle(
            mem, entity_list, mem.u32(local_ctrl + OFF["m_hPlayerPawn"])
        )
        local_team = mem.i32(local_pawn + OFF["m_iTeamNum"]) if local_pawn else 0

        players = collect_players(mem, local_pawn, local_team, team_check=True)

        overlay.begin()
        for p in players:
            feet = world_to_screen(vm, p["origin"], overlay.width, overlay.height)
            head = world_to_screen(vm, p["head"], overlay.width, overlay.height)
            if not feet or not head:
                continue

            height = feet[1] - head[1]
            if height < 4:
                continue
            width = height / 1.8
            x = head[0] - width * 0.5
            y = head[1]

            colour = TEAM_COLOUR.get(p["team"], BOX_COLOUR)
            overlay.rect(x, y, width, height, colour)

            bar_h = height * (p["health"] / 100.0)
            overlay.fill(x - 6, y, 3, height, 0x202020)
            overlay.fill(x - 6, y + (height - bar_h), 3, bar_h, 0x00FF00)

            overlay.text(x, y - 16, f"{p['name']} [{p['health']}]", NAME_COLOUR)
        overlay.end()

        time.sleep(1 / 144)

    win32gui.DestroyWindow(overlay.hwnd)


if __name__ == "__main__":
    main()
