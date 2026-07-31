#include "common/dpapi.h"

#include <windows.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <vector>

namespace lar {

std::string dpapi_protect_to_base64(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Helm API key", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    DWORD b64_len = 0;
    std::string b64;
    if (CryptBinaryToStringA(out.pbData, out.cbData,
                             CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64_len) &&
        b64_len > 0) {
        std::vector<char> buffer(b64_len);
        if (CryptBinaryToStringA(out.pbData, out.cbData,
                                 CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, buffer.data(), &b64_len)) {
            // b64_len excludes the terminator after the second call.
            b64.assign(buffer.data(), b64_len);
        }
    }
    LocalFree(out.pbData);
    return b64;
}

std::string dpapi_unprotect_from_base64(const std::string& base64) {
    if (base64.empty()) return {};
    DWORD bin_len = 0;
    if (!CryptStringToBinaryA(base64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &bin_len, nullptr, nullptr) ||
        bin_len == 0)
        return {};
    std::vector<BYTE> blob(bin_len);
    if (!CryptStringToBinaryA(base64.c_str(), 0, CRYPT_STRING_BASE64, blob.data(), &bin_len, nullptr, nullptr))
        return {};

    DATA_BLOB in{bin_len, blob.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    // The decrypted copy in LocalAlloc memory is zeroed before release; the
    // returned std::string is the only remaining copy.
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

} // namespace lar
