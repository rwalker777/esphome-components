#include "../../components/ups_hid/ups_hid.h"
#include "../../components/ups_hid/transport_factory.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace esphome::ups_hid;
bool protocol_available = true, reports_available = true;
unsigned recovery_requests = 0;
bool usb_connected = true;
uint32_t usb_connection = 1;
bool descriptor_based = true;

class TestTransport : public IUsbTransport {
 public:
  esp_err_t initialize() override { return ESP_OK; }
  esp_err_t deinitialize() override { return ESP_OK; }
  bool is_connected() const override { return usb_connected; }
  uint16_t get_vendor_id() const override { return descriptor_based ? 0x09ae : 0x051d; }
  uint16_t get_product_id() const override { return 0x3024; }
  uint32_t connection_id() const override { return usb_connection; }
  esp_err_t request_reenumeration() override {
    ++recovery_requests;
    usb_connected = false;
    return ESP_OK;
  }
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
  bool initialize() override { return protocol_available; }
  bool read_data(UpsData &data) override {
    data.hid_status.descriptor_based = descriptor_based;
    if (!reports_available) {
      return false;
    }
    data.hid_status.ac_present = true;
    data.battery.level = 100;
    data.power.status = "Online";
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
  esphome::binary_sensor::BinarySensor communication;
  hub.register_binary_sensor(&communication, "communication");
  hub.set_protocol_selection("tripplite");
  hub.setup();
  assert(communication.has_state() && !communication.state);
  hub.update();
  assert(hub.is_online() && hub.get_battery_level() == 100);
  assert(communication.has_state() && communication.state);

  // A transient failed poll must not leave the normal control-button protocol
  // guard disabled after fresh data returns, without needing re-enumeration.
  reports_available = false;
  hub.update();
  assert(!communication.state);
  reports_available = true;
  hub.update();
  assert(communication.state && recovery_requests == 0);
  assert(hub.get_ups_data().device.detected_protocol == DeviceInfo::PROTOCOL_TRIPPLITE_HID);

  // This USB session stays broken until the driver actually re-enumerates it.
  reports_available = false;
  protocol_available = false;
  for (unsigned poll = 0; poll < 6; ++poll) {
    esphome::test_millis += 5000;
    hub.update();
    assert(communication.has_state() && !communication.state);
  }
  assert(recovery_requests == 1 && !usb_connected);
  assert(!hub.is_failed());
  assert(hub.status_has_warning());
  assert(!hub.has_power_status() && std::isnan(hub.get_battery_level()));

  // A new enumeration still needs fresh protocol initialization and data.
  usb_connected = true;
  ++usb_connection;
  protocol_available = true;
  reports_available = true;
  hub.update();
  assert(hub.is_online() && hub.get_battery_level() == 100);
  assert(!hub.status_has_warning());
  assert(communication.state);

  // Detection failures use the same recovery path, with 30/60/120/240/300s
  // spacing until a successful read resets backoff. A fresh connection alone
  // must not reset that spacing or establish communication.
  reports_available = false;
  protocol_available = false;
  uint32_t previous_reset = esphome::test_millis;
  for (uint32_t interval : {30000u, 60000u, 120000u, 240000u, 300000u, 300000u}) {
    unsigned before = recovery_requests;
    for (uint32_t elapsed = 5000; elapsed <= interval; elapsed += 5000) {
      esphome::test_millis = previous_reset + elapsed;
      hub.update();
      assert(recovery_requests == before + unsigned(elapsed == interval));
    }
    previous_reset = esphome::test_millis;
    usb_connected = true;
    ++usb_connection;
    assert(!communication.state);
  }
  // Recovery remains scheduled while no device can enumerate.
  usb_connected = false;
  unsigned before = recovery_requests;
  esphome::test_millis += 300000;
  hub.update();
  assert(recovery_requests == before + 1);

  usb_connected = true;
  ++usb_connection;
  protocol_available = true;
  reports_available = true;
  hub.update();
  assert(hub.is_online() && !hub.status_has_warning());
  assert(communication.state);

  // Legacy protocols must not republish their last sample as fresh or invent
  // a fault after disconnect. This path has no descriptor-based invalidation.
  descriptor_based = false;
  UpsHidComponent legacy;
  esphome::sensor::Sensor battery;
  esphome::binary_sensor::BinarySensor ac_present, fault;
  legacy.register_sensor(&battery, "battery_level");
  legacy.register_binary_sensor(&ac_present, "online");
  legacy.register_binary_sensor(&fault, "fault");
  legacy.setup();
  legacy.update();
  assert(battery.state == 100 && ac_present.state);
  usb_connected = false;
  for (unsigned poll = 0; poll < 6; ++poll) {
    legacy.update();
    assert(std::isnan(battery.state));
    assert(!ac_present.has_state() && !fault.has_state());
  }
  std::cout << "UPS polling recovery checks passed\n";
}
