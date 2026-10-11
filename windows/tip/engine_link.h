// The text service's connection to the engine: the per-user named pipe of AquaTypeEngine --command serve-pipe.  The
// engine is started (windowless, detached) when the pipe is not there; replies are read on a thread of their own and
// handed to the composer through a queue, with an optional window message to say one has arrived.
#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include "composer.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace aqtip {

struct EngineLaunch {                // how to start the engine when it is not running
    std::wstring exe, args;          // empty exe: never start it, only connect
};
EngineLaunch default_launch(HMODULE self);   // the engine and its data beside this module (see windows/README.md)

class PipeEngine : public EngineLink {
public:
    PipeEngine(std::wstring pipe, EngineLaunch launch);
    ~PipeEngine() override;
    void set_notify(HWND hwnd, UINT msg) { std::lock_guard<std::mutex> g(m_); notify_hwnd_ = hwnd; notify_msg_ = msg; }
    bool send(const std::string& line) override;
    bool next_line(std::string* line, uint32_t wait_ms) override;
    static std::wstring default_pipe_name();
private:
    bool connect(uint32_t wait_ms);
    void reader(HANDLE pipe);
    std::wstring pipe_name_; EngineLaunch launch_;
    std::mutex m_; std::condition_variable cv_; std::deque<std::string> lines_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE; std::thread reader_;
    HWND notify_hwnd_ = nullptr; UINT notify_msg_ = 0;
    ULONGLONG last_launch_ = 0, last_failed_ = 0; bool closing_ = false;
};

}  // namespace aqtip
