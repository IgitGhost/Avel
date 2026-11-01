// Authenticode: whether a file is signed, by whom, and whether it is Windows' own.
//
// Most of Windows' own files carry no signature of their own: a catalog in CatRoot lists their
// hashes and is signed instead. So the embedded signature is checked first and then the catalogs
// (SHA-256, then SHA-1). Whether a file is Microsoft's is decided by where the signer's chain ends
// (CERT_CHAIN_POLICY_MICROSOFT_ROOT), never by the signer's name, which anyone can put in a
// certificate:
//   - one of Microsoft's product roots ("Microsoft Root Certificate Authority 2010" and older):
//     Windows' own files ("Microsoft Windows", "Microsoft Windows Publisher");
//   - its application root ("Microsoft Root Certificate Authority 2011", checked with
//     MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG): Microsoft's other programs
//     (Edge and its updater, SQL Server, WSL, Office, ...).
// Hardware vendors' driver packages are signed by Microsoft too (WHQL): their catalogs, and often
// the files themselves, carry "Microsoft Windows Hardware Compatibility Publisher", issued by
// "Microsoft Windows Third Party Component CA", under a product root. Those files are the vendor's
// (an audio or graphics vendor's service in the DriverStore), not Windows'. The names are compared
// only once the chain has verified to a product root, where only Microsoft could have issued them.
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

// Which of Microsoft's roots the signer's chain ends in.
enum class MicrosoftRoot : std::uint8_t {
    None,         // Not Microsoft's (or no chain)
    Product,      // A product root: Windows' own files, and WHQL-signed driver packages
    Application,  // The application root: Microsoft's programs that aren't part of Windows
};

struct Signature {
    SignatureState state = SignatureState::Error;
    std::wstring signer;  // The signing certificate's simple display name ("Microsoft Windows")
    MicrosoftRoot root = MicrosoftRoot::None;  // Where the signer's chain ends (set whatever the state)
    bool catalog = false;                      // Signed through a catalog rather than a signature of its own
    // Windows' own file: valid, through a product root, and not a WHQL-signed driver package's.
    bool windows = false;
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
std::string_view to_string(MicrosoftRoot root);

}  // namespace avel::platform
