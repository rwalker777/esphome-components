#include "protocol_factory.h"
#include "ups_hid.h"
#include "esphome/core/log.h"
#include "esphome/components/logger/logger.h"
#include <algorithm>
#include <cctype>

namespace esphome {
namespace ups_hid {

static const char *const FACTORY_TAG = "ups_hid.factory";

// Forward declarations for protocol creator functions
extern std::unique_ptr<UpsProtocolBase> create_apc_protocol(UpsHidComponent* parent);
extern std::unique_ptr<UpsProtocolBase> create_cyberpower_protocol(UpsHidComponent* parent);
extern std::unique_ptr<UpsProtocolBase> create_goldenmate_protocol(UpsHidComponent* parent);
extern std::unique_ptr<UpsProtocolBase> create_generic_protocol(UpsHidComponent* parent);

// Static registry implementations
std::unordered_map<uint16_t, std::vector<ProtocolFactory::ProtocolInfo>>&
ProtocolFactory::get_vendor_registry() {
    static std::unordered_map<uint16_t, std::vector<ProtocolInfo>> vendor_registry;
    return vendor_registry;
}

std::vector<ProtocolFactory::ProtocolInfo>&
ProtocolFactory::get_fallback_registry() {
    static std::vector<ProtocolInfo> fallback_registry;
    return fallback_registry;
}

void ProtocolFactory::ensure_initialized() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    ESP_LOGI(FACTORY_TAG, "Initializing built-in UPS protocol support");

    // Register APC protocol
    {
        ProtocolInfo info;
        info.creator = create_apc_protocol;
        info.name = "APC";
        info.description = "APC HID Protocol";
        info.supported_vendors = {0x051D};
        info.priority = 200;
        register_protocol_for_vendor(0x051D, info);
    }

    // Register CyberPower protocol
    {
        ProtocolInfo info;
        info.creator = create_cyberpower_protocol;
        info.name = "CyberPower";
        info.description = "CyberPower HID Protocol";
        info.supported_vendors = {0x0764};
        info.priority = 200;
        register_protocol_for_vendor(0x0764, info);
    }

    // Register GoldenMate protocol
    {
        ProtocolInfo info;
        info.creator = create_goldenmate_protocol;
        info.name = "GoldenMate";
        info.description = "GoldenMate HID Protocol";
        info.supported_vendors = {0x075D};
        info.priority = 200;
        register_protocol_for_vendor(0x075D, info);
    }

    // Register Generic protocol as fallback
    {
        ProtocolInfo info;
        info.creator = create_generic_protocol;
        info.name = "Generic HID Protocol";
        info.description = "Universal HID protocol fallback";
        info.supported_vendors = {};
        info.priority = 10;
        register_fallback_protocol(info);
    }
}

void ProtocolFactory::register_protocol_for_vendor(uint16_t vendor_id, const ProtocolInfo& info) {
    auto& registry = get_vendor_registry();
    registry[vendor_id].push_back(info);

    std::sort(registry[vendor_id].begin(), registry[vendor_id].end(),
              [](const ProtocolInfo& a, const ProtocolInfo& b) {
                  return a.priority > b.priority;
              });
}

void ProtocolFactory::register_fallback_protocol(const ProtocolInfo& info) {
    auto& registry = get_fallback_registry();
    registry.push_back(info);

    std::sort(registry.begin(), registry.end(),
              [](const ProtocolInfo& a, const ProtocolInfo& b) {
                  return a.priority > b.priority;
              });
}

std::unique_ptr<UpsProtocolBase> ProtocolFactory::create_for_vendor(uint16_t vendor_id, UpsHidComponent* parent) {
    ensure_initialized();

    if (!parent) return nullptr;

    auto& vendor_registry = get_vendor_registry();
    auto vendor_it = vendor_registry.find(vendor_id);

    if (vendor_it != vendor_registry.end()) {
        for (const auto& info : vendor_it->second) {
            auto protocol = info.creator(parent);
            if (protocol && protocol->detect()) {
                ESP_LOGI(FACTORY_TAG, "Successfully created protocol '%s' for vendor 0x%04X", info.name.c_str(), vendor_id);
                return protocol;
            }
        }
    }

    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        auto protocol = info.creator(parent);
        if (protocol && protocol->detect()) {
            ESP_LOGI(FACTORY_TAG, "Successfully created fallback protocol '%s'", info.name.c_str());
            return protocol;
        }
    }

    ESP_LOGW(FACTORY_TAG, "No suitable protocol found for vendor 0x%04X", vendor_id);
    return nullptr;
}

std::vector<ProtocolFactory::ProtocolInfo> ProtocolFactory::get_protocols_for_vendor(uint16_t vendor_id) {
    ensure_initialized();
    std::vector<ProtocolInfo> protocols;
    auto& vendor_registry = get_vendor_registry();
    auto vendor_it = vendor_registry.find(vendor_id);

    if (vendor_it != vendor_registry.end()) {
        for (const auto& info : vendor_it->second) protocols.push_back(info);
    }

    for (const auto& info : get_fallback_registry()) protocols.push_back(info);
    return protocols;
}

std::vector<std::pair<uint16_t, ProtocolFactory::ProtocolInfo>> ProtocolFactory::get_all_protocols() {
    ensure_initialized();
    std::vector<std::pair<uint16_t, ProtocolInfo>> all_protocols;

    for (const auto& vendor_pair : get_vendor_registry()) {
        for (const auto& info : vendor_pair.second) all_protocols.emplace_back(vendor_pair.first, info);
    }

    for (const auto& info : get_fallback_registry()) all_protocols.emplace_back(0x0000, info);
    return all_protocols;
}

bool ProtocolFactory::has_vendor_support(uint16_t vendor_id) {
    ensure_initialized();
    auto& vendor_registry = get_vendor_registry();
    auto it = vendor_registry.find(vendor_id);
    return (it != vendor_registry.end() && !it->second.empty()) || !get_fallback_registry().empty();
}

std::unique_ptr<UpsProtocolBase> ProtocolFactory::create_by_name(const std::string& protocol_name, UpsHidComponent* parent) {
    ensure_initialized();
    if (!parent) return nullptr;

    auto search_registry = [&](const std::vector<ProtocolInfo>& registry) -> std::unique_ptr<UpsProtocolBase> {
        for (const auto& info : registry) {
            std::string info_name_lower = info.name;
            std::string protocol_name_lower = protocol_name;
            std::transform(info_name_lower.begin(), info_name_lower.end(), info_name_lower.begin(), [](unsigned char c) { return std::tolower(c); });
            std::transform(protocol_name_lower.begin(), protocol_name_lower.end(), protocol_name_lower.begin(), [](unsigned char c) { return std::tolower(c); });

            if (info_name_lower.find(protocol_name_lower) != std::string::npos) {
                auto protocol = info.creator(parent);
                if (protocol) return protocol;
            }
        }
        return nullptr;
    };

    for (const auto& vendor_pair : get_vendor_registry()) {
        if (auto p = search_registry(vendor_pair.second)) return p;
    }

    if (auto p = search_registry(get_fallback_registry())) return p;

    ESP_LOGE(FACTORY_TAG, "No protocol found with name containing '%s'", protocol_name.c_str());
    return nullptr;
}

} // namespace ups_hid
} // namespace esphome
