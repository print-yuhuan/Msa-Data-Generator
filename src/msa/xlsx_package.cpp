#include "xlsx_package.hpp"
#include "xlsx.hpp"
#include "core.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <string_view>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <msopc.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#else
#include <archive.h>
#include <archive_entry.h>
#include <libxml/xmlreader.h>
#endif

namespace msa::xlsx {
namespace {
constexpr std::size_t part_limit = 16 * 1024 * 1024, inflated_limit = 64 * 1024 * 1024;
constexpr unsigned entry_limit = 2048;
void check(bool ok, const char *message) {
    if (!ok)
        throw ConfigError(message);
}
std::uint32_t little(const std::string &bytes, std::size_t offset, unsigned count) {
    check(offset <= bytes.size() && count <= bytes.size() - offset, "XLSX ZIP 目录不完整");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= std::uint32_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}
// Inspect archive dimensions before letting the platform traverse XML relationship parts.
void check_archive(const std::string &bytes) {
    check(bytes.size() >= 22 && bytes.size() <= xlsx_read_limit, "XLSX 文件长度无效或超过 16 MiB");
    auto end = bytes.size() - 22;
    const auto first = end > 65535 ? end - 65535 : 0;
    for (;;) {
        if (bytes.compare(end, 4, "PK\x05\x06") == 0 &&
            little(bytes, end + 20, 2) == bytes.size() - end - 22)
            break;
        check(end > first, "文件不是完整的 XLSX ZIP 工作簿");
        --end;
    }
    const auto entries = little(bytes, end + 10, 2);
    check(entries > 0 && entries <= entry_limit && little(bytes, end + 4, 4) == 0 &&
              little(bytes, end + 8, 2) == entries,
          "XLSX ZIP 条目过多或包含不支持的分卷");
    std::size_t at = little(bytes, end + 16, 4), total = 0;
    check(at <= end && little(bytes, end + 12, 4) == end - at, "XLSX ZIP 目录范围无效");
    std::set<std::string_view> names;
    for (unsigned i = 0; i < entries; ++i) {
        check(end - at >= 46 && little(bytes, at, 4) == 0x02014b50, "XLSX ZIP 条目无效");
        const std::size_t size = little(bytes, at + 24, 4);
        check((little(bytes, at + 8, 2) & 1) == 0, "请先解除 XLSX 文件加密");
        check(size <= part_limit && size <= inflated_limit - total, "XLSX 解压数据超过允许范围");
        total += size;
        const auto name_size = little(bytes, at + 28, 2);
        const std::size_t count =
            46 + name_size + little(bytes, at + 30, 2) + little(bytes, at + 32, 2);
        check(count <= end - at, "XLSX ZIP 条目范围无效");
        const std::string_view name(bytes.data() + at + 46, name_size);
        check(!name.empty() && name.find('\0') == name.npos && names.insert(name).second,
              "XLSX ZIP 部件名无效或重复");
        at += count;
    }
    check(at == end, "XLSX ZIP 条目数与目录不符");
}
void put(std::string &bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 255u));
}
std::uint32_t crc32(const std::string &bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (unsigned i = 0; i < t.size(); ++i) {
            auto crc = i;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
            t[i] = crc;
        }
        return t;
    }();
    std::uint32_t value = ~0u;
    for (unsigned char c : bytes)
        value = table[(value ^ c) & 255u] ^ (value >> 8);
    return ~value;
}
#ifdef _WIN32
using Microsoft::WRL::ComPtr;
class Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  public:
    Apartment() {
        check(SUCCEEDED(result) || result == RPC_E_CHANGED_MODE, "无法初始化 Windows 工作簿读取器");
    }
    ~Apartment() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};
ComPtr<IStream> byte_stream(const std::string &bytes) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(bytes.data()),
                                    static_cast<UINT>(bytes.size())));
    check(stream.Get() != nullptr, "无法分配工作簿读取缓冲区");
    return stream;
}
std::string utf8(const wchar_t *value, UINT count) {
    if (!count)
        return {};
    const auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, count, nullptr, 0,
                                       nullptr, nullptr);
    check(n > 0, "XLSX XML 含无效字符");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, count, out.data(), n, nullptr,
                        nullptr);
    return out;
}
std::string contents(IStream *stream) {
    STATSTG stat{};
    check(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart <= part_limit,
          "XLSX XML 部件超过读取上限");
    std::string out;
    std::array<char, 32768> buf;
    for (;;) {
        ULONG n = 0;
        const auto hr = stream->Read(buf.data(), static_cast<ULONG>(buf.size()), &n);
        check(SUCCEEDED(hr) && n <= part_limit - out.size(), "XLSX XML 部件读取失败或超过上限");
        out.append(buf.data(), n);
        if (hr == S_FALSE || n == 0)
            break;
    }
    return out;
}
#endif
} // namespace

