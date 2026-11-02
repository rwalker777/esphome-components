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
