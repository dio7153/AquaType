// AquaTypeEngine for Windows: the same program as Sources/AquaTypeEngine/main.swift - the input method starts it as a
// child process and talks to it in JSON lines over stdin/stdout - in C++, on the decoder's C API (aquadecoder.dll, the
// CPU backend).  Every rule below is the Swift engine's; the comments there carry the reasons.
//   AquaTypeEngine --command serve-ui-jsonl --fallback-aqds F --fallback-manifest M
//                  [--decoder-dir D] [--metallib L] [--char-to-keys C] [--char-readings R] [--state-dir S]
//                  (R: a character's other readings, terra_pinyin.dict.yaml; by default the one beside C, if there)
//   AquaTypeEngine --check-char-coverage C
//   AquaTypeEngine --selftest [--raw K] --decoder-dir D ...
//   AquaTypeEngine --version
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "aquadecoder.h"
#include "aqds.h"
#include "memory.h"
#include "settings.h"
#include "util.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

using namespace aqw;
using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

// ---------------------------------------------------------------- the contract (Sources/AquaTypeUIContract/UIFallback.swift)
namespace contract {
const char* request_schema = "aquatype-ui-fallback-request/v1";
const char* response_schema = "aquatype-ui-fallback-response/v1";
const char* commit_schema = "aquatype-ui-personal-commit/v1";
const char* ack_schema = "aquatype-ui-personal-commit-ack/v1";
const char* failure_schema = "aquatype-ui-failure/v1";
const char* visible_path = "DETERMINISTIC_RIME_AQDS_PERSONAL_V1";
const char* decoder_visible_path = "DECODER_V1";
const char* src_aqds = "G_NATIVE_AQDS"; const char* src_memory = "G_DECODER_MEMORY"; const char* src_model = "G_DECODER_MODEL"; const char* src_chars = "G_CHAR_TABLE";

bool is_han(char32_t v) {
    return (v >= 0x3400 && v <= 0x4DBF) || (v >= 0x4E00 && v <= 0x9FFF) || (v >= 0xF900 && v <= 0xFAFF) || (v >= 0x20000 && v <= 0x2A6DF)
        || (v >= 0x2A700 && v <= 0x2EBEF) || (v >= 0x2F800 && v <= 0x2FA1F) || (v >= 0x30000 && v <= 0x323AF);
}
bool is_cjk(const std::string& t) { if (t.empty()) return false; for (char32_t c : code_points(t)) if (!is_han(c)) return false; return true; }
std::string identity(const std::string& raw) { return base64(raw); }
std::string context_identity(const std::string& ctx) { return sha256_hex(ctx); }
bool printable_id(const std::string& s) { if (s.empty() || s.size() > 128) return false; for (unsigned char c : s) if (c < 0x21 || c > 0x7E) return false; return true; }
bool printable_keys(const std::string& s) { if (s.empty() || s.size() > 256) return false; for (unsigned char c : s) if (c < 0x20 || c > 0x7E) return false; return true; }
struct invalid : std::runtime_error { using std::runtime_error::runtime_error; };

struct Request {
    std::string client_id, raw_keys, raw_identity, committed, context_identity; uint64_t generation = 0; int max_candidates = 9;
    static Request parse(const json& j) {
        Request r; if (j.at("schema").get<std::string>() != request_schema) throw invalid("invalid UI fallback request envelope");
        r.client_id = j.at("client_id").get<std::string>(); r.generation = j.at("client_generation").get<uint64_t>(); r.raw_keys = j.at("raw_keys").get<std::string>();
        r.raw_identity = j.at("raw_identity").get<std::string>(); r.max_candidates = j.at("max_candidates").get<int>();
        if (j.contains("committed_context_utf8")) { r.committed = j.at("committed_context_utf8").get<std::string>(); r.context_identity = j.at("context_identity").get<std::string>(); }
        else { if (j.contains("context_identity")) throw invalid("context identity requires an explicit context field"); r.context_identity = contract::context_identity(""); }
        return r;
    }
    void validate() const {
        if (!printable_id(client_id) || generation == 0 || !printable_keys(raw_keys) || raw_identity != identity(raw_keys)
            || committed.size() > 768 || utf16_units(committed) > 256 || context_identity != contract::context_identity(committed) || max_candidates < 1 || max_candidates > 64)
            throw invalid("invalid UI fallback request envelope");
    }
};
struct Commit {
    std::string client_id, event_id, raw_keys, raw_identity, text; uint64_t generation = 0; int64_t timestamp = 0; int count_delta = 1;
    static Commit parse(const json& j) {
        Commit c; if (j.at("schema").get<std::string>() != commit_schema) throw invalid("invalid UI personal commit envelope");
        c.client_id = j.at("client_id").get<std::string>(); c.event_id = j.at("event_id").get<std::string>(); c.generation = j.at("client_generation").get<uint64_t>();
        c.raw_keys = j.at("raw_keys").get<std::string>(); c.raw_identity = j.at("raw_identity").get<std::string>(); c.text = j.at("selected_text").get<std::string>();
        c.timestamp = j.at("timestamp_unix_seconds").get<int64_t>(); c.count_delta = j.at("count_delta").get<int>(); return c;
    }
    void validate() const {
        bool text_ok = !text.empty() && text.size() <= 256, han = false;
        for (char32_t cp : code_points(text)) { if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) text_ok = false; han |= is_han(cp); }   // CharacterSet.controlCharacters: Cc
        if (!printable_id(client_id) || !printable_id(event_id) || generation == 0 || !printable_keys(raw_keys) || raw_identity != identity(raw_keys)
            || !text_ok || !han || timestamp < 0 || timestamp > 4102444800LL || count_delta != 1)
            throw invalid("invalid UI personal commit envelope");
    }
};
struct Candidate { std::string text; std::vector<std::string> provenance; std::optional<int> consumed; };
}  // namespace contract

