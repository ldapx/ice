#include "icelib/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ice {
namespace {

void appendTail(std::string &tail, const char *data, std::size_t size, std::size_t limit) {
    if (limit == 0)
        return;
    if (size >= limit) {
        tail.assign(data + (size - limit), limit);
        return;
    }
    tail.append(data, size);
    if (tail.size() > limit)
        tail.erase(0, tail.size() - limit);
}

#ifdef _WIN32
std::string joinArguments(const std::vector<std::string> &arguments) {
    std::string result;
    for (const auto &argument : arguments) {
        if (!result.empty())
            result.push_back(' ');
        result.push_back('"');
        for (const char character : argument) {
            if (character == '"' || character == '\\')
                result.push_back('\\');
            result.push_back(character);
        }
        result.push_back('"');
    }
    return result;
}

std::string readFileTail(const std::filesystem::path &path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0)
        return {};
    const auto wanted = static_cast<std::streamoff>(std::min<std::uintmax_t>(static_cast<std::uintmax_t>(size), limit));
    input.seekg(size - wanted, std::ios::beg);
    std::string result(static_cast<std::size_t>(wanted), '\0');
    input.read(result.data(), static_cast<std::streamsize>(wanted));
    result.resize(static_cast<std::size_t>(input.gcount()));
    return result;
}
#endif

#ifndef _WIN32

