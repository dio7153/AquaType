#include "composer.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

namespace aqtip {
using json = nlohmann::json;
static const bool TRACE = GetEnvironmentVariableA("AQTIP_TRACE", nullptr, 0) > 0;
#define TR(...) do { if (TRACE) { std::fprintf(stderr, __VA_ARGS__); std::fflush(stderr); } } while (0)

namespace {
const char* kRawKeys = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/- 6347";
bool is_tone(char c) { return c == ' ' || c == '6' || c == '3' || c == '4' || c == '7'; }
constexpr size_t kKeyCap = 190, kMaxSentenceChars = 120;
constexpr int kMaxCandidates = 63, kPanelLines = 9;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {}; int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
std::vector<char32_t> code_points(const std::wstring& w) {
    std::vector<char32_t> o; for (size_t i = 0; i < w.size(); i++) { char32_t c = w[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; } o.push_back(c); }
    return o;
}
bool is_han(char32_t v) {
    return (v >= 0x3400 && v <= 0x4DBF) || (v >= 0x4E00 && v <= 0x9FFF) || (v >= 0xF900 && v <= 0xFAFF) || (v >= 0x20000 && v <= 0x2A6DF)
        || (v >= 0x2A700 && v <= 0x2EBEF) || (v >= 0x2F800 && v <= 0x2FA1F) || (v >= 0x30000 && v <= 0x323AF);
}
std::wstring from_cp(char32_t v) {
    std::wstring w; if (v >= 0x10000) { v -= 0x10000; w += (wchar_t)(0xD800 + (v >> 10)); w += (wchar_t)(0xDC00 + (v & 0x3FF)); } else w += (wchar_t)v; return w;
}
bool all_han(const std::wstring& t) { auto c = code_points(t); return !c.empty() && std::all_of(c.begin(), c.end(), is_han); }
bool any_han(const std::wstring& t) { auto c = code_points(t); return std::any_of(c.begin(), c.end(), is_han); }
std::string base64(const std::string& b) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; std::string o; size_t i = 0;
    for (; i + 3 <= b.size(); i += 3) { uint32_t v = ((uint8_t)b[i] << 16) | ((uint8_t)b[i + 1] << 8) | (uint8_t)b[i + 2]; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63]; }
    if (b.size() - i == 1) { uint32_t v = (uint8_t)b[i] << 16; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "=="; }
    else if (b.size() - i == 2) { uint32_t v = ((uint8_t)b[i] << 16) | ((uint8_t)b[i + 1] << 8); o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '='; }
    return o;
}
// SHA-256 (FIPS 180-4), small and self-contained: the text service is loaded into every app and keeps its imports few
std::string sha256_hex(const std::string& msg) {
    static const uint32_t K[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::string m = msg; const uint64_t bits = (uint64_t)msg.size() * 8; m += (char)0x80; while (m.size() % 64 != 56) m += (char)0; for (int i = 7; i >= 0; i--) m += (char)((bits >> (i * 8)) & 0xFF);
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64]; for (int i = 0; i < 16; i++) w[i] = ((uint8_t)m[off + 4 * i] << 24) | ((uint8_t)m[off + 4 * i + 1] << 16) | ((uint8_t)m[off + 4 * i + 2] << 8) | (uint8_t)m[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) { uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) { uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj; hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    static const char* X = "0123456789abcdef"; std::string o; for (uint32_t v : h) for (int i = 28; i >= 0; i -= 4) o += X[(v >> i) & 15]; return o;
}
std::string new_uuid() {
    GUID g; CoCreateGuid(&g); char b[40];
    sprintf_s(b, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}
int64_t unix_now() { FILETIME ft; GetSystemTimeAsFileTime(&ft); return (int64_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL - 11644473600ULL); }
}  // namespace

bool Composer::is_raw_key(char c) { return c && std::strchr(kRawKeys, c) != nullptr; }

char Composer::key_char(uint32_t vk) {
    if (vk >= '0' && vk <= '9') return (char)vk;
    if (vk >= 'A' && vk <= 'Z') return (char)(vk - 'A' + 'a');
    switch (vk) {
        case VK_SPACE: return ' '; case VK_OEM_1: return ';'; case VK_OEM_PLUS: return '='; case VK_OEM_COMMA: return ',';
        case VK_OEM_MINUS: return '-'; case VK_OEM_PERIOD: return '.'; case VK_OEM_2: return '/'; case VK_OEM_3: return '`';
        case VK_OEM_4: return '['; case VK_OEM_5: return '\\'; case VK_OEM_6: return ']'; case VK_OEM_7: return '\'';
        default: return 0;
    }
}
static char shifted(char c) {
    static const std::map<char, char> m = {{'1','!'},{'2','@'},{'3','#'},{'4','$'},{'5','%'},{'6','^'},{'7','&'},{'8','*'},{'9','('},{'0',')'},{'-','_'},{'=','+'},
        {'[','{'},{']','}'},{'\\','|'},{';',':'},{'\'','"'},{',','<'},{'.','>'},{'/','?'},{'`','~'}};
    auto it = m.find(c); return it == m.end() ? (char)std::toupper((unsigned char)c) : it->second;
}

std::wstring Composer::echo(const std::string& raw) {
    static const std::map<char, wchar_t> sym = {
        {'1',L'ㄅ'},{'q',L'ㄆ'},{'a',L'ㄇ'},{'z',L'ㄈ'},{'2',L'ㄉ'},{'w',L'ㄊ'},{'s',L'ㄋ'},{'x',L'ㄌ'},{'e',L'ㄍ'},{'d',L'ㄎ'},{'c',L'ㄏ'},
        {'r',L'ㄐ'},{'f',L'ㄑ'},{'v',L'ㄒ'},{'5',L'ㄓ'},{'t',L'ㄔ'},{'g',L'ㄕ'},{'b',L'ㄖ'},{'y',L'ㄗ'},{'h',L'ㄘ'},{'n',L'ㄙ'},
        {'u',L'ㄧ'},{'j',L'ㄨ'},{'m',L'ㄩ'},{'8',L'ㄚ'},{'i',L'ㄛ'},{'k',L'ㄜ'},{',',L'ㄝ'},{'9',L'ㄞ'},{'o',L'ㄟ'},{'l',L'ㄠ'},{'.',L'ㄡ'},
        {'0',L'ㄢ'},{'p',L'ㄣ'},{';',L'ㄤ'},{'/',L'ㄥ'},{'-',L'ㄦ'},{' ',L'ˉ'},{'6',L'ˊ'},{'3',L'ˇ'},{'4',L'ˋ'},{'7',L'˙'}};
    std::wstring o;
    for (char c : raw) { auto it = sym.find(c); if (it != sym.end()) o += it->second; if (is_tone(c)) o += L' '; }
    while (!o.empty() && o.back() == L' ') o.pop_back();
    return o;
}
std::wstring Composer::bopomofo_text(const std::string& raw) {
    std::wstring o = echo(raw);
    o.erase(std::remove_if(o.begin(), o.end(), [](wchar_t c) { return c == L' ' || c == L'ˉ'; }), o.end());
    return o;
}

Composer::Composer(Host& host, EngineLink& engine) : host_(host), engine_(engine), client_id_(new_uuid()) { pick_client_id_ = client_id_ + "-pick"; }

void Composer::begin_seq() { seq_++; }

// ---------------------------------------------------------------- keys
bool Composer::wants(const Key& k) const {
    const bool comp = composing(); const char c = key_char(k.vk);
    if (k.ctrl || k.alt) return comp;                                     // commit (or edit) the composition before the chord reaches the app
    if (ascii_mode()) return false;
    if (comp) return true;                                                // while composing every key is ours to route
    if (k.vk == VK_RETURN && !k.shift && pressed_while_waiting(k)) return true;
    if (k.caps && settings_.caps_english) return false;
    if (!c || c == ' ') return false;                                    // a space with nothing composed is a space
    if (k.shift && k.vk >= 'A' && k.vk <= 'Z') return false;
    if (k.shift) return true;                                             // shifted punctuation
    return is_raw_key(c) || c == '`' || c == '=' || c == '[' || c == ']' || c == '\\' || c == '\'';
}

Result Composer::key(const Key& k) {
    pump();
    const bool comp = composing(); const char c = key_char(k.vk);
    if (k.ctrl || k.alt) {
        if (!comp) return Result::Pass;
        if (k.alt && !k.ctrl && k.vk == VK_BACK) { clear_composition(); return Result::Eaten; }
        if (k.ctrl && !k.alt && (k.vk == VK_BACK || k.vk == 'H')) { backspace(); return Result::Eaten; }
        commit_whole(/*remember=*/false); return Result::Pass;                 // a shortcut ends the composition; what it committed is not learned
    }
    if (ascii_mode()) return Result::Pass;
    // An Enter pressed while the last Enter waited for its answer: that one committed the composition, this one is a
    // repeat, not a new line (in a chat it would send the message).  AquaType drops it the same way.
    if (!comp && k.vk == VK_RETURN && !k.shift && pressed_while_waiting(k)) { waited_to_ = 0; return Result::Eaten; }
    // CapsLock on = English, as AquaType and Microsoft Bopomofo do; a setting can make it ignored instead.
    if (k.caps && settings_.caps_english && c && c != ' ') { if (comp) commit_whole(); return Result::Pass; }

    if (comp && sentence_mode()) { bool handled = false; const Result r = sentence_key(k, c, &handled); if (handled) return r; }
    if (comp) switch (k.vk) {
        case VK_BACK: backspace(); return Result::Eaten;
        case VK_DELETE: return Result::Eaten;
        case VK_ESCAPE: clear_composition(); return Result::Eaten;
        case VK_TAB: if (!k.shift && !cands_.empty() && panel_raw_ == raw_) { const int g = cands_[highlighted_].group; int i = highlighted_ + 1;
                         while (i < (int)cands_.size() && cands_[i].group == g) i++; move(i < (int)cands_.size() ? i : 0); }
                     return Result::Eaten;
        case VK_DOWN: if (panel_raw_ == raw_) move(highlighted_ + 1); return Result::Eaten;
        case VK_UP: if (panel_raw_ == raw_) move(highlighted_ - 1); return Result::Eaten;
        case VK_RIGHT: case VK_NEXT: page(true); return Result::Eaten;
        case VK_LEFT: case VK_PRIOR: page(false); return Result::Eaten;
        case VK_HOME: if (panel_raw_ == raw_) move(0); return Result::Eaten;
        case VK_END: if (panel_raw_ == raw_) move((int)cands_.size() - 1); return Result::Eaten;
        case VK_RETURN:
            if (k.shift) { commit_whole(); return Result::Pass; }
            if (waiting_for_current()) wait_for_answer(commit_wait_ms(raw_.size()));
            if (sentence_mode() || !(panel_raw_ == raw_ && !cands_.empty()) || !commit_highlighted()) commit_whole();
            return Result::Eaten;
        default: break;
    }
    if (k.shift && k.vk >= 'A' && k.vk <= 'Z') {                         // a capital, typed as it is; the mode stays Chinese (as Microsoft Bopomofo)
        if (comp) commit_whole(); return Result::Pass;
    }
    if (c && !k.shift && is_raw_key(c)) {
        if (c == ' ' && !comp) return Result::Pass;
        // An initial that is no syllable on its own (ㄅ…ㄒ), then Space: the symbol itself, for writing bopomofo
        if (c == ' ' && settings_.initial_symbol && raw_.size() == 1 && std::strchr("1qaz2wsxedcrfv", raw_[0])) { commit_bopomofo(); return Result::Eaten; }
        if (c == ' ' && is_tone(raw_.back())) {
            // The syllable already has its tone.  AquaType swallows the Space (an extra syllable would turn 注意 into 注意阿,
            // and confirming would commit a first sentence that is often not the one meant); 整句直出 confirms, as
            // Microsoft Bopomofo does.
            if (!sentence_mode()) return Result::Eaten;
            if (waiting_for_current()) wait_for_answer(commit_wait_ms(raw_.size()));
            commit_whole();
            return Result::Eaten;
        }
        if (raw_.size() >= kKeyCap && !is_tone(c)) { host_.beep(); return Result::Eaten; }
        begin_seq(); caret_ = -1; set_raw(raw_ + c); return Result::Eaten;   // typed keys go at the end
    }
    if (c) return punctuation(k, c);
    if (comp) { commit_whole(); return Result::Pass; }                    // F-keys, the numeric keypad, ...: the composition goes first
    return Result::Pass;
}

Result Composer::punctuation(const Key& k, char c) {
    const char p = k.shift ? shifted(c) : c; std::wstring sym;
    switch (p) {
        case '`': sym = L"·"; break; case '=': sym = L"＝"; break; case '[': sym = L"「"; break; case ']': sym = L"」"; break; case '\\': sym = L"、"; break;
        case '{': sym = L"『"; break; case '}': sym = L"』"; break; case '<': sym = L"，"; break; case '>': sym = L"。"; break;
        case '\'': sym = single_quote_open_ ? L"’" : L"‘"; single_quote_open_ = !single_quote_open_; break;
        case '"': sym = double_quote_open_ ? L"”" : L"“"; double_quote_open_ = !double_quote_open_; break;
        default: if (p >= 0x21 && p <= 0x7E) sym = std::wstring(1, (wchar_t)(p + 0xFEE0)); break;      // full width
    }
    if (composing()) commit_whole();
    if (sym.empty()) return Result::Pass;
    host_.commit(sym); return Result::Eaten;
}

// The keys of the last syllable of `raw` (which has no trailing Space): read from the start as initial, medial, final
// and tone, each at most once, as AquaType does - so a syllable typed without its tone before it is not taken along.
static size_t last_syllable_keys(const std::string& raw) {
    auto in = [](const char* set, char c) { return c && std::strchr(set, c) != nullptr; };
    size_t i = 0, start = 0;
    while (i < raw.size()) {
        start = i;
        if (is_tone(raw[i])) { i++; continue; }
        bool any = false;
        if (in("1qaz2wsxedcrfv5tgbyhn", raw[i])) { i++; any = true; }
        if (i < raw.size() && in("ujm", raw[i])) { i++; any = true; }
        if (i < raw.size() && in("8ik,9ol.0p;/-", raw[i])) { i++; any = true; }
        if (!any) i++;
        else if (i < raw.size() && is_tone(raw[i])) i++;
    }
    return raw.size() - start;
}

void Composer::backspace() {
    std::string r = raw_;
    if (!r.empty() && is_tone(r.back())) {                                // a whole syllable: its tone, its keys and a tone-1 Space
        while (!r.empty() && r.back() == ' ') r.pop_back();
        if (!r.empty()) r.erase(r.size() - std::max<size_t>(1, last_syllable_keys(r)));
    } else if (!r.empty()) r.pop_back();
    if (r.empty() || echo(r).empty()) { clear_composition(); return; }
    locks_.erase(locks_.lower_bound((int)segments(r).size()), locks_.end());
    begin_seq(); set_raw(r, /*preserve_panel=*/true);
}

void Composer::clear_composition() {
    raw_.clear(); panel_raw_.clear(); cands_.clear(); highlighted_ = 0; pieces_.clear(); queued_raw_.reset(); begin_seq();
    caret_ = -1; locks_.clear(); close_pick(); panel_version_++;
    host_.set_preedit(L"");
}

void Composer::set_raw(const std::string& raw, bool preserve_panel) {
    raw_ = raw;
    if (raw_.empty()) { clear_composition(); return; }
    refresh_preedit();
    // The old panel stays up until the new answer arrives: after a Backspace as an interim answer for more keys; for a
    // longer input it is simply the last answer.  Neither can be committed (panel_raw_ != raw_).
    (void)preserve_panel;
    request(raw_);
}

// ---------------------------------------------------------------- the engine
void Composer::request(const std::string& raw) {
    if (in_flight_ && GetTickCount64() - in_flight_at_ > reply_timeout_ms(in_flight_raw_.size())) { in_flight_ = false; engine_unavailable_ = true; }   // a reply that never came
    // Keys typed while an answer is out go at once: the engine stops decoding the older keys of this client (the decoder
    // halts at its next step) and answers only the newest, so the answer when typing stops does not wait behind a stale one.
    send_request(raw);
}

void Composer::send_request(const std::string& raw) {
    if (context_seq_ != seq_) {                                           // captured once per generation, then frozen
        std::wstring w = widen(host_.context_before_caret());
        if (w.size() > 256) w.erase(0, w.size() - 256);
        if (!w.empty() && w[0] >= 0xDC00 && w[0] <= 0xDFFF) w.erase(0, 1); // never start inside a surrogate pair
        std::string s = narrow(w);
        while (s.size() > 768) { w.erase(0, (w[0] >= 0xD800 && w[0] <= 0xDBFF) ? 2 : 1); s = narrow(w); }
        context_ = s; context_seq_ = seq_;
    }
    json j = {{"schema", "aquatype-ui-fallback-request/v1"}, {"client_id", client_id_}, {"client_generation", seq_}, {"raw_keys", raw}, {"raw_identity", base64(raw)},
              {"committed_context_utf8", context_}, {"context_identity", sha256_hex(context_)}, {"max_candidates", kMaxCandidates}};
    TR("[send] seq %llu raw %s\n", seq_, raw.c_str());
    if (!engine_.send(j.dump() + "\n")) { TR("[send] failed\n"); engine_unavailable_ = true; return; }
    in_flight_ = true; in_flight_raw_ = raw; in_flight_seq_ = seq_; in_flight_at_ = GetTickCount64();
}

void Composer::handle_line(const std::string& line) {
    json j; try { j = json::parse(line); } catch (...) { return; }
    if (!j.is_object() || !j.contains("schema") || !j["schema"].is_string()) return;
    const std::string schema = j["schema"];
    if (j.value("client_id", std::string()) == pick_client_id_) { handle_pick_line(line); return; }
    if (j.value("client_id", std::string()) != client_id_) return;
    if (schema == "aquatype-ui-failure/v1") {
        if (!j.contains("event_id")) { in_flight_ = false; engine_unavailable_ = true; }
        return;
    }
    if (schema != "aquatype-ui-fallback-response/v1") return;                   // commit acks are not waited for
    const uint64_t gen = j.value("client_generation", (uint64_t)0); const std::string rraw = j.value("raw_keys", std::string());
    TR("[reply] gen %llu raw %s (current seq %llu raw %s)\n", gen, rraw.c_str(), seq_, raw_.c_str());
    if (in_flight_ && gen == in_flight_seq_ && rraw == in_flight_raw_) in_flight_ = false;
    engine_unavailable_ = false;
    const std::string path = j.value("visible_path", std::string());
    const bool decoder = path == "DECODER_V1";
    if (!decoder && path != "DETERMINISTIC_RIME_AQDS_PERSONAL_V1") return;
    const bool current = rraw == raw_ && gen == seq_ && published_seq_ != seq_;
    const bool interim = !current && !rraw.empty() && rraw.size() < raw_.size() && raw_.compare(0, rraw.size(), rraw) == 0 && panel_raw_ != raw_
                         && j.value("context_identity", std::string()) == sha256_hex(context_);
    if (current || interim) {
        std::vector<Candidate> cs; std::set<std::string> seen;
        static const std::set<std::string> all = {"G_DECODER_MEMORY", "G_DECODER_MODEL", "G_NATIVE_AQDS", "G_CHAR_TABLE"}, fallback = {"G_NATIVE_AQDS", "G_CHAR_TABLE"};
        for (auto& r : j.value("candidates", json::array())) {
            Candidate c; c.text = r.value("text_utf8", std::string()); c.wtext = widen(c.text);
            if (c.text.empty() || !all_han(c.wtext) || !host_.drawable(c.wtext)) continue;
            bool ok = true; for (auto& p : r.value("provenance", json::array())) { std::string s = p.get<std::string>(); if (!(decoder ? all : fallback).count(s)) ok = false; c.provenance.push_back(s); }
            if (!ok || c.provenance.empty()) continue;
            if (r.contains("consumed_keys") && r["consumed_keys"].is_number_integer()) c.consumed = r["consumed_keys"].get<int>();
            if (!seen.insert(c.text + "#" + (c.consumed ? std::to_string(*c.consumed) : "all")).second) continue;
            c.group = c.provenance[0] == "G_NATIVE_AQDS" ? 1 : c.provenance[0] == "G_CHAR_TABLE" ? 2 : 0;
            c.memory = std::find(c.provenance.begin(), c.provenance.end(), "G_DECODER_MEMORY") != c.provenance.end();
            cs.push_back(std::move(c)); if (cs.size() >= 512) break;
        }
        if (settings_.bopomofo_candidate) {                               // last, its own group: the bopomofo as typed
            Candidate b; b.wtext = bopomofo_text(rraw); b.text = narrow(b.wtext); b.provenance = {"BOPOMOFO"}; b.group = 3;
            if (!b.wtext.empty()) cs.push_back(std::move(b));
        }
        if (!cs.empty()) { cands_ = std::move(cs); panel_raw_ = rraw; highlighted_ = 0; if (current) published_seq_ = seq_; panel_version_++; }
    }
    // the next request: the newest keys typed while this one was out, or the current keys again when a commit renumbered them
    if (!in_flight_ && !raw_.empty()) {
        if (queued_raw_) { std::string q = *queued_raw_; queued_raw_.reset(); if (q == raw_) send_request(q); else if (published_seq_ != seq_) send_request(raw_); }
        else if (published_seq_ != seq_) send_request(raw_);
    }
}

bool Composer::pump() {
    const size_t before = cands_.size(); const std::string praw = panel_raw_; const uint64_t pub = published_seq_, pv = panel_version_; std::string line; bool any = false;
    while (engine_.next_line(&line, 0)) { handle_line(line); any = true; }
    return any && (cands_.size() != before || panel_raw_ != praw || published_seq_ != pub || panel_version_ != pv);
}

bool Composer::waiting_for_current() const { return composing() && !(panel_raw_ == raw_ && published_seq_ == seq_) && !engine_unavailable_ && waited_seq_ != seq_; }

void Composer::wait_for_answer(uint32_t ms) {
    TR("[wait] seq %llu raw %s in_flight %d queued %d for %u ms\n", seq_, raw_.c_str(), (int)in_flight_, (int)queued_raw_.has_value(), ms);
    waited_seq_ = seq_; waited_from_ = GetTickCount();
    if (!in_flight_ && !queued_raw_) send_request(raw_);
    const uint64_t deadline = GetTickCount64() + ms; std::string line;
    while (!(panel_raw_ == raw_ && published_seq_ == seq_) && !engine_unavailable_) {
        const uint64_t now = GetTickCount64(); if (now >= deadline) { engine_unavailable_ = true; in_flight_ = false; break; }
        if (engine_.next_line(&line, (uint32_t)(deadline - now))) handle_line(line);
    }
    waited_to_ = GetTickCount();
}

// ---------------------------------------------------------------- commits
bool Composer::commit_highlighted() {
    if (cands_.empty() || panel_raw_ != raw_ || highlighted_ < 0 || highlighted_ >= (int)cands_.size()) return false;
    const Candidate c = cands_[highlighted_];
    if (c.consumed && *c.consumed > 0 && *c.consumed < (int)raw_.size()) {   // a piece: commit it and go on composing the rest
        const std::string keys = raw_.substr(0, *c.consumed), rest = raw_.substr(*c.consumed);
        host_.commit(c.wtext);
        if (!host_.secure_input()) record(keys, c.text);
        pieces_.push_back({c.text, keys});
        panel_raw_.clear(); cands_.clear(); highlighted_ = 0; begin_seq(); set_raw(rest);
        return true;
    }
    commit_text(c.wtext, raw_, true); return true;
}

void Composer::commit_whole(bool remember) {
    if (!composing()) return;
    if (waiting_for_current()) wait_for_answer(commit_wait_ms(raw_.size()));
    if (sentence_mode()) {                                                // what is shown, picks included; bopomofo in it is not learned
        const std::wstring t = sentence(); commit_text(t, raw_, remember && all_han(t)); return;
    }
    if (panel_raw_ == raw_ && !cands_.empty()) {
        int pick = -1;
        if (highlighted_ > 0 && highlighted_ < (int)cands_.size() && !cands_[highlighted_].consumed) pick = highlighted_;
        else for (int i = 0; i < (int)cands_.size(); i++) if (!cands_[i].consumed) { pick = i; break; }
        if (pick >= 0) { commit_text(cands_[pick].wtext, raw_, remember); return; }
    }
    commit_bopomofo();                                                    // nothing covers the whole input
}
// the bopomofo itself, which is never remembered
void Composer::commit_bopomofo() {
    const std::wstring e = bopomofo_text(raw_); raw_.clear(); panel_raw_.clear(); cands_.clear(); highlighted_ = 0; pieces_.clear(); queued_raw_.reset(); begin_seq();
    caret_ = -1; locks_.clear(); close_pick(); panel_version_++;
    host_.commit(e);
}

void Composer::commit_text(const std::wstring& text, const std::string& keys, bool remember) {
    host_.commit(text);
    const std::string t = narrow(text);
    if (remember && !host_.secure_input()) {
        record(keys, t);
        if (!pieces_.empty()) {                                           // a sentence committed in pieces is also remembered whole
            std::string all_text, all_keys; for (auto& [pt, pk] : pieces_) { all_text += pt; all_keys += pk; } all_text += t; all_keys += keys;
            const size_t n = code_points(widen(all_text)).size();
            if (pieces_.size() + 1 >= 2 && n >= 2 && n <= kMaxSentenceChars && all_text.size() <= 256 && !all_keys.empty() && all_keys.size() <= 256) record(all_keys, all_text);
        }
    }
    raw_.clear(); panel_raw_.clear(); cands_.clear(); highlighted_ = 0; pieces_.clear(); queued_raw_.reset(); begin_seq();
    caret_ = -1; locks_.clear(); close_pick(); panel_version_++;
}

void Composer::record(const std::string& keys, const std::string& text) {
    if (keys.empty() || text.empty() || !any_han(widen(text))) return;
    json j = {{"schema", "aquatype-ui-personal-commit/v1"}, {"client_id", client_id_}, {"event_id", new_uuid()}, {"client_generation", std::max<uint64_t>(1, seq_)},
              {"raw_keys", keys}, {"raw_identity", base64(keys)}, {"selected_text", text}, {"timestamp_unix_seconds", unix_now()}, {"count_delta", 1}};
    engine_.send(j.dump() + "\n");                                        // fire and forget: the ack is not waited for
}

// ---------------------------------------------------------------- the panel
std::vector<std::vector<int>> Composer::pages_of(const std::vector<Candidate>& list) const {
    std::vector<std::vector<int>> out; std::vector<int> cur; int lines = 0;
    for (int i = 0; i < (int)list.size(); i++) {
        const int need = std::max(1, (int)((code_points(list[i].wtext).size() + line_chars_ - 1) / line_chars_));
        if (!cur.empty() && lines + need > kPanelLines) { out.push_back(cur); cur.clear(); lines = 0; }
        cur.push_back(i); lines += need;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
int Composer::page_of(int index) const { auto ps = pages(); for (int p = 0; p < (int)ps.size(); p++) for (int i : ps[p]) if (i == index) return p; return 0; }
void Composer::move(int to) {
    if (sentence_mode()) { if (!pick_.empty()) { pick_hi_ = std::max(0, std::min((int)pick_.size() - 1, to)); panel_version_++; } return; }
    if (cands_.empty()) return; highlighted_ = std::max(0, std::min((int)cands_.size() - 1, to)); }
void Composer::page(bool down) {
    if (sentence_mode()) {
        if (!choosing_ || pick_.empty()) return;
        auto ps = pages(); const int p = page_of(pick_hi_);
        if (down) { if (p + 1 < (int)ps.size()) pick_hi_ = ps[p + 1][0]; } else pick_hi_ = p > 0 ? ps[p - 1][0] : ps[0][0];
        panel_version_++; return;
    }
    if (cands_.empty() || panel_raw_ != raw_) return;
    auto ps = pages(); int p = page_of(highlighted_);
    if (down) { if (p + 1 < (int)ps.size()) highlighted_ = ps[p + 1][0]; }
    else if (p > 0) highlighted_ = ps[p - 1][0];                          // on the first page it stays where it is, as AquaType
}
PanelView Composer::panel() const {
    PanelView v;
    if (sentence_mode()) {
        if (!choosing_ || pick_.empty()) return v;
        v.cands = &pick_; v.highlighted = pick_hi_; auto ps = pages(); v.rows = ps[std::min((int)ps.size() - 1, page_of(pick_hi_))]; return v;
    }
    v.cands = &cands_; v.highlighted = highlighted_; v.interim = panel_raw_ != raw_;
    if (!cands_.empty()) { auto ps = pages(); v.rows = ps[std::min((int)ps.size() - 1, page_of(highlighted_))]; }
    return v;
}
void Composer::select(int index) {
    if (sentence_mode()) { pick(index); return; }
    if (panel_raw_ != raw_ || index < 0 || index >= (int)cands_.size()) return; highlighted_ = index; commit_highlighted(); }
bool Composer::hover(int index) {
    if (sentence_mode()) { if (index < 0 || index >= (int)pick_.size() || index == pick_hi_) return false; pick_hi_ = index; panel_version_++; return true; }
    if (panel_raw_ != raw_ || index < 0 || index >= (int)cands_.size() || index == highlighted_) return false; highlighted_ = index; return true; }

// ---------------------------------------------------------------- 整句直出
// The composition shows the sentence the engine ranks first, with the characters picked by hand kept in place; keys the
// engine has not answered yet show as bopomofo after it.  ← → move the caret over the characters, ↓ opens the
// candidates for the one at the caret: the characters the other sentences put there, then what the engine offers for
// that syllable alone (asked separately, with the sentence before it as context), then its bopomofo.  A pick holds its
// place while more keys are typed; the whole is committed with Enter, as Microsoft Bopomofo does.
std::vector<std::string> Composer::segments(const std::string& raw) {
    std::vector<std::string> s; std::string cur;
    for (char c : raw) { cur += c; if (is_tone(c)) { s.push_back(cur); cur.clear(); } }
    if (!cur.empty()) s.push_back(cur);
    return s;
}
const Candidate* Composer::best_whole() const {
    const Candidate* first = nullptr;
    for (const auto& c : cands_) {
        if (c.consumed || c.group == 3) continue;
        if (!first) first = &c;
        const auto cp = code_points(c.wtext); bool ok = true;
        for (const auto& [i, t] : locks_) { const auto lc = code_points(t); if (lc.size() == 1 && (i >= (int)cp.size() || cp[i] != lc[0])) { ok = false; break; } }
        if (ok) return &c;
    }
    return first;
}
static bool answered_prefix(const std::string& panel_raw, const std::string& raw) { return !panel_raw.empty() && panel_raw.size() <= raw.size() && raw.compare(0, panel_raw.size(), panel_raw) == 0; }
std::vector<std::wstring> Composer::slots() const {
    std::vector<std::wstring> out; size_t done = 0;
    if (answered_prefix(panel_raw_, raw_)) if (const Candidate* b = best_whole()) { for (char32_t v : code_points(b->wtext)) out.push_back(from_cp(v)); done = panel_raw_.size(); }
    for (const auto& [i, t] : locks_) if (i < (int)out.size()) out[i] = t;
    for (wchar_t ch : bopomofo_text(raw_.substr(done))) out.push_back(std::wstring(1, ch));
    return out;
}
int Composer::chinese_slots() const {
    if (!answered_prefix(panel_raw_, raw_)) return 0;
    const Candidate* b = best_whole(); return b ? (int)code_points(b->wtext).size() : 0;
}
std::wstring Composer::sentence() const { std::wstring t; for (auto& s : slots()) t += s; return t; }
int Composer::target() const { const int n = (int)slots().size(); if (n == 0) return 0; return std::min(caret_ < 0 ? n : caret_, n - 1); }
bool Composer::target_span(int* offset, int* length) const {
    if (!composing() || !sentence_mode()) return false;
    const auto s = slots(); if (s.empty()) return false;
    const int k = std::min(choosing_ ? pick_k_ : target(), (int)s.size() - 1);
    int o = 0; for (int i = 0; i < k; i++) o += (int)s[i].size();
    *offset = o; *length = (int)s[k].size(); return true;
}
void Composer::refresh_preedit() {
    if (!composing()) return;
    if (!sentence_mode()) { host_.set_preedit(echo(raw_)); return; }
    const auto s = slots(); std::wstring t; int caret = -1;
    for (int i = 0; i < (int)s.size(); i++) { if (i == caret_) caret = (int)t.size(); t += s[i]; }
    host_.set_preedit(t, caret);
}
void Composer::open_pick() {
    const auto s = slots(); if (s.empty()) return;
    const int k = target(), n = chinese_slots();
    pick_.clear(); pick_hi_ = 0; pick_k_ = k; choosing_ = true; panel_version_++;
    std::set<std::wstring> seen;
    auto add = [&](const std::wstring& t, int group) { if (t.empty() || !seen.insert(t).second) return; Candidate c; c.wtext = t; c.text = narrow(t); c.group = group; c.provenance = {"PICK"}; pick_.push_back(std::move(c)); };
    add(s[k], 2);
    if (k < n) for (const auto& c : cands_) { if (c.consumed || c.group == 3) continue; const auto cp = code_points(c.wtext); if ((int)cp.size() == n) add(from_cp(cp[k]), 2); }
    pick_engine_from_ = (int)pick_.size(); pick_bopomofo_.clear();
    const auto segs = segments(panel_raw_);
    if (k < n && (int)segs.size() == n) {                                 // one syllable per character: the syllable can be asked about alone
        if (settings_.bopomofo_candidate) pick_bopomofo_ = bopomofo_text(segs[k]);
        std::wstring before = widen(context_); for (int i = 0; i < k; i++) before += s[i];
        if (before.size() > 256) before.erase(0, before.size() - 256);
        if (!before.empty() && before[0] >= 0xDC00 && before[0] <= 0xDFFF) before.erase(0, 1);
        const std::string ctx = narrow(before);
        json j = {{"schema", "aquatype-ui-fallback-request/v1"}, {"client_id", pick_client_id_}, {"client_generation", ++pick_gen_}, {"raw_keys", segs[k]}, {"raw_identity", base64(segs[k])},
                  {"committed_context_utf8", ctx}, {"context_identity", sha256_hex(ctx)}, {"max_candidates", kMaxCandidates}};
        engine_.send(j.dump() + "\n");
    }
    if (!pick_bopomofo_.empty()) add(pick_bopomofo_, 3);
}
void Composer::handle_pick_line(const std::string& line) {
    json j; try { j = json::parse(line); } catch (...) { return; }
    if (!choosing_ || j.value("schema", std::string()) != "aquatype-ui-fallback-response/v1" || j.value("client_generation", (uint64_t)0) != pick_gen_) return;
    std::vector<Candidate> more;
    for (auto& r : j.value("candidates", json::array())) {
        if (r.contains("consumed_keys") && r["consumed_keys"].is_number_integer()) continue;
        const std::wstring w = widen(r.value("text_utf8", std::string()));
        if (!all_han(w) || code_points(w).size() != 1) continue;
        auto same = [&](const Candidate& c) { return c.wtext == w; };
        if (std::any_of(pick_.begin(), pick_.end(), same) || std::any_of(more.begin(), more.end(), same)) continue;
        Candidate c; c.wtext = w; c.text = narrow(w); c.group = 2; c.provenance = {"PICK"}; more.push_back(std::move(c));
        if (more.size() >= 60) break;
    }
    if (more.empty()) return;
    const int at = std::max(0, std::min(pick_engine_from_, (int)pick_.size() - (pick_bopomofo_.empty() ? 0 : 1)));   // before the bopomofo
    pick_.insert(pick_.begin() + at, more.begin(), more.end()); pick_engine_from_ = at + (int)more.size();
    panel_version_++;
}
void Composer::pick(int index) {
    if (!choosing_ || index < 0 || index >= (int)pick_.size()) return;
    const int k = pick_k_; locks_[k] = pick_[index].wtext; close_pick(); panel_version_++;
    const int n = (int)slots().size(); caret_ = k + 1 >= n ? -1 : k + 1;  // on to the next character, as Microsoft Bopomofo does
    refresh_preedit();
}
void Composer::delete_before_caret() {
    const int c = caret_, n = chinese_slots(); const auto segs = segments(panel_raw_);
    if (c <= 0) return;
    if (c > n || (int)segs.size() != n) { host_.beep(); return; }
    std::string keep; for (int i = 0; i < n; i++) if (i != c - 1) keep += segs[i];
    const std::string r = keep + raw_.substr(panel_raw_.size());
    std::map<int, std::wstring> l; for (auto& [i, t] : locks_) { if (i < c - 1) l[i] = t; else if (i > c - 1) l[i - 1] = t; } locks_ = l;
    caret_ = c - 1;
    if (r.empty()) { clear_composition(); return; }
    // the shown sentence loses that character at once; the new answer replaces it
    std::vector<Candidate> cs;
    for (auto cd : cands_) { if (cd.consumed || cd.group == 3) continue; auto cp = code_points(cd.wtext); if ((int)cp.size() != n) continue;
        cp.erase(cp.begin() + (c - 1)); std::wstring w; for (char32_t v : cp) w += from_cp(v); cd.wtext = w; cd.text = narrow(w); cs.push_back(std::move(cd)); }
    cands_ = std::move(cs); panel_raw_ = cands_.empty() ? std::string() : keep;
    begin_seq(); set_raw(r);
}
Result Composer::sentence_key(const Key& k, char, bool* handled) {
    *handled = true;
    if (choosing_) {
        switch (k.vk) {
            case VK_DOWN: move(pick_hi_ + 1); return Result::Eaten;
            case VK_UP: move(pick_hi_ - 1); return Result::Eaten;
            case VK_NEXT: page(true); return Result::Eaten;
            case VK_PRIOR: page(false); return Result::Eaten;
            case VK_RETURN: case VK_SPACE: pick(pick_hi_); return Result::Eaten;
            case VK_ESCAPE: case VK_BACK: close_pick(); panel_version_++; return Result::Eaten;
            case VK_LEFT: case VK_RIGHT: case VK_HOME: case VK_END: close_pick(); panel_version_++; break;   // the list closes, the caret moves
            default:
                if (k.vk >= '1' && k.vk <= '9' && !k.shift) {                  // while the list is open the digits pick, as in Microsoft Bopomofo
                    auto ps = pages(); const int p = page_of(pick_hi_), i = (int)(k.vk - '1');
                    if (i < (int)ps[p].size()) pick(ps[p][i]);
                    return Result::Eaten;
                }
                close_pick(); panel_version_++; *handled = false; return Result::Pass;   // any other key does what it always does
        }
    }
    const int n = (int)slots().size(), c0 = caret_ < 0 ? n : caret_;
    switch (k.vk) {
        case VK_LEFT: caret_ = std::max(0, c0 - 1); refresh_preedit(); return Result::Eaten;
        case VK_RIGHT: caret_ = c0 + 1 >= n ? -1 : c0 + 1; refresh_preedit(); return Result::Eaten;
        case VK_HOME: caret_ = 0; refresh_preedit(); return Result::Eaten;
        case VK_END: caret_ = -1; refresh_preedit(); return Result::Eaten;
        case VK_DOWN: if (waiting_for_current()) wait_for_answer(commit_wait_ms(raw_.size())); open_pick(); refresh_preedit(); return Result::Eaten;
        case VK_UP: case VK_TAB: case VK_NEXT: case VK_PRIOR: return Result::Eaten;
        case VK_BACK: if (c0 < n) { delete_before_caret(); return Result::Eaten; } break;   // at the end: the last syllable, as always
        default: break;
    }
    *handled = false; return Result::Pass;
}

// ---------------------------------------------------------------- modes and focus
void Composer::shift_tapped() {
    if (ascii_mode()) { ascii_global_ = false; ascii_session_ = false; return; }
    if (composing()) {
        if (waiting_for_current()) wait_for_answer(commit_wait_ms(raw_.size()));
        const bool whole = panel_raw_ == raw_ && std::any_of(cands_.begin(), cands_.end(), [](const Candidate& c) { return !c.consumed && c.group != 3; });
        if (!whole) return;                                               // nothing in Chinese to commit: the mode stays
        commit_whole();
    }
    ascii_global_ = true;
}
void Composer::focus_lost() { if (composing()) commit_whole(); pieces_.clear(); ascii_session_ = false; }

}  // namespace aqtip
