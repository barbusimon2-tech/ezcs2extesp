#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

struct BaseOffsets {
    uintptr_t dwEntityList = 0x2554050;
    uintptr_t dwLocalPlayerController = 0x2383DA0;
    uintptr_t dwViewMatrix = 0x23AE550;
} BASE;

struct MemberOffsets {
    uintptr_t m_iHealth = 0x34C;
    uintptr_t m_iTeamNum = 0x3E7;
    uintptr_t m_lifeState = 0x354;
    uintptr_t m_pGameSceneNode = 0x330;
    uintptr_t m_vOldOrigin = 0x13B8;
    uintptr_t m_modelState = 0x140;
    uintptr_t m_hPlayerPawn = 0x914;
    uintptr_t m_sSanitizedPlayerName = 0x868;
    uintptr_t m_iConnected = 0x6EC;
} OFF;

constexpr int BONE_HEAD = 6;
constexpr int BONE_STRIDE = 32;

constexpr COLORREF COLOR_T = RGB(255, 70, 60);
constexpr COLORREF COLOR_CT = RGB(60, 180, 255);
constexpr COLORREF COLOR_WHITE = RGB(255, 255, 255);
constexpr COLORREF COLOR_GREEN = RGB(0, 255, 0);
constexpr COLORREF COLOR_DARK = RGB(32, 32, 32);

struct Vec3 {
    float x, y, z;
};

struct Matrix4x4 {
    float m[4][4];
};

struct PlayerData {
    int health;
    int team;
    Vec3 origin;
    Vec3 head;
    std::string name;
};

static bool enable_debug_privilege() {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool success = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(token);
    return success;
}

static std::string get_executable_dir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string path(buf);
    size_t last_slash = path.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        return path.substr(0, last_slash);
    }
    return ".";
}

static bool parse_json_uintptr(const std::string& content, const std::string& key, uintptr_t& out_val) {
    size_t pos = 0;
    while ((pos = content.find("\"" + key + "\"", pos)) != std::string::npos) {
        pos += key.length() + 2;
        size_t colon = content.find(':', pos);
        if (colon == std::string::npos) break;
        pos = colon + 1;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\r' || content[pos] == '\n' || content[pos] == '\"')) {
            pos++;
        }
        if (pos >= content.size()) break;
        char* end_ptr = nullptr;
        unsigned long long val = std::strtoull(content.c_str() + pos, &end_ptr, 0);
        if (end_ptr != (content.c_str() + pos)) {
            out_val = (uintptr_t)val;
            return true;
        }
    }
    return false;
}

static bool load_dumper_offsets() {
    const std::vector<std::string> paths = {
        get_executable_dir() + "\\offsets.json",
        get_executable_dir() + "\\..\\offsets.json",
        "offsets.json",
        "tools\\offsets.json",
        "..\\offsets.json"
    };

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (!file.is_open()) continue;
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        uintptr_t el = 0, lpc = 0, vm = 0;
        int found = 0;
        if (parse_json_uintptr(content, "dwEntityList", el)) { BASE.dwEntityList = el; found++; }
        if (parse_json_uintptr(content, "dwLocalPlayerController", lpc)) { BASE.dwLocalPlayerController = lpc; found++; }
        if (parse_json_uintptr(content, "dwViewMatrix", vm)) { BASE.dwViewMatrix = vm; found++; }

        uintptr_t val = 0;
        if (parse_json_uintptr(content, "m_iHealth", val)) OFF.m_iHealth = val;
        if (parse_json_uintptr(content, "m_iTeamNum", val)) OFF.m_iTeamNum = val;
        if (parse_json_uintptr(content, "m_lifeState", val)) OFF.m_lifeState = val;
        if (parse_json_uintptr(content, "m_pGameSceneNode", val)) OFF.m_pGameSceneNode = val;
        if (parse_json_uintptr(content, "m_vOldOrigin", val)) OFF.m_vOldOrigin = val;
        if (parse_json_uintptr(content, "m_modelState", val)) OFF.m_modelState = val;
        if (parse_json_uintptr(content, "m_hPlayerPawn", val)) OFF.m_hPlayerPawn = val;
        if (parse_json_uintptr(content, "m_sSanitizedPlayerName", val)) OFF.m_sSanitizedPlayerName = val;
        if (parse_json_uintptr(content, "m_iConnected", val)) OFF.m_iConnected = val;

        if (found == 3) return true;
    }
    return false;
}

