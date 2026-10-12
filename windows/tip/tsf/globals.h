// AquaType's text service for Windows (TSF): identities and module-wide state.  The structure follows Microsoft's
// SampleIME (Windows-classic-samples, MIT); the composition logic is windows/tip/composer.*.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <msctf.h>
#include <atomic>

namespace aqtsf {

// {87FA8DDA-2BA4-476C-8510-93DCD553AD2B}
inline constexpr CLSID CLSID_AquaType = {0x87fa8dda, 0x2ba4, 0x476c, {0x85, 0x10, 0x93, 0xdc, 0xd5, 0x53, 0xad, 0x2b}};
// {3A6B588B-D85E-4EB1-A28D-38F3F7BEBE92}
inline constexpr GUID GUID_Profile = {0x3a6b588b, 0xd85e, 0x4eb1, {0xa2, 0x8d, 0x38, 0xf3, 0xf7, 0xbe, 0xbe, 0x92}};
// {28E66F23-17C6-4122-B758-19ECBA9E72D2}: the composition's display attribute (a dotted underline)
inline constexpr GUID GUID_DisplayAttributeInput = {0x28e66f23, 0x17c6, 0x4122, {0xb7, 0x58, 0x19, 0xec, 0xba, 0x9e, 0x72, 0xd2}};
// {0F91081F-D415-4540-A35C-9B3097663289}: Shift released on its own (the preserved key that switches 中/英)
inline constexpr GUID GUID_PreservedShift = {0x0f91081f, 0xd415, 0x4540, {0xa3, 0x5c, 0x9b, 0x30, 0x97, 0x66, 0x32, 0x89}};
inline constexpr LANGID kLangId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);   // zh-TW
inline constexpr wchar_t kDescription[] = L"AquaType 注音";

extern HINSTANCE g_module;
extern std::atomic<long> g_locks;      // objects alive + server locks: DllCanUnloadNow
inline void lock_module() { g_locks++; }
inline void unlock_module() { g_locks--; }

}  // namespace aqtsf