// ---------------------------------------------------------------- the tables
struct CharKeyIndex {
    size_t count = 0; std::unordered_map<std::string, std::vector<std::string>> by_raw;
    explicit CharKeyIndex(const std::wstring& path) {
        const json d = json::parse(read_file(path)); if (!d.is_object()) throw std::runtime_error("char_to_keys.json must be {char: keys}");
        for (auto it = d.begin(); it != d.end(); ++it) by_raw[it.value().get<std::string>()].push_back(it.key());   // nlohmann's object is sorted by key, as the Swift side sorts
        count = d.size();
    }
    // char_to_keys.json gives each character one reading; 行 typed ㄏㄤˊ or 蔚 typed ㄩˋ has no character there.  The
    // other readings come from a Rime dictionary ("char<TAB>pinyin<TAB>share%", terra_pinyin): a reading with at least
    // `min_share` percent (below that they are mostly tone sandhi, 一 yi2 1%) is added under its keys.  Pinyin is turned
    // into keys by what the table itself types for the characters that have one reading only.  A reading used less than
    // the table's own sorts after the characters of its frequency tier (ranked / known / other) that read it as their table reading.
    std::set<std::string> minor;      // raw + '\t' + char
    size_t added = 0;
    void add_readings(const std::wstring& path, double min_share = 5.0) {
        std::map<std::string, std::vector<std::pair<std::string, double>>> readings; std::map<std::string, std::string> table;
        for (auto& [raw, chs] : by_raw) for (auto& ch : chs) table[ch] = raw;
        const std::string text = read_file(path); bool body = false; size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at); if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at); at = end + 1; if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!body) { body = line.rfind("...", 0) == 0; continue; }
            if (line.empty() || line[0] == '#') continue;
            const size_t t1 = line.find('\t'); if (t1 == std::string::npos) continue;
            const size_t t2 = line.find('\t', t1 + 1);
            const std::string ch = line.substr(0, t1), py = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
            if (code_points(ch).size() != 1 || !table.count(ch)) continue;
            double share = -1; if (t2 != std::string::npos && line.back() == '%') share = std::atof(line.c_str() + t2 + 1);
            readings[ch].push_back({py, share});
        }
        std::map<std::string, std::map<std::string, int>> votes;
        for (auto& [ch, rs] : readings) if (rs.size() == 1) votes[rs[0].first][table[ch]]++;
        std::map<std::string, std::string> keys_of;
        for (auto& [py, v] : votes) keys_of[py] = std::max_element(v.begin(), v.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        for (auto& [ch, rs] : readings) {
            if (rs.size() < 2) continue;
            const std::string& own = table[ch]; double own_share = 0; std::map<std::string, double> by_keys;
            for (auto& [py, share] : rs) { auto k = keys_of.find(py); if (k == keys_of.end()) continue; const double s = share < 0 ? 100 : share;
                if (k->second == own) own_share = std::max(own_share, s); else by_keys[k->second] = std::max(by_keys[k->second], s); }
            for (auto& [keys, s] : by_keys) { if (s < min_share) continue;
                by_raw[keys].push_back(ch); added++; if (s < own_share) minor.insert(keys + "\t" + ch); }
        }
    }
    bool is_minor(const std::string& raw, const std::string& ch) const { return minor.count(raw + "\t" + ch) != 0; }
    const std::vector<std::string>& chars(const std::string& raw) const { static const std::vector<std::string> none; auto it = by_raw.find(raw); return it == by_raw.end() ? none : it->second; }
    std::vector<std::string> coverage_gaps() const {
        static const std::string keyboard = "1234567890-qwertyuiop asdfghjkl;zxcvbnm,./"; std::vector<std::string> missing;
        for (auto& [raw, chs] : by_raw) { bool typable = !raw.empty() && raw.find_first_not_of(keyboard) == std::string::npos;
            for (auto& ch : chs) if (!typable || code_points(ch).size() != 1) missing.push_back(ch); }
        std::sort(missing.begin(), missing.end()); return missing;
    }
};

