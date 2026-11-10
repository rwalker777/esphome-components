#include "../../components/ups_hid/transport_esp32.h"
#include "esphome/core/helpers.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <vector>

using esphome::ups_hid::Esp32UsbTransport;
namespace {
usb_host_client_config_t client{};
void (*client_task)(void *) = nullptr;
void (*library_task)(void *) = nullptr;
void *task_arg = nullptr;
struct EndTaskSlice {};
int task_iterations = 0;
int library_iterations = 0;
std::deque<std::function<void()>> events;
usb_device_handle_t opened_device = nullptr;
usb_transfer_t *pending_transfer = nullptr;
bool interface_claimed = false;
int transfers = 0, semaphores = 0, opens = 0, closes = 0, submissions = 0;
int port_off = 0, port_on = 0, library_devices = 0;
bool fail_port_off = false, fail_port_on = false;
bool retain_library_device = false;
std::vector<uint8_t> descriptor_reply;

void new_device() {
  library_devices = 1;
  usb_host_client_event_msg_t event{};
  event.event = USB_HOST_CLIENT_EVENT_NEW_DEV;
  event.new_dev.address = 1;
  client.async.client_event_callback(&event, client.async.callback_arg);
}
void device_gone() {
  usb_host_client_event_msg_t event{};
  event.event = USB_HOST_CLIENT_EVENT_DEV_GONE;
  event.dev_gone.dev_hdl = opened_device;
  client.async.client_event_callback(&event, client.async.callback_arg);
}
void run_client(int iterations = 1) {
  task_iterations = iterations;
  try { client_task(task_arg); } catch (const EndTaskSlice &) {}
  assert(task_iterations == 0);
}
void run_library(int iterations = 1) {
  library_iterations = iterations;
  try { library_task(task_arg); } catch (const EndTaskSlice &) {}
  assert(library_iterations == 0);
}
void complete_transfer() {
  auto *transfer = pending_transfer;
  assert(transfer);
  pending_transfer = nullptr;
  transfer->in_flight = false;
  transfer->status = 1;  // Disconnect/error completion after caller timeout.
  transfer->actual_num_bytes = 0;
  transfer->callback(transfer);
}
}  // namespace

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t, void *arg, unsigned, TaskHandle_t *handle) {
  if (std::strcmp(name, "usb_client_task") == 0) { client_task = task; task_arg = arg; }
  if (std::strcmp(name, "usb_lib_task") == 0) library_task = task;
  *handle = arg;
  return pdTRUE;
}
void vTaskDelay(uint32_t ticks) {
  esphome::test_millis += ticks;
  if (task_iterations > 0 && --task_iterations == 0) throw EndTaskSlice{};
}
void vTaskDelete(TaskHandle_t) {}
SemaphoreHandle_t xSemaphoreCreateBinary() { ++semaphores; return new TestSemaphore; }
void vSemaphoreDelete(SemaphoreHandle_t sem) { --semaphores; delete sem; }
void xSemaphoreGive(SemaphoreHandle_t sem) { sem->signaled = true; }
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t) { return sem->signaled ? pdTRUE : 0; }
esp_err_t usb_host_transfer_alloc(size_t size, int, usb_transfer_t **out) {
  ++transfers;
  *out = new usb_transfer_t;
  (*out)->data_buffer = new uint8_t[size]{};
  return ESP_OK;
}
esp_err_t usb_host_transfer_free(usb_transfer_t *transfer) {
  assert(!transfer->in_flight);
  --transfers;
  delete[] transfer->data_buffer;
  delete transfer;
  return ESP_OK;
}
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t, usb_transfer_t *transfer) {
  assert(opened_device == transfer->device_handle && !pending_transfer);
  ++submissions;
  transfer->in_flight = true;
  pending_transfer = transfer;
  if (!descriptor_reply.empty()) {
    auto *setup = reinterpret_cast<usb_setup_packet_t *>(transfer->data_buffer);
    assert(setup->bmRequestType == 0x80 && setup->bRequest == 6 && setup->wValue == 0x302);
    assert(setup->wIndex == 0x409 && setup->wLength == 255 && transfer->num_bytes == 264);
    std::memcpy(transfer->data_buffer + 8, descriptor_reply.data(), descriptor_reply.size());
    transfer->actual_num_bytes = 8 + descriptor_reply.size();
    transfer->status = USB_TRANSFER_STATUS_COMPLETED;
    transfer->in_flight = false;
    pending_transfer = nullptr;
    transfer->callback(transfer);
  }
  return ESP_OK;
}
esp_err_t usb_host_client_register(const usb_host_client_config_t *config, usb_host_client_handle_t *out) {
  client = *config;
  *out = &client;
  return ESP_OK;
}
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, uint32_t) {
  esphome::test_millis += 100;
  if (!events.empty()) {
    auto event = std::move(events.front()); events.pop_front(); event();
  }
  return ESP_OK;
}
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t *out) {
  assert(!opened_device);
  *out = opened_device = reinterpret_cast<void *>(uintptr_t(++opens));
  return ESP_OK;
}
esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t) {
  assert(!interface_claimed);
  interface_claimed = true;
  return ESP_OK;
}
esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t) {
  assert(interface_claimed && !pending_transfer);
  interface_claimed = false;
  return ESP_OK;
}
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t device) {
  assert(device == opened_device && !interface_claimed && !pending_transfer);
  opened_device = nullptr;
  if (!retain_library_device) library_devices = 0;
  ++closes;
  return ESP_OK;
}
esp_err_t usb_host_lib_set_root_port_power(bool enable) {
  if ((enable && fail_port_on) || (!enable && fail_port_off)) return ESP_FAIL;
  if (enable) {
    assert(!opened_device && !pending_transfer && library_devices == 0);
    ++port_on;
    events.push_back(new_device);
  } else {
    ++port_off;
  }
  return ESP_OK;
}
esp_err_t usb_host_lib_info(usb_host_lib_info_t *info) { info->num_devices = library_devices; return ESP_OK; }
esp_err_t usb_host_device_info(usb_device_handle_t, usb_device_info_t *info) { *info = {}; return ESP_OK; }
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t, const usb_device_desc_t **out) {
  static const usb_device_desc_t descriptor{}; *out = &descriptor; return ESP_OK;
}
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t **out) {
  static const usb_config_desc_t descriptor{}; *out = &descriptor; return ESP_OK;
}
const usb_intf_desc_t *usb_parse_interface_descriptor(const usb_config_desc_t *, int, int, int *) {
  static const usb_intf_desc_t descriptor{}; return &descriptor;
}
esp_err_t usb_host_device_addr_list_fill(int, uint8_t *, int *count) { *count = 0; return ESP_OK; }
esp_err_t usb_host_client_deregister(usb_host_client_handle_t) { return ESP_OK; }
esp_err_t usb_host_install(const usb_host_config_t *) { return ESP_OK; }
esp_err_t usb_host_uninstall() { return ESP_OK; }
esp_err_t usb_host_device_free_all() { return ESP_OK; }
esp_err_t usb_host_lib_handle_events(uint32_t timeout, uint32_t *flags) {
  assert(timeout == 200);  // Idle library handling must still advance recovery.
  if (library_iterations == 0) throw EndTaskSlice{};
  --library_iterations;
  esphome::test_millis += timeout;
  *flags = 0;
  return ESP_ERR_TIMEOUT;
}

