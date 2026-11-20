#include "core.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace msa {
std::string sha256(const std::string &bytes) {
    std::array<unsigned char, 32> digest{};
#ifdef _WIN32
    // BCryptHash is the Windows 10+ system one-shot API. CSV inputs are bounded.
    if (bytes.size() > std::numeric_limits<ULONG>::max())
        throw std::runtime_error("SHA-256 输入过大");
    auto *data = reinterpret_cast<unsigned char *>(const_cast<char *>(bytes.data()));
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data,
                                   static_cast<ULONG>(bytes.size()), digest.data(),
                                   static_cast<ULONG>(digest.size()))))
        throw std::runtime_error("系统 SHA-256 计算失败");
#else
    unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != digest.size())
        throw std::runtime_error("系统 SHA-256 计算失败");
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
} // namespace msa