struct CharFrequency {
    std::unordered_map<std::string, int> rank; std::set<std::string> known; bool ok = false;
    explicit CharFrequency(const std::wstring& dir) {
        try {
            const json t = json::parse(read_file(dir + L"\\tables.json")); std::vector<std::string> chars = t.at("chars").get<std::vector<std::string>>(); if (chars.empty()) return;
            known.insert(chars.begin(), chars.end());
            const std::string r = read_file(dir + L"\\norm_rank.bin"); if (r.size() < 4) return;
            int32_t n; std::memcpy(&n, r.data(), 4); if (n <= 0 || r.size() < 4 + (size_t)n * 4) return;
            for (int32_t i = 0; i < n; i++) { int32_t id; std::memcpy(&id, r.data() + 4 + (size_t)i * 4, 4); if (id >= 0 && id < (int)chars.size() && !rank.count(chars[id])) rank[chars[id]] = i; }
            ok = true;
        } catch (...) {}
    }
    std::pair<int, int> order(const std::string& ch) const { auto it = rank.find(ch); if (it != rank.end()) return {0, it->second}; return {known.count(ch) ? 1 : 2, INT_MAX}; }
};

namespace bopomofo {
const std::string tone_keys = "3467";              // sorted, as the Swift side iterates them
bool is_tone(char c) { return tone_keys.find(c) != std::string::npos; }
bool ends_syllable(char c) { return c == ' ' || is_tone(c); }
const std::map<char, std::vector<char>>& neighbours() {
    static const std::map<char, std::vector<char>> m = [] {
        const std::vector<std::string> rows = {"1234567890-", "qwertyuiop", "asdfghjkl;", "zxcvbnm,./"}; std::map<char, std::set<char>> s;
        for (int ri = 0; ri < (int)rows.size(); ri++) for (int ci = 0; ci < (int)rows[ri].size(); ci++) { auto& n = s[rows[ri][ci]];
            if (ci > 0) n.insert(rows[ri][ci - 1]); if (ci + 1 < (int)rows[ri].size()) n.insert(rows[ri][ci + 1]);
            for (int dr : {-1, 1}) { int r2 = ri + dr; if (r2 < 0 || r2 >= (int)rows.size()) continue; for (int dc : {0, -1}) { int c2 = ci + dc; if (c2 >= 0 && c2 < (int)rows[r2].size()) n.insert(rows[r2][c2]); } } }
        std::map<char, std::vector<char>> out; for (auto& [k, v] : s) out[k] = std::vector<char>(v.begin(), v.end()); return out; }();
    return m;
}
}  // namespace bopomofo

// ---------------------------------------------------------------- the decoder session
class DecoderSession {
public:
    aq_decoder* handle = nullptr; std::vector<std::string> syllable_keys;
    static std::unique_ptr<DecoderSession> open(const std::string& dir, const char* metallib) {
        aq_decoder* d = aq_open(dir.c_str(), metallib); if (!d) { log(std::string("[engine] decoder unavailable: ") + aq_last_error()); return nullptr; }
        auto s = std::make_unique<DecoderSession>(); s->handle = d;
        try { s->syllable_keys = json::parse(read_file(widen(dir) + L"\\tables.json")).at("syllables").get<std::vector<std::string>>(); }
        catch (...) { log("[engine] tables.json has no syllables; the third source cannot follow the model's reading"); }
        return s;
    }
    std::optional<std::string> keys_of_reading(int id) const { if (id < 0 || id >= (int)syllable_keys.size()) return std::nullopt; std::string k; for (char c : syllable_keys[id]) if (c != ' ') k += c; return k; }
    std::optional<json> compose(const std::string& raw, const std::string& ctx) {
        char* out = aq_decode(handle, raw.c_str(), ctx.empty() ? nullptr : ctx.c_str());
        if (!out) { log(std::string("[engine] aq_decode failed: ") + aq_last_error()); return std::nullopt; }
        std::string s(out); aq_free(out);
        try { json p = json::parse(s); if (p.contains("candidates") && p["candidates"].is_array()) return p["candidates"]; } catch (...) {}
        log("[engine] compose JSON unexpected: " + std::to_string(s.size()) + " bytes"); return std::nullopt;
    }
    int candidate_budget() const { return 63; }
    std::vector<int32_t> readings(const std::string& keys) {
        if (keys.empty()) return {}; char* out = aq_readings(handle, keys.c_str()); if (!out) return {};
        std::string s(out); aq_free(out); try { return json::parse(s).get<std::vector<int32_t>>(); } catch (...) { return {}; }
    }
    void end_sentence() { aq_reset(handle); }
    int32_t char_id(const std::string& ch) { return aq_char_id(handle, ch.c_str()); }

