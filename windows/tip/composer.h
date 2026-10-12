// The composition state machine of the Windows text service: what third_party/squirrel/sources/SquirrelInputController.swift
// does on the Mac, without TSF - keys in, effects out (preedit, commits, the candidate panel, engine requests).  The
// text service drives it from its key event sink and draws what it asks for; tests drive it with a fake host.
//
// Differences from the Mac shell, all deliberate:
//   - A key that ends the composition while the engine's answer is still pending waits for that answer in place (up to
//     the reply timeout) instead of being queued and replayed later.  Same result for the person typing, much less state.
//   - When the engine is unavailable there is no librime to convert with: the composition is committed as bopomofo.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "settings.h"

namespace aqtip {

struct Key {
    uint32_t vk = 0;                 // virtual-key code
    bool shift = false, ctrl = false, alt = false, caps = false;
    uint32_t time = 0;               // when it was pressed (GetMessageTime / GetTickCount); 0: unknown
};

struct Candidate {
    std::string text;                // UTF-8, as the engine sent it
    std::wstring wtext;
    std::vector<std::string> provenance;
    std::optional<int> consumed;     // keys it accounts for; nullopt = all of them
    int group = 0;                   // 0 sentences, 1 words, 2 characters, 3 the bopomofo itself
    bool memory = false;             // shown with the 「記憶」 note
};

struct PanelView {                   // what the candidate window shows
    std::vector<int> rows;           // indices into the candidate list, this page
    int highlighted = -1;            // index into the candidate list
    const std::vector<Candidate>* cands = nullptr;
    bool interim = false;            // an answer for fewer keys than typed
};

// What the text service does for the composer.  Called synchronously, inside the key's edit session.
class Host {
public:
    virtual ~Host() = default;
    // start or update the composition; empty ends it with nothing inserted.  caret: where the caret goes inside it, in
    // UTF-16 units from its start; -1 at the end
    virtual void set_preedit(const std::wstring& text, int caret = -1) = 0;
    virtual void commit(const std::wstring& text) = 0;           // insert text in place of the composition, ending it
    virtual void beep() = 0;
    virtual std::string context_before_caret() = 0;             // up to 256 UTF-16 units before the insertion point, UTF-8; "" when unknown
    virtual bool secure_input() = 0;                             // a password field: nothing is remembered
    // Whether every character of a candidate has a font to draw it (else it shows as a box): AquaType leaves out the
    // ones that would not show.
    virtual bool drawable(const std::wstring&) { return true; }
};

// The engine connection, as the composer needs it.
class EngineLink {
public:
    virtual ~EngineLink() = default;
    virtual bool send(const std::string& line) = 0;             // false when there is no engine
    virtual bool next_line(std::string* line, uint32_t wait_ms) = 0;   // the next reply line, waiting at most wait_ms
};

enum class Result { Eaten, Pass };

class Composer {
public:
    Composer(Host& host, EngineLink& engine);
    void set_settings(const Settings& s) { settings_ = s; }
    const Settings& settings() const { return settings_; }
    // Would key() eat this key?  No side effects (OnTestKeyDown).
    bool wants(const Key& k) const;
    Result key(const Key& k);
    // Shift alone: down then up with nothing in between, under 0.5 s.  The text service tracks the tap and calls this.
    void shift_tapped();
    void note_other_key() {}
    // Replies that arrived while nothing was waiting for them; returns true when the panel changed.
    bool pump();
    // A click on a candidate row.
    void select(int index);
    void page(bool down);
    // The pointer over a candidate row: it becomes the highlighted one (returns true when that changed).
    bool hover(int index);
    // How many full-width characters a line of the candidate window holds: a page has nine lines, and a long candidate
    // takes as many as it wraps to (the window works it out from the screen, as the Mac panel does).
    void set_line_chars(int n) { line_chars_ = std::max(8, n); }
    // Focus left the field or the input method was switched away: commit what is there.
    void focus_lost();

    bool composing() const { return !raw_.empty(); }
    bool ascii_mode() const { return ascii_global_ || ascii_session_; }
    void reset_session_mode() { ascii_session_ = false; }
    void set_ascii(bool ascii) { ascii_global_ = ascii; ascii_session_ = false; }   // the mode chosen from outside (the taskbar indicator)
    PanelView panel() const;
    bool panel_visible() const { return composing() && (!sentence_mode() || choosing_); }
    // 整句直出 (the default): the composition shows the sentence itself, as Microsoft Bopomofo does; ← → move inside it
    // and ↓ opens the candidates for the character at the caret.  Off: the bopomofo is shown and picked from the panel.
    bool sentence_mode() const { return settings_.sentence_preedit; }
    // The character the candidates are for, in UTF-16 units from the composition's start (the panel lines up with it).
    bool target_span(int* offset, int* length) const;
    // Shows the composition again (an answer arrived while no key was being handled).
    void refresh_preedit();

