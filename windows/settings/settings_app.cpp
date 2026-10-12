// AquaTypeSettings.exe: the settings window.  Opened from the taskbar's 中/英 indicator (right click, 「設定…」) or from
// Windows Settings' 「選項」 for the keyboard.  Saves to HKCU\Software\AquaType (windows/tip/settings.h), tells the
// running text services to reread, and restarts the engine when a setting it reads at start has changed.
#include "settings.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {
using aqtip::Settings;

enum Id { kSentence = 99, kShift = 100, kCaps, kInitial, kBopomofo, kEnEng, kAnAng, kRetroflex, kLexicon, kSize, kClear, kOk, kCancel, kApply, kCtrlSpace };

struct Item { int id; const wchar_t* text; bool Settings::*field; };
struct Section { const wchar_t* title; std::vector<Item> items; };
const std::vector<Section> kSections = {
    {L"輸入方式", {{kSentence, L"整句直出（直接顯示整句；← → 回到某個字，↓ 改選）", &Settings::sentence_preedit}}},
    {L"按鍵", {{kShift, L"單按 Shift 切換中／英", &Settings::shift_toggle},
               {kCtrlSpace, L"Ctrl＋空白鍵切換中／英", &Settings::ctrl_space_toggle},
               {kCaps, L"CapsLock 開著時打英文（不勾：CapsLock 不影響注音）", &Settings::caps_english}}},
    {L"注音", {{kInitial, L"聲母加空白鍵直接打出注音符號（ㄅ 空白 → ㄅ）", &Settings::initial_symbol},
               {kBopomofo, L"候選字最後附上注音本身（「注」）", &Settings::bopomofo_candidate}}},
    {L"發音容錯與選字", {{kEnEng, L"ㄣ／ㄥ 不分（因該 → 應該）", &Settings::fuzzy_en_eng},
                        {kAnAng, L"ㄢ／ㄤ 不分", &Settings::fuzzy_an_ang},
                        {kRetroflex, L"捲舌不分（ㄓㄗ、ㄔㄘ、ㄕㄙ）", &Settings::fuzzy_retroflex},
                        {kLexicon, L"常用詞優先", &Settings::lexicon_priority}}},
};

HWND g_wnd = nullptr; HFONT g_font = nullptr, g_head = nullptr; UINT g_dpi = 96;
Settings g_saved;
std::vector<HWND> g_heads;

int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
std::wstring exe_dir() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); std::wstring d = p; return d.substr(0, d.find_last_of(L'\\')); }

Settings read_controls() {
    Settings s = g_saved;
    for (auto& sec : kSections) for (auto& it : sec.items) s.*it.field = IsDlgButtonChecked(g_wnd, it.id) == BST_CHECKED;
    const LRESULT i = SendDlgItemMessageW(g_wnd, kSize, CB_GETCURSEL, 0, 0); if (i >= 0 && i <= 2) s.candidate_size = (int)i;
    return s;
}
void write_controls(const Settings& s) {
    for (auto& sec : kSections) for (auto& it : sec.items) CheckDlgButton(g_wnd, it.id, s.*it.field ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(g_wnd, kSize, CB_SETCURSEL, s.candidate_size, 0);
}
void update_apply() { const Settings s = read_controls();
    const bool changed = !(s == g_saved);
    EnableWindow(GetDlgItem(g_wnd, kApply), changed); }

// ---- the engine: stopped (only the one installed next to this program) and started again with its logon command
void stop_engine() {
    const std::wstring dir = exe_dir() + L"\\";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"AquaTypeEngine.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID); if (!h) continue;
        wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n) && _wcsnicmp(path, dir.c_str(), dir.size()) == 0) { TerminateProcess(h, 0); WaitForSingleObject(h, 3000); }
        CloseHandle(h);
    }
    CloseHandle(snap);
}
void start_engine() {
    wchar_t cmd[4096]; DWORD n = sizeof cmd;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"AquaTypeEngine", RRF_RT_REG_SZ, nullptr, cmd, &n) != ERROR_SUCCESS) return;   // the text service starts it on the next key
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

