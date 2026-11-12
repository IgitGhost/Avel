#include "core/changes/serialization.h"

#include <windows.h>

#include <wil/resource.h>

#include <cstring>

#include "core/platform/text.h"

namespace avel::changes {

void put_text(Json& json, std::string_view field, std::wstring_view text) {
    const std::string utf8 = platform::to_utf8(text);
    if (platform::to_utf16(utf8) == text) {
        json[std::string(field)] = utf8;
        return;
    }
    json[std::string(field) + "_hex"] =
        to_hex(reinterpret_cast<const std::byte*>(text.data()), text.size() * sizeof(wchar_t));
}

std::optional<std::wstring> get_text(const Json& json, std::string_view field) {
    if (const auto found = json.find(std::string(field)); found != json.end() && found->is_string()) {
        return platform::to_utf16(found->get_ref<const std::string&>());
    }
    if (const auto found = json.find(std::string(field) + "_hex"); found != json.end() && found->is_string()) {
        const auto bytes = from_hex(found->get_ref<const std::string&>());
        if (!bytes || bytes->size() % sizeof(wchar_t) != 0) {
            return std::nullopt;
        }
        std::wstring text(bytes->size() / sizeof(wchar_t), L'\0');
        std::memcpy(text.data(), bytes->data(), bytes->size());
        return text;
    }
    return std::nullopt;
}

std::string to_hex(const std::byte* data, std::size_t size) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        const auto byte = static_cast<unsigned char>(data[i]);
        text.push_back(kDigits[byte >> 4]);
        text.push_back(kDigits[byte & 0x0f]);
    }
    return text;
}

std::string to_hex(const std::vector<std::byte>& data) {
    return to_hex(data.data(), data.size());
}

std::optional<std::vector<std::byte>> from_hex(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::byte> data(text.size() / 2);
    for (std::size_t i = 0; i < data.size(); ++i) {
        const int high = digit(text[2 * i]);
        const int low = digit(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        data[i] = static_cast<std::byte>(high << 4 | low);
    }
    return data;
}

namespace {

bool write_and_flush(const std::wstring& path, std::string_view text) {
    wil::unique_hfile file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return false;
    }
    DWORD written = 0;
    return ::WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
           written == text.size() && ::FlushFileBuffers(file.get());
}

}  // namespace

