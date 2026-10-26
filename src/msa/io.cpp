#include "core.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace msa {
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
std::filesystem::path configured_csv_path(const Json &cfg) {
    const auto c = normalize_config(cfg);
    auto directory = std::filesystem::u8path(c["output"]["directory"].get<std::string>());
    if (directory.has_root_name() && !directory.has_root_directory())
        throw ConfigError("输出目录请使用完整路径或普通相对路径，不能使用盘符相对路径。");
    if (directory.empty() || directory.is_relative())
        directory = config_path().parent_path() / directory;
    return (directory / std::filesystem::u8path(c["output"]["filename"].get<std::string>()))
        .lexically_normal();
}
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
std::string read_text(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("无法读取文件：" + path.u8string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad())
        throw std::runtime_error("读取失败：" + path.u8string());
    std::string s = buffer.str();
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
    return normalize_config(Json::parse(read_text(path)));
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
Matrix parse_csv(const std::string &bytes) {
    std::string s = bytes;
    if (s.compare(0, 3, "\xef\xbb\xbf") == 0)
        s.erase(0, 3);
    Matrix matrix;
    std::istringstream in(s);
    in.imbue(std::locale::classic());
    std::string line;
    const bool labeled = !s.empty() && s.front() == ',';
    std::size_t columns = 0;
    std::vector<std::size_t> operatorRows;
    if (labeled) {
        std::getline(in, line);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        columns = std::count(line.begin(), line.end(), ',');
        std::string expected;
        for (std::size_t part = 1; part <= columns; ++part)
            expected += ',' + std::to_string(part);
        if (line != expected)
            throw ConfigError("宽表 CSV 列号必须从 1 开始连续排列");
    }
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (labeled) {
            if (line.size() < 2 || line[1] != ',' || line[0] < 'A' || line[0] > 'Z')
                throw ConfigError("宽表 CSV 缺少有效人员标记");
            const auto op = std::size_t(line[0] - 'A');
            if (op == operatorRows.size())
                operatorRows.push_back(0);
            else if (operatorRows.empty() || op != operatorRows.size() - 1)
                throw ConfigError("宽表 CSV 人员必须从 A 开始分组连续排列");
            ++operatorRows.back();
            line.erase(0, 2);
        }
        std::vector<double> row;
        std::string cell;
        bool quoted = false, closed = false;
        auto flush = [&] {
            auto lo = cell.find_first_not_of(" \t\r\n"), hi = cell.find_last_not_of(" \t\r\n");
            if (lo == cell.npos)
                throw ConfigError("CSV 包含空数值");
            row.push_back(parse_number(cell.substr(lo, hi - lo + 1)));
            cell.clear();
            closed = false;
        };
        for (std::size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        cell += '"';
                        ++i;
                    } else {
                        quoted = false;
                        closed = true;
                    }
                } else
                    cell += c;
            } else if (c == ',') {
                flush();
            } else if (c == '"' && cell.empty() && !closed)
                quoted = true;
            else {
                if (closed && c != ' ' && c != '\t')
                    throw ConfigError("CSV 引号格式无效");
                cell += c;
            }
        }
        if (quoted)
            throw ConfigError("CSV 引号未闭合");
        flush();
        if (labeled && row.size() != columns)
            throw ConfigError("宽表 CSV 数据列数与列号不符");
        matrix.push_back(std::move(row));
    }
    if (labeled && (operatorRows.empty() ||
                    !std::all_of(operatorRows.begin(), operatorRows.end(),
                                 [&](std::size_t rows) { return rows == operatorRows.front(); })))
        throw ConfigError("宽表 CSV 人员测量次数不一致或缺少数据");
    return matrix;
}
Matrix read_csv(const std::filesystem::path &path) { return parse_csv(read_text(path)); }
Json export_csv(const Result &result, const std::filesystem::path &path, ExistingFile policy) {
    return export_csv(result, path, result.config.value("output", Json::object()), policy);
}
Json export_csv(const Result &result, const std::filesystem::path &path, const Json &output,
                ExistingFile policy) {
    // Immutable generation configuration is the sole authority for all checks.
    auto cfg = validate_request(result.config);
    if (!qualified(calculate(result.matrix, cfg), cfg))
        throw std::invalid_argument("内存数据复验失败，未写入文件。");
    const auto rounded = parse_csv(csv_text(result.matrix, cfg["generation"]["decimal_places"]));
    if (!qualified(calculate(rounded, cfg), cfg))
        throw std::invalid_argument("CSV 精度复验失败，未写入文件。");
    const auto content = export_csv_text(result, output);
    if (parse_export_csv(content, cfg) != rounded)
        throw std::invalid_argument("CSV 记录对应关系复验失败，未写入文件。");
    atomic_text(path, content, policy);
    // read_text intentionally removes BOM for config/CSV parsing. Read raw bytes
    // here so the format-specific BOM and exact row order are also verified.
    std::ifstream verification(path, std::ios::binary);
    if (!verification)
        throw std::runtime_error("无法回读导出文件：" + path.u8string());
    const std::string saved((std::istreambuf_iterator<char>(verification)), {});
    if (verification.bad())
        throw std::runtime_error("回读导出文件失败：" + path.u8string());
    if (saved != content)
        throw std::invalid_argument("保存后的 CSV 内容或运行顺序与预期不符，请勿使用该文件。");
    auto actual = calculate(parse_export_csv(saved, cfg), cfg);
    if (!qualified(actual, cfg))
        throw std::invalid_argument("保存后的 CSV 复验失败，请勿使用该文件。");
    return actual;
}
} // namespace msa
