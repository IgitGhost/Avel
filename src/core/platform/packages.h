// Packaged apps (MSIX and AppX) through the public Windows.Management.Deployment.PackageManager,
// as ADR 0008 decided: no Appx cmdlets, no StateRepository or AppxAllUserStore reads. Every call
// runs on a fresh multithreaded-apartment thread, so a caller on a single-threaded apartment (the
// GUI thread) is left alone. Listing other users' packages and every change need administrator
// rights.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace avel::platform {

enum class PackageSignatureKind : std::uint8_t { None, Developer, Enterprise, Store, System };

struct PackageInfo {
    std::wstring full_name;    // e.g. Microsoft.BingNews_4.55.62231.0_x64__8wekyb3d8bbwe
    std::wstring family_name;  // e.g. Microsoft.BingNews_8wekyb3d8bbwe
    std::wstring name;         // The identity name, e.g. Microsoft.BingNews
    std::wstring publisher;    // The publisher's display name
    std::wstring version;      // a.b.c.d
    std::wstring display_name;
    std::wstring install_location;  // Under WindowsApps; empty when unknown
    // The folder of the desktop program the package gives an identity to (a sparse package, or one
    // with an external location: EffectiveExternalPath), e.g. PowerToys' or Edge's. Empty for a
    // package that holds its own files.
    std::wstring external_location;
    PackageSignatureKind signature_kind = PackageSignatureKind::None;
    bool framework = false;
    bool resource = false;
    bool bundle = false;
    bool optional = false;  // An optional package of another app
    // The users it is registered for (FindUsers). In a listing without administrator rights, only
    // the current user's SID.
    std::vector<std::wstring> user_sids;
};

struct PackageListing {
    std::vector<PackageInfo> packages;  // Main and optional packages, frameworks and resources included
    // Full names of the provisioned packages (FindProvisionedPackages); empty without administrator
    // rights.
    std::vector<std::wstring> provisioned;
    bool all_users = false;   // False without administrator rights: the current user's packages only
    std::int32_t error = 0;   // The HRESULT that cut the listing short, 0 when complete
};

PackageListing list_packages();

// A package that is on this PC (staged), for any user. Nullopt when it isn't.
std::optional<PackageInfo> find_package(const std::wstring& full_name);

// Whether some version of the family is on this PC (staged), for any user or provisioned.
bool family_staged(const std::wstring& family_name);

// The users (SIDs) that have some version of the family registered.
std::vector<std::wstring> family_users(const std::wstring& family_name);

// The full name of the family's provisioned package, if it is provisioned.
std::optional<std::wstring> provisioned_package(const std::wstring& family_name);

// The outcome of a change. `hresult` and `extended` are the operation's codes (DeploymentResult's
// ExtendedErrorCode). `confirmed` is whether the state the operation is meant to reach was then
// read back (ADR 0008: a registration can report success while nothing was registered), and
// `full_name` the package acted on or registered, when known.
struct PackageOperation {
    std::int32_t hresult = 0;
    std::int32_t extended = 0;
    bool confirmed = false;
    std::wstring full_name;
};

// RemovePackageAsync(full, RemovalOptions::None): the current user's registration. Confirmed when
// the current user no longer has the family.
PackageOperation remove_for_current_user(const std::wstring& full_name);

// RegisterPackageByFamilyNameAsync for the current user: whatever version of the family is staged.
// Confirmed when the current user has the family.
PackageOperation register_for_current_user(const std::wstring& family_name);

// DeprovisionPackageForAllUsersAsync. Confirmed when the family is no longer provisioned.
PackageOperation deprovision(const std::wstring& family_name);

// ProvisionPackageForAllUsersAsync, while the family is staged. Confirmed when it is provisioned.
PackageOperation provision(const std::wstring& family_name);

// RemovePackageAsync(full, RemovalOptions::RemoveForAllUsers): every user, deprovisioned, and the
// staged package deleted. Irreversible. Confirmed when the family is no longer staged.
PackageOperation remove_for_all_users(const std::wstring& full_name);

}  // namespace avel::platform
