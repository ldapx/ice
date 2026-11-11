#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ice {

// Cooperative cancellation shared between the caller and a running process.
// Callers may cancel from another thread while runProcess is blocked.
class ProcessCancellation {
  public:
    void cancel() noexcept;
    bool cancelled() const noexcept;

  private:
    std::atomic_bool cancelled_ = false;
};

struct ProcessOptions {
    // argv[0] is the executable. No shell interpolation is performed.
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    // Variables added to or replacing the parent environment.
    std::map<std::string, std::string> environment;
    // Full merged stdout/stderr log. Empty keeps the output in memory only.
    std::filesystem::path logPath;
    // Upper bound for ProcessResult::logTail, counted in bytes.
    std::size_t logTailBytes = 64 * 1024;
    // Zero disables the timeout.
    int timeoutSeconds = 0;
};

struct ProcessResult {
    bool started = false;
    bool cancelled = false;
    bool timedOut = false;
    int exitCode = -1;
    // Terminal signal number on POSIX; zero when the process exited normally.
    int signal = 0;
    int pid = 0;
    std::string error;
    std::string logTail;
    std::filesystem::path logPath;
    std::uint64_t durationMilliseconds = 0;

    bool succeeded() const {
        return started && !cancelled && !timedOut && exitCode == 0 && signal == 0 && error.empty();
    }
};

using ProcessOutputCallback = std::function<void(const std::string &)>;
using ProcessStartedCallback = std::function<void(int pid)>;

// Runs one external process with merged stdout/stderr, bounded tail capture,
// optional full log file, timeout, and cooperative cancellation.
//
// On POSIX the child runs in its own process group, so cancellation terminates
// the whole tool process tree. On platforms without a POSIX process API this
// falls back to std::system; streaming, timeout, and cancellation are then
// unavailable and the log is read back from disk when possible.
ProcessResult runProcess(const ProcessOptions &options, const ProcessCancellation *cancellation = nullptr,
                         const ProcessOutputCallback &onOutput = {}, const ProcessStartedCallback &onStarted = {});

} // namespace ice
