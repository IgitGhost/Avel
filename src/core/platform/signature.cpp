#include "core/platform/signature.h"

#include <windows.h>
// After windows.h, in this order.
#include <wincrypt.h>
#include <wintrust.h>
#include <bcrypt.h>
#include <mscat.h>
#include <softpub.h>

#include <wil/resource.h>

#include <vector>

#include "core/platform/file_ops.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::platform {

namespace {

// The chain ends in one of Microsoft's product roots (Windows' own files), or in its root for
// application signing (Microsoft's other programs).
bool chains_to_microsoft(PCCERT_CHAIN_CONTEXT chain) {
    for (const DWORD flags :
         {DWORD{0}, DWORD{MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG}}) {
        CERT_CHAIN_POLICY_PARA para{};
        para.cbSize = sizeof(para);
        para.dwFlags = flags;
        CERT_CHAIN_POLICY_STATUS status{};
        status.cbSize = sizeof(status);
        if (::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_MICROSOFT_ROOT, chain, &para, &status) &&
            status.dwError == ERROR_SUCCESS) {
            return true;
        }
    }
    return false;
}

// The signer's name and chain, from a verification's state, whatever its result.
void read_signer(HANDLE state, Signature& signature) {
    CRYPT_PROVIDER_DATA* provider = state ? ::WTHelperProvDataFromStateData(state) : nullptr;
    CRYPT_PROVIDER_SGNR* signer =
        provider ? ::WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    if (!signer) {
        return;
    }
    const CRYPT_PROVIDER_CERT* leaf = ::WTHelperGetProvCertFromChain(signer, 0);
    if (leaf && leaf->pCert) {
        const DWORD size =
            ::CertGetNameStringW(leaf->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
        if (size > 1) {
            std::wstring name(size, L'\0');
            const DWORD written = ::CertGetNameStringW(leaf->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                                                       name.data(), size);
            name.resize(written > 0 ? written - 1 : 0);
            signature.signer = std::move(name);
        }
    }
    signature.microsoft = signer->pChainContext && chains_to_microsoft(signer->pChainContext);
}

// WinVerifyTrust over `data` (a file or a catalog member), without the network: no revocation
// checks, no downloads. The signer is read before its state is closed. Returns its result.
LONG verify(WINTRUST_DATA& data, Signature& signature) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const HWND no_user = static_cast<HWND>(INVALID_HANDLE_VALUE);
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    const LONG result = ::WinVerifyTrust(no_user, &action, &data);
    read_signer(data.hWVTStateData, signature);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    ::WinVerifyTrust(no_user, &action, &data);
    return result;
}

// The file has no signature of its own.
bool unsigned_result(LONG result) {
    return result == TRUST_E_NOSIGNATURE || result == TRUST_E_SUBJECT_FORM_UNKNOWN ||
           result == TRUST_E_PROVIDER_UNKNOWN;
}

SignatureState state_of(LONG result) {
    if (result == ERROR_SUCCESS) {
        return SignatureState::Valid;
    }
    // The file couldn't be read: nothing is known about its signature.
    if (result == CRYPT_E_FILE_ERROR || result == E_ACCESSDENIED ||
        result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || result == E_OUTOFMEMORY) {
        return SignatureState::Error;
    }
    return SignatureState::Invalid;
}

bool rewind(HANDLE file) { return ::SetFilePointerEx(file, LARGE_INTEGER{}, nullptr, FILE_BEGIN) != FALSE; }

std::wstring upper_hex(const std::vector<BYTE>& bytes) {
    constexpr wchar_t kDigits[] = L"0123456789ABCDEF";
    std::wstring text;
    text.reserve(bytes.size() * 2);
    for (const BYTE byte : bytes) {
        text.push_back(kDigits[byte >> 4]);
        text.push_back(kDigits[byte & 0x0f]);
    }
    return text;
}