    static std::string previous_sentence(const std::string& committed) {
        static const std::set<char32_t> enders = {U'。', U'！', U'？', U'!', U'?', U'；', U';', U'…', U'\n', U'\r'};
        static const std::set<char32_t> edge = {U' ', U'\t', U'\r', U'　', U'「', U'」', U'『', U'』', U'（', U'）', U'(', U')', U'《', U'》', U'〈', U'〉', U'【', U'】', U',', U'，', U'、', U':', U'：', U'“', U'”', U'"', U'\''};
        std::vector<std::vector<char32_t>> pieces; std::vector<char32_t> cur;
        for (char32_t c : code_points(committed)) { if (enders.count(c)) { if (!cur.empty()) { pieces.push_back(cur); cur.clear(); } } else cur.push_back(c); }
        if (pieces.empty()) return "";
        auto& last = pieces.back(); size_t a = 0, b = last.size();
        while (a < b && edge.count(last[a])) a++; while (b > a && edge.count(last[b - 1])) b--;
        return utf8(last, a, b);
    }
    std::vector<std::string> prev_sentence_tokens(const std::string& committed, const std::string& client) {
        std::string prev = previous_sentence(committed);
        if (prev.empty()) { auto t = last_commit_text.find(client); auto at = last_commit_at.find(client);
            if (t != last_commit_text.end() && !t->second.empty() && (double)unix_now() - (at == last_commit_at.end() ? 0.0 : at->second) <= commit_context_lifetime) prev = t->second; }
        std::vector<char32_t> cps = code_points(prev); size_t from = cps.size() > 64 ? cps.size() - 64 : 0; std::vector<std::string> out;
        for (size_t i = from; i < cps.size(); i++) out.push_back("{\"kind\":0,\"chars\":[" + std::to_string(char_id(utf8(cps[i]))) + "],\"weights\":[1.0]}");
        return out;
    }
    std::map<std::string, json> last_rows_by_client; std::map<std::string, std::string> last_commit_text; std::map<std::string, double> last_commit_at;
    std::deque<std::string> clients_by_recency;
    static constexpr int clients_kept = 16, rows_kept = 64; static constexpr double commit_context_lifetime = 120;
    void keep_rows(const json& rows, const std::string& client) { json kept = json::array(); for (size_t i = 0; i < rows.size() && (int)i < rows_kept; i++) kept.push_back(rows[i]); last_rows_by_client[client] = kept; touch(client); }
    void touch(const std::string& client) {
        clients_by_recency.erase(std::remove(clients_by_recency.begin(), clients_by_recency.end(), client), clients_by_recency.end()); clients_by_recency.push_back(client);
        while ((int)clients_by_recency.size() > clients_kept) { std::string gone = clients_by_recency.front(); clients_by_recency.pop_front(); last_rows_by_client.erase(gone); last_commit_text.erase(gone); last_commit_at.erase(gone); }
    }
    std::optional<json> decode(const std::string& client, const std::string& raw, const std::string& committed) {
        auto tokens = prev_sentence_tokens(committed, client); std::string ctx;
        if (!tokens.empty()) { ctx = "["; for (size_t i = 0; i < tokens.size(); i++) { if (i) ctx += ","; ctx += tokens[i]; } ctx += "]"; }
        return compose(raw, ctx);
    }
    void note_commit(const std::string& client, const std::string& text) { last_commit_text[client] = text; last_commit_at[client] = (double)unix_now(); touch(client); }
    void warm_up() { compose("u04vu/6", ""); compose("5j/ 2u6ru03g4jp6ej4", ""); aq_reset(handle); }
};

// ---------------------------------------------------------------- the engine
static std::vector<int32_t> reading_ids(const json& row) { std::vector<int32_t> r; if (row.contains("reading") && row["reading"].is_array()) for (auto& v : row["reading"]) r.push_back(v.get<int32_t>()); return r; }

struct Engine {
    std::unique_ptr<AQDSFallbackStore> fallback; std::unique_ptr<CharKeyIndex> char_index; std::unique_ptr<DecoderSession> decoder;
    std::unique_ptr<CharFrequency> freq; std::unique_ptr<PersonalMemoryStore> memory;

