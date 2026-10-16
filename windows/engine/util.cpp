#include "util.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace aqw {

void log(const std::string& s) { std::string line = s + "\n"; std::fwrite(line.data(), 1, line.size(), stderr); std::fflush(stderr); }

std::vector<char32_t> code_points(const std::string& s) {
    std::vector<char32_t> out; out.reserve(s.size()); size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i]; char32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; } else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; } else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out.push_back(cp); i += n;
    }
    return out;
}
std::string utf8(char32_t cp) {
    std::string o;
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    return o;
}
std::string utf8(const std::vector<char32_t>& cps, size_t from, size_t to) {
    std::string o; if (to > cps.size()) to = cps.size(); for (size_t i = from; i < to; i++) o += utf8(cps[i]); return o;
}
bool valid_utf8(const std::string& s) {
    return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0;
}
size_t utf16_units(const std::string& s) { return s.empty() ? 0 : (size_t)MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); }
std::wstring widen(const std::string& s) {
    if (s.empty()) return {}; int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}

std::string base64(const std::string& b) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; size_t i = 0;
    for (; i + 3 <= b.size(); i += 3) { uint32_t v = ((uint8_t)b[i] << 16) | ((uint8_t)b[i + 1] << 8) | (uint8_t)b[i + 2]; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63]; }
    if (b.size() - i == 1) { uint32_t v = (uint8_t)b[i] << 16; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "=="; }
    else if (b.size() - i == 2) { uint32_t v = ((uint8_t)b[i] << 16) | ((uint8_t)b[i + 1] << 8); o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '='; }
    return o;
}

namespace {
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    Sha256() { if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 || BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) throw std::runtime_error("SHA-256 unavailable"); }
    ~Sha256() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void update(const void* p, size_t n) { if (n && BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0) != 0) throw std::runtime_error("SHA-256 failed"); }
    std::string hex() { uint8_t d[32]; BCryptFinishHash(h, d, 32, 0); static const char* X = "0123456789abcdef"; std::string o; for (uint8_t b : d) { o += X[b >> 4]; o += X[b & 15]; } return o; }
};
}
std::string sha256_hex(const std::string& b) { Sha256 s; s.update(b.data(), b.size()); return s.hex(); }
std::string sha256_file_hex(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("cannot read " + narrow(path));
    Sha256 s; std::vector<char> buf(1 << 20);
    while (f) { f.read(buf.data(), (std::streamsize)buf.size()); s.update(buf.data(), (size_t)f.gcount()); }
    return s.hex();
}

MappedFile::MappedFile(const std::wstring& path) {
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) { file_ = nullptr; throw std::runtime_error("cannot open " + narrow(path)); }
    LARGE_INTEGER sz; if (!GetFileSizeEx(file_, &sz) || sz.QuadPart <= 0) { CloseHandle(file_); throw std::runtime_error("cannot stat " + narrow(path)); }
    size_ = (size_t)sz.QuadPart;
    map_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map_) { CloseHandle(file_); throw std::runtime_error("cannot map " + narrow(path)); }
    data_ = (const uint8_t*)MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0);
    if (!data_) { CloseHandle(map_); CloseHandle(file_); throw std::runtime_error("cannot map " + narrow(path)); }
}
MappedFile::~MappedFile() { if (data_) UnmapViewOfFile(data_); if (map_) CloseHandle(map_); if (file_) CloseHandle(file_); }

std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("cannot read " + narrow(path));
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
int64_t unix_now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

}  // namespace aqw
