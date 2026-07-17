#include "protocol_factory.h"
#include "ups_hid.h"
#include "esphome/core/log.h"
#include "esphome/components/logger/logger.h"
#include <algorithm>

namespace esphome {
namespace ups_hid {

static const char *const FACTORY_TAG = "ups_hid.factory";

// Forward declarations for protocol creator functions
// These are defined in their respective protocol implementation files
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
    // Initialize built-in protocols on first access
    // This ensures all protocols are registered regardless of static initializer order
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    
    ESP_LOGI(FACTORY_TAG, "Initializing built-in UPS protocol support");
    
    // Register APC protocol for vendor 0x051D
    {
        ProtocolInfo info;
        info.creator = create_apc_protocol;
        info.name = "APC";
        info.description = "APC HID Protocol";
        info.supported_vendors = {0x051D};
        info.priority = 200;
        auto& registry = get_vendor_registry();
        registry[0x051D].push_back(info);
        ESP_LOGI(FACTORY_TAG, "Registered APC protocol for vendor 0x051D");
    }
    
    // Register CyberPower protocol for vendor 0x0764
    {
        ProtocolInfo info;
        info.creator = create_cyberpower_protocol;
        info.name = "CyberPower";
        info.description = "CyberPower HID Protocol";
        info.supported_vendors = {0x0764};
        info.priority = 200;
        auto& registry = get_vendor_registry();
        registry[0x0764].push_back(info);
        ESP_LOGI(FACTORY_TAG, "Registered CyberPower protocol for vendor 0x0764");
    }
    
    // Register GoldenMate protocol for vendor 0x075D
    {
        ProtocolInfo info;
        info.creator = create_goldenmate_protocol;
        info.name = "GoldenMate";
        info.description = "GoldenMate HID Protocol";
        info.supported_vendors = {0x075D};
        info.priority = 200;
        auto& registry = get_vendor_registry();
        registry[0x075D].push_back(info);
        ESP_LOGI(FACTORY_TAG, "Registered GoldenMate protocol for vendor 0x075D");
    }
    
    // Register Generic protocol as fallback (always last)
    {
        ProtocolInfo info;
        info.creator = create_generic_protocol;
        info.name = "Generic HID Protocol";
        info.description = "Universal HID protocol fallback for unknown UPS vendors";
        info.supported_vendors = {};
        info.priority = 10;
        auto& registry = get_fallback_registry();
        registry.push_back(info);
        ESP_LOGI(FACTORY_TAG, "Registered Generic HID protocol as fallback");
    }
}

void ProtocolFactory::register_protocol_for_vendor(uint16_t vendor_id, 
                                                   const ProtocolInfo& info) {
    ensure_initialized();
    
    auto& registry = get_vendor_registry();
    registry[vendor_id].push_back(info);
    
    // Sort by priority (higher first)
    std::sort(registry[vendor_id].begin(), registry[vendor_id].end(),
              [](const ProtocolInfo& a, const ProtocolInfo& b) {
                  return a.priority > b.priority;
              });
}

void ProtocolFactory::register_fallback_protocol(const ProtocolInfo& info) {
    ensure_initialized();
    
    auto& registry = get_fallback_registry();
    registry.push_back(info);
    
    // Sort by priority (higher first)
    std::sort(registry.begin(), registry.end(),
              [](const ProtocolInfo& a, const ProtocolInfo& b) {
                  return a.priority > b.priority;
              });
}

std::unique_ptr<UpsProtocolBase> 
ProtocolFactory::create_for_vendor(uint16_t vendor_id, UpsHidComponent* parent) {
    ensure_initialized();
    
    if (!parent) {
        ESP_LOGE(FACTORY_TAG, "Cannot create protocol with null parent component");
        return nullptr;
    }
    
    // Try vendor-specific protocols first
    auto& vendor_registry = get_vendor_registry();
    auto vendor_it = vendor_registry.find(vendor_id);
    
    if (vendor_it != vendor_registry.end()) {
        ESP_LOGD(FACTORY_TAG, "Found %zu vendor-specific protocols for 0x%04X", 
                 vendor_it->second.size(), vendor_id);
        
        for (const auto& info : vendor_it->second) {
            ESP_LOGD(FACTORY_TAG, "Trying vendor protocol '%s' for 0x%04X", 
                     info.name.c_str(), vendor_id);
            
            auto protocol = info.creator(parent);
            if (protocol && protocol->detect()) {
                ESP_LOGI(FACTORY_TAG, "Successfully created protocol '%s' for vendor 0x%04X", 
                         info.name.c_str(), vendor_id);
                return protocol;
            }
        }
    }
    
    // Try fallback protocols
    auto& fallback_registry = get_fallback_registry();
    ESP_LOGD(FACTORY_TAG, "Trying %zu fallback protocols for vendor 0x%04X", 
             fallback_registry.size(), vendor_id);
    
    for (const auto& info : fallback_registry) {
        ESP_LOGD(FACTORY_TAG, "Trying fallback protocol '%s' for 0x%04X", 
                 info.name.c_str(), vendor_id);
        
        auto protocol = info.creator(parent);
        if (protocol && protocol->detect()) {
            ESP_LOGI(FACTORY_TAG, "Successfully created fallback protocol '%s' for vendor 0x%04X", 
                     info.name.c_str(), vendor_id);
            return protocol;
        }
    }
    
    ESP_LOGW(FACTORY_TAG, "No suitable protocol found for vendor 0x%04X", vendor_id);
    return nullptr;
}

std::vector<ProtocolFactory::ProtocolInfo> 
ProtocolFactory::get_protocols_for_vendor(uint16_t vendor_id) {
    ensure_initialized();
    
    std::vector<ProtocolInfo> protocols;
    
    // Add vendor-specific protocols first
    auto& vendor_registry = get_vendor_registry();
    auto vendor_it = vendor_registry.find(vendor_id);
    
    if (vendor_it != vendor_registry.end()) {
        for (const auto& info : vendor_it->second) {
            protocols.push_back(info);
        }
    }
    
    // Add fallback protocols
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        protocols.push_back(info);
    }
    