    ojson respond(const contract::Request& req) {
        using namespace contract;
        size_t limit = (size_t)req.max_candidates; std::vector<Candidate> output; std::unordered_map<std::string, size_t> index_by_text;
        auto add = [&](const std::string& text, const char* source, std::optional<int> consumed = std::nullopt) {
            if (text.empty() || !is_cjk(text)) return;
            const std::string key = consumed ? text + "#" + std::to_string(*consumed) : text;
            auto it = index_by_text.find(key);
            if (it != index_by_text.end()) { auto& p = output[it->second].provenance; if (std::find(p.begin(), p.end(), source) == p.end()) p.push_back(source); }
            else if (output.size() < limit) { index_by_text[key] = output.size(); output.push_back({text, {source}, consumed}); }
        };
        std::string visible = visible_path; std::optional<json> model_top;
        if (decoder) if (auto rows = decoder->decode(req.client_id, req.raw_keys, req.committed)) {
            rerank_by_lexicon(*rows);
            visible = decoder_visible_path; decoder->keep_rows(*rows, req.client_id);
            if (!rows->empty()) model_top = (*rows)[0];
            const int64_t now = unix_now();
            if (memory) for (auto& t : memory->exact_recall(req.raw_keys, now)) add(t, src_memory);
            limit = (size_t)std::min(req.max_candidates, decoder->candidate_budget()) + output.size();
            std::set<std::string> model_texts; for (auto& row : *rows) if (row.contains("text") && row["text"].is_string()) model_texts.insert(row["text"].get<std::string>());
            for (size_t rank = 0; rank < rows->size(); rank++) { const json& row = (*rows)[rank]; if (!row.contains("text") || !row["text"].is_string()) continue;
                const std::string text = row["text"].get<std::string>(); std::string fused; const bool has = memory && memory->fuse(text, reading_ids(row), now, &fused);
                if (has && (rank == 0 || !model_texts.count(fused))) add(fused, src_model);
                add(text, src_model);
                if (has && rank != 0 && model_texts.count(fused)) add(fused, src_model); }
        }
        limit = SIZE_MAX;
        const std::string& raw = req.raw_keys; const int raw_count = (int)raw.size();

        struct Syl { std::string keys; int raw_count; }; std::vector<Syl> syllables; std::string pending;
        for (char k : raw) { pending += k;
            if (bopomofo::ends_syllable(k)) { std::string keys; for (char c : pending) if (c != ' ') keys += c;
                if (keys.empty() && !syllables.empty()) syllables.back().raw_count += (int)pending.size(); else syllables.push_back({keys, (int)pending.size()});
                pending.clear(); } }
        auto consumed = [&](size_t count) -> std::optional<int> { int used = 0; for (size_t i = 0; i < count; i++) used += syllables[i].raw_count; return used == raw_count ? std::nullopt : std::optional<int>(used); };
        auto every_tone = [&](const std::string& base) { std::vector<std::string> out; if (!char_index || base.empty()) return out; std::set<std::string> seen;
            std::vector<std::string> qs = {base}; for (char t : bopomofo::tone_keys) qs.push_back(base + t);
            for (auto& q : qs) for (auto& c : char_index->chars(q)) if (seen.insert(c).second) out.push_back(c); return out; };
        auto chars_with_tolerance = [&](const std::string& syl) -> std::vector<std::string> {
            if (!char_index || syl.empty()) return {};
            const auto& exact = char_index->chars(syl); if (!exact.empty()) return exact;
            std::string keys = syl; if (bopomofo::is_tone(keys.back())) keys.pop_back();
            auto blind = every_tone(keys); if (!blind.empty()) return blind;
            auto uni = [&](const std::vector<std::string>& variants) { std::set<std::string> seen; std::vector<std::string> out;
                for (auto& v : variants) for (auto& c : char_index->chars(v)) if (seen.insert(c).second) out.push_back(c); return out; };
            std::vector<std::string> nv; for (size_t i = 0; i < syl.size(); i++) { auto it = bopomofo::neighbours().find(syl[i]); if (it == bopomofo::neighbours().end()) continue; for (char n : it->second) { std::string v = syl; v[i] = n; nv.push_back(v); } }
            auto nb = uni(nv); if (!nb.empty()) return nb;
            std::vector<std::string> sv; for (size_t i = 0; i + 1 < syl.size(); i++) { std::string v = syl; std::swap(v[i], v[i + 1]); sv.push_back(v); }
            auto sw = uni(sv); if (!sw.empty()) return sw;
            std::vector<std::string> dv; for (size_t i = 0; i < syl.size(); i++) { std::string v = syl; v.erase(i, 1); dv.push_back(v); }
            return uni(dv);
        };
        struct Piece { std::string keys; std::optional<int> consumed; }; std::vector<Piece> model_syllables;
        if (decoder && model_top && model_top->contains("reading") && model_top->contains("char_ends") && (*model_top)["reading"].size() == (*model_top)["char_ends"].size()) {
            const json& rd = (*model_top)["reading"]; const json& en = (*model_top)["char_ends"];
            for (size_t i = 0; i < rd.size(); i++) { auto k = decoder->keys_of_reading(rd[i].get<int>()); const int end = en[i].get<int>();
                if (!k || end < 0 || end >= raw_count) { model_syllables.clear(); break; }
                model_syllables.push_back({*k, end + 1 == raw_count ? std::nullopt : std::optional<int>(end + 1)}); }
        }
        struct Word { std::string text; std::optional<int> consumed; double weight; }; std::vector<Word> words;
        struct Ch { std::string text; std::optional<int> consumed; bool minor = false; }; std::vector<Ch> chars;
        auto add_words = [&](const std::vector<Piece>& pieces) {
            if (pieces.size() < 2) return;
            for (size_t count = std::min<size_t>(pieces.size(), 6); count >= 2; count--) { std::string piece; for (size_t i = 0; i < count; i++) piece += pieces[i].keys;
                std::vector<AQDSCandidate> rows; try { rows = fallback->exact_rows(piece, 4096); } catch (...) { continue; }
                if (rows.empty()) continue;
                for (auto& r : rows) words.push_back({r.text, pieces[count - 1].consumed, r.weight});
                break; }
        };
        { std::vector<Piece> ps; for (size_t i = 0; i < syllables.size(); i++) ps.push_back({syllables[i].keys, consumed(i + 1)}); add_words(ps); }
        if (words.empty()) add_words(model_syllables);
        std::vector<std::string> typing;
        if (!syllables.empty() && char_index && !char_index->chars(syllables[0].keys).empty()) { for (auto& c : char_index->chars(syllables[0].keys)) chars.push_back({c, consumed(1), char_index->is_minor(syllables[0].keys, c)}); }
        else if (syllables.empty() && !(typing = every_tone(pending)).empty()) { for (auto& c : typing) chars.push_back({c, std::nullopt}); }
        else if (!model_syllables.empty() && char_index && !char_index->chars(model_syllables[0].keys).empty()) { for (auto& c : char_index->chars(model_syllables[0].keys)) chars.push_back({c, model_syllables[0].consumed, char_index->is_minor(model_syllables[0].keys, c)}); }
        else if (!syllables.empty()) { for (auto& c : chars_with_tolerance(syllables[0].keys)) chars.push_back({c, consumed(1)}); }
        std::stable_sort(words.begin(), words.end(), [&](const Word& a, const Word& b) { int ca = a.consumed.value_or(raw_count), cb = b.consumed.value_or(raw_count); if (ca != cb) return ca > cb; return a.weight > b.weight; });
        for (auto& w : words) add(w.text, src_aqds, w.consumed);
        if (freq && freq->ok) std::stable_sort(chars.begin(), chars.end(), [&](const Ch& a, const Ch& b) {
            int ca = a.consumed.value_or(raw_count), cb = b.consumed.value_or(raw_count); if (ca != cb) return ca > cb;
            auto oa = freq->order(a.text), ob = freq->order(b.text); if (oa.first != ob.first) return oa.first < ob.first; if (a.minor != b.minor) return b.minor; if (oa.second != ob.second) return oa.second < ob.second; return a.text < b.text; });
        for (auto& c : chars) add(c.text, src_chars, c.consumed);

        ojson out; out["schema"] = response_schema; out["client_id"] = req.client_id; out["client_generation"] = req.generation; out["raw_keys"] = req.raw_keys;
        out["raw_identity"] = req.raw_identity; out["context_identity"] = req.context_identity; out["visible_path"] = visible; out["candidates"] = ojson::array();
        for (auto& c : output) { ojson o; o["text_utf8"] = c.text; o["provenance"] = c.provenance; if (c.consumed) o["consumed_keys"] = *c.consumed; out["candidates"].push_back(o); }
        return out;
    }

