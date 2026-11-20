#include "core.hpp"
#include "xlsx.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace msa {
namespace {
std::string read_file_bytes(const std::filesystem::path &path, std::size_t max_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("无法读取文件：" + path.u8string());
    std::array<char, 64 * 1024> buffer;
    std::string bytes;
    do {
        const auto remaining = max_bytes - bytes.size();
        const auto request = remaining < buffer.size() ? remaining + 1 : buffer.size();
        in.read(buffer.data(), static_cast<std::streamsize>(request));
        const auto count = static_cast<std::size_t>(in.gcount());
        if (count > remaining)
            throw std::runtime_error("文件超过读取上限（" + std::to_string(max_bytes) +
                                     " 字节）：" + path.u8string());
        bytes.append(buffer.data(), count);
    } while (in);
    if (in.bad() || !in.eof())
        throw std::runtime_error("读取失败：" + path.u8string());
    return bytes;
}
} // namespace
std::filesystem::path config_path() {
#ifdef _WIN32
    wchar_t home[32768]{};
    if (!GetEnvironmentVariableW(L"USERPROFILE", home, 32768))
        throw std::runtime_error("无法确定用户配置目录");
    return std::filesystem::path(home) / L"MSA数据生成器" / L"msa_generator.json";
#else
    auto home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("HOME 未设置");
    return std::filesystem::path(home) / "MSA数据生成器" / "msa_generator.json";
#endif
}
std::filesystem::path desktop_directory() {
#ifdef _WIN32
    PWSTR raw = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DONT_VERIFY, nullptr, &raw);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(raw, &CoTaskMemFree);
    if (FAILED(result) || !raw || !*raw)
        throw ConfigError("无法确定系统桌面位置，请设置完整的数据目录。");
    return std::filesystem::path(raw);
#else
    const char *home_env = std::getenv("HOME");
    if (!home_env || !std::filesystem::path(home_env).is_absolute())
        throw ConfigError("无法确定用户桌面，请设置 HOME 或完整的数据目录。");
    const std::filesystem::path home(home_env);
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const auto settings = xdg && std::filesystem::path(xdg).is_absolute()
                              ? std::filesystem::path(xdg) : home / ".config";
    std::ifstream dirs(settings / "user-dirs.dirs");
    std::string line;
    constexpr std::string_view key = "XDG_DESKTOP_DIR=";
    while (std::getline(dirs, line)) {
        if (line.compare(0, key.size(), key) != 0)
            continue;
        // The XDG file stores absolute paths or a quoted $HOME-relative path.
        // Interpret the value locally; shell expressions are never evaluated.
        std::istringstream field(line.substr(key.size()));
        std::string value;
        if (!(field >> std::quoted(value)) || value.empty())
            break;
        if (value == "$HOME" || value == "${HOME}")
            return home;
        for (const std::string_view prefix : {"$HOME/", "${HOME}/"})
            if (value.compare(0, prefix.size(), prefix) == 0)
                return (home / value.substr(prefix.size())).lexically_normal();
        if (std::filesystem::path(value).is_absolute())
            return std::filesystem::path(value).lexically_normal();
        break;
    }
    return home / "Desktop";
