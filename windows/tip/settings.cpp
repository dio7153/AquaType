#include "settings.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace aqtip {
namespace {
bool get(const wchar_t* name, DWORD* out) {
    DWORD v = 0, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS) return false;
    *out = v; return true;
}
void get_bool(const wchar_t* name, bool* b) { DWORD v; if (get(name, &v)) *b = v != 0; }
}  // namespace

Settings Settings::load() {
    Settings s;
    get_bool(L"SentencePreedit", &s.sentence_preedit); get_bool(L"ShiftToggle", &s.shift_toggle); get_bool(L"CapsLockEnglish", &s.caps_english);
    get_bool(L"CtrlSpaceToggle", &s.ctrl_space_toggle);
    get_bool(L"InitialSymbol", &s.initial_symbol); get_bool(L"BopomofoCandidate", &s.bopomofo_candidate);
    get_bool(L"FuzzyEnEng", &s.fuzzy_en_eng); get_bool(L"FuzzyAnAng", &s.fuzzy_an_ang); get_bool(L"FuzzyRetroflex", &s.fuzzy_retroflex);
    get_bool(L"LexiconPriority", &s.lexicon_priority);
    DWORD v; if (get(L"CandidateSize", &v) && v <= 2) s.candidate_size = (int)v;
    return s;
}

bool Settings::save() const {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    bool ok = true;
    auto put = [&](const wchar_t* name, DWORD v) { ok = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v) == ERROR_SUCCESS && ok; };
    put(L"SentencePreedit", sentence_preedit); put(L"ShiftToggle", shift_toggle); put(L"CapsLockEnglish", caps_english);
    put(L"CtrlSpaceToggle", ctrl_space_toggle);
    put(L"InitialSymbol", initial_symbol); put(L"BopomofoCandidate", bopomofo_candidate);
    put(L"FuzzyEnEng", fuzzy_en_eng); put(L"FuzzyAnAng", fuzzy_an_ang); put(L"FuzzyRetroflex", fuzzy_retroflex);
    put(L"LexiconPriority", lexicon_priority); put(L"CandidateSize", (DWORD)candidate_size);
    RegCloseKey(k); return ok;
}

std::string Settings::fuzzy_pairs() const {
    std::string p;
    auto add = [&](const char* pairs) { if (!p.empty()) p += ' '; p += pairs; };
    if (fuzzy_en_eng) add("p/");                  // ㄣ ㄥ
    if (fuzzy_an_ang) add("0;");                  // ㄢ ㄤ
    if (fuzzy_retroflex) add("5y th gn");         // ㄓㄗ ㄔㄘ ㄕㄙ
    return p.empty() ? "0" : p;                   // "0": none
}

}  // namespace aqtip