class Mem {
public:
    HANDLE handle = NULL;
    DWORD pid = 0;
    uintptr_t client = 0;

    bool attach(const char* proc_name = "cs2.exe") {
        enable_debug_privilege();
        pid = find_pid(proc_name);
        if (!pid) return false;

        handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!handle) {
            handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        }
        if (!handle) {
            handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        }
        if (!handle) return false;

        client = find_module_base(pid, "client.dll");
        return client != 0;
    }

    ~Mem() {
        if (handle) CloseHandle(handle);
    }

    template <typename T>
    bool read_raw(uintptr_t addr, T* out) {
        if (!addr || !handle) return false;
        SIZE_T bytes_read = 0;
        return ReadProcessMemory(handle, (LPCVOID)addr, out, sizeof(T), &bytes_read) && bytes_read == sizeof(T);
    }

    template <typename T>
    T read(uintptr_t addr, T fallback = T{}) {
        T val{};
        if (read_raw(addr, &val)) return val;
        return fallback;
    }

    uintptr_t ptr(uintptr_t addr) { return read<uintptr_t>(addr, 0); }
    int32_t i32(uintptr_t addr) { return read<int32_t>(addr, 0); }
    uint8_t u8(uintptr_t addr) { return read<uint8_t>(addr, 0); }
    uint32_t u32(uintptr_t addr) { return read<uint32_t>(addr, 0); }
    float f32(uintptr_t addr) { return read<float>(addr, 0.0f); }
    Vec3 vec3(uintptr_t addr) { return read<Vec3>(addr, Vec3{ 0.0f, 0.0f, 0.0f }); }

    bool read_matrix(uintptr_t addr, Matrix4x4& out) {
        return read_raw<Matrix4x4>(addr, &out);
    }

    std::string cstr(uintptr_t addr, size_t max_len = 32) {
        if (!addr || !handle) return "";
        char buf[64]{};
        SIZE_T bytes_read = 0;
        size_t to_read = (std::min)(max_len, sizeof(buf) - 1);
        if (ReadProcessMemory(handle, (LPCVOID)addr, buf, to_read, &bytes_read) && bytes_read > 0) {
            buf[bytes_read] = '\0';
            return std::string(buf);
        }
        return "";
    }

private:
    static DWORD find_pid(const char* proc_name) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        DWORD result = 0;
        if (Process32First(snap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, proc_name) == 0) {
                    result = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        return result;
    }

    uintptr_t find_module_base(DWORD target_pid, const char* mod_name) {
        if (handle) {
            HMODULE mods[1024]{};
            DWORD cb_needed = 0;
            if (EnumProcessModulesEx(handle, mods, sizeof(mods), &cb_needed, LIST_MODULES_ALL)) {
                DWORD count = cb_needed / sizeof(HMODULE);
                for (DWORD i = 0; i < count; ++i) {
                    char name[MAX_PATH]{};
                    if (GetModuleBaseNameA(handle, mods[i], name, sizeof(name))) {
                        if (_stricmp(name, mod_name) == 0) {
                            MODULEINFO mi{};
                            if (GetModuleInformation(handle, mods[i], &mi, sizeof(mi))) {
                                return (uintptr_t)mi.lpBaseOfDll;
                            }
                            return (uintptr_t)mods[i];
                        }
                    }
                }
            }
        }

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, target_pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me{};
            me.dwSize = sizeof(me);
            if (Module32First(snap, &me)) {
                do {
                    if (_stricmp(me.szModule, mod_name) == 0) {
                        uintptr_t base = (uintptr_t)me.modBaseAddr;
                        CloseHandle(snap);
                        return base;
                    }
                } while (Module32Next(snap, &me));
            }
            CloseHandle(snap);
        }

        return 0;
    }
};

static uintptr_t entity_from_handle(Mem& mem, uintptr_t entity_list, uint32_t handle) {
    if (handle == 0xFFFFFFFF || !handle) return 0;
    uintptr_t entry = mem.ptr(entity_list + 0x8 * ((handle & 0x7FFF) >> 9) + 16);
    if (!entry) return 0;
    return mem.ptr(entry + 112 * (handle & 0x1FF));
}

