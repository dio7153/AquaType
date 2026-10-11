// Small platform helpers for the Windows engine: UTF-8 <-> code points and UTF-16, base64, SHA-256 (CNG), a read-only
// mapped file, logging to stderr.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aqw {

void log(const std::string& s);                                   // one line to stderr

// UTF-8: a Swift Character is one code point for everything this engine handles (Han text, ASCII keys)
std::vector<char32_t> code_points(const std::string& s);
std::string utf8(const std::vector<char32_t>& cps, size_t from = 0, size_t to = std::string::npos);
std::string utf8(char32_t cp);
bool valid_utf8(const std::string& s);
size_t utf16_units(const std::string& s);
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

std::string base64(const std::string& bytes);
std::string sha256_hex(const std::string& bytes);
std::string sha256_file_hex(const std::wstring& path);           // throws on I/O errors

// a whole file, read-only, mapped
class MappedFile {
public:
    explicit MappedFile(const std::wstring& path);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete; MappedFile& operator=(const MappedFile&) = delete;
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
private:
    void* file_ = nullptr; void* map_ = nullptr; const uint8_t* data_ = nullptr; size_t size_ = 0;
};

std::string read_file(const std::wstring& path);                  // throws when it cannot be read
int64_t unix_now();

}  // namespace aqw