    return protocols;
}

std::vector<std::pair<uint16_t, ProtocolFactory::ProtocolInfo>> 
ProtocolFactory::get_all_protocols() {
    ensure_initialized();
    
    std::vector<std::pair<uint16_t, ProtocolInfo>> all_protocols;
    
    // Add vendor-specific protocols
    auto& vendor_registry = get_vendor_registry();
    for (const auto& vendor_pair : vendor_registry) {
        uint16_t vendor_id = vendor_pair.first;
        for (const auto& info : vendor_pair.second) {
            all_protocols.emplace_back(vendor_id, info);
        }
    }
    
    // Add fallback protocols (use 0x0000 as special vendor ID for fallbacks)
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        all_protocols.emplace_back(0x0000, info);
    }
    
    return all_protocols;
}

bool ProtocolFactory::has_vendor_support(uint16_t vendor_id) {
    ensure_initialized();
    
    auto& vendor_registry = get_vendor_registry();
    auto it = vendor_registry.find(vendor_id);
    
    // Has support if vendor-specific protocols exist OR fallback protocols exist
    bool has_vendor_specific = (it != vendor_registry.end() && !it->second.empty());
    bool has_fallback = !get_fallback_registry().empty();
    
    return has_vendor_specific || has_fallback;
}

std::unique_ptr<UpsProtocolBase> 
ProtocolFactory::create_by_name(const std::string& protocol_name, UpsHidComponent* parent) {
    ensure_initialized();
    
    if (!parent) {
        ESP_LOGE(FACTORY_TAG, "Cannot create protocol with null parent component");
        return nullptr;
    }
    
    ESP_LOGD(FACTORY_TAG, "Creating protocol by name: %s", protocol_name.c_str());
    
    // Search through all registered protocols to find one with matching name
    auto& vendor_registry = get_vendor_registry();
    for (const auto& vendor_pair : vendor_registry) {
        for (const auto& info : vendor_pair.second) {
            // Match protocol name (case-insensitive)
            std::string info_name_lower = info.name;
            std::string protocol_name_lower = protocol_name;
            std::transform(info_name_lower.begin(), info_name_lower.end(), info_name_lower.begin(), ::tolower);
            std::transform(protocol_name_lower.begin(), protocol_name_lower.end(), protocol_name_lower.begin(), ::tolower);
            
            if (info_name_lower.find(protocol_name_lower) != std::string::npos) {
                ESP_LOGD(FACTORY_TAG, "Found matching protocol '%s' for name '%s'", 
                         info.name.c_str(), protocol_name.c_str());
                auto protocol = info.creator(parent);
                if (protocol) {
                    ESP_LOGI(FACTORY_TAG, "Successfully created protocol '%s' by name", 
                             protocol->get_protocol_name().c_str());
                    return protocol;
                }
            }
        }
    }
    
    // Search through fallback protocols
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        std::string info_name_lower = info.name;
        std::string protocol_name_lower = protocol_name;
        std::transform(info_name_lower.begin(), info_name_lower.end(), info_name_lower.begin(), ::tolower);
        std::transform(protocol_name_lower.begin(), protocol_name_lower.end(), protocol_name_lower.begin(), ::tolower);
        
        if (info_name_lower.find(protocol_name_lower) != std::string::npos) {
            ESP_LOGD(FACTORY_TAG, "Found matching fallback protocol '%s' for name '%s'", 
                     info.name.c_str(), protocol_name.c_str());
            auto protocol = info.creator(parent);
            if (protocol) {
                ESP_LOGI(FACTORY_TAG, "Successfully created fallback protocol '%s' by name", 
                         protocol->get_protocol_name().c_str());
                return protocol;
            }
        }
    }
    
    ESP_LOGE(FACTORY_TAG, "No protocol found with name containing '%s'", protocol_name.c_str());
    return nullptr;
}

} // namespace ups_hid
} // namespace esphome
