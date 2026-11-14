// Packaged apps' ChangeSet items (ADR 0008): a user's registration, the provisioning, and the
// removal for every user, through platform/packages. The executor reads and changes packages only
// through PackageSystem, so tests can stand in for Windows' packages.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/platform/packages.h"

namespace avel::changes {

// The package calls the executor makes. Each default calls platform/packages (and
// platform::current_user_sid); tests override them all.
class PackageSystem {
public:
    virtual ~PackageSystem() = default;

    // The account Avel runs as: the only one whose registrations it can change.
    virtual std::wstring current_user_sid();
    virtual std::vector<std::wstring> family_users(const std::wstring& family_name);
    virtual std::optional<std::wstring> provisioned_package(const std::wstring& family_name);
    virtual bool family_staged(const std::wstring& family_name);

    virtual platform::PackageOperation remove_for_current_user(const std::wstring& full_name);
    virtual platform::PackageOperation register_for_current_user(const std::wstring& family_name);
    virtual platform::PackageOperation deprovision(const std::wstring& family_name);
    virtual platform::PackageOperation provision(const std::wstring& family_name);
    virtual platform::PackageOperation remove_for_all_users(const std::wstring& full_name);

    // Whether `sid` has some version of the family registered.
    bool registered(const std::wstring& family_name, const std::wstring& sid);
};

// The outcome of an operation as an item's status: done when the state it is meant to reach was
// read back (PackageOperation::confirmed), never by its code alone; failed otherwise, with the
// operation's code (its extended code when the operation itself reported success).
ItemStatus package_status(const platform::PackageOperation& operation);

}  // namespace avel::changes