bool apply() {
    const Settings s = read_controls();
    if (!s.save()) { MessageBoxW(g_wnd, L"設定沒有存成功。", L"AquaType 設定", MB_ICONWARNING); return false; }
    const bool engine = s.fuzzy_en_eng != g_saved.fuzzy_en_eng || s.fuzzy_an_ang != g_saved.fuzzy_an_ang || s.fuzzy_retroflex != g_saved.fuzzy_retroflex || s.lexicon_priority != g_saved.lexicon_priority;
    g_saved = s;
    DWORD_PTR r; SendMessageTimeoutW(HWND_BROADCAST, RegisterWindowMessageW(aqtip::kSettingsChangedMessage), 0, 0, SMTO_ABORTIFHUNG, 500, &r);
    if (engine) { stop_engine(); start_engine(); }
    update_apply(); return true;
}

void clear_memory() {
    if (MessageBoxW(g_wnd, L"清除後，AquaType 會忘記所有記住的選字，無法復原。\n確定要清除嗎？", L"清除個人記憶", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    PWSTR home = nullptr; if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &home))) return;
    const std::wstring file = std::wstring(home) + L"\\.aquatype\\personal_memory.v2.tsv"; CoTaskMemFree(home);
    stop_engine();                                               // it keeps the memory in memory and would write it back
    const bool ok = DeleteFileW(file.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    start_engine();
    MessageBoxW(g_wnd, ok ? L"個人記憶已清除。" : L"清除失敗：檔案正在使用中，請稍後再試。", L"清除個人記憶", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}

// ---- layout, in DIPs scaled to the window's monitor
void make_fonts() {
    if (g_font) DeleteObject(g_font); if (g_head) DeleteObject(g_head);
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
    g_head = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
}
constexpr int kW = 440, kMargin = 20, kRow = 26, kHeadGap = 14, kButtonW = 88, kButtonH = 28;
int layout() {
    int y = kMargin; size_t h = 0;
    for (auto& sec : kSections) {
        MoveWindow(g_heads[h++], S(kMargin), S(y), S(kW - 2 * kMargin), S(22), FALSE); y += 26;
        for (auto& it : sec.items) { MoveWindow(GetDlgItem(g_wnd, it.id), S(kMargin + 8), S(y), S(kW - 2 * kMargin - 8), S(22), FALSE); y += kRow; }
        y += kHeadGap;
    }
    MoveWindow(g_heads[h++], S(kMargin), S(y), S(kW - 2 * kMargin), S(22), FALSE); y += 28;
    MoveWindow(GetDlgItem(g_wnd, 200), S(kMargin + 8), S(y + 3), S(120), S(22), FALSE);
    MoveWindow(GetDlgItem(g_wnd, kSize), S(kMargin + 132), S(y), S(120), S(200), FALSE); y += 30 + kHeadGap;
    MoveWindow(g_heads[h++], S(kMargin), S(y), S(kW - 2 * kMargin), S(22), FALSE); y += 28;
    MoveWindow(GetDlgItem(g_wnd, kClear), S(kMargin + 8), S(y), S(140), S(kButtonH), FALSE);
    MoveWindow(GetDlgItem(g_wnd, 201), S(kMargin + 158), S(y + 5), S(kW - kMargin - 158 - kMargin), S(22), FALSE); y += kButtonH + 24;
    const int bx = kW - kMargin - 3 * kButtonW - 16;
    MoveWindow(GetDlgItem(g_wnd, kOk), S(bx), S(y), S(kButtonW), S(kButtonH), FALSE);
    MoveWindow(GetDlgItem(g_wnd, kCancel), S(bx + kButtonW + 8), S(y), S(kButtonW), S(kButtonH), FALSE);
    MoveWindow(GetDlgItem(g_wnd, kApply), S(bx + 2 * kButtonW + 16), S(y), S(kButtonW), S(kButtonH), FALSE);
    y += kButtonH + kMargin;
    EnumChildWindows(g_wnd, [](HWND c, LPARAM) -> BOOL { const bool head = std::find(g_heads.begin(), g_heads.end(), c) != g_heads.end();
        SendMessageW(c, WM_SETFONT, (WPARAM)(head ? g_head : g_font), FALSE); return TRUE; }, 0);
    InvalidateRect(g_wnd, nullptr, TRUE);
    return y;                                                    // the client height, in DIPs
}
void fit_window(int client_h, const RECT* suggested) {
    RECT r{0, 0, S(kW), S(client_h)}; AdjustWindowRectExForDpi(&r, GetWindowLongW(g_wnd, GWL_STYLE), FALSE, GetWindowLongW(g_wnd, GWL_EXSTYLE), g_dpi);
    if (suggested) SetWindowPos(g_wnd, nullptr, suggested->left, suggested->top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(g_wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

void create_controls(HWND w) {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) { return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, w, (HMENU)(INT_PTR)id, nullptr, nullptr); };
    for (auto& sec : kSections) {
        g_heads.push_back(make(L"STATIC", sec.title, SS_LEFT, -1));
        for (auto& it : sec.items) make(L"BUTTON", it.text, BS_AUTOCHECKBOX | WS_TABSTOP, it.id);
    }
    g_heads.push_back(make(L"STATIC", L"外觀", SS_LEFT, -1));
    make(L"STATIC", L"候選字大小", SS_LEFT, 200);
    HWND cb = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kSize);
    for (const wchar_t* t : {L"小", L"中（預設）", L"大"}) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
    g_heads.push_back(make(L"STATIC", L"個人記憶", SS_LEFT, -1));
    make(L"BUTTON", L"清除個人記憶…", BS_PUSHBUTTON | WS_TABSTOP, kClear);
    make(L"STATIC", L"忘記輸入法記住的選字", SS_LEFT, 201);
    make(L"BUTTON", L"確定", BS_DEFPUSHBUTTON | WS_TABSTOP, kOk);
    make(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, kCancel);
    make(L"BUTTON", L"套用", BS_PUSHBUTTON | WS_TABSTOP, kApply);
}

LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: g_wnd = w; g_dpi = GetDpiForWindow(w); make_fonts(); create_controls(w); write_controls(g_saved); fit_window(layout(), nullptr); update_apply(); return 0;
        case WM_DPICHANGED: g_dpi = HIWORD(wp); make_fonts(); fit_window(layout(), (RECT*)lp); return 0;
        case WM_CTLCOLORSTATIC: { HDC dc = (HDC)wp; SetBkMode(dc, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW); }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kOk: if (apply()) DestroyWindow(w); return 0;
                case kCancel: DestroyWindow(w); return 0;
                case kApply: apply(); return 0;
                case kClear: clear_memory(); return 0;
                case kSize: if (HIWORD(wp) == CBN_SELCHANGE) update_apply(); return 0;
                default: if (HIWORD(wp) == BN_CLICKED) update_apply(); return 0;
            }
        case WM_CLOSE: DestroyWindow(w); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // --bmp FILE: draw the window into a bitmap, shown off screen without taking focus (checking the layout without a
    // window on anyone's desktop)
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t* bmp_path = argc >= 3 && lstrcmpW(argv[1], L"--bmp") == 0 ? argv[2] : nullptr;
    // one window: a second launch brings the first forward
    HANDLE once = bmp_path ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\AquaTypeSettings");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) { if (HWND h = FindWindowW(L"AquaTypeSettings", nullptr)) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); } return 0; }
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
    g_saved = Settings::load();
    WNDCLASSEXW wc{sizeof wc}; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"AquaTypeSettings";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(WS_EX_CONTROLPARENT | (bmp_path ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0), wc.lpszClassName, L"AquaType 設定", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             bmp_path ? -32000 : CW_USEDEFAULT, bmp_path ? -32000 : CW_USEDEFAULT, 400, 400, nullptr, nullptr, inst, nullptr);
    if (!w) return 1;
    if (bmp_path) {
        ShowWindow(w, SW_SHOWNOACTIVATE); UpdateWindow(w);
        MSG m; for (int i = 0; i < 20; i++) { while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(10); }
        RECT r; GetWindowRect(w, &r); const int W = r.right - r.left, H = r.bottom - r.top;
        HDC sdc = GetDC(nullptr), dc = CreateCompatibleDC(sdc); HBITMAP bmp = CreateCompatibleBitmap(sdc, W, H); HGDIOBJ old = SelectObject(dc, bmp);
        PrintWindow(w, dc, PW_RENDERFULLCONTENT);
        BITMAPINFOHEADER bi{sizeof bi, W, -H, 1, 32, BI_RGB}; std::vector<BYTE> px((size_t)W * H * 4);
        GetDIBits(dc, bmp, 0, H, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        BITMAPFILEHEADER fh{0x4D42, (DWORD)(sizeof fh + sizeof bi + px.size()), 0, 0, sizeof fh + sizeof bi};
        if (FILE* f = _wfopen(bmp_path, L"wb")) { fwrite(&fh, sizeof fh, 1, f); fwrite(&bi, sizeof bi, 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f); }
        SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); ReleaseDC(nullptr, sdc); DestroyWindow(w); return 0;
    }
    ShowWindow(w, show); SetForegroundWindow(w);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    if (once) CloseHandle(once);
    return 0;
}