bool write_file_durably(const std::wstring& path, std::string_view text, bool replace) {
    if (!replace || ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return write_and_flush(path, text);
    }
    const std::wstring temporary = path + L".new";
    if (!write_and_flush(temporary, text)) {
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    if (!::ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr,
                        nullptr)) {
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

namespace {

void put_address(Json& json, const platform::RegistryAddress& address) {
    json["hive"] = address.hive == platform::Hive::Machine ? "machine" : "user";
    put_text(json, "sid", address.sid);
    put_text(json, "path", address.path);
}

bool get_address(const Json& json, platform::RegistryAddress& address) {
    const auto hive = json.value("hive", std::string());
    const auto sid = get_text(json, "sid");
    const auto path = get_text(json, "path");
    if ((hive != "machine" && hive != "user") || !sid || !path) {
        return false;
    }
    address = {hive == "machine" ? platform::Hive::Machine : platform::Hive::User, *sid, *path};
    return true;
}

// A value edit's value: {"type", "data"} (its name is the item's), or null when absent.
Json edit_value_to_json(const std::optional<platform::RawValue>& value) {
    if (!value) {
        return nullptr;
    }
    Json json;
    json["type"] = value->type;
    json["data"] = to_hex(value->data);
    return json;
}

bool edit_value_from_json(const Json& json, const std::wstring& name,
                          std::optional<platform::RawValue>& value) {
    if (json.is_null()) {
        value.reset();
        return true;
    }
    const auto type = json.is_object() ? json.find("type") : json.end();
    const auto data = json.is_object() ? json.find("data") : json.end();
    if (type == json.end() || !type->is_number_unsigned() || data == json.end() || !data->is_string()) {
        return false;
    }
    auto bytes = from_hex(data->get_ref<const std::string&>());
    if (!bytes) {
        return false;
    }
    value = platform::RawValue{name, type->get<DWORD>(), std::move(*bytes)};
    return true;
}

// Text list items: a string, or {"text_hex"} for one that UTF-8 can't carry (see put_text).
Json text_list_to_json(const std::vector<std::wstring>& items) {
    Json json = Json::array();
    for (const auto& item : items) {
        Json entry;
        put_text(entry, "text", item);
        json.push_back(entry.contains("text") ? entry["text"] : entry);
    }
    return json;
}

bool text_list_from_json(const Json& json, std::vector<std::wstring>& items) {
    if (!json.is_array()) {
        return false;
    }
    items.clear();
    for (const auto& entry : json) {
        const auto text = entry.is_string()
                              ? std::optional(platform::to_utf16(entry.get_ref<const std::string&>()))
                              : get_text(entry, "text");
        if (!text) {
            return false;
        }
        items.push_back(*text);
    }
    return true;
}

}  // namespace

Json to_json(const leftovers::ServiceStart& start) {
    Json json;
    json["start_type"] = start.start_type;
    json["delayed"] = start.delayed;
    return json;
}

bool from_json(const Json& json, leftovers::ServiceStart& start) {
    const auto type = json.is_object() ? json.find("start_type") : json.end();
    const auto delayed = json.is_object() ? json.find("delayed") : json.end();
    if (type == json.end() || !type->is_number_unsigned() || type->get<std::uint64_t>() > 0xFFFFFFFF ||
        delayed == json.end() || !delayed->is_boolean()) {
        return false;
    }
    start = {type->get<std::uint32_t>(), delayed->get<bool>()};
    return true;
}

Json to_json(const leftovers::Location& location) {
    using namespace leftovers;
    Json json;
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, FileLocation>) {
                json["kind"] = item.directory ? "folder" : "file";
                put_text(json, "path", item.path);
            } else if constexpr (std::is_same_v<T, RegistryKeyLocation>) {
                json["kind"] = "registry-key";
                put_address(json, item.key);
            } else if constexpr (std::is_same_v<T, RegistryValueLocation>) {
                json["kind"] = "registry-value";
                put_address(json, item.key);
                put_text(json, "value", item.value_name);
            } else if constexpr (std::is_same_v<T, ServiceLocation>) {
                json["kind"] = "service";
                put_text(json, "name", item.name);
                if (item.driver) {
                    json["driver"] = true;
                }
            } else if constexpr (std::is_same_v<T, TaskLocation>) {
                json["kind"] = "task";
                put_text(json, "path", item.path);
                if (item.folder) {
                    json["folder"] = true;
                }
            } else if constexpr (std::is_same_v<T, PackageLocation>) {
                json["kind"] = "package";
                put_text(json, "full_name", item.full_name);
                put_text(json, "user_sid", item.user_sid);
            } else if constexpr (std::is_same_v<T, FirewallRuleLocation>) {
                json["kind"] = "firewall-rule";
                put_text(json, "rule_id", item.rule_id);
            } else if constexpr (std::is_same_v<T, StartupApprovedLocation>) {
                json["kind"] = "startup-approved";
                put_address(json, item.key);
                put_text(json, "value", item.value_name);
                json["expected"] = edit_value_to_json(item.expected);
                json["desired"] = edit_value_to_json(item.desired);
            } else if constexpr (std::is_same_v<T, TaskStateLocation>) {
                json["kind"] = "task-state";
                put_text(json, "path", item.path);
                json["enabled_before"] = item.enabled_before;
                json["enabled_after"] = item.enabled_after;
            } else if constexpr (std::is_same_v<T, ServiceStartLocation>) {
                json["kind"] = "service-start";
                put_text(json, "name", item.name);
                json["before"] = to_json(item.before);
                json["after"] = to_json(item.after);
            } else {
                static_assert(std::is_same_v<T, ValueEditLocation>);
                json["kind"] = "value-edit";
                put_address(json, item.key);
                put_text(json, "value", item.value_name);
                json["edit"] = to_string(item.kind);
                json["expected"] = edit_value_to_json(item.expected);
                json["desired"] = edit_value_to_json(item.desired);
                json["entries"] = text_list_to_json(item.entries);
                json["delta"] = item.delta;
            }
        },
        location);
    return json;
}