    static std::wstring echo(const std::string& raw);              // the bopomofo shown inline
    static std::wstring bopomofo_text(const std::string& raw);     // the bopomofo as text: no spaces, no first-tone mark
    static bool is_raw_key(char c);
    static char key_char(uint32_t vk);                              // the US-layout character of a key, 0 for none

private:
    void begin_seq();
    void set_raw(const std::string& raw, bool preserve_panel = false);
    void request(const std::string& raw);
    void send_request(const std::string& raw);
    void handle_line(const std::string& line);
    bool waiting_for_current() const;
    void wait_for_answer(uint32_t ms);
    uint32_t reply_timeout_ms(size_t keys) const { return 1500 + 20 * (uint32_t)keys; }
    // A key that commits waits longer: the person asked for the text, and the only thing left without an answer is the
    // bopomofo itself (there is no librime conversion to fall back on here).  A busy machine can take seconds.
    uint32_t commit_wait_ms(size_t keys) const { return 5000 + 20 * (uint32_t)keys; }

    bool commit_highlighted();                                       // Return / click: partial commits allowed
    void commit_whole(bool remember = true);                         // punctuation, Shift, focus loss: the whole input
    void commit_text(const std::wstring& text, const std::string& keys, bool remember);
    void commit_bopomofo();
    void record(const std::string& keys, const std::string& text);
    void clear_composition();
    void backspace();
    void move(int to);
    std::vector<std::vector<int>> pages() const { return pages_of(sentence_mode() ? pick_ : cands_); }
    std::vector<std::vector<int>> pages_of(const std::vector<Candidate>& list) const;
    int page_of(int index) const;
    // 整句直出
    static std::vector<std::string> segments(const std::string& raw);   // each syllable's keys, its tone key last; the last may be unfinished
    const Candidate* best_whole() const;                                 // the sentence shown: the best one that keeps the characters picked
    std::vector<std::wstring> slots() const;                             // the composition, one entry per character (a pick may be bopomofo)
    int chinese_slots() const;                                           // how many leading slots the engine's answer accounts for
    std::wstring sentence() const;
    int target() const;
    void open_pick();
    void pick(int index);
    void close_pick() { choosing_ = false; pick_.clear(); pick_hi_ = 0; }
    void delete_before_caret();
    Result sentence_key(const Key& k, char c, bool* handled);
    void handle_pick_line(const std::string& line);
    Result punctuation(const Key& k, char c);

    Host& host_; EngineLink& engine_;
    Settings settings_;
    std::string client_id_;
    std::string raw_; uint64_t seq_ = 0;
    std::string context_; uint64_t context_seq_ = 0;
    // the panel
    std::string panel_raw_; std::vector<Candidate> cands_; int highlighted_ = 0; uint64_t published_seq_ = 0;
    // the request in flight and the one queued behind it
    bool in_flight_ = false; std::string in_flight_raw_; uint64_t in_flight_seq_ = 0; uint64_t in_flight_at_ = 0;
    std::optional<std::string> queued_raw_;
    bool engine_unavailable_ = false; int retries_ = 0; std::string retry_raw_;
    uint64_t waited_seq_ = 0;
    uint32_t waited_from_ = 0, waited_to_ = 0;   // the last wait for an answer before a commit (GetTickCount)
    bool pressed_while_waiting(const Key& k) const { return k.time && waited_to_ && k.time - waited_from_ <= waited_to_ - waited_from_; }
    std::vector<std::pair<std::string, std::string>> pieces_;          // (text, keys) committed in parts
    bool ascii_global_ = false, ascii_session_ = false;
    bool single_quote_open_ = false, double_quote_open_ = false;
    // 整句直出: the caret (in characters, -1 = at the end), the characters picked by hand (by position), the list open
    int caret_ = -1; std::map<int, std::wstring> locks_;
    int line_chars_ = 20;
    bool choosing_ = false; std::vector<Candidate> pick_; int pick_hi_ = 0, pick_k_ = 0, pick_engine_from_ = 0; uint64_t pick_gen_ = 0;
    std::string pick_client_id_; std::wstring pick_bopomofo_;
    uint64_t panel_version_ = 0;
};

}  // namespace aqtip
