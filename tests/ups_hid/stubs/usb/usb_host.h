#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
struct usb_setup_packet_t {
  uint8_t bmRequestType, bRequest;
  uint16_t wValue, wIndex, wLength;
};
static_assert(sizeof(usb_setup_packet_t) == 8);
constexpr int USB_TRANSFER_STATUS_COMPLETED = 0;
struct usb_transfer_t {
  uint8_t *data_buffer;
  void *context = nullptr, *device_handle = nullptr;
  void (*callback)(usb_transfer_t *) = nullptr;
  uint8_t bEndpointAddress = 0;
  int num_bytes = 0, actual_num_bytes = 0, status = 0;
  uint32_t timeout_ms = 0;
  bool in_flight = false;
};
esp_err_t usb_host_transfer_alloc(size_t size, int, usb_transfer_t **transfer);
esp_err_t usb_host_transfer_free(usb_transfer_t *transfer);

// Minimal host API boundary for compiling the real ESP32 transport on the host.
using usb_device_handle_t = void *;
using usb_host_client_handle_t = void *;
enum usb_speed_t { USB_SPEED_LOW };
struct usb_device_desc_t {
  uint8_t bMaxPacketSize0 = 8, bDeviceClass = 0;
  uint16_t idVendor = 0x09ae, idProduct = 0x3024;
};
struct usb_config_desc_t { uint16_t wTotalLength = 34; uint8_t bNumInterfaces = 1; };
struct usb_intf_desc_t { uint8_t bInterfaceClass = 3, bInterfaceNumber = 0; };
struct usb_device_info_t { usb_speed_t speed = USB_SPEED_LOW; };
enum { USB_HOST_CLIENT_EVENT_NEW_DEV, USB_HOST_CLIENT_EVENT_DEV_GONE };
struct usb_host_client_event_msg_t {
  int event;
  struct { uint8_t address; } new_dev{};
  struct { usb_device_handle_t dev_hdl; } dev_gone{};
};
struct usb_host_client_config_t {
  bool is_synchronous;
  int max_num_event_msg;
  struct {
    void (*client_event_callback)(const usb_host_client_event_msg_t *, void *);
    void *callback_arg;
  } async;
};
struct usb_host_config_t { bool skip_phy_setup; int intr_flags; };
struct usb_host_lib_info_t { int num_devices = 0; };
constexpr int ESP_INTR_FLAG_LEVEL1 = 1;
constexpr uint32_t USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS = 1, USB_HOST_LIB_EVENT_FLAGS_ALL_FREE = 2;
esp_err_t usb_host_install(const usb_host_config_t *);
esp_err_t usb_host_uninstall();
esp_err_t usb_host_lib_handle_events(uint32_t, uint32_t *);
esp_err_t usb_host_lib_set_root_port_power(bool);
esp_err_t usb_host_lib_info(usb_host_lib_info_t *);
esp_err_t usb_host_client_register(const usb_host_client_config_t *, usb_host_client_handle_t *);
esp_err_t usb_host_client_deregister(usb_host_client_handle_t);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, uint32_t);
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t *);
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t);
esp_err_t usb_host_device_info(usb_device_handle_t, usb_device_info_t *);
esp_err_t usb_host_device_addr_list_fill(int, uint8_t *, int *);
esp_err_t usb_host_device_free_all();
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t, const usb_device_desc_t **);
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t **);
esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t);
esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t);
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t, usb_transfer_t *);
const usb_intf_desc_t *usb_parse_interface_descriptor(const usb_config_desc_t *, int, int, int *);
