#include "../../components/ups_hid/ups_hid.h"
#include "../../components/ups_hid/transport_factory.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace esphome::ups_hid;
bool protocol_available = true, reports_available = true;
unsigned initialization_attempts = 0;

class TestTransport : public IUsbTransport {
 public:
  esp_err_t initialize() override { return ESP_OK; }
  esp_err_t deinitialize() override { return ESP_OK; }
  bool is_connected() const override { return true; }
  uint16_t get_vendor_id() const override { return 0x09ae; }
  uint16_t get_product_id() const override { return 0x3024; }
  uint32_t connection_id() const override { return 1; }
  esp_err_t hid_get_report(uint8_t, uint8_t, uint8_t *, size_t *, uint32_t) override {
    return ESP_ERR_NOT_SUPPORTED;
  }
  esp_err_t hid_set_report(uint8_t, uint8_t, const uint8_t *, size_t, uint32_t, uint32_t) override {
    return ESP_ERR_NOT_SUPPORTED;
  }
  esp_err_t get_string_descriptor(uint8_t, std::string &) override { return ESP_ERR_NOT_SUPPORTED; }
  std::string get_last_error() const override { return {}; }
};

class TestProtocol : public UpsProtocolBase {
 public:
  using UpsProtocolBase::UpsProtocolBase;
  bool detect() override { return true; }
  bool initialize() override {
    ++initialization_attempts;
    return protocol_available;
  }
  bool read_data(UpsData &data) override {
    data.hid_status.descriptor_based = true;
    if (!reports_available) {
      return false;
    }
    data.hid_status.ac_present = true;
    data.battery.level = 100;
    return true;
  }
  DeviceInfo::DetectedProtocol get_protocol_type() const override { return DeviceInfo::PROTOCOL_TRIPPLITE_HID; }
  std::string get_protocol_name() const override { return "Test protocol"; }
};

namespace esphome::ups_hid {
std::unique_ptr<IUsbTransport> UsbTransportFactory::create(TransportType, bool) {
  return std::make_unique<TestTransport>();
}
std::unique_ptr<UpsProtocolBase> ProtocolFactory::create_by_name(const std::string &, UpsHidComponent *parent) {
  return std::make_unique<TestProtocol>(parent);
}
std::unique_ptr<UpsProtocolBase> ProtocolFactory::create_for_vendor(uint16_t, UpsHidComponent *parent) {
  return std::make_unique<TestProtocol>(parent);
}
}  // namespace esphome::ups_hid

int main() {
  UpsHidComponent hub;
  hub.set_protocol_selection("tripplite");
  hub.setup();
  hub.update();
  assert(hub.is_online() && hub.get_battery_level() == 100);

  // Lose reports on an established connection, then fail reinitialization for
  // longer than the old permanent-failure threshold. Poll as ESPHome does:
  // failed components no longer receive scheduler callbacks.
  reports_available = false;
  protocol_available = false;
  for (unsigned poll = 0; poll < 30; ++poll) {
    if (!hub.is_failed()) {
      hub.update();
    }
  }
  assert(!hub.is_failed());
  assert(hub.status_has_warning());
  assert(initialization_attempts > 6);
  assert(!hub.has_power_status() && std::isnan(hub.get_battery_level()));

  protocol_available = true;
  reports_available = true;
  hub.update();
  assert(hub.is_online() && hub.get_battery_level() == 100);
  assert(!hub.status_has_warning());
  std::cout << "UPS polling recovery checks passed\n";
}
