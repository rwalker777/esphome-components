#pragma once

#include "esp_err.h"
#include <vector>
#include <cstdint>
#include <memory>
#include <string>

namespace esphome {
namespace ups_hid {

/**
 * Abstract USB Transport Interface
 * 
 * Provides a clean abstraction over USB communication, allowing for
 * different implementations (ESP32 hardware, simulation, etc.)
 * 
 * Design Pattern: Strategy Pattern for transport selection
 */
class IUsbTransport {
public:
    virtual ~IUsbTransport() = default;
    
    // Transport lifecycle
    virtual esp_err_t initialize() = 0;
    virtual esp_err_t deinitialize() = 0;
    
    // Device management
    virtual bool is_connected() const = 0;
    virtual uint16_t get_vendor_id() const = 0;
    virtual uint16_t get_product_id() const = 0;
    
    virtual uint32_t connection_id() const { return 0; }

    // HID communication
    virtual esp_err_t hid_get_report(uint8_t report_type, uint8_t report_id, 
                                   uint8_t* data, size_t* data_len, 
                                   uint32_t timeout_ms = 1000) = 0;
    
    // Nonzero expected_connection binds submission to a particular enumeration.
    // Zero preserves legacy callers' current-device behavior.
    virtual esp_err_t hid_set_report(uint8_t report_type, uint8_t report_id,
                                   const uint8_t* data, size_t data_len,
                                   uint32_t timeout_ms = 1000, uint32_t expected_connection = 0) = 0;
    
    // Optional capability; simulation and other transports fail closed.
    virtual esp_err_t get_hid_report_descriptor(std::vector<uint8_t>& descriptor, uint32_t timeout_ms) {
        descriptor.clear();
        return ESP_ERR_NOT_SUPPORTED;
    }

    // String descriptors
    virtual esp_err_t get_string_descriptor(uint8_t string_index, 
                                          std::string& result) = 0;
    
    // Error information
    virtual std::string get_last_error() const = 0;
};

// Forward declaration - implementation in usb_transport_factory.h
class UsbTransportFactory;

} // namespace ups_hid
} // namespace esphome