static bool world_to_screen(const Matrix4x4& m, const Vec3& pos, int width, int height, float& out_x, float& out_y) {
    float w = m.m[3][0] * pos.x + m.m[3][1] * pos.y + m.m[3][2] * pos.z + m.m[3][3];
    if (w < 0.001f) return false;
    float inv = 1.0f / w;
    float px = (m.m[0][0] * pos.x + m.m[0][1] * pos.y + m.m[0][2] * pos.z + m.m[0][3]) * inv;
    float py = (m.m[1][0] * pos.x + m.m[1][1] * pos.y + m.m[1][2] * pos.z + m.m[1][3]) * inv;
    out_x = (width * 0.5f) + px * (width * 0.5f);
    out_y = (height * 0.5f) - py * (height * 0.5f);
    return true;
}

static std::vector<PlayerData> collect_players(Mem& mem, uintptr_t local_pawn, int local_team, bool team_check) {
    std::vector<PlayerData> out;
    uintptr_t entity_list = mem.ptr(mem.client + BASE.dwEntityList);
    if (!entity_list) return out;

    for (int i = 1; i < 64; ++i) {
        uintptr_t entry = mem.ptr(entity_list + 0x8 * ((i & 0x7FFF) >> 9) + 16);
        if (!entry) continue;
        uintptr_t ctrl = mem.ptr(entry + 112 * (i & 0x1FF));
        if (!ctrl) continue;
        if (mem.i32(ctrl + OFF.m_iConnected) != 0) continue;

        uintptr_t pawn = entity_from_handle(mem, entity_list, mem.u32(ctrl + OFF.m_hPlayerPawn));
        if (!pawn || pawn == local_pawn) continue;
        if (mem.u8(pawn + OFF.m_lifeState) != 0) continue;

        int health = mem.i32(pawn + OFF.m_iHealth);
        if (health <= 0 || health > 100) continue;

        int team = mem.i32(pawn + OFF.m_iTeamNum);
        if (team_check && team == local_team) continue;

        Vec3 origin = mem.vec3(pawn + OFF.m_vOldOrigin);
        Vec3 head = { origin.x, origin.y, origin.z + 72.0f };

        uintptr_t scene = mem.ptr(pawn + OFF.m_pGameSceneNode);
        if (scene) {
            uintptr_t bones = mem.ptr(scene + OFF.m_modelState + 0x80);
            if (bones) {
                Vec3 b = mem.vec3(bones + BONE_HEAD * BONE_STRIDE);
                if ((b.x != 0.0f || b.y != 0.0f || b.z != 0.0f) && std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z)) {
                    head = { b.x, b.y, b.z + 10.0f };
                }
            }
        }

        uintptr_t name_ptr = mem.ptr(ctrl + OFF.m_sSanitizedPlayerName);
        std::string name = name_ptr ? mem.cstr(name_ptr, 32) : "";
        if (name.empty()) name = "player";

        out.push_back({ health, team, origin, head, name });
    }
    return out;
}

class Overlay {
public:
    HWND hwnd = NULL;
    HDC hdc = NULL;
    HDC mem_dc = NULL;
    HBITMAP bitmap = NULL;
    HBITMAP old_bitmap = NULL;
    int width = 0;
    int height = 0;

    Overlay() {
        width = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        make_window();
        hdc = GetDC(hwnd);
        mem_dc = CreateCompatibleDC(hdc);
        bitmap = CreateCompatibleBitmap(hdc, width, height);
        old_bitmap = (HBITMAP)SelectObject(mem_dc, bitmap);
        SetBkMode(mem_dc, TRANSPARENT);
    }

    ~Overlay() {
        if (mem_dc) {
            SelectObject(mem_dc, old_bitmap);
            DeleteDC(mem_dc);
        }
        if (bitmap) DeleteObject(bitmap);
        if (hdc && hwnd) ReleaseDC(hwnd, hdc);
        if (hwnd) DestroyWindow(hwnd);
    }

