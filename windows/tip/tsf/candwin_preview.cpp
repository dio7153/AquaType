// candwin_preview: the candidate window drawn into PNG files (light and dark, a few states), to compare against the
// design mockups without putting a window on anyone's desktop.
//   candwin_preview <out dir>
#include "candwin.h"
#include <string>
namespace aqtsf { HINSTANCE g_module = GetModuleHandleW(nullptr); std::atomic<long> g_locks{0}; }
using namespace aqtsf;
static aqtip::Candidate cand(const wchar_t* t, int group, bool memory = false) { aqtip::Candidate c; c.wtext = t; c.group = group; c.memory = memory; c.provenance = {"G_DECODER_MODEL"}; return c; }
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring out = argc > 1 ? argv[1] : L".";
    std::vector<aqtip::Candidate> list = {cand(L"今天天氣很好", 0), cand(L"今天天氣好好", 0, true), cand(L"今天天氣很冷", 0), cand(L"今天", 1), cand(L"金田", 1),
                                          cand(L"今", 2), cand(L"金", 2), cand(L"斤", 2), cand(L"巾", 2)};
    for (int i = 0; i < 54; i++) list.push_back(cand(L"津", 2));
    std::vector<aqtip::Candidate> longl = {cand(L"今天天氣很好我們下午一起去河邊散步然後順路買晚餐回家", 0), cand(L"今天天氣很好我們下午一起去和邊散步", 0, true), cand(L"今天", 1), cand(L"金田", 1)};
    CandidateWindow w; aqtip::PanelView v;
    v.cands = &list; v.rows = {0, 1, 2, 3, 4, 5, 6, 7, 8}; v.highlighted = 0;
    for (UINT dpi : {96u, 192u}) for (bool dark : {false, true}) {
        const std::wstring n = out + L"\\cand-" + (dark ? L"dark" : L"light") + (dpi == 192 ? L"@2x" : L"") + L".png";
        w.render_png(v, n.c_str(), dpi, dark);
    }
    aqtip::PanelView l; l.cands = &longl; l.rows = {0, 1, 2, 3}; l.highlighted = 0; w.render_png(l, (out + L"\\cand-long-light.png").c_str(), 96, false, 3);
    aqtip::PanelView it = v; it.interim = true; w.render_png(it, (out + L"\\cand-interim-light.png").c_str(), 96, false);
    aqtip::PanelView p2 = v; p2.rows = {9, 10, 11, 12, 13, 14, 15, 16, 17}; p2.highlighted = 11; w.render_png(p2, (out + L"\\cand-page2-dark.png").c_str(), 96, true);
    // the last page: the bopomofo itself in its own 「注」 group, highlighted; then the large text size
    std::vector<aqtip::Candidate> tail = list; tail.push_back(cand(L"ㄐㄧㄣ", 3));
    aqtip::PanelView zh; zh.cands = &tail; zh.rows = {54, 55, 56, 57, 58, 59, 60, 61, 62, 63}; zh.highlighted = 63; w.render_png(zh, (out + L"\\cand-bopomofo-light.png").c_str(), 96, false);
    w.set_text_size(22); w.render_png(v, (out + L"\\cand-large-light.png").c_str(), 96, false);
    w.set_text_size(16); w.render_png(v, (out + L"\\cand-small-light.png").c_str(), 96, false);
    return 0;
}