#include "memory.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include "util.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace aqw {

static std::vector<std::string> split(const std::string& s, char sep, bool keep_empty) {
    std::vector<std::string> out; size_t a = 0;
    while (true) { size_t b = s.find(sep, a); std::string part = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (keep_empty || !part.empty()) out.push_back(part); if (b == std::string::npos) break; a = b + 1; }
    return out;
}
// Swift's Double description: the shortest round-trip form, with ".0" on integral values
static std::string double_text(double v) {
    char buf[64]; auto r = std::to_chars(buf, buf + sizeof buf, v); std::string s(buf, r.ptr);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

PersonalMemoryStore::PersonalMemoryStore(const std::string& state_dir) {
    folder_ = widen(state_dir); name_ = L"personal_memory.v2.tsv"; path_ = folder_ + L"\\" + name_;
    remove_stale_temporaries(); read_rows_from_disk(); reindex();
}

// A save writes .<file>.<pid>.tmp beside the file and renames it; an engine ended in between leaves its copy behind.
// Those of processes that are gone are removed; another engine's, still being written, is left alone.
void PersonalMemoryStore::remove_stale_temporaries() {
    const std::wstring prefix = L"." + name_ + L".";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((folder_ + L"\\" + prefix + L"*.tmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName; if (n.size() <= prefix.size() + 4) continue;
        std::wstring mid = n.substr(prefix.size(), n.size() - prefix.size() - 4); if (mid.empty() || mid.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
        const DWORD pid = (DWORD)std::stoul(mid); if (pid == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        bool alive = false; if (p) { DWORD code = 0; alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE; CloseHandle(p); }
        else alive = GetLastError() != ERROR_INVALID_PARAMETER;                    // no such process: ERROR_INVALID_PARAMETER
        if (!alive) DeleteFileW((folder_ + L"\\" + n).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool PersonalMemoryStore::identity_on_disk(Identity* id) const {
    HANDLE f = CreateFileW(path_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION i; const bool ok = GetFileInformationByHandle(f, &i) != 0; CloseHandle(f); if (!ok) return false;
    id->volume = i.dwVolumeSerialNumber; id->index = ((uint64_t)i.nFileIndexHigh << 32) | i.nFileIndexLow; id->size = ((uint64_t)i.nFileSizeHigh << 32) | i.nFileSizeLow;
    id->mtime = ((uint64_t)i.ftLastWriteTime.dwHighDateTime << 32) | i.ftLastWriteTime.dwLowDateTime; return true;
}

// Replace rows with what is in the file now.  No file is an ordinary first run; a file that cannot be looked at is not
// taken for an empty memory (nothing is written this time); one that cannot be read is moved aside.
void PersonalMemoryStore::read_rows_from_disk() {
    rows_.clear(); not_looked_at_ = false;
    have_seen_ = identity_on_disk(&last_seen_on_disk_);
    const DWORD attr = GetFileAttributesW(path_.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return;
        have_seen_ = false; not_looked_at_ = true;
        log("[engine] personal memory could not be looked at (error " + std::to_string(e) + "); this change is not written"); return;
    }
    std::string text; bool ok = true;
    try { text = read_file(path_); } catch (...) { ok = false; }
    if (ok && !valid_utf8(text)) ok = false;
    if (!ok) {
        have_seen_ = false;
        const std::wstring aside = path_ + L".unreadable-" + std::to_wstring(unix_now());
        if (MoveFileExW(path_.c_str(), aside.c_str(), 0)) log("[engine] personal memory could not be read; kept at " + narrow(aside) + " and starting a new one");
        else { unreadable_ = true; log("[engine] personal memory could not be read or moved aside; it will not be written to this session"); }
        return;
    }
    int inconsistent = 0; const int64_t horizon = unix_now() + 86400;
    for (const std::string& line0 : split(text, '\n', false)) {
        std::string line = line0; if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f = split(line, '\t', true); if (f.size() < 6) continue;
        std::vector<int32_t> readings; if (!f[3].empty()) for (auto& x : split(f[3], ',', false)) { try { readings.push_back((int32_t)std::stol(x)); } catch (...) {} }
        if (!consistent(f[0], readings)) { inconsistent++; continue; }
        double count = 1; try { size_t used = 0; double c = std::stod(f[4], &used); if (used == f[4].size() && std::isfinite(c)) count = std::min(std::max(c, 0.0), 1e6); } catch (...) {}
        int64_t last = 0; try { last = std::stoll(f[5]); } catch (...) {} last = std::min(std::max<int64_t>(last, 0), horizon);
        rows_.push_back({f[0], f[1], f[2], readings, count, last});
    }
    if (inconsistent > 0) log("[engine] personal memory: " + std::to_string(inconsistent) + " rows whose readings do not match their text left out");
}

void PersonalMemoryStore::refresh_from_disk() { Identity now; if (have_seen_ && identity_on_disk(&now) && now == last_seen_on_disk_) return; read_rows_from_disk(); }

// Six tab-separated fields with no escaping: safe because the commit envelope refuses control characters in the text and
// anything but printable ASCII in the keys, and every path here has been through it.
void PersonalMemoryStore::save() {
    if (unreadable_ || not_looked_at_) { log("[engine] personal memory not written: the existing file could not be read and must not be replaced"); return; }
    std::string out;
    for (auto& r : rows_) { std::string rd; for (size_t i = 0; i < r.readings.size(); i++) { if (i) rd += ","; rd += std::to_string(r.readings[i]); }
        out += r.text + "\t" + r.typed_keys + "\t" + r.clean_keys + "\t" + rd + "\t" + double_text(r.count) + "\t" + std::to_string(r.last_seen) + "\n"; }
    const std::wstring tmp = folder_ + L"\\." + name_ + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { log("[engine] personal memory could not be written beside " + narrow(path_)); return; }
    DWORD written = 0; const bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &written, nullptr) && written == out.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) { log("[engine] personal memory could not be written beside " + narrow(path_)); DeleteFileW(tmp.c_str()); return; }
    bool moved = false;
    for (int attempt = 0; attempt < 20 && !(moved = MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0); attempt++) Sleep(5);   // a reader holding the old file open
    if (!moved) { log("[engine] personal memory could not be replaced at " + narrow(path_) + " (error " + std::to_string(GetLastError()) + ")"); DeleteFileW(tmp.c_str()); have_seen_ = false; }
    else have_seen_ = identity_on_disk(&last_seen_on_disk_);
}

void PersonalMemoryStore::with_exclusive_lock(const std::function<void()>& body) {
    const std::wstring lp = path_ + L".lock";
    HANDLE f = CreateFileW(lp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { if (!lock_unavailable_reported_) { lock_unavailable_reported_ = true; log("[engine] personal memory lock could not be opened at " + narrow(lp) + "; writing without it"); } body(); return; }
    OVERLAPPED ov = {};
    if (!LockFileEx(f, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) { if (!lock_unavailable_reported_) { lock_unavailable_reported_ = true; log("[engine] personal memory lock is not supported here; writing without it"); } CloseHandle(f); body(); return; }
    try { body(); } catch (...) { UnlockFileEx(f, 0, 1, 0, &ov); CloseHandle(f); throw; }
    UnlockFileEx(f, 0, 1, 0, &ov); CloseHandle(f);
}

void PersonalMemoryStore::mutate(const std::function<void()>& change) {
    if (in_transaction_) { change(); reindex(); return; }
    with_exclusive_lock([&] { refresh_from_disk(); change(); save(); });
    reindex();
}
void PersonalMemoryStore::transaction(const std::function<void()>& changes) {
    with_exclusive_lock([&] { refresh_from_disk(); reindex(); in_transaction_ = true; changes(); in_transaction_ = false; save(); });
    reindex();
}

void PersonalMemoryStore::reindex() {
    by_typed_.clear(); by_clean_.clear(); by_first_reading_.clear();
    for (size_t i = 0; i < rows_.size(); i++) { const auto& r = rows_[i];
        if (!r.typed_keys.empty()) by_typed_[r.typed_keys].push_back(i);
        if (!r.clean_keys.empty()) by_clean_[r.clean_keys].push_back(i);
        if (!r.readings.empty() && (int)r.readings.size() >= min_fusion_readings) by_first_reading_[r.readings[0]].push_back(i); }
}

double PersonalMemoryStore::support(const MemoryRecord& r, int64_t now) const {
    const double days = std::max(0.0, (double)(now - r.last_seen) / 86400.0), decay = std::pow(2.0, -days / 180.0);
    return r.count * (r.count >= confirmed_count ? 0.25 + 0.75 * decay : decay);
}
bool PersonalMemoryStore::consistent(const std::string& text, const std::vector<int32_t>& readings) { return readings.empty() || readings.size() == code_points(text).size(); }

void PersonalMemoryStore::record(const std::string& text, const std::string& typed, const std::string& clean, const std::vector<int32_t>& readings_in, int64_t now) {
    if (text.empty()) return;
    if (!consistent(text, readings_in)) { log("[engine] personal memory: " + std::to_string(code_points(text).size()) + " characters for " + std::to_string(readings_in.size()) + " readings not remembered"); return; }
    mutate([&] {
        const std::vector<char32_t> chars = code_points(text);
        if (chars.size() == readings_in.size()) {
            for (size_t start = 0; start < readings_in.size(); start++) { auto it = by_first_reading_.find(readings_in[start]); if (it == by_first_reading_.end()) continue;
                for (size_t i : it->second) { if (i >= rows_.size()) continue; const size_t n = rows_[i].readings.size();
                    if (start + n > readings_in.size() || rows_[i].text == text || !std::equal(rows_[i].readings.begin(), rows_[i].readings.end(), readings_in.begin() + start)
                        || utf8(chars, start, start + n) != rows_[i].text) continue;
                    rows_[i].last_seen = std::max(rows_[i].last_seen, now); } }
        }
        std::vector<int32_t> readings = readings_in;
        auto it = std::find_if(rows_.begin(), rows_.end(), [&](const MemoryRecord& r) { return r.text == text && r.typed_keys == typed; });
        if (it != rows_.end()) { it->count += 1; it->last_seen = now; if (it->readings.empty()) it->readings = readings; if (it->clean_keys.empty()) it->clean_keys = clean; }
        else {
            if (readings.empty()) { auto k = std::find_if(rows_.begin(), rows_.end(), [&](const MemoryRecord& r) { return r.text == text && !r.readings.empty(); }); if (k != rows_.end()) readings = k->readings; }
            rows_.push_back({text, typed, clean, readings, 1, now});
        }
        if (!readings.empty()) for (auto& r : rows_) if (r.text == text && r.readings.empty()) r.readings = readings;
        prune(now);
    });
}

double PersonalMemoryStore::count(const std::string& typed, const std::string& text) const {
    double best = 0; auto it = by_typed_.find(typed); if (it == by_typed_.end()) return 0;
    for (size_t i : it->second) if (rows_[i].text == text) best = std::max(best, rows_[i].count);
    return best;
}
void PersonalMemoryStore::retract(const std::string& typed, const std::string& text) {
    mutate([&] { for (auto& r : rows_) if (r.typed_keys == typed && r.text == text) { r.count = std::max(0.0, r.count - 1); break; } });
}
void PersonalMemoryStore::demote(const std::string& keys, const std::string& chosen) {
    mutate([&] { for (auto& r : rows_) if ((r.clean_keys == keys || r.typed_keys == keys) && r.text != chosen && r.count > 1) r.count -= 0.5; });
}
void PersonalMemoryStore::prune(int64_t now) {
    if ((int)rows_.size() <= max_rows) return;
    std::vector<size_t> order(rows_.size()); for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return support(rows_[a], now) > support(rows_[b], now); });
    std::vector<char> drop(rows_.size(), 0); for (size_t k = max_rows; k < order.size(); k++) drop[order[k]] = 1;
    std::vector<MemoryRecord> kept; for (size_t i = 0; i < rows_.size(); i++) if (!drop[i]) kept.push_back(rows_[i]); rows_.swap(kept);
}

void PersonalMemoryStore::note_outcome(const std::string& client, const std::string& typed, const std::string& text, int64_t now) {
    auto prev = last_commit_.find(client);
    if (prev != last_commit_.end() && prev->second.keys == typed && prev->second.text != text && code_points(prev->second.text).size() >= 2 && code_points(text).size() >= 2 && now - prev->second.at <= correction_window)
        retract(typed, prev->second.text);
    auto top = exact_recall(typed, now); if (!top.empty() && top[0] != text) demote(typed, text);
    for (auto it = last_commit_.begin(); it != last_commit_.end();) { if (now - it->second.at > correction_window) it = last_commit_.erase(it); else ++it; }
    last_commit_[client] = {typed, text, now};
}

std::vector<std::string> PersonalMemoryStore::exact_recall(const std::string& keys, int64_t now) const {
    std::set<std::string> seen; std::vector<std::pair<std::string, double>> hits;
    auto scan = [&](const std::unordered_map<std::string, std::vector<size_t>>& m) { auto it = m.find(keys); if (it == m.end()) return;
        for (size_t i : it->second) if (rows_[i].count > 0 && seen.insert(rows_[i].text).second) hits.push_back({rows_[i].text, support(rows_[i], now)}); };
    scan(by_typed_); scan(by_clean_);
    std::sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    std::vector<std::string> out; for (size_t i = 0; i < hits.size() && (int)i < display_per_key; i++) out.push_back(hits[i].first);
    return out;
}

bool PersonalMemoryStore::fuse(const std::string& text, const std::vector<int32_t>& readings, int64_t now, std::string* result) const {
    const std::vector<char32_t> chars = code_points(text);
    if (chars.size() != readings.size() || readings.empty()) return false;
    struct Match { size_t start, len; std::string text; double support; };
    std::vector<Match> matches, agreements;
    for (size_t start = 0; start < readings.size(); start++) { auto it = by_first_reading_.find(readings[start]); if (it == by_first_reading_.end()) continue;
        for (size_t i : it->second) { const auto& r = rows_[i]; const size_t n = r.readings.size();
            if (!(r.count > 0) || (int)n < min_fusion_readings || start + n > readings.size()) continue;
            if (!std::equal(r.readings.begin(), r.readings.end(), readings.begin() + start)) continue;
            const double s = support(r, now);
            if (utf8(chars, start, start + n) == r.text) { agreements.push_back({start, n, r.text, s}); continue; }
            matches.push_back({start, n, r.text, s}); } }
    matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const Match& m) {
        return std::any_of(agreements.begin(), agreements.end(), [&](const Match& a) { return a.start < m.start + m.len && m.start < a.start + a.len && a.support > m.support; }); }), matches.end());
    if (matches.empty()) return false;
    std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.support != b.support ? a.support > b.support : a.len > b.len; });
    std::vector<char> taken(readings.size(), 0); std::vector<Match> applied;
    for (auto& m : matches) { bool clash = false; for (size_t k = m.start; k < m.start + m.len; k++) clash |= taken[k] != 0; if (clash) continue;
        for (size_t k = m.start; k < m.start + m.len; k++) taken[k] = 1; applied.push_back(m); }
    if (applied.empty()) return false;
    std::sort(applied.begin(), applied.end(), [](const Match& a, const Match& b) { return a.start < b.start; });
    std::string out; size_t i = 0;
    for (auto& m : applied) { if (i < m.start) out += utf8(chars, i, m.start); out += m.text; i = m.start + m.len; }
    if (i < chars.size()) out += utf8(chars, i);
    if (out == text) return false;
    *result = out; return true;
}

}  // namespace aqw
