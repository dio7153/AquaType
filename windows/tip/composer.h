// The composition state machine of the Windows text service: what third_party/squirrel/sources/SquirrelInputController.swift
// does on the Mac, without TSF - keys in, effects out (preedit, commits, the candidate panel, engine requests).  The
// text service drives it from its key event sink and draws what it asks for; tests drive it with a fake host.
//
// Differences from the Mac shell, all deliberate:
//   - A key that ends the composition while the engine's answer is still pending waits for that answer in place (up to
//     the reply timeout) instead of being queued and replayed later.  Same result for the person typing, much less state.
//   - When the engine is unavailable there is no librime to convert with: the composition is committed as bopomofo.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aqtip {

struct Key {
    uint32_t vk = 0;                 // virtual-key code
    bool shift = false, ctrl = false, alt = false, caps = false;
};

struct Candidate {
    std::string text;                // UTF-8, as the engine sent it
    std::wstring wtext;
    std::vector<std::string> provenance;
    std::optional<int> consumed;     // keys it accounts for; nullopt = all of them
    int group = 0;                   // 0 sentences, 1 words, 2 characters
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
    virtual void set_preedit(const std::wstring& text) = 0;      // start or update the composition; empty ends it with nothing inserted
    virtual void commit(const std::wstring& text) = 0;           // insert text in place of the composition, ending it
    virtual void beep() = 0;
    virtual std::string context_before_caret() = 0;             // up to 256 UTF-16 units before the insertion point, UTF-8; "" when unknown
    virtual bool secure_input() = 0;                             // a password field: nothing is remembered
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
    // Focus left the field or the input method was switched away: commit what is there.
    void focus_lost();

    bool composing() const { return !raw_.empty(); }
    bool ascii_mode() const { return ascii_global_ || ascii_session_; }
    void reset_session_mode() { ascii_session_ = false; }
    void set_ascii(bool ascii) { ascii_global_ = ascii; ascii_session_ = false; }   // the mode chosen from outside (the taskbar indicator)
    PanelView panel() const;
    bool panel_visible() const { return composing(); }

    static std::wstring echo(const std::string& raw);              // the bopomofo shown inline
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

    bool commit_highlighted();                                       // Return / click: partial commits allowed
    void commit_whole(bool remember = true);                         // punctuation, Shift, focus loss: the whole input
    void commit_text(const std::wstring& text, const std::string& keys, bool remember);
    void record(const std::string& keys, const std::string& text);
    void clear_composition();
    void backspace();
    void move(int to);
    std::vector<std::vector<int>> pages() const;
    int page_of(int index) const;
    Result punctuation(const Key& k, char c);

    Host& host_; EngineLink& engine_;
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
    std::vector<std::pair<std::string, std::string>> pieces_;          // (text, keys) committed in parts
    bool ascii_global_ = false, ascii_session_ = false;
    bool single_quote_open_ = false, double_quote_open_ = false;
};

}  // namespace aqtip
