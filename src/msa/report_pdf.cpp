#include "core.hpp"
#include "report_pdf.hpp"
#include <algorithm>
#include <cstdio>
#include <random>
#include <string_view>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <share.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace msa {
namespace {
namespace fs = std::filesystem;
constexpr auto print_timeout = std::chrono::seconds(60);
constexpr auto browser_error = "导出 PDF 需要本机的 Microsoft Edge 或 Google Chrome。";
constexpr auto incomplete_pdf = "浏览器未生成完整的 PDF 报告。";

class PrintDirectory {
    fs::path parent_ = fs::absolute(fs::temp_directory_path()).lexically_normal();
    fs::path path_;

  public:
    PrintDirectory() {
#ifdef _WIN32
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            auto candidate = parent_ / ("msa-print-" + std::to_string(random()) + "-" +
                                        std::to_string(random()));
            if (fs::create_directory(candidate)) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("无法创建 PDF 临时目录。");
#else
        auto pattern = (parent_ / "msa-print-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw std::runtime_error("无法创建 PDF 临时目录。");
        path_ = pattern;
#endif
    }
    PrintDirectory(const PrintDirectory &) = delete;
    ~PrintDirectory() {
        // On Windows GetTempPath can return a trailing separator. Compare the
        // underlying parent directory rather than the spelling of its path.
        std::error_code error;
        if (!fs::equivalent(path_.parent_path(), parent_, error) ||
            path_.filename().u8string().rfind("msa-print-", 0) != 0)
            return;
        fs::remove(path_ / "study.html", error);
        fs::remove(path_ / "study.pdf", error);
        // Antivirus scanning and a terminating browser can briefly hold profile files open.
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(20 << (attempt - 1)));
            fs::remove_all(path_, error);
            if (!error)
                return;
        }
    }
    const fs::path &path() const { return path_; }
};

std::string file_url(const fs::path &file) {
    const auto path = file.generic_u8string();
    std::string url = path.rfind("//", 0) == 0 ? "file:"
                      : path.front() == '/'    ? "file://"
                                               : "file:///";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : path) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '/' || ch == ':' || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            url += static_cast<char>(ch);
        else {
            url += '%';
            url += hex[ch >> 4];
            url += hex[ch & 15];
        }
    }
    return url;
}

std::vector<std::string> print_arguments(const fs::path &input, const fs::path &output) {
    return {"--headless", "--disable-gpu", "--disable-extensions", "--no-first-run",
            "--disable-background-networking", "--no-pdf-header-footer",
            "--user-data-dir=" + (input.parent_path() / "profile").u8string(),
            "--print-to-pdf=" + output.u8string(), file_url(input)};
}

bool ends_with_pdf_eof(std::string_view bytes) {
    const auto last = bytes.find_last_not_of(" \t\r\n\f");
    return last != bytes.npos && last >= 4 && bytes.substr(last - 4, 5) == "%%EOF";
}

#ifdef _WIN32
// Open only when the writer has released the file. Check the tail before
// allocating: a partially written report may be probed more than once.
std::string completed_pdf(const fs::path &output) {
    std::unique_ptr<FILE, decltype(&std::fclose)> file(_wfsopen(output.c_str(), L"rb", _SH_DENYWR),
                                                       &std::fclose);
    if (!file || std::fseek(file.get(), 0, SEEK_END) != 0)
        return {};
    const auto size = std::ftell(file.get());
    if (size < 0 || size > 64 * 1024 * 1024)
        throw std::runtime_error("PDF 报告超出读取限制。");
    if (size < 10)
        return {};
    char tail[1024];
    const auto count = static_cast<std::size_t>(std::min(size, static_cast<long>(sizeof(tail))));
    if (std::fseek(file.get(), -static_cast<long>(count), SEEK_END) != 0 ||
        std::fread(tail, 1, count, file.get()) != count || !ends_with_pdf_eof({tail, count}))
        return {};
    std::rewind(file.get());
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (std::fread(bytes.data(), 1, bytes.size(), file.get()) != bytes.size() ||
        bytes.compare(0, 5, "%PDF-") != 0)
        return {};
    return bytes;
}