bool writeAll(int descriptor, const char *data, std::size_t size) {
    while (size > 0) {
        const auto written = ::write(descriptor, data, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

void resetChildSignals() {
    for (const auto signalNumber : {SIGINT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD}) {
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(signalNumber, &action, nullptr);
    }
}

int childExitCode(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

int childSignal(int status) {
    if (WIFSIGNALED(status))
        return WTERMSIG(status);
    return 0;
}

#endif

} // namespace

void ProcessCancellation::cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }

bool ProcessCancellation::cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }

ProcessResult runProcess(const ProcessOptions &options, const ProcessCancellation *cancellation,
                         const ProcessOutputCallback &onOutput, const ProcessStartedCallback &onStarted) {
    ProcessResult result;
    result.logPath = options.logPath;
    if (options.arguments.empty()) {
        result.error = "Process command is empty";
        return result;
    }

    const auto start = std::chrono::steady_clock::now();
    const auto finish = [&] {
        result.durationMilliseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    };

#if defined(_WIN32)
    // Portable fallback. Timeout and streaming cancellation require a native
    // process API; the process still runs to completion here.
    if (cancellation && cancellation->cancelled()) {
        result.started = true;
        result.cancelled = true;
        result.error = "Process cancelled before start";
        finish();
        return result;
    }
    std::string command = joinArguments(options.arguments);
    std::error_code status;
    if (!options.logPath.empty()) {
        std::filesystem::create_directories(options.logPath.parent_path(), status);
        command += " > \"" + options.logPath.string() + "\" 2>&1";
    }
    const auto native = std::system(command.c_str());
    result.started = true;
    result.exitCode = native;
    result.pid = 0;
    if (!options.logPath.empty())
        result.logTail = readFileTail(options.logPath, options.logTailBytes);
    if (result.exitCode != 0)
        result.error = "Process exited with code " + std::to_string(result.exitCode);
    finish();
    return result;
#else
    std::error_code directoryStatus;
    if (!options.workingDirectory.empty())
        std::filesystem::create_directories(options.workingDirectory, directoryStatus);

    int logDescriptor = -1;
    if (!options.logPath.empty()) {
        std::filesystem::create_directories(options.logPath.parent_path(), directoryStatus);
        logDescriptor = ::open(options.logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (logDescriptor < 0) {
            result.error = "Unable to open process log: " + options.logPath.string();
            finish();
            return result;
        }
    }

    int outputPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if (::pipe(outputPipe) != 0 || ::pipe(errorPipe) != 0) {
        result.error = std::string("Unable to create process pipe: ") + std::strerror(errno);
        if (outputPipe[0] >= 0) {
            ::close(outputPipe[0]);
            ::close(outputPipe[1]);
        }
        if (errorPipe[0] >= 0) {
            ::close(errorPipe[0]);
            ::close(errorPipe[1]);
        }
        if (logDescriptor >= 0)
            ::close(logDescriptor);
        finish();
        return result;
    }
    ::fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC);

    const auto pid = ::fork();
    if (pid < 0) {
        result.error = std::string("Unable to fork process: ") + std::strerror(errno);
        ::close(outputPipe[0]);
        ::close(outputPipe[1]);
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        if (logDescriptor >= 0)
            ::close(logDescriptor);
        finish();
        return result;
    }

    if (pid == 0) {
        ::setpgid(0, 0);
        resetChildSignals();
        ::close(outputPipe[0]);
        ::close(errorPipe[0]);
        if (::dup2(outputPipe[1], STDOUT_FILENO) < 0 || ::dup2(outputPipe[1], STDERR_FILENO) < 0)
            ::_exit(127);
        ::close(outputPipe[1]);
        if (!options.workingDirectory.empty() && ::chdir(options.workingDirectory.c_str()) != 0) {
            const int error = errno;
            writeAll(errorPipe[1], reinterpret_cast<const char *>(&error), sizeof(error));
            ::_exit(127);
        }
        for (const auto &[name, value] : options.environment) {
            if (::setenv(name.c_str(), value.c_str(), 1) != 0) {
                const int error = errno;
                writeAll(errorPipe[1], reinterpret_cast<const char *>(&error), sizeof(error));
                ::_exit(127);
            }
        }
        std::vector<char *> arguments;
        arguments.reserve(options.arguments.size() + 1);
        for (const auto &argument : options.arguments)
            arguments.push_back(const_cast<char *>(argument.c_str()));
        arguments.push_back(nullptr);
        ::execvp(arguments.front(), arguments.data());
        const int error = errno;
        writeAll(errorPipe[1], reinterpret_cast<const char *>(&error), sizeof(error));
        ::_exit(127);
    }

    ::close(outputPipe[1]);
    ::close(errorPipe[1]);
    int execError = 0;
    ssize_t errorBytes = 0;
    do {
        errorBytes = ::read(errorPipe[0], &execError, sizeof(execError));
    } while (errorBytes < 0 && errno == EINTR);
    ::close(errorPipe[0]);
    if (errorBytes > 0) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        result.error = std::string("Unable to start '") + options.arguments.front() + "': " + std::strerror(execError);
        if (logDescriptor >= 0)
            ::close(logDescriptor);
        finish();
        return result;
    }

    result.started = true;
    result.pid = static_cast<int>(pid);
    if (onStarted)
        onStarted(result.pid);

    std::string tail;
    std::array<char, 8192> buffer{};
    const auto deadline = options.timeoutSeconds > 0 ? start + std::chrono::seconds(options.timeoutSeconds)
                                                     : std::chrono::steady_clock::time_point::max();
    bool terminateSent = false;
    bool killSent = false;
    auto terminationStart = std::chrono::steady_clock::now();
    bool eof = false;

    while (!eof) {
        struct pollfd descriptor{outputPipe[0], POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, 100);
        if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
            const auto bytes = ::read(outputPipe[0], buffer.data(), buffer.size());
            if (bytes > 0) {
                if (logDescriptor >= 0)
                    writeAll(logDescriptor, buffer.data(), static_cast<std::size_t>(bytes));
                appendTail(tail, buffer.data(), static_cast<std::size_t>(bytes), options.logTailBytes);
                if (onOutput)
                    onOutput(std::string(buffer.data(), static_cast<std::size_t>(bytes)));
            } else if (bytes == 0) {
                eof = true;
            } else if (errno != EINTR) {
                eof = true;
            }
        } else if (ready < 0 && errno != EINTR) {
            eof = true;
        }

        if (cancellation && cancellation->cancelled())
            result.cancelled = true;
        const auto now = std::chrono::steady_clock::now();
        if (!result.timedOut && now >= deadline)
            result.timedOut = true;
        if (!terminateSent && (result.cancelled || result.timedOut)) {
            ::kill(-pid, SIGTERM);
            ::kill(pid, SIGTERM);
            terminateSent = true;
            terminationStart = now;
        }
        if (terminateSent && !killSent && now - terminationStart > std::chrono::seconds(2)) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            killSent = true;
        }
    }

    int status = 0;
    bool reaped = false;
    while (!reaped) {
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            reaped = true;
            break;
        }
        if (waited < 0 && errno != EINTR)
            break;
        const auto now = std::chrono::steady_clock::now();
        if (cancellation && cancellation->cancelled())
            result.cancelled = true;
        if (!result.timedOut && now >= deadline)
            result.timedOut = true;
        if (!terminateSent && (result.cancelled || result.timedOut)) {
            ::kill(-pid, SIGTERM);
            ::kill(pid, SIGTERM);
            terminateSent = true;
            terminationStart = now;
        }
        if (terminateSent && !killSent && now - terminationStart > std::chrono::seconds(2)) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            killSent = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::close(outputPipe[0]);
    if (logDescriptor >= 0)
        ::close(logDescriptor);

    if (reaped) {
        result.exitCode = childExitCode(status);
        result.signal = childSignal(status);
    }
    // An externally signalled OpenROAD/Yosys process is treated as a
    // cancellation so the run can be resumed instead of reported as a crash.
    if (!result.cancelled && (result.signal == SIGTERM || result.signal == SIGINT))
        result.cancelled = true;
    if (result.cancelled)
        result.exitCode = -1;
    else if (result.signal != 0)
        result.error = "Process terminated by signal " + std::to_string(result.signal);
    else if (result.exitCode != 0)
        result.error = "Process exited with code " + std::to_string(result.exitCode);
    if (result.timedOut)
        result.error = "Process exceeded the " + std::to_string(options.timeoutSeconds) + " second timeout";
    result.logTail = std::move(tail);
    finish();
    return result;
#endif
}

} // namespace ice