bool from_json(const Json& json, leftovers::Location& location) {
    using namespace leftovers;
    if (!json.is_object()) {
        return false;
    }
    const std::string kind = json.value("kind", std::string());
    if (kind == "file" || kind == "folder") {
        const auto path = get_text(json, "path");
        location = FileLocation{path.value_or(L""), kind == "folder"};
        return path.has_value();
    }
    if (kind == "registry-key" || kind == "registry-value") {
        platform::RegistryAddress address;
        if (!get_address(json, address)) {
            return false;
        }
        if (kind == "registry-key") {
            location = RegistryKeyLocation{address};
            return true;
        }
        const auto value = get_text(json, "value");
        location = RegistryValueLocation{address, value.value_or(L"")};
        return value.has_value();
    }
    const auto text = [&](std::string_view field) { return get_text(json, field); };
    if (kind == "service" && text("name")) {
        location = ServiceLocation{*text("name"), json.value("driver", false)};
        return true;
    }
    if (kind == "task" && text("path")) {
        location = TaskLocation{*text("path"), json.value("folder", false)};
        return true;
    }
    if (kind == "package" && text("full_name") && text("user_sid")) {
        location = PackageLocation{*text("full_name"), *text("user_sid")};
        return true;
    }
    if (kind == "firewall-rule" && text("rule_id")) {
        location = FirewallRuleLocation{*text("rule_id")};
        return true;
    }
    if (kind == "value-edit") {
        ValueEditLocation edit;
        const auto value = text("value");
        const auto delta = json.find("delta");
        if (!get_address(json, edit.key) || !value || !parse(json.value("edit", std::string()), edit.kind) ||
            !json.contains("expected") || !json.contains("desired") || !json.contains("entries") ||
            delta == json.end() || !delta->is_number_integer()) {
            return false;
        }
        edit.value_name = *value;
        edit.delta = delta->get<std::int64_t>();
        if (!edit_value_from_json(json["expected"], edit.value_name, edit.expected) ||
            !edit_value_from_json(json["desired"], edit.value_name, edit.desired) ||
            !text_list_from_json(json["entries"], edit.entries)) {
            return false;
        }
        location = std::move(edit);
        return true;
    }
    if (kind == "startup-approved") {
        StartupApprovedLocation approval;
        const auto value = text("value");
        if (!get_address(json, approval.key) || !value || !json.contains("expected") ||
            !json.contains("desired")) {
            return false;
        }
        approval.value_name = *value;
        if (!edit_value_from_json(json["expected"], approval.value_name, approval.expected) ||
            !edit_value_from_json(json["desired"], approval.value_name, approval.desired)) {
            return false;
        }
        location = std::move(approval);
        return true;
    }
    if (kind == "task-state") {
        const auto path = text("path");
        const auto before = json.find("enabled_before");
        const auto after = json.find("enabled_after");
        if (!path || before == json.end() || !before->is_boolean() || after == json.end() ||
            !after->is_boolean()) {
            return false;
        }
        location = TaskStateLocation{*path, before->get<bool>(), after->get<bool>()};
        return true;
    }
    if (kind == "service-start") {
        ServiceStartLocation service;
        const auto name = text("name");
        if (!name || !json.contains("before") || !json.contains("after") ||
            !from_json(json["before"], service.before) || !from_json(json["after"], service.after)) {
            return false;
        }
        service.name = *name;
        location = std::move(service);
        return true;
    }
    return false;
}

Json to_json(const ValueSnapshot& value) {
    Json json;
    put_text(json, "name", value.name);
    json["type"] = value.type;
    json["data"] = to_hex(value.data);
    return json;
}

Json to_json(const KeySnapshot& key) {
    Json json;
    put_text(json, "name", key.name);
    if (!key.class_name.empty()) {
        put_text(json, "class", key.class_name);
    }
    json["security"] = to_hex(key.security);
    json["values"] = Json::array();
    for (const auto& value : key.values) {
        json["values"].push_back(to_json(value));
    }
    json["subkeys"] = Json::array();
    for (const auto& subkey : key.subkeys) {
        json["subkeys"].push_back(to_json(subkey));
    }
    return json;
}

bool from_json(const Json& json, ValueSnapshot& value) {
    const auto name = get_text(json, "name");
    if (!json.is_object() || !name || !json.contains("type") || !json["type"].is_number_unsigned() ||
        !json.contains("data") || !json["data"].is_string()) {
        return false;
    }
    const auto data = from_hex(json["data"].get_ref<const std::string&>());
    if (!data) {
        return false;
    }
    value = {*name, json["type"].get<DWORD>(), *data};
    return true;
}

bool from_json(const Json& json, KeySnapshot& key) {
    const auto name = get_text(json, "name");
    if (!json.is_object() || !name || !json.contains("security") || !json["security"].is_string() ||
        !json.contains("values") || !json["values"].is_array() || !json.contains("subkeys") ||
        !json["subkeys"].is_array()) {
        return false;
    }
    const auto security = from_hex(json["security"].get_ref<const std::string&>());
    if (!security) {
        return false;
    }
    key = {};
    key.name = *name;
    key.class_name = get_text(json, "class").value_or(L"");
    key.security = *security;
    for (const auto& item : json["values"]) {
        if (!from_json(item, key.values.emplace_back())) {
            return false;
        }
    }
    for (const auto& item : json["subkeys"]) {
        if (!from_json(item, key.subkeys.emplace_back())) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> read_file(const std::wstring& path) {
    wil::unique_hfile file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !::GetFileSizeEx(file.get(), &size) || size.QuadPart > 512ll * 1024 * 1024) {
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!text.empty() && (!::ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr) ||
                          read != text.size())) {
        return std::nullopt;
    }
    return text;
}

}  // namespace avel::changes