fs::path browser_path() {
    for (const auto *binary : {L"msedge.exe", L"chrome.exe"}) {
        const auto key =
            std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + binary;
        for (auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            wchar_t path[32768];
            DWORD size = sizeof(path);
            if (RegGetValueW(hive, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) ==
                    ERROR_SUCCESS &&
                fs::is_regular_file(path))
                return path;
        }
    }
    for (const auto *variable : {L"ProgramFiles(x86)", L"ProgramFiles", L"LOCALAPPDATA"}) {
        wchar_t folder[32768];
        const auto length = GetEnvironmentVariableW(variable, folder, 32768);
        if (!length || length >= 32768)
            continue;
        for (const auto *relative : {L"Microsoft\\Edge\\Application\\msedge.exe",
                                     L"Google\\Chrome\\Application\\chrome.exe"}) {
            const auto p = fs::path(folder) / relative;
            if (fs::is_regular_file(p))
                return p;
        }
    }
    throw std::runtime_error(browser_error);
}
struct Handle {
    HANDLE value;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
void stop_job(HANDLE job) {
    // Closing a kill-on-close job starts termination asynchronously. Wait for the
    // child processes to release their profile before removing the temporary files.
    if (!TerminateJobObject(job, 1))
        return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    while (std::chrono::steady_clock::now() < deadline &&
           QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info),
                                     nullptr) &&
           info.ActiveProcesses)
        Sleep(10);
}
std::string print(const fs::path &browser, const fs::path &input, const fs::path &output,
                  std::chrono::steady_clock::time_point deadline) {
    std::wstring command = L"\"" + browser.wstring() + L"\"";
    // All arguments are generated here. Windows file names cannot contain a quote,
    // and every path argument ends with a fixed file or directory name.
    for (const auto &arg : print_arguments(input, output))
        command += L" \"" + fs::u8path(arg).wstring() + L"\"";
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                               &limits, sizeof(limits)))
        throw std::runtime_error("无法准备 PDF 后台任务。");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(browser.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("无法启动 PDF 后台打印。");
    Handle browser_process{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, browser_process.value) ||
        ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(browser_process.value, 1);
        WaitForSingleObject(browser_process.value, 2000);
        throw std::runtime_error("无法启动隔离的 PDF 后台任务。");
    }
    struct StopJobOnReturn {
        HANDLE job;
        ~StopJobOnReturn() { stop_job(job); }
    } cleanup{job.value};
    // On cold startup the launcher may exit before its printing child.
    // Sample job activity first so a child's last write is visible on the final check.
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &info,
                                       sizeof(info), nullptr))
            throw std::runtime_error("无法读取 PDF 后台任务状态。");
        auto bytes = completed_pdf(output);
        if (!bytes.empty())
            return bytes;
        if (!info.ActiveProcesses) {
            DWORD code;
            if (!GetExitCodeProcess(browser_process.value, &code) || code != 0)
                throw std::runtime_error("浏览器生成 PDF 失败，请检查本机浏览器是否可用。");
            return {};
        }
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("PDF 导出超时，请重试。");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
#else
void print(const std::vector<std::string> &args) {
    std::vector<char *> argv{nullptr};
    for (const auto &arg : args)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions) != 0)
        throw std::runtime_error("无法准备 PDF 后台任务。");
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        throw std::runtime_error("无法准备 PDF 后台任务。");
    }
    const bool ready =
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) == 0 &&
        posix_spawnattr_setpgroup(&attr, 0) == 0 &&
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
    pid_t pid = -1;
    if (ready) {
        for (const char *browser :
             {"microsoft-edge", "google-chrome", "chromium", "chromium-browser",
              "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
              "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}) {
            argv.front() = const_cast<char *>(browser);
            if (posix_spawnp(&pid, browser, &actions, &attr, argv.data(), environ) == 0)
                break;
            pid = -1;
        }
    }
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (pid < 0)
        throw std::runtime_error(browser_error);
    const auto deadline = std::chrono::steady_clock::now() + print_timeout;
    bool exited = false;
    while (std::chrono::steady_clock::now() < deadline) {
        // Keep the leader waitable until its private process group has been
        // terminated. Its PID cannot be reused for a different group in the meantime.
        siginfo_t info{};
        const int result =
            waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
        if (result == 0 && info.si_pid == pid) {
            exited = true;
            break;
        }
        if (result < 0 && errno != EINTR)
            throw std::runtime_error("无法读取 PDF 后台任务状态。");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(-pid, SIGKILL);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (!exited)
        throw std::runtime_error("PDF 导出超时，请重试。");
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("浏览器生成 PDF 失败，请检查本机浏览器是否可用。");
}
#endif
} // namespace

#ifdef _WIN32
std::string detail::print_report_pdf_with_browser(const std::string &html, const fs::path &browser) {
    PrintDirectory temp;
    const auto input = temp.path() / "study.html";
    atomic_text(input, html, ExistingFile::Preserve);
    const auto deadline = std::chrono::steady_clock::now() + print_timeout;
    // Some first-run launchers initialize the isolated profile and then exit
    // successfully without printing. Reuse that profile but never a partial file.
    for (const auto *filename : {"study.pdf", "study-retry.pdf"}) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("PDF 导出超时，请重试。");
        auto bytes = print(browser, input, temp.path() / filename, deadline);
        if (!bytes.empty())
            return bytes;
    }
    throw std::runtime_error(incomplete_pdf);
}

std::string print_report_pdf(const std::string &html) {
    return detail::print_report_pdf_with_browser(html, browser_path());
}
#else
std::string print_report_pdf(const std::string &html) {
    PrintDirectory temp;
    const auto input = temp.path() / "study.html", output = temp.path() / "study.pdf";
    atomic_text(input, html, ExistingFile::Preserve);
    print(print_arguments(input, output));
    auto bytes = read_text(output, 64 * 1024 * 1024);
    if (bytes.compare(0, 5, "%PDF-") != 0 || !ends_with_pdf_eof(bytes))
        throw std::runtime_error(incomplete_pdf);
    return bytes;
}
#endif
} // namespace msa