    // A short input is one word, and with no sentence around it the model leans on whatever its web text spelled most:
    // 南德 over 難得, 寫的 over 寫得.  The dictionary knows which spellings are words and how common they are, so a
    // candidate of two to four characters that the dictionary lists for its own reading gains up to `weight` (log of its
    // count, full at 100 000; 4, the best of 2 / 3 / 4 on the porting evaluation, with no loss on whole sentences).  Longer candidates are sentences, where the model's context decides.  AQ_LEXICON_WEIGHT:
    // 0 turns it off.
    void rerank_by_lexicon(json& rows) const {
        static const double weight = [] { const char* e = std::getenv("AQ_LEXICON_WEIGHT"); return e ? std::atof(e) : 4.0; }();
        if (weight <= 0 || !fallback || !decoder) return;
        std::unordered_map<std::string, std::vector<AQDSCandidate>> cache;
        std::vector<std::pair<double, size_t>> order;
        for (size_t i = 0; i < rows.size(); i++) {
            json& r = rows[i]; double s = r.value("score", 0.0);
            const std::string text = r.value("text", std::string()); const size_t n = code_points(text).size();
            const std::vector<int32_t> rd = reading_ids(r);
            if (n >= 2 && n <= 4 && rd.size() == n) {
                std::string keys; bool ok = true;
                for (int32_t id : rd) { auto k = decoder->keys_of_reading(id); if (!k) { ok = false; break; } keys += *k; }
                if (ok) {
                    auto it = cache.find(keys);
                    if (it == cache.end()) { std::vector<AQDSCandidate> found; try { found = fallback->exact_rows(keys, 4096); } catch (...) {} it = cache.emplace(keys, std::move(found)).first; }
                    for (const auto& w : it->second) if (w.text == text) { s += weight * std::min(1.0, std::log10(1.0 + std::max(0.0, w.weight)) / 5.0); r["score"] = s; break; }
                }
            }
            order.push_back({s, i});
        }
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        json sorted = json::array(); for (auto& [s, i] : order) sorted.push_back(rows[i]); rows = std::move(sorted);
    }

    ojson commit(const contract::Commit& ev) {
        double effective = 0;
        if (decoder) {
            remember_commit(ev); decoder->note_commit(ev.client_id, ev.text); decoder->end_sentence();
            effective = memory ? memory->count(ev.raw_keys, ev.text) : 0;
        }
        ojson a; a["schema"] = contract::ack_schema; a["client_id"] = ev.client_id; a["event_id"] = ev.event_id; a["raw_identity"] = ev.raw_identity; a["status"] = "RECORDED"; a["effective_count"] = effective;
        return a;
    }
    void remember_commit(const contract::Commit& ev) {
        if (!memory) return;
        const int64_t now = ev.timestamp; const std::string& text = ev.text; const std::string& typed = ev.raw_keys;
        std::vector<int32_t> readings;
        auto it = decoder->last_rows_by_client.find(ev.client_id);
        if (it != decoder->last_rows_by_client.end()) for (auto& r : it->second) if (r.contains("text") && r["text"] == text) { readings = reading_ids(r); break; }
        const std::vector<int32_t> typed_readings = decoder->readings(typed);
        if (readings.empty()) readings = typed_readings;
        const std::string clean = (!readings.empty() && typed_readings == readings) ? typed : "";
        memory->transaction([&] { memory->note_outcome(ev.client_id, typed, text, now); memory->record(text, typed, clean, readings, now); });
    }
};

