// The person's choices, kept in HKCU\Software\AquaType (DWORD values) and shared by the text service, the engine and
// AquaTypeSettings.exe.  Missing values take the defaults below.  The settings app broadcasts kSettingsChangedMessage
// after saving; the text service rereads on that and on activation, the engine reads once at start (the settings app
// restarts it).
#pragma once
#include <string>

namespace aqtip {

inline constexpr wchar_t kSettingsKey[] = L"Software\\AquaType";
inline constexpr wchar_t kSettingsChangedMessage[] = L"AquaTypeSettingsChanged";   // RegisterWindowMessage name

struct Settings {
    bool sentence_preedit = true;        // 整句直出: the composition shows the sentence; ← → ↓ to go back and pick (Microsoft Bopomofo's way)
    bool shift_toggle = true;            // a Shift tap switches 中/英
    bool caps_english = false;           // CapsLock on types English (Microsoft Bopomofo's way); off: CapsLock is ignored
    bool ctrl_space_toggle = true;       // Ctrl+Space switches 中/英 (Microsoft Bopomofo's way)
    bool initial_symbol = true;          // an initial alone then Space types the symbol itself (ㄅ␣ → ㄅ)
    bool bopomofo_candidate = true;      // the bopomofo itself, last in the list (the 「注」 group)
    bool fuzzy_en_eng = true;            // ㄣ/ㄥ
    bool fuzzy_an_ang = true;            // ㄢ/ㄤ
    bool fuzzy_retroflex = true;         // ㄓㄗ ㄔㄘ ㄕㄙ
    bool lexicon_priority = true;        // dictionary words before the model's guesses for 2-4 characters
    int candidate_size = 1;              // 0 small, 1 medium, 2 large

    bool operator==(const Settings& o) const {
        return sentence_preedit == o.sentence_preedit && shift_toggle == o.shift_toggle && caps_english == o.caps_english && ctrl_space_toggle == o.ctrl_space_toggle && initial_symbol == o.initial_symbol && bopomofo_candidate == o.bopomofo_candidate
            && fuzzy_en_eng == o.fuzzy_en_eng && fuzzy_an_ang == o.fuzzy_an_ang && fuzzy_retroflex == o.fuzzy_retroflex && lexicon_priority == o.lexicon_priority
            && candidate_size == o.candidate_size;
    }
    static Settings load();
    bool save() const;
    std::string fuzzy_pairs() const;     // the decoder's AQ_FUZZY: key pairs separated by spaces, "0" for none
    float candidate_text_size() const { return candidate_size == 0 ? 16.f : candidate_size == 2 ? 22.f : 18.f; }
};

}  // namespace aqtip
