#include "msa/report_pdf.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;

// A process-level fixture. The real browser test checks PDF rendering and layout.
constexpr char body[] = "%PDF-1.7\n1 0 obj\n<<>>\nendobj\n";
constexpr char trailer[] = "%%EOF\r\n";
const std::string complete = std::string(body) + trailer;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int browser(const fs::path &self, const fs::path &output) {
    std::ifstream input(output.parent_path() / "study.html");
    std::string scenario;
    require(static_cast<bool>(std::getline(input, scenario)), "missing fixture scenario");
    const fs::path launches = fs::temp_directory_path() / "launches";
    std::ofstream(launches, std::ios::binary | std::ios::app).put('x');
    if (scenario == "launcher") {
        // Return successfully while a child inside the same Windows job is still printing.
        std::wstring command = L"\"" + self.wstring() + L"\" --delayed-writer \"" + output.wstring() + L"\"";
        PROCESS_INFORMATION process{};
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        require(CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
                "could not start the PDF writer fixture");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else if (scenario == "cold-start") {
        // Some first launches exit cleanly without starting a printer.
        if (fs::file_size(launches) > 1)
            std::ofstream(output, std::ios::binary) << complete;
    } else if (scenario == "incomplete") {
        std::ofstream(output, std::ios::binary) << body << "% earlier %%EOF marker\nunfinished";
    } else {
        throw std::runtime_error("unknown fixture scenario");
    }
    return 0;
}

void run_cases(const fs::path &self, const fs::path &parent) {
    fs::create_directories(parent);
    const auto temp = fs::absolute(parent) /
                      (L"PDF 临时目录 %" + std::to_wstring(GetCurrentProcessId()) +
                       L"-" + std::to_wstring(GetTickCount64()));
    require(fs::create_directory(temp), "fixture temp directory exists");
    require(SetEnvironmentVariableW(L"TMP", temp.c_str()) &&
                SetEnvironmentVariableW(L"TEMP", temp.c_str()), "could not isolate PDF temp directory");
    const auto launches = temp / "launches";
    for (const char *scenario : {"launcher", "cold-start"}) {
        require(msa::detail::print_report_pdf_with_browser(scenario, self) == complete,
                "backend returned before the whole PDF was ready");
        require(fs::file_size(launches) == (std::string(scenario) == "launcher" ? 1u : 2u),
                "incorrect number of browser launches");
        fs::remove(launches);
        require(fs::is_empty(temp), "PDF job left temporary files after a successful export");
    }
    bool rejected = false;
    try {
        msa::detail::print_report_pdf_with_browser("incomplete", self);
    } catch (const std::runtime_error &error) {
        require(std::string(error.what()).find("未生成完整的 PDF") != std::string::npos,
                "truncated output should report a browser printing error");
        rejected = true;
    }
    require(rejected && fs::file_size(launches) == 2,
            "unfinished PDF was accepted or retry was skipped");
    fs::remove(launches);
    require(fs::is_empty(temp), "PDF job left temporary files after a failed export");
    fs::remove(temp);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--delayed-writer") {
            Sleep(120);
            const fs::path output(argv[2]);
            std::ofstream(output, std::ios::binary) << body;
            Sleep(200);
            const HANDLE writer = CreateFileW(output.c_str(), FILE_APPEND_DATA, 0, nullptr,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(writer != INVALID_HANDLE_VALUE, "could not lock the fixture PDF");
            DWORD written = 0;
            const bool completeWrite = WriteFile(writer, trailer, sizeof(trailer) - 1,
                                                 &written, nullptr) && written == sizeof(trailer) - 1;
            Sleep(100);
            CloseHandle(writer);
            require(completeWrite, "could not finish the fixture PDF");
            return 0;
        }
        const auto self = fs::absolute(argv[0]);
        const std::wstring prefix = L"--print-to-pdf=";
        for (int index = 1; index < argc; ++index) {
            const std::wstring arg = argv[index];
            if (arg.compare(0, prefix.size(), prefix) == 0)
                return browser(self, arg.substr(prefix.size()));
        }
        require(argc == 2, "expected a test output directory");
        run_cases(self, argv[1]);
        std::cout << "PDF launcher handoff, cold-start recovery and truncated output passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