#endif
}
std::filesystem::path resolve_output_directory(const std::string &setting) {
    auto directory = std::filesystem::u8path(setting);
    if (directory.has_root_name() && !directory.has_root_directory())
        throw ConfigError("输出目录请使用完整路径或普通相对路径，不能使用盘符相对路径。");
#ifdef _WIN32
    if (!directory.has_root_name() && directory.has_root_directory())
        throw ConfigError("目录以路径分隔符开头时须包含盘符或完整网络共享路径。");
#endif
    if (directory.empty())
        return desktop_directory();
    if (directory.is_relative())
        directory = desktop_directory() / directory;
    return directory.lexically_normal();
}
std::filesystem::path configured_data_path(const Json &cfg) {
    const auto c = normalize_config(cfg);
    return (resolve_output_directory(c["output"]["directory"].get<std::string>()) /
            std::filesystem::u8path(c["output"]["filename"].get<std::string>() + "." +
                                    c["output"]["file_type"].get<std::string>()))
        .lexically_normal();
}
std::filesystem::path configured_csv_path(const Json &cfg) { return configured_data_path(cfg); }
void atomic_text(const std::filesystem::path &path, const std::string &bytes, ExistingFile policy) {
    auto dir = path.parent_path();
    if (dir.empty())
        dir = ".";
    std::filesystem::create_directories(dir);
    std::filesystem::path temp;
#ifdef _WIN32
    // CREATE_NEW avoids collisions between simultaneous exports and processes.
    static std::atomic<unsigned long> counter{0};
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100; ++i) {
        // 临时名称不拼接目标名称，保留接近文件系统组件长度上限的合法文件名。
        temp = dir / (L".msa-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(++counter) + L".tmp");
        file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS)
            throw std::runtime_error("无法创建临时文件：" + path.u8string());
    }
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("无法分配临时文件");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0,
                  len =
                      static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 0x40000000));
            if (!WriteFile(file, bytes.data() + offset, len, &written, nullptr) || !written)
                throw std::runtime_error("文件写入失败");
            offset += written;
        }
        if (!FlushFileBuffers(file))
            throw std::runtime_error("文件同步失败");
        if (!CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            throw std::runtime_error("文件关闭失败");
        }
        file = INVALID_HANDLE_VALUE;
        const DWORD flags = MOVEFILE_WRITE_THROUGH |
                            (policy == ExistingFile::Replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temp.c_str(), path.c_str(), flags)) {
            const auto error = GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
                throw FileExistsError("文件已存在，未覆盖：" + path.u8string());
            throw std::runtime_error("无法替换目标文件：" + path.u8string());
        }
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        std::error_code e;
        std::filesystem::remove(temp, e);
        throw;
    }