// The file's signature through a catalog that lists its hash in `algorithm`; false if none does.
bool verify_catalog(const std::wstring& path, HANDLE file, const wchar_t* algorithm, Signature& signature) {
    HCATADMIN admin = nullptr;
    if (!::CryptCATAdminAcquireContext2(&admin, nullptr, algorithm, nullptr, 0)) {
        return false;
    }
    const auto release_admin = wil::scope_exit([&] { ::CryptCATAdminReleaseContext(admin, 0); });
    DWORD size = 0;
    if (!rewind(file)) {
        return false;
    }
    ::CryptCATAdminCalcHashFromFileHandle2(admin, file, &size, nullptr, 0);  // Asks for the size
    if (size == 0) {
        return false;
    }
    std::vector<BYTE> hash(size);
    if (!rewind(file) || !::CryptCATAdminCalcHashFromFileHandle2(admin, file, &size, hash.data(), 0)) {
        return false;
    }
    hash.resize(size);
    HCATINFO info = ::CryptCATAdminEnumCatalogFromHash(admin, hash.data(), size, 0, nullptr);
    if (!info) {
        return false;
    }
    const auto release_info = wil::scope_exit([&] { ::CryptCATAdminReleaseCatalogContext(admin, info, 0); });
    CATALOG_INFO catalog{};
    catalog.cbStruct = sizeof(catalog);
    signature.catalog = true;
    if (!::CryptCATCatalogInfoFromContext(info, &catalog, 0)) {
        signature.state = SignatureState::Error;  // Listed, but its catalog can't be read
        return true;
    }
    const std::wstring tag = upper_hex(hash);
    WINTRUST_CATALOG_INFO member{};
    member.cbStruct = sizeof(member);
    member.pcwszCatalogFilePath = catalog.wszCatalogFile;
    member.pcwszMemberTag = tag.c_str();
    member.pcwszMemberFilePath = path.c_str();
    member.hMemberFile = file;
    member.pbCalculatedFileHash = hash.data();
    member.cbCalculatedFileHash = size;
    member.hCatAdmin = admin;
    WINTRUST_DATA data{};
    data.dwUnionChoice = WTD_CHOICE_CATALOG;
    data.pCatalog = &member;
    rewind(file);
    signature.state = state_of(verify(data, signature));
    return true;
}

}  // namespace

Signature verify_signature(const std::wstring& path) {
    Signature signature;
    if (path.empty()) {
        signature.state = SignatureState::Missing;
        return signature;
    }
    wil::unique_hfile file(::CreateFileW(extended(path).c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const DWORD error = ::GetLastError();
        const bool missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
                             error == ERROR_INVALID_NAME || error == ERROR_BAD_PATHNAME;
        signature.state = missing ? SignatureState::Missing : SignatureState::Error;
        return signature;
    }

    WINTRUST_FILE_INFO embedded{};
    embedded.cbStruct = sizeof(embedded);
    embedded.pcwszFilePath = path.c_str();
    embedded.hFile = file.get();
    WINTRUST_DATA data{};
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &embedded;
    const LONG result = verify(data, signature);
    if (!unsigned_result(result)) {
        signature.state = state_of(result);
        return signature;
    }

    signature = {};
    for (const wchar_t* algorithm : {BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA1_ALGORITHM}) {
        if (verify_catalog(path, file.get(), algorithm, signature)) {
            return signature;
        }
    }
    signature = {};
    signature.state = SignatureState::Unsigned;
    return signature;
}

Signature SignatureCache::get(const std::wstring& path) {
    const std::wstring key = to_lower(normalize_path(path));
    {
        const std::lock_guard lock(mutex_);
        if (const auto found = entries_.find(key); found != entries_.end()) {
            return found->second;
        }
    }
    Signature signature = verify_signature(path);
    const std::lock_guard lock(mutex_);
    return entries_.emplace(key, std::move(signature)).first->second;
}

std::string_view to_string(SignatureState state) {
    switch (state) {
        case SignatureState::Valid:
            return "valid";
        case SignatureState::Unsigned:
            return "unsigned";
        case SignatureState::Invalid:
            return "invalid";
        case SignatureState::Missing:
            return "missing";
        case SignatureState::Error:
            return "error";
    }
    return "unknown";
}

}  // namespace avel::platform
