#include "icelib/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
std::wstring utf8ToWide(const std::string &text) {
    if (text.empty())
        return {};
    int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string wideToUtf8(const std::wstring &text) {
    if (text.empty())
        return {};
    const auto length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
                        nullptr);
    return result;
}

std::wstring quoteWindowsArgument(const std::wstring &argument) {
    if (argument.empty())
        return L"\"\"";
    const bool needsQuotes = argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes)
        return argument;

    std::wstring result = L"\"";
    for (std::size_t index = 0; index < argument.size();) {
        std::size_t backslashes = 0;
        while (index < argument.size() && argument[index] == L'\\') {
            ++index;
            ++backslashes;
        }
        if (index == argument.size()) {
            result.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[index] == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(argument[index]);
        }
        ++index;
    }
    result.push_back(L'"');
    return result;
}

std::wstring buildCommandLine(const std::vector<std::string> &arguments) {
    std::wstring commandLine;
    for (const auto &argument : arguments) {
        if (!commandLine.empty())
            commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(utf8ToWide(argument));
    }
    return commandLine;
}

std::wstring upperWindows(std::wstring text) {
    for (auto &character : text)
        character = static_cast<wchar_t>(std::towupper(character));
    return text;
}

std::wstring buildEnvironmentBlock(const std::map<std::string, std::string> &overrides) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (LPWCH block = GetEnvironmentStringsW()) {
        for (LPWCH entry = block; *entry;) {
            const std::wstring value(entry);
            entry += value.size() + 1;
            const auto equals = value.find(L'=', value.empty() || value.front() != L'=' ? 0 : 1);
            if (equals == std::wstring::npos)
                continue;
            entries.emplace_back(value.substr(0, equals), value.substr(equals + 1));
        }
        FreeEnvironmentStringsW(block);
    }

    for (const auto &[name, value] : overrides) {
        const auto wideName = utf8ToWide(name);
        const auto wideValue = utf8ToWide(value);
        const auto upperName = upperWindows(wideName);
        bool replaced = false;
        for (auto &[existingName, existingValue] : entries) {
            if (upperWindows(existingName) == upperName) {
                existingValue = wideValue;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            entries.emplace_back(wideName, wideValue);
    }

    std::sort(entries.begin(), entries.end(),
              [](const auto &left, const auto &right) { return upperWindows(left.first) < upperWindows(right.first); });
    std::wstring block;
    for (const auto &[name, value] : entries) {
        block += name;
        block.push_back(L'=');
        block += value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::string windowsErrorMessage(DWORD error) {
    LPWSTR buffer = nullptr;
    const auto length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = buffer && length > 0 ? std::wstring(buffer, length) : L"unknown error";
    if (buffer)
        LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
        message.pop_back();
    return wideToUtf8(message);
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
    if (cancellation && cancellation->cancelled()) {
        result.started = true;
        result.cancelled = true;
        result.error = "Process cancelled before start";
        finish();
        return result;
    }

    std::error_code directoryStatus;
    if (!options.workingDirectory.empty())
        std::filesystem::create_directories(options.workingDirectory, directoryStatus);

    std::ofstream log;
    if (!options.logPath.empty()) {
        if (!options.logPath.parent_path().empty())
            std::filesystem::create_directories(options.logPath.parent_path(), directoryStatus);
        log.open(options.logPath, std::ios::binary | std::ios::trunc);
        if (!log) {
            result.error = "Unable to open process log: " + options.logPath.string();
            finish();
            return result;
        }
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE readPipe = INVALID_HANDLE_VALUE;
    HANDLE writePipe = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) {
        result.error = "Unable to create process pipe: " + windowsErrorMessage(GetLastError());
        finish();
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    const auto commandLineText = buildCommandLine(options.arguments);
    std::vector<wchar_t> commandLine(commandLineText.begin(), commandLineText.end());
    commandLine.push_back(L'\0');

    std::wstring environmentBlock;
    LPVOID environment = nullptr;
    if (!options.environment.empty()) {
        environmentBlock = buildEnvironmentBlock(options.environment);
        environment = environmentBlock.data();
    }

    const auto workingDirectory =
        options.workingDirectory.empty() ? std::wstring{} : options.workingDirectory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(
        nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process);
    if (!created) {
        const auto error = GetLastError();
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        result.error = "Unable to start '" + options.arguments.front() + "': " + windowsErrorMessage(error);
        finish();
        return result;
    }
    CloseHandle(writePipe);
    writePipe = INVALID_HANDLE_VALUE;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    bool jobAssigned = false;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        jobAssigned = AssignProcessToJobObject(job, process.hProcess) != 0;
    }

    result.started = true;
    result.pid = static_cast<int>(process.dwProcessId);
    if (onStarted)
        onStarted(result.pid);

    std::string tail;
    std::array<char, 8192> buffer{};
    const auto deadline = options.timeoutSeconds > 0 ? start + std::chrono::seconds(options.timeoutSeconds)
                                                     : std::chrono::steady_clock::time_point::max();
    const auto drain = [&] {
        DWORD available = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD bytes = 0;
            const auto wanted = static_cast<DWORD>(std::min<std::size_t>(available, buffer.size()));
            if (!ReadFile(readPipe, buffer.data(), wanted, &bytes, nullptr) || bytes == 0)
                break;
            if (log)
                log.write(buffer.data(), static_cast<std::streamsize>(bytes));
            appendTail(tail, buffer.data(), static_cast<std::size_t>(bytes), options.logTailBytes);
            if (onOutput)
                onOutput(std::string(buffer.data(), static_cast<std::size_t>(bytes)));
        }
    };

    bool terminated = false;
    bool processExited = false;
    DWORD exitCode = 1;
    while (!processExited) {
        drain();
        const auto wait = WaitForSingleObject(process.hProcess, 25);
        if (wait == WAIT_OBJECT_0) {
            processExited = true;
            drain();
            GetExitCodeProcess(process.hProcess, &exitCode);
            break;
        }
        if (wait == WAIT_FAILED)
            break;

        const auto now = std::chrono::steady_clock::now();
        if (cancellation && cancellation->cancelled())
            result.cancelled = true;
        if (!result.timedOut && now >= deadline)
            result.timedOut = true;
        if (!terminated && (result.cancelled || result.timedOut)) {
            terminated = true;
            if (jobAssigned)
                TerminateJobObject(job, 1);
            else
                TerminateProcess(process.hProcess, 1);
        }
    }

    if (!processExited) {
        if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
        }
        drain();
    }
    GetExitCodeProcess(process.hProcess, &exitCode);
    if (log)
        log.flush();
    CloseHandle(readPipe);
    if (job)
        CloseHandle(job);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    result.exitCode = static_cast<int>(exitCode);
    if (result.cancelled || result.timedOut)
        result.exitCode = -1;
    if (result.timedOut)
        result.error = "Process exceeded the " + std::to_string(options.timeoutSeconds) + " second timeout";
    else if (result.cancelled)
        result.error = "Process cancelled";
    else if (result.exitCode != 0)
        result.error = "Process exited with code " + std::to_string(result.exitCode);
    result.logTail = std::move(tail);
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
