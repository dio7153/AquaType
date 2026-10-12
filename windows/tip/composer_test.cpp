// composer_test: the composition state machine against the real engine (serve-pipe), with a host that records what the
// text service would do.  Each case types keys and checks the text committed.
//   composer_test <AquaTypeEngine.exe> <payload dir> <char_to_keys.json>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "composer.h"
#include "engine_link.h"

using namespace aqtip;

struct RecordingHost : Host {
    std::wstring preedit, committed; int beeps = 0, caret = -1;
    void set_preedit(const std::wstring& t, int c) override { preedit = t; caret = c; }
    void commit(const std::wstring& t) override { committed += t; preedit.clear(); }
    void beep() override { beeps++; }
    std::string context_before_caret() override { return ""; }
    bool secure_input() override { return false; }
};

static std::string u8(const std::wstring& w) { if (w.empty()) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s; }
static uint32_t vk_of(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A'; if (c >= '0' && c <= '9') return c;
    switch (c) { case ' ': return VK_SPACE; case ';': return VK_OEM_1; case ',': return VK_OEM_COMMA; case '-': return VK_OEM_MINUS; case '.': return VK_OEM_PERIOD; case '/': return VK_OEM_2; }
    return 0;
}
static void type(Composer& c, const std::string& keys) { for (char ch : keys) { Key k; k.vk = vk_of(ch); std::printf("%c", ch); c.key(k); } std::printf("|\n"); }
static void press(Composer& c, uint32_t vk, bool shift = false) { Key k; k.vk = vk; k.shift = shift; c.key(k); }
static void settle(Composer& c) { for (int i = 0; i < 100; i++) { c.pump(); Sleep(20); } c.refresh_preedit(); }
static void classic(Composer& c) { Settings s; s.sentence_preedit = false; c.set_settings(s); }   // the bopomofo shown, picked from the panel

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8); setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) { std::fprintf(stderr, "usage: composer_test <engine.exe> <payload dir> <char_to_keys.json>\n"); return 2; }
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); const std::wstring state = std::wstring(tmp) + L"aq-composer-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(state.c_str(), nullptr); DeleteFileW((state + L"\\personal_memory.v2.tsv").c_str());
    // a pipe and a state folder of this run's own: two sessions running the test at once must not share an engine
    const std::wstring pipe = L"\\\\.\\pipe\\AquaTypeEngine-composer-test-" + std::to_wstring(GetCurrentProcessId());
    auto w = [](const char* s) { std::string t(s); return std::wstring(t.begin(), t.end()); };
    const std::wstring P = w(argv[2]);
    std::wstring cmd = L"\"" + w(argv[1]) + L"\" --command serve-pipe --pipe " + pipe + L" --decoder-dir \"" + P + L"\\decoder\" --char-to-keys \"" + w(argv[3]) + L"\" --state-dir \"" + state +
                       L"\" --fallback-aqds \"" + P + L"\\dict_store\\dictionary_fallback.v1.aqds\" --fallback-manifest \"" + P + L"\\dict_store\\dictionary_fallback.v1.aqds.manifest.json\"";
    STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { std::fprintf(stderr, "cannot start the engine\n"); return 1; }
    std::printf("engine started\n");
    { HANDLE h; while ((h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)) == INVALID_HANDLE_VALUE) Sleep(100); CloseHandle(h); }   // the model is loaded
    std::printf("engine ready\n");

    int failed = 0;
    auto check = [&](const char* name, const std::function<void(Composer&, RecordingHost&)>& body, const std::wstring& want) {
        std::printf("-- %s\n", name); RecordingHost host; PipeEngine engine(pipe, EngineLaunch{}); Composer c(host, engine);
        auto t0 = GetTickCount64(); body(c, host);
        const bool ok = host.committed == want; failed += !ok;
        std::printf("%s %-34s committed \"%s\"%s (%llu ms)\n", ok ? "ok  " : "FAIL", name, u8(host.committed).c_str(), ok ? "" : (" want \"" + u8(want) + "\"").c_str(), GetTickCount64() - t0);
    };
    // the engine loads its model first; the first case waits for it through the reply timeout of Enter
    check("sentence, Enter right away", [](Composer& c, RecordingHost&) { type(c, "rup wu0 wu0 fu4cp3cl3"); press(c, VK_RETURN); }, L"今天天氣很好");
    check("sentence, Enter after the panel", [](Composer& c, RecordingHost&) { type(c, "ji3ap7au/6wu0 vu84j3d9 cjo4"); settle(c); press(c, VK_RETURN); }, L"我們明天下午開會");
    check("echo while composing", [](Composer& c, RecordingHost& h) { classic(c); type(c, "rup wu0 "); if (h.preedit != L"ㄐㄧㄣˉ ㄊㄧㄢˉ") h.committed = L"echo:" + h.preedit; else { press(c, VK_ESCAPE); } }, L"");
    check("Escape cancels", [](Composer& c, RecordingHost&) { type(c, "rup wu0 "); press(c, VK_ESCAPE); }, L"");
    check("Backspace takes a syllable", [](Composer& c, RecordingHost&) { type(c, "rup wu0 cl3"); press(c, VK_BACK); settle(c); press(c, VK_RETURN); }, L"今天");
    check("Tab to words, piece by piece", [](Composer& c, RecordingHost&) { classic(c); type(c, "rup wu0 wu0 fu4cp3cl3"); settle(c); press(c, VK_TAB); press(c, VK_RETURN); settle(c); press(c, VK_RETURN); }, L"今天天氣很好");
    check("punctuation commits first", [](Composer& c, RecordingHost&) { type(c, "rup wu0 "); press(c, VK_OEM_COMMA, true); }, L"今天，");
    check("full stop alone", [](Composer& c, RecordingHost&) { press(c, VK_OEM_PERIOD, true); }, L"。");
    check("quotes alternate", [](Composer& c, RecordingHost&) { press(c, VK_OEM_7, true); press(c, VK_OEM_7, true); }, L"“”");
    check("a typo is read through", [](Composer& c, RecordingHost&) { type(c, "rupp wu0 wu0 fu4cp3cl3"); settle(c); press(c, VK_RETURN); }, L"今天天氣很好");
    check("Shift tap: English, then back", [](Composer& c, RecordingHost&) { type(c, "rup wu0 "); settle(c); c.shift_tapped(); Key k; k.vk = 'A'; if (c.key(k) != Result::Pass) throw 1; c.shift_tapped(); type(c, "cl3"); settle(c); press(c, VK_RETURN); }, L"今天好");

    check("Shift+letter: a capital, still Chinese", [](Composer& c, RecordingHost&) { type(c, "rup wu0 "); settle(c); Key k; k.vk = 'A'; k.shift = true; if (c.key(k) != Result::Pass || c.ascii_mode()) throw 1; type(c, "cl3"); settle(c); press(c, VK_RETURN); }, L"今天好");
    check("Space after a tone commits", [](Composer& c, RecordingHost&) { type(c, "cl3"); settle(c); Key k; k.vk = VK_SPACE; c.key(k); if (c.composing()) throw 1; }, L"好");
    check("initial + Space: the symbol", [](Composer& c, RecordingHost&) { type(c, "1 q "); if (c.composing()) throw 1; }, L"ㄅㄆ");
    check("the bopomofo, last candidate", [](Composer& c, RecordingHost&) { classic(c); type(c, "cl3"); settle(c); press(c, VK_END); press(c, VK_RETURN); }, L"ㄏㄠˇ");
    check("CapsLock is ignored", [](Composer& c, RecordingHost&) { for (char ch : std::string("cl3")) { Key k; k.vk = vk_of(ch); k.caps = true; c.key(k); } settle(c); press(c, VK_RETURN); }, L"好");
    check("CapsLock English when chosen", [](Composer& c, RecordingHost&) { Settings s; s.caps_english = true; c.set_settings(s); Key k; k.vk = 'C'; k.caps = true; if (c.wants(k) || c.key(k) != Result::Pass) throw 1; }, L"");
    // 整句直出 (the default)
    check("sentence shown in the composition", [](Composer& c, RecordingHost& h) { type(c, "rup wu0 wu0 fu4cp3cl3"); settle(c); if (h.preedit != L"今天天氣很好") { h.committed = L"preedit:" + h.preedit; return; } press(c, VK_RETURN); }, L"今天天氣很好");
    check("go back and pick 妳", [](Composer& c, RecordingHost& h) {
        type(c, "su3cl3"); settle(c); press(c, VK_LEFT); press(c, VK_LEFT); if (h.caret != 0) throw 1;
        press(c, VK_DOWN); settle(c); const PanelView v = c.panel(); int at = -1;
        for (int i = 0; i < (int)v.cands->size(); i++) if ((*v.cands)[i].wtext == L"妳") at = i;
        if (at < 0) { h.committed = L"no 妳 in the list"; return; }
        c.select(at); if (h.preedit != L"妳好") { h.committed = L"preedit:" + h.preedit; return; } press(c, VK_RETURN); }, L"妳好");
    check("a pick holds while typing on", [](Composer& c, RecordingHost& h) {
        type(c, "su3"); settle(c); press(c, VK_DOWN); settle(c); const PanelView v = c.panel(); int at = -1;
        for (int i = 0; i < (int)v.cands->size(); i++) if ((*v.cands)[i].wtext == L"妳") at = i;
        if (at < 0) { h.committed = L"no 妳 in the list"; return; }
        c.select(at); type(c, "cl3"); settle(c); press(c, VK_RETURN); }, L"妳好");
    check("Backspace inside the sentence", [](Composer& c, RecordingHost&) { type(c, "su3cl3"); settle(c); press(c, VK_LEFT); press(c, VK_BACK); settle(c); press(c, VK_RETURN); }, L"好");
    check("Esc closes the list, Enter commits", [](Composer& c, RecordingHost&) { type(c, "rup wu0 "); settle(c); press(c, VK_DOWN); press(c, VK_ESCAPE); if (c.panel_visible()) throw 1; press(c, VK_RETURN); }, L"今天");   // (你好 is 妳好 by now: the earlier cases taught the memory)
    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