// ---------------------------------------------------------------- arguments, serving
struct Args {
    std::map<std::string, std::string> values; std::set<std::string> flags;
    Args(int argc, char** argv) { for (int i = 1; i < argc;) { std::string k = argv[i]; if (k.rfind("--", 0) == 0 && i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) { values[k] = argv[i + 1]; i += 2; } else { flags.insert(k); i++; } } }
    std::string required(const std::string& k) const { auto it = values.find(k); if (it == values.end() || it->second.empty()) throw contract::invalid("missing " + k); return it->second; }
    const char* get(const std::string& k) const { auto it = values.find(k); return it == values.end() ? nullptr : it->second.c_str(); }
};

static aq_decoder* g_maintain = nullptr;
static void emit(const ojson& v) { std::string s = v.dump() + "\n"; std::fwrite(s.data(), 1, s.size(), stdout); std::fflush(stdout); if (g_maintain) aq_maintain(g_maintain); }

// One line of the protocol: the reply to write back, if any.  A line that fails is answered with a failure addressed to
// whatever of its request can still be read; a line that cannot be read that far gets no reply.
static std::optional<ojson> handle_line(Engine& engine, const std::string& line) {
    json j;
    try {
        j = json::parse(line);
        const std::string schema = j.at("schema").get<std::string>();
        if (schema == contract::request_schema) { auto r = contract::Request::parse(j); r.validate(); return engine.respond(r); }
        if (schema == contract::commit_schema) { auto c = contract::Commit::parse(j); c.validate(); return engine.commit(c); }
        log("[engine] unknown message schema"); return std::nullopt;
    } catch (const std::exception& ex) {
        log(std::string("[engine] request failed: ") + ex.what());
        if (!j.is_object()) return std::nullopt;
        auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
        ojson f; f["schema"] = contract::failure_schema; f["client_id"] = str("client_id");
        f["client_generation"] = j.contains("client_generation") && j["client_generation"].is_number_unsigned() ? j["client_generation"].get<uint64_t>() : 0;
        f["raw_identity"] = str("raw_identity"); if (j.contains("event_id") && j["event_id"].is_string()) f["event_id"] = j["event_id"]; f["reason"] = ex.what();
        return f;
    }
}

// ---- serve-pipe: one engine for every input method instance of this user.  Each app that loads the text service
// connects to the pipe; requests from all of them are answered one at a time (the decoder is single-threaded), each on
// the connection it came in on, and the between-keystroke housekeeping runs after the reply is written.
static std::wstring default_pipe_name() {
    wchar_t user[256]; DWORD n = 256; if (!GetUserNameW(user, &n)) wcscpy_s(user, L"user");
    return std::wstring(L"\\\\.\\pipe\\AquaTypeEngine-") + user;
}
// Owner and SYSTEM full access; AppContainer apps (the Store apps the text service is loaded into) read/write; low
// integrity allowed to write, since browsers' sandboxed processes run there.
static bool pipe_security(SECURITY_ATTRIBUTES* sa) {
    HANDLE tok = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD len = 0; GetTokenInformation(tok, TokenUser, nullptr, 0, &len); std::vector<uint8_t> buf(len);
    const bool ok = GetTokenInformation(tok, TokenUser, buf.data(), len, &len) != 0; CloseHandle(tok); if (!ok) return false;
    LPWSTR sid = nullptr; if (!ConvertSidToStringSidW(((TOKEN_USER*)buf.data())->User.Sid, &sid)) return false;
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")(A;;GRGW;;;AC)S:(ML;;NW;;;LW)"; LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return false;
    sa->nLength = sizeof(*sa); sa->lpSecurityDescriptor = sd; sa->bInheritHandle = FALSE; return true;   // kept for the life of the process
}
static int serve_pipe(Engine& engine, const std::wstring& name) {
    SECURITY_ATTRIBUTES sa{}; const bool secured = pipe_security(&sa);
    std::mutex lock; bool first = true;
    while (true) {
        HANDLE p = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0), PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, secured ? &sa : nullptr);
        if (p == INVALID_HANDLE_VALUE) {
            if (first && GetLastError() == ERROR_ACCESS_DENIED) { log("[engine] another engine already serves " + narrow(name)); return 0; }
            log("[engine] cannot create pipe (error " + std::to_string(GetLastError()) + ")"); Sleep(1000); continue;
        }
        if (first) { log("[engine] serving " + narrow(name)); first = false; }
        if (!ConnectNamedPipe(p, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(p); continue; }
        std::thread([&engine, &lock, p] {
            std::string pending; char buf[8192]; DWORD got = 0;
            while (ReadFile(p, buf, sizeof buf, &got, nullptr) && got > 0) {
                pending.append(buf, got); size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, nl); pending.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back(); if (line.empty()) continue;
                    std::optional<ojson> reply; { std::lock_guard<std::mutex> g(lock); reply = handle_line(engine, line); }
                    if (reply) { std::string s = reply->dump() + "\n"; DWORD w = 0; if (!WriteFile(p, s.data(), (DWORD)s.size(), &w, nullptr)) break; }
                    std::lock_guard<std::mutex> g(lock); if (g_maintain) aq_maintain(g_maintain);
                }
            }
            DisconnectNamedPipe(p); CloseHandle(p);
        }).detach();
    }
}
static Engine make_engine(const Args& a) {
    Engine e;
    e.fallback = std::make_unique<AQDSFallbackStore>(widen(a.required("--fallback-aqds")), widen(a.required("--fallback-manifest")));
    std::string state; if (const char* s = a.get("--state-dir")) state = s; else { char* home = nullptr; size_t n = 0; _dupenv_s(&home, &n, "USERPROFILE"); state = std::string(home ? home : ".") + "\\.aquatype"; free(home); }
    CreateDirectoryW(widen(state).c_str(), nullptr);                    // the user's profile is already private to them
    if (const char* p = a.get("--char-to-keys")) { try { e.char_index = std::make_unique<CharKeyIndex>(widen(p)); } catch (...) {} }
    if (!e.char_index) log("[engine] char_to_keys.json not found; the third source has no character table");
    if (e.char_index) {                                                 // the other readings: --char-readings, or the dictionary beside char_to_keys.json
        std::wstring r = a.get("--char-readings") ? widen(a.get("--char-readings")) : widen(a.get("--char-to-keys"));
        if (!a.get("--char-readings")) { const size_t slash = r.find_last_of(L"\\/"); r = (slash == std::wstring::npos ? L"" : r.substr(0, slash + 1)) + L"terra_pinyin.dict.yaml"; }
        if (GetFileAttributesW(r.c_str()) != INVALID_FILE_ATTRIBUTES) {
            try { e.char_index->add_readings(r); log("[engine] other readings: " + std::to_string(e.char_index->added)); }
            catch (const std::exception& ex) { log(std::string("[engine] other readings not read: ") + ex.what()); }
        }
    }
    if (const char* d = a.get("--decoder-dir")) { e.decoder = DecoderSession::open(d, a.get("--metallib")); if (e.decoder) e.decoder->warm_up(); }
    if (e.decoder) g_maintain = e.decoder->handle;
    if (const char* d = a.get("--decoder-dir")) { e.freq = std::make_unique<CharFrequency>(widen(d)); if (!e.freq->ok) e.freq.reset(); }
    if (!e.freq) log("[engine] no character frequency table; the third source falls back to code-point order");
    log(std::string("[engine] ready decoder=") + (e.decoder ? "true" : "false") + " chars=" + std::to_string(e.char_index ? e.char_index->count : 0) + " freq=" + (e.freq ? "true" : "false") + " state=" + state);
    e.memory = std::make_unique<PersonalMemoryStore>(state);
    log("[engine] personal memory rows=" + std::to_string(e.memory->rows().size()));
    return e;
}

