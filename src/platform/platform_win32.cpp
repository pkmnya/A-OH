#include "platform/platform.hpp"
#include "common/utf8.hpp"

#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>

namespace aoh {

namespace {

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

uint64_t filetime_to_u64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

constexpr UINT WM_TRAYICON = WM_APP + 42;
constexpr UINT ID_TRAY_OPEN_SETTINGS = 1001;
constexpr UINT ID_TRAY_PICK_EXIT_MP3 = 1002;
constexpr UINT ID_TRAY_PICK_IDLE_MP3 = 1003;
constexpr UINT ID_TRAY_TEST_EXIT = 1004;
constexpr UINT ID_TRAY_TEST_IDLE = 1005;
constexpr UINT ID_TRAY_TOGGLE_WATCH = 1006;
constexpr UINT ID_TRAY_TOGGLE_AUTOSTART = 1007;
constexpr UINT ID_TRAY_EXIT = 1008;

HWND g_tray_hwnd = nullptr;
const TrayCallbacks* g_tray_cb = nullptr;
UINT g_taskbar_created_msg = 0;
NOTIFYICONDATAW g_nid = {};

void add_tray_icon(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    lstrcpynW(g_nid.szTip, L"A-OH 终端音效助手 (右键设置)", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbar_created_msg && msg == g_taskbar_created_msg) {
        add_tray_icon(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            if (menu && g_tray_cb) {
                AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_SETTINGS, L"打开音效设置网页 (&S)");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_TRAY_PICK_EXIT_MP3, L"选择[退出] MP3 文件...");
                AppendMenuW(menu, MF_STRING, ID_TRAY_PICK_IDLE_MP3, L"选择[闲置] MP3 文件...");
                AppendMenuW(menu, MF_STRING, ID_TRAY_TEST_EXIT, L"试听[退出]音效 (打断不重叠)");
                AppendMenuW(menu, MF_STRING, ID_TRAY_TEST_IDLE, L"试听[闲置]音效 (打断不重叠)");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                UINT watch_flags = MF_STRING | (g_tray_cb->is_watch_enabled && g_tray_cb->is_watch_enabled() ? MF_CHECKED : MF_UNCHECKED);
                UINT auto_flags = MF_STRING | (g_tray_cb->is_autostart_enabled && g_tray_cb->is_autostart_enabled() ? MF_CHECKED : MF_UNCHECKED);
                AppendMenuW(menu, watch_flags, ID_TRAY_TOGGLE_WATCH, L"监听批处理/终端退出");
                AppendMenuW(menu, auto_flags, ID_TRAY_TOGGLE_AUTOSTART, L"开机自启动");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出 A-OH (&Q)");

                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(menu);
            }
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            if (g_tray_cb && g_tray_cb->on_open_settings) {
                g_tray_cb->on_open_settings();
            }
        }
        return 0;