    void pump() {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    void begin() {
        RECT rc{ 0, 0, width, height };
        FillRect(mem_dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }

    void end() {
        BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
    }

    void rect(float x, float y, float w, float h, COLORREF colour, int thickness = 1) {
        HPEN pen = CreatePen(PS_SOLID, thickness, colour);
        HGDIOBJ old_pen = SelectObject(mem_dc, pen);
        HGDIOBJ old_brush = SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
        Rectangle(mem_dc, (int)x, (int)y, (int)(x + w), (int)(y + h));
        SelectObject(mem_dc, old_brush);
        SelectObject(mem_dc, old_pen);
        DeleteObject(pen);
    }

    void fill(float x, float y, float w, float h, COLORREF colour) {
        HBRUSH brush = CreateSolidBrush(colour);
        RECT rc{ (int)x, (int)y, (int)(x + w), (int)(y + h) };
        FillRect(mem_dc, &rc, brush);
        DeleteObject(brush);
    }

    void text(float x, float y, const std::string& str, COLORREF colour) {
        SetTextColor(mem_dc, colour);
        RECT rc{ (int)x, (int)y, (int)x + 400, (int)y + 20 };
        DrawTextA(mem_dc, str.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    void make_window() {
        WNDCLASSA cls{};
        cls.lpfnWndProc = WndProc;
        cls.hInstance = GetModuleHandleA(NULL);
        cls.lpszClassName = "CppEspOverlay";
        cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        cls.hCursor = LoadCursorA(NULL, IDC_ARROW);
        RegisterClassA(&cls);

        hwnd = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            "CppEspOverlay",
            "CppEspOverlay",
            WS_POPUP,
            0,
            0,
            width,
            height,
            NULL,
            NULL,
            GetModuleHandleA(NULL),
            NULL
        );

        SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_COLORKEY);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
};

int main() {
    if (load_dumper_offsets()) {
        printf("offsets: loaded from offsets.json\n");
    } else {
        printf("offsets: using built-in defaults\n");
    }

    if (!BASE.dwEntityList || !BASE.dwLocalPlayerController || !BASE.dwViewMatrix) {
        printf("Module offsets are zero — fill BASE or supply offsets.json.\n");
        return 1;
    }

    Mem mem;
    if (!mem.attach("cs2.exe")) {
        printf("could not attach to cs2.exe\n");
        return 1;
    }

    Overlay overlay;
    printf("attached, client.dll at 0x%llX — END to quit\n", (unsigned long long)mem.client);

    while (true) {
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            break;
        }
        overlay.pump();

        Matrix4x4 vm{};
        if (!mem.read_matrix(mem.client + BASE.dwViewMatrix, vm)) {
            Sleep(50);
            continue;
        }

        uintptr_t local_ctrl = mem.ptr(mem.client + BASE.dwLocalPlayerController);
        if (!local_ctrl) {
            Sleep(50);
            continue;
        }

        uintptr_t entity_list = mem.ptr(mem.client + BASE.dwEntityList);
        uintptr_t local_pawn = entity_from_handle(mem, entity_list, mem.u32(local_ctrl + OFF.m_hPlayerPawn));
        int local_team = local_pawn ? mem.i32(local_pawn + OFF.m_iTeamNum) : 0;

        std::vector<PlayerData> players = collect_players(mem, local_pawn, local_team, true);

        overlay.begin();
        for (const auto& p : players) {
            float feet_x = 0, feet_y = 0;
            float head_x = 0, head_y = 0;
            if (!world_to_screen(vm, p.origin, overlay.width, overlay.height, feet_x, feet_y)) continue;
            if (!world_to_screen(vm, p.head, overlay.width, overlay.height, head_x, head_y)) continue;

            float height = feet_y - head_y;
            if (height < 4.0f) continue;
            float width = height / 1.8f;
            float x = head_x - width * 0.5f;
            float y = head_y;

            COLORREF colour = (p.team == 2) ? COLOR_T : (p.team == 3 ? COLOR_CT : COLOR_WHITE);
            overlay.rect(x, y, width, height, colour);

            float bar_h = height * (p.health / 100.0f);
            overlay.fill(x - 6, y, 3, height, COLOR_DARK);
            overlay.fill(x - 6, y + (height - bar_h), 3, bar_h, COLOR_GREEN);

            char label[64];
            snprintf(label, sizeof(label), "%s [%d]", p.name.c_str(), p.health);
            overlay.text(x, y - 16, label, COLOR_WHITE);
        }
        overlay.end();

        Sleep(7);
    }

    return 0;
}