int main() {
  Esp32UsbTransport transport;
  assert(transport.initialize() == ESP_OK && !transport.is_connected());
  events.push_back(new_device);
  run_client();
  assert(transport.is_connected() && opens == 1);
  uint32_t original_connection = transport.connection_id();

  // String descriptors use the same pending-transfer owner as HID reports.
  // A caller timeout must leave USB resources owned by the pending completion.
  uint8_t report[2]{};
  std::string description;
  assert(transport.get_string_descriptor(1, description) == ESP_ERR_TIMEOUT && description.empty());
  assert(transfers == 1 && semaphores == 1 && pending_transfer);
  assert(transport.request_reenumeration() == ESP_OK && !transport.is_connected());
  assert(transport.connection_id() != original_connection);
  assert(transport.request_reenumeration() == ESP_ERR_INVALID_STATE);
  assert(transport.hid_set_report(3, 1, report, sizeof(report), 10, original_connection) == ESP_ERR_INVALID_STATE);
  assert(submissions == 1);

  // A queued pre-reset NEW_DEV cannot restore the old session. Failed port
  // operations retry without reporting recovery or discarding transfer owners.
  events.push_back(new_device);
  run_client();
  fail_port_off = true;
  run_library(3);
  assert(opens == 1 && port_off == 0 && !transport.is_connected());
  fail_port_off = false;
  run_library(3);
  assert(port_off == 0);  // Port errors wait for the hub's next backed-off request.
  assert(transport.request_reenumeration() == ESP_OK);
  run_library(3);
  assert(port_off == 1 && port_on == 0);
  events.push_back(device_gone);
  run_client(3);
  run_library(3);
  assert(closes == 0 && transfers == 1 && port_on == 0);
  complete_transfer();
  assert(transfers == 0 && semaphores == 0);
  retain_library_device = true;
  run_client(3);
  run_library(3);
  assert(closes == 1 && port_on == 0 && !transport.is_connected());
  library_devices = 0;  // Host library finishes freeing its disconnected device.
  retain_library_device = false;
  fail_port_on = true;
  run_library(3);
  assert(closes == 1 && port_on == 0 && !transport.is_connected());
  fail_port_on = false;
  run_library(3);
  assert(port_on == 0);
  assert(transport.request_reenumeration() == ESP_OK);
  run_library(3);
  run_client();
  assert(port_on == 1 && opens == 2 && transport.is_connected());
  assert(transport.connection_id() > original_connection);
  assert(transport.hid_set_report(3, 1, report, sizeof(report), 10, original_connection) == ESP_ERR_INVALID_STATE);
  assert(submissions == 1);  // An uncertain command was never replayed.

  // A normal disconnect followed by NEW_DEV in the same event drain must open
  // the replacement; deferring all cleanup until after the drain loses NEW_DEV.
  uint32_t recovered_connection = transport.connection_id();
  events.push_back([] { device_gone(); new_device(); });
  run_client();
  assert(transport.is_connected() && opens == 3 && closes == 2);
  assert(transport.connection_id() > recovered_connection);
  assert(port_off == 1 && port_on == 1);

  // An empty port has no device to emit ALL_FREE, so recovery must progress
  // without waiting for a notification that can never arrive.
  events.push_back(device_gone);
  run_client();
  assert(transport.request_reenumeration() == ESP_OK);
  run_library(3);
  run_client();
  assert(port_off == 2 && port_on == 2 && transport.is_connected());
  // String reads keep UTF-16LE decoding and trailing-space trimming while the
  // shared transfer helper pads the 255-byte request to the endpoint MPS.
  descriptor_reply = {8, 3, 'A', 0, 0xa9, 0, ' ', 0};
  assert(transport.get_string_descriptor(2, description) == ESP_OK && description == "A?");
  assert(transport.deinitialize() == ESP_OK);
  assert(!opened_device && !pending_transfer && transfers == 0 && semaphores == 0);
  std::cout << "ESP32 USB transport recovery checks passed\n";
}