    case WM_COMMAND:
        if (g_tray_cb) {
            switch (LOWORD(wParam)) {
            case ID_TRAY_OPEN_SETTINGS:
                if (g_tray_cb->on_open_settings) g_tray_cb->on_open_settings();
                break;
            case ID_TRAY_PICK_EXIT_MP3:
                if (g_tray_cb->on_pick_exit_mp3) g_tray_cb->on_pick_exit_mp3();
                break;
            case ID_TRAY_PICK_IDLE_MP3:
                if (g_tray_cb->on_pick_idle_mp3) g_tray_cb->on_pick_idle_mp3();
                break;
            case ID_TRAY_TEST_EXIT:
                if (g_tray_cb->on_test_exit) g_tray_cb->on_test_exit();
                break;
            case ID_TRAY_TEST_IDLE:
                if (g_tray_cb->on_test_idle) g_tray_cb->on_test_idle();
                break;
            case ID_TRAY_TOGGLE_WATCH:
                if (g_tray_cb->on_toggle_watch) g_tray_cb->on_toggle_watch();
                break;
            case ID_TRAY_TOGGLE_AUTOSTART:
                if (g_tray_cb->on_toggle_autostart) g_tray_cb->on_toggle_autostart();
                break;
            case ID_TRAY_EXIT:
                if (g_tray_cb->on_quit) g_tray_cb->on_quit();
                PostQuitMessage(0);
                break;
            }
        }
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (g_tray_cb && g_tray_cb->on_quit) g_tray_cb->on_quit();
        request_tray_quit();
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

void net_init() {
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

void net_cleanup() {
    WSACleanup();
}

std::string get_executable_path() {
    wchar_t buf[32768] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return wide_to_utf8(std::wstring(buf, len));
}

std::string get_daemon_autostart_path() {
    std::filesystem::path exe = path_from_utf8(get_executable_path());
    std::filesystem::path tray_exe = exe.parent_path() / L"a-oh-tray.exe";
    std::error_code ec;
    if (std::filesystem::exists(tray_exe, ec)) {
        return path_to_utf8(tray_exe);
    }
    return path_to_utf8(exe);
}

std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    FILETIME now_ft = {};
    GetSystemTimeAsFileTime(&now_ft);
    uint64_t now_100ns = filetime_to_u64(now_ft);

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessInfo info;
            info.pid = pe.th32ProcessID;
            info.name = to_lower_ascii(wide_to_utf8(pe.szExeFile));

            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (hp) {
                FILETIME create_ft = {}, exit_ft = {}, kernel_ft = {}, user_ft = {};
                if (GetProcessTimes(hp, &create_ft, &exit_ft, &kernel_ft, &user_ft)) {
                    uint64_t c = filetime_to_u64(create_ft);
                    info.start_key = c;
                    if (now_100ns >= c) {
                        info.age_ms = (now_100ns - c) / 10000ULL;
                    }
                }
                CloseHandle(hp);
            }
            out.push_back(std::move(info));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

static constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t kRunValue[] = L"A-OH";

bool is_autostart_enabled() {
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD cb = 0;
    LONG r = RegQueryValueExW(hkey, kRunValue, nullptr, &type, nullptr, &cb);
    RegCloseKey(hkey);
    return (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && cb > sizeof(wchar_t));
}

bool set_autostart(bool enable, std::string& err) {
    HKEY hkey = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hkey, nullptr);
    if (r != ERROR_SUCCESS) {
        err = "Failed to open Windows Run registry key";
        return false;
    }

    if (enable) {
        std::wstring cmd = L"\"" + utf8_to_wide(get_daemon_autostart_path()) + L"\" daemon";
        DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        r = RegSetValueExW(hkey, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), bytes);
    } else {
        r = RegDeleteValueW(hkey, kRunValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(hkey);
    if (r != ERROR_SUCCESS) {
        err = "Failed to update Windows Run registry key";
        return false;
    }
    return true;
}

bool open_url_in_browser(const std::string& url) {
    std::wstring wurl = utf8_to_wide(url);
    HINSTANCE res = ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(res) > 32;
}

std::string pick_file_dialog(const std::string& title_utf8, const std::string& kind) {
    wchar_t file_buf[4096] = {};
    std::wstring title = utf8_to_wide(title_utf8);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_tray_hwnd;
    if (kind == "midi") {
        ofn.lpstrFilter = L"MIDI Files (*.mid;*.midi)\0*.mid;*.midi\0All Files (*.*)\0*.*\0";
    } else {
        ofn.lpstrFilter = L"Audio Files (*.mp3;*.wav;*.m4a;*.aac)\0*.mp3;*.wav;*.m4a;*.aac\0All Files (*.*)\0*.*\0";
    }
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = ARRAYSIZE(file_buf);
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        return wide_to_utf8(file_buf);
    }
    return {};
}

void run_tray_or_wait_loop(const TrayCallbacks& cb) {
    g_tray_cb = &cb;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    g_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AOH_Tray_Window_Class";
    RegisterClassExW(&wc);

    g_tray_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"A-OH", WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_tray_hwnd) {
        add_tray_icon(g_tray_hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_tray_hwnd) {
        DestroyWindow(g_tray_hwnd);
        g_tray_hwnd = nullptr;
    }
    g_tray_cb = nullptr;
}

void request_tray_quit() {
    if (g_tray_hwnd) {
        PostMessageW(g_tray_hwnd, WM_CLOSE, 0, 0);
    }
}

} // namespace aoh