// ZIP32 STORE, UTF-8 names, fixed 1980-01-01 DOS date and explicit sizes. No clock, filesystem
// metadata, compression version or OS-specific fields enter the reproducible document bytes.
std::string zip_stored(const std::vector<std::pair<std::string, std::string>> &parts) {
    std::string out, directory;
    for (const auto &[name, xml] : parts) {
        const auto start = static_cast<std::uint32_t>(out.size());
        const auto size = static_cast<std::uint32_t>(xml.size());
        std::string common;
        put(common, 20, 2);
        put(common, 0x800, 2);
        put(common, 0, 2);
        put(common, 0, 2);
        put(common, 33, 2);
        put(common, crc32(xml), 4);
        put(common, size, 4);
        put(common, size, 4);
        put(common, static_cast<std::uint32_t>(name.size()), 2);
        put(common, 0, 2);
        put(out, 0x04034b50, 4);
        out += common;
        out += name;
        out += xml;
        put(directory, 0x02014b50, 4);
        put(directory, 20, 2);
        directory += common;
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 4);
        put(directory, start, 4);
        directory += name;
    }
    const auto start = static_cast<std::uint32_t>(out.size());
    out += directory;
    put(out, 0x06054b50, 4);
    put(out, 0, 4);
    put(out, static_cast<std::uint32_t>(parts.size()), 2);
    put(out, static_cast<std::uint32_t>(parts.size()), 2);
    put(out, static_cast<std::uint32_t>(directory.size()), 4);
    put(out, start, 4);
    put(out, 0, 2);
    return out;
}