static int run(int argc, char** argv) {
    Args a(argc, argv);
    if (a.flags.count("--version")) { std::printf("AquaTypeEngine %s\n", aq_version()); return 0; }
    if (const char* p = a.get("--check-char-coverage")) { CharKeyIndex idx(widen(p)); auto gaps = idx.coverage_gaps();
        std::printf("{\"chars\":%zu,\"missing\":%zu,\"ok\":%s}\n", idx.count, gaps.size(), gaps.empty() ? "true" : "false"); return gaps.empty() ? 0 : 1; }
    // The pipe engine is the installed one: the person's settings (AquaTypeSettings.exe) choose the pronunciation pairs and
    // the dictionary weight, unless the environment already does (evaluation runs).
    if (a.get("--command") && std::string(a.get("--command")) == "serve-pipe") {
        const aqtip::Settings s = aqtip::Settings::load();
        if (!std::getenv("AQ_FUZZY")) _putenv_s("AQ_FUZZY", s.fuzzy_pairs().c_str());
        if (!std::getenv("AQ_LEXICON_WEIGHT")) _putenv_s("AQ_LEXICON_WEIGHT", s.lexicon_priority ? "4" : "0");
    }
    Engine engine = make_engine(a);
    if (a.flags.count("--selftest")) { contract::Request r; r.client_id = "selftest"; r.generation = 1; r.raw_keys = a.get("--raw") ? a.get("--raw") : "ji3"; r.raw_identity = contract::identity(r.raw_keys);
        r.context_identity = contract::context_identity(""); r.max_candidates = 9; emit(engine.respond(r)); return 0; }
    const std::string command = a.get("--command") ? a.get("--command") : "serve-ui-jsonl";
    if (command != "serve-ui-jsonl" && command != "serve-pipe") throw contract::invalid("unknown --command");
    // The engine answers a user who is waiting: above-normal priority, the counterpart of the Mac engine's user-interactive QoS.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    if (command == "serve-pipe") return serve_pipe(engine, a.get("--pipe") ? widen(a.get("--pipe")) : default_pipe_name());
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (auto reply = handle_line(engine, line)) { try { emit(*reply); } catch (...) {} }
    }    return 0;
}

int main(int argc, char** argv) {
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
    // --log FILE: stderr to a file, for an engine started without a console (the text service starts it detached)
    for (int i = 1; i + 1 < argc; i++) if (std::string(argv[i]) == "--log") { FILE* f = nullptr; _wfreopen_s(&f, widen(argv[i + 1]).c_str(), L"a", stderr); }
    try { return run(argc, argv); } catch (const std::exception& e) { log(std::string("AquaTypeEngine failed: ") + e.what()); return 1; }
}