#else
    std::string pattern = (dir / ".msa-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    if (fd < 0)
        throw std::runtime_error("无法创建临时文件");
    temp = name.data();
    try {
        std::size_t off = 0;
        while (off < bytes.size()) {
            auto n = write(fd, bytes.data() + off, bytes.size() - off);
            if (n <= 0)
                throw std::runtime_error("文件写入失败");
            off += n;
        }
        if (fsync(fd))
            throw std::runtime_error("同步失败");
        if (close(fd)) {
            fd = -1;
            throw std::runtime_error("文件关闭失败");
        }
        fd = -1;
        if (policy == ExistingFile::Replace)
            std::filesystem::rename(temp, path);
        else {
            // Same-directory hard link is an atomic, portable no-replace commit.
            if (link(temp.c_str(), path.c_str())) {
                if (errno == EEXIST)
                    throw FileExistsError("文件已存在，未覆盖：" + path.u8string());
                throw std::runtime_error("无法提交目标文件：" + path.u8string());
            }
            std::filesystem::remove(temp);
        }
    } catch (...) {
        if (fd >= 0)
            close(fd);
        std::error_code e;
        std::filesystem::remove(temp, e);
        throw;
    }
#endif
}
bool same_file_path(const std::filesystem::path &a, const std::filesystem::path &b) {
    std::error_code error;
    if (std::filesystem::equivalent(a, b, error))
        return true;
    const auto aa = std::filesystem::weakly_canonical(std::filesystem::absolute(a));
    const auto bb = std::filesystem::weakly_canonical(std::filesystem::absolute(b));
#ifdef _WIN32
    return CompareStringOrdinal(aa.c_str(), -1, bb.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return aa == bb;
#endif
}
std::string read_text(const std::filesystem::path &path, std::size_t max_bytes) {
    auto s = read_file_bytes(path, max_bytes);
    if (s.compare(0, 3, "\xef\xbb\xbf") == 0)
        s.erase(0, 3);
    return s;
}
void save_config(const Json &cfg, const std::filesystem::path &path) {
    atomic_text(path, normalize_config(cfg).dump(2) + '\n');
}
Json load_config(const std::filesystem::path &path) {
    if (!std::filesystem::exists(path)) {
        try {
            atomic_text(path, default_config().dump(2) + '\n', ExistingFile::Preserve);
        } catch (const FileExistsError &) {
            // A concurrent initializer won. Read it instead of replacing it.
        }
    }
    return normalize_config(Json::parse(read_text(path, config_read_limit)));
}
std::string csv_text(const Matrix &matrix, int dp) {
    std::string s;
    for (auto &row : matrix) {
        bool first = true;
        for (double v : row) {
            if (!first)
                s += ',';
            first = false;
            s += fixed(v, dp);
        }
        s += "\r\n";
    }
    return s;
}
Matrix read_csv(const std::filesystem::path &path) {
    return parse_csv(read_text(path, csv_read_limit));
}
std::string data_file_type(const std::filesystem::path &path) {
    auto extension = path.extension().u8string();
    for (char &ch : extension)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + ('a' - 'A'));
    return extension == ".xlsx" ? "xlsx" : "csv";
}
namespace {
Matrix decode_data(const std::string &bytes, const std::string &type, const Json &config) {
    return type == "xlsx" ? parse_export_csv(csv_from_xlsx(bytes), config)
                          : parse_export_csv(bytes, config);
}
std::string encode_data(const Result &study, const Json &settings, const std::string &type,
                        int decimal_places) {
    auto bytes = export_csv_text(study, settings);
    if (type == "xlsx")
        return xlsx_from_csv(bytes, decimal_places);
    return bytes;
}
ImportedData import_sheet(const XlsxWorkbook &workbook, std::size_t index, const Json &config) {
    const auto &names = workbook.sheet_names();
    if (index >= names.size())
        throw ConfigError("XLSX 所选工作表编号无效");
    try {
        return {parse_export_csv(workbook.csv(index), config), names[index]};
    } catch (const std::invalid_argument &e) {
        throw ConfigError("工作表“" + names[index] + "”：" + e.what());
    }
}
} // namespace
Matrix read_data(const std::filesystem::path &path, const Json &config) {
    const auto type = data_file_type(path);
    return decode_data(read_file_bytes(path, type == "xlsx" ? xlsx_read_limit : csv_read_limit),
                       type, config);
}
Matrix read_data(const std::filesystem::path &path, const Json &config,
                 const std::string &sheet_name) {
    if (data_file_type(path) != "xlsx")
        throw ConfigError("只有 XLSX 工作簿可以指定工作表");
    const XlsxWorkbook workbook(read_file_bytes(path, xlsx_read_limit));
    const auto &names = workbook.sheet_names();
    const auto sheet = std::find(names.begin(), names.end(), sheet_name);
    if (sheet == names.end())
        throw ConfigError("XLSX 找不到工作表“" + sheet_name + "”");
    return import_sheet(workbook, static_cast<std::size_t>(sheet - names.begin()), config).matrix;
}
std::optional<ImportedData> read_data(const std::filesystem::path &path, const Json &config,
                                      const WorksheetSelector &selector) {
    if (data_file_type(path) != "xlsx")
        return ImportedData{parse_export_csv(read_file_bytes(path, csv_read_limit), config), {}};
    const XlsxWorkbook workbook(read_file_bytes(path, xlsx_read_limit));
    const auto &names = workbook.sheet_names();
    if (names.size() == 1 || !selector)
        return import_sheet(workbook, 0, config);
    const auto index = selector(names);
    if (!index)
        return std::nullopt;
    return import_sheet(workbook, *index, config);
}
Json export_data(const Result &result, const std::filesystem::path &path, ExistingFile policy) {
    return export_data(result, path, result.config.value("output", Json::object()), policy);
}
Json export_data(const Result &result, const std::filesystem::path &path, const Json &output,
                 ExistingFile policy, Json *receipt) {
    // Immutable generation configuration is the sole authority for all checks.
    auto cfg = validate_request(result.config);
    if (!qualified(calculate(result.matrix, cfg), cfg))
        throw std::invalid_argument("内存数据复验失败，未写入文件。");
    const auto rounded = parse_csv(csv_text(result.matrix, cfg["generation"]["decimal_places"]));
    if (!qualified(calculate(rounded, cfg), cfg))
        throw std::invalid_argument("导出精度复验失败，未写入文件。");
    const auto type = data_file_type(path);
    const auto content = encode_data(result, output, type, cfg["generation"]["decimal_places"]);
    if (decode_data(content, type, cfg) != rounded)
        throw std::invalid_argument("导出记录对应关系复验失败，未写入文件。");
    atomic_text(path, content, policy);
    // Verify raw bytes, including the format-specific BOM and row order.
    const auto saved = read_file_bytes(path, content.size());
    if (saved != content)
        throw std::invalid_argument("保存后的数据文件内容或运行顺序与预期不符，请勿使用该文件。");
    auto actual = calculate(decode_data(saved, type, cfg), cfg);
    if (!qualified(actual, cfg))
        throw std::invalid_argument("保存后的数据文件复验失败，请勿使用该文件。");
    if (receipt) {
        const auto settings = normalize_output(output);
        *receipt = {{"schema_version", 1},
                    {"file_type", type},
                    {"data_path", path.u8string()},
                    {"format", settings.at("format")},
                    {"run_order", settings.at("run_order")},
                    {"effective_run_seed", effective_run_seed(result, settings)},
                    {"sha256", sha256(saved)}};
        if (type == "csv")
            (*receipt)["csv_path"] = path.u8string();
    }
    return actual;
}
Json export_csv(const Result &result, const std::filesystem::path &path, ExistingFile policy) {
    return export_data(result, path, policy);
}
Json export_csv(const Result &result, const std::filesystem::path &path, const Json &output,
                ExistingFile policy, Json *receipt) {
    return export_data(result, path, output, policy, receipt);
}
void replay_data(const Json &report, const std::filesystem::path &path, ExistingFile policy) {
    if (!report.is_object() || !report.contains("export") || report.value("analysis_only", false))
        throw ConfigError("重放需要含 export 清单的生成报告。请用 --report 保存新版生成报告。");
    const auto &receipt = report.at("export");
    if (!receipt.is_object() || receipt.at("schema_version") != 1)
        throw ConfigError("不支持此数据文件导出清单版本");
    const auto type = receipt.value("file_type", std::string("csv"));
    if (type != data_file_type(path))
        throw ConfigError("重放目标类型与报告中的原始数据文件类型不一致");
    Result study;
    study.config = normalize_config(report.at("config"));
    study.matrix = report.at("matrix").get<Matrix>();
    auto settings = study.config.at("output");
    settings["format"] = receipt.at("format");
    settings["run_order"] = receipt.at("run_order");
    const auto &seed = receipt.at("effective_run_seed");
    const bool shuffled =
        settings.at("format") != "default" && settings.at("run_order").at("mode") != "standard";
    if (shuffled == seed.is_null())
        throw ConfigError("导出清单中的实际排序种子与运行顺序不一致");
    settings["run_order"]["seed"] = seed;
    const auto bytes = encode_data(study, settings, type, study.config["generation"]["decimal_places"]);
    if (sha256(bytes) != receipt.at("sha256"))
        throw ConfigError("重建数据文件的 SHA-256 与报告不一致，未写入文件");
    atomic_text(path, bytes, policy);
}
void replay_csv(const Json &report, const std::filesystem::path &path, ExistingFile policy) {
    replay_data(report, path, policy);
}
} // namespace msa