#ifdef _WIN32
struct Package::Impl {
    Apartment apartment;
    ComPtr<IOpcFactory> factory;
    ComPtr<IOpcPackage> package;
    ComPtr<IOpcPartSet> parts;
    ComPtr<IOpcPart> find(const std::string &name) const {
        const auto path = std::filesystem::u8path("/" + name).wstring();
        ComPtr<IOpcPartUri> uri;
        ComPtr<IOpcPart> p;
        check(SUCCEEDED(factory->CreatePartUri(path.c_str(), &uri)) &&
                  SUCCEEDED(parts->GetPart(uri.Get(), &p)),
              "XLSX 中缺少工作簿关系所引用的部件");
        return p;
    }
};
Package::Package(const std::string &bytes) : impl(std::make_unique<Impl>()) {
    check_archive(bytes);
    auto stream = byte_stream(bytes);
    check(SUCCEEDED(CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&impl->factory))) &&
              SUCCEEDED(impl->factory->ReadPackageFromStream(stream.Get(), OPC_READ_DEFAULT,
                                                             &impl->package)) &&
              SUCCEEDED(impl->package->GetPartSet(&impl->parts)),
          "Windows 无法打开 XLSX 工作簿");
}
std::string Package::part(const std::string &name) const {
    ComPtr<IStream> stream;
    check(SUCCEEDED(impl->find(name)->GetContentStream(&stream)), "无法读取 XLSX 部件");
    return contents(stream.Get());
}
std::string Package::relationships(const std::string &source) const {
    ComPtr<IOpcRelationshipSet> rel;
    auto hr = source.empty() ? impl->package->GetRelationshipSet(&rel)
                             : impl->find(source)->GetRelationshipSet(&rel);
    ComPtr<IStream> stream;
    check(SUCCEEDED(hr) && SUCCEEDED(rel->GetRelationshipsContentStream(&stream)),
          "无法读取 XLSX 关系");
    return contents(stream.Get());
}
void read_xml(const std::string &bytes, const std::function<void(const Node &)> &visit) {
    auto stream = byte_stream(bytes);
    ComPtr<IXmlReader> reader;
    check(SUCCEEDED(CreateXmlReader(__uuidof(IXmlReader),
                                    reinterpret_cast<void **>(reader.GetAddressOf()), nullptr)) &&
              SUCCEEDED(
                  reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) &&
              SUCCEEDED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64)) &&
              SUCCEEDED(reader->SetInput(stream.Get())),
          "无法创建安全的工作簿 XML 读取器");
    const auto string_value = [&](bool name) {
        const wchar_t *s = nullptr;
        UINT n = 0;
        const auto hr = name ? reader->GetLocalName(&s, &n) : reader->GetValue(&s, &n);
        check(SUCCEEDED(hr), "XLSX XML 内容无效");
        return utf8(s, n);
    };
    XmlNodeType kind;
    HRESULT hr;
    while ((hr = reader->Read(&kind)) == S_OK) {
        if (kind == XmlNodeType_Element) {
            Node node{Node::Open, string_value(true), {}, {}};
            const bool empty = reader->IsEmptyElement() != FALSE;
            if (reader->MoveToFirstAttribute() == S_OK) {
                do {
                    node.attributes[string_value(true)] = string_value(false);
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
            visit(node);
            if (empty)
                visit({Node::Close, node.name, {}, {}});
        } else if (kind == XmlNodeType_EndElement)
            visit({Node::Close, string_value(true), {}, {}});
        else if (kind == XmlNodeType_Text || kind == XmlNodeType_CDATA ||
                 kind == XmlNodeType_Whitespace)
            visit({Node::Text, {}, string_value(false), {}});
    }
    check(hr == S_FALSE, "XLSX 包含无效 XML 或不支持的文档类型声明");
}
#else
struct Package::Impl {
    std::string bytes;
};
Package::Package(const std::string &bytes) : impl(std::make_unique<Impl>()) {
    check_archive(bytes);
    impl->bytes = bytes;
}
std::string Package::part(const std::string &name) const {
    const auto &bytes = impl->bytes;
    const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(),
                                                                        &archive_read_free);
    // The seekable ZIP reader skips other parts using their compressed sizes.
    check(reader && archive_read_support_format_zip_seekable(reader.get()) == ARCHIVE_OK &&
              archive_read_open_memory(reader.get(), bytes.data(), bytes.size()) == ARCHIVE_OK,
          "无法打开 XLSX ZIP");
    archive_entry *entry;
    int rc;
    unsigned entries = 0;
    while ((rc = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        check(++entries <= entry_limit, "XLSX 中的部件过多");
        const char *member = archive_entry_pathname(entry);
        if (archive_entry_filetype(entry) != AE_IFREG || !member || member != name)
            continue;
        const auto declared = archive_entry_size(entry);
        check(declared >= 0 && static_cast<std::uint64_t>(declared) <= part_limit,
              "XLSX 部件解压尺寸无效");
        const auto size = static_cast<std::size_t>(declared);
        std::string out;
        std::array<char, 32768> buf;
        for (;;) {
            const auto n = archive_read_data(reader.get(), buf.data(), buf.size());
            check(n >= 0 && static_cast<std::size_t>(n) <= size - out.size(),
                  "XLSX 部件超过解压上限或已损坏");
            if (!n) {
                check(out.size() == size, "XLSX 部件解压尺寸与目录不符");
                return out;
            }
            out.append(buf.data(), static_cast<std::size_t>(n));
        }
    }
    check(rc == ARCHIVE_EOF, "XLSX ZIP 不完整");
    throw ConfigError("XLSX 中缺少工作簿关系所引用的部件");
}
std::string Package::relationships(const std::string &source) const {
    if (source.empty())
        return part("_rels/.rels");
    const auto path = std::filesystem::u8path(source);
    return part(
        (path.parent_path() / "_rels" / (path.filename().string() + ".rels")).generic_string());
}
void read_xml(const std::string &bytes, const std::function<void(const Node &)> &visit) {
    using Reader = std::unique_ptr<xmlTextReader, decltype(&xmlFreeTextReader)>;
    const Reader reader(
        xmlReaderForMemory(bytes.data(), static_cast<int>(bytes.size()), nullptr, nullptr,
                           XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
        &xmlFreeTextReader);
    check(bool(reader), "无法打开 XLSX XML");
    const auto str = [](const xmlChar *v) -> std::string {
        return v ? reinterpret_cast<const char *>(v) : "";
    };
    auto *r = reader.get();
    int rc;
    while ((rc = xmlTextReaderRead(r)) == 1) {
        const auto kind = xmlTextReaderNodeType(r);
        check(kind != XML_READER_TYPE_DOCUMENT_TYPE && kind != XML_READER_TYPE_ENTITY_REFERENCE &&
                  xmlTextReaderDepth(r) < 64,
              "XLSX XML 文档类型或嵌套深度不受支持");
        if (kind == XML_READER_TYPE_ELEMENT) {
            Node node{Node::Open, str(xmlTextReaderConstLocalName(r)), {}, {}};
            const bool empty = xmlTextReaderIsEmptyElement(r) == 1;
            if (xmlTextReaderMoveToFirstAttribute(r) == 1) {
                do {
                    node.attributes[str(xmlTextReaderConstLocalName(r))] =
                        str(xmlTextReaderConstValue(r));
                } while (xmlTextReaderMoveToNextAttribute(r) == 1);
                xmlTextReaderMoveToElement(r);
            }
            visit(node);
            if (empty)
                visit({Node::Close, node.name, {}, {}});
        } else if (kind == XML_READER_TYPE_END_ELEMENT)
            visit({Node::Close, str(xmlTextReaderConstLocalName(r)), {}, {}});
        else if (kind == XML_READER_TYPE_TEXT || kind == XML_READER_TYPE_CDATA ||
                 kind == XML_READER_TYPE_WHITESPACE ||
                 kind == XML_READER_TYPE_SIGNIFICANT_WHITESPACE)
            visit({Node::Text, {}, str(xmlTextReaderConstValue(r)), {}});
    }
    check(rc == 0, "XLSX XML 内容无效");
}
#endif
Package::~Package() = default;
} // namespace msa::xlsx
