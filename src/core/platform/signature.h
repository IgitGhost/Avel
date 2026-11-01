// Authenticode: whether a file is signed, by whom, and whether by Microsoft.
//
// Most of Windows' own files carry no signature of their own: a catalog in CatRoot lists their
// hashes and is signed instead. So the embedded signature is checked first and then the catalogs
// (SHA-256, then SHA-1). Whether a file is Microsoft's is decided by its chain ending in one of
// Microsoft's roots (CERT_CHAIN_POLICY_MICROSOFT_ROOT, product and application roots), never by the
// signer's name, which anyone can put in a certificate.
//
// Nothing here touches the network: no revocation checks and no certificate downloads, only what
// the machine's caches hold.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace avel::platform {

enum class SignatureState : std::uint8_t {
    Valid,     // Signed (embedded or by a catalog), and the signature checks out to a trusted root
    Unsigned,  // No embedded signature, and no catalog lists the file
    Invalid,   // Signed, but the signature is bad, expired, or its chain isn't trusted
    Missing,   // The file doesn't exist
    Error,     // It couldn't be checked (access denied, not a file, ...)
};

struct Signature {
    SignatureState state = SignatureState::Error;
    std::wstring signer;     // The signing certificate's simple display name ("Microsoft Windows")
    bool microsoft = false;  // The signer's chain ends in a Microsoft root (set whatever the state)
    bool catalog = false;    // Signed through a catalog rather than a signature of its own
};

// Checks `path`. Thread-safe.
Signature verify_signature(const std::wstring& path);

// verify_signature, once per file: for callers that check many files, several of them the same.
// Thread-safe; two threads asking for one new file may both check it.
class SignatureCache {
public:
    Signature get(const std::wstring& path);

private:
    std::mutex mutex_;
    std::unordered_map<std::wstring, Signature> entries_;  // By normalized, lowercased path
};

std::string_view to_string(SignatureState state);

}  // namespace avel::platform
