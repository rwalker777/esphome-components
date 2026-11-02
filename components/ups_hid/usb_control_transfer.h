#pragma once

#include "hid_power_device.h"
#include "esphome/core/log.h"
#include "esp_err.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include <memory>
#include <new>
#include <cstring>

namespace esphome {
namespace ups_hid {

// Submit supplies a connection-checked device handle and submits the transfer.
// Both caller and completion callback own the resources. A timeout is not USB
// cancellation: no resource is freed, and no new request starts, until completion.
template <typename Submit>
esp_err_t usb_control_transfer(usb_setup_packet_t setup, size_t packet_size, uint8_t *input, const uint8_t *output,
                               size_t *length, uint32_t timeout_ms, const std::shared_ptr<std::atomic<bool>> &pending,
                               Submit submit) {
  bool reading = (setup.bmRequestType & 0x80) != 0;
  if (!length || !*length || *length > hid_power::MAX_DESCRIPTOR_SIZE ||
      (reading ? (!input || output) : (!output || input))) {
    return ESP_ERR_INVALID_ARG;
  }
  size_t requested = *length;
  *length = 0;
  if (packet_size != 8 && packet_size != 16 && packet_size != 32 && packet_size != 64) {
    return ESP_ERR_INVALID_SIZE;
  }
  size_t payload_size = reading ? ((requested + packet_size - 1) / packet_size) * packet_size : requested;
  bool expected = false;
  if (!pending->compare_exchange_strong(expected, true)) {
    return ESP_ERR_INVALID_STATE;
  }

  struct Context {
    usb_transfer_t *transfer{nullptr};
    SemaphoreHandle_t done{nullptr};
    std::shared_ptr<std::atomic<bool>> pending;
    esp_err_t result{ESP_FAIL};
    size_t actual{0};
    bool pending_released{false};
    ~Context() {
      if (done) {
        vSemaphoreDelete(done);
      }
      if (transfer) {
        usb_host_transfer_free(transfer);
      }
      if (!pending_released) {
        pending->store(false);
      }
    }
  };
  auto context = std::make_shared<Context>();
  context->pending = pending;
  context->done = xSemaphoreCreateBinary();
  if (!context->done) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t ret = usb_host_transfer_alloc(sizeof(setup) + payload_size, 0, &context->transfer);
  if (ret != ESP_OK) {
    return ret;
  }
  auto *transfer = context->transfer;
  setup.wLength = requested;
  std::memcpy(transfer->data_buffer, &setup, sizeof(setup));
  if (!reading) {
    std::memcpy(transfer->data_buffer + sizeof(setup), output, requested);
  }
  transfer->bEndpointAddress = 0;
  transfer->num_bytes = sizeof(setup) + payload_size;
  transfer->timeout_ms = timeout_ms;
  auto *callback_owner = new (std::nothrow) std::shared_ptr<Context>(context);
  if (!callback_owner) {
    return ESP_ERR_NO_MEM;
  }
  transfer->context = callback_owner;
  transfer->callback = [](usb_transfer_t *t) {
    std::unique_ptr<std::shared_ptr<Context>> owner(static_cast<std::shared_ptr<Context> *>(t->context));
    auto ctx = *owner;
    ctx->result = t->status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
    ctx->actual = t->actual_num_bytes;
    if (ctx->result != ESP_OK) {
      const auto *request = reinterpret_cast<const usb_setup_packet_t *>(t->data_buffer);
      ESP_LOGW("ups_hid.esp32_usb", "USB completion request=0x%02X value=0x%04X status=%u actual=%d",
               request->bRequest, request->wValue, unsigned(t->status), t->actual_num_bytes);
    }
    ctx->pending_released = true;
    ctx->pending->store(false);
    xSemaphoreGive(ctx->done);
  };
  ret = submit(transfer);
  if (ret != ESP_OK) {
    delete callback_owner;  // A failed submission must not invoke the callback.
    return ret;
  }
  if (xSemaphoreTake(context->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    ESP_LOGW("ups_hid.esp32_usb", "USB request=0x%02X value=0x%04X timed out after %lu ms; awaiting completion",
             setup.bRequest, setup.wValue, static_cast<unsigned long>(timeout_ms));
    return ESP_ERR_TIMEOUT;
  }
  if (context->result != ESP_OK) {
    return context->result;
  }
  if (context->actual < sizeof(setup) || context->actual - sizeof(setup) > requested ||
      (!reading && context->actual != sizeof(setup) + requested)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *length = context->actual - sizeof(setup);
  if (reading) {
    std::memcpy(input, transfer->data_buffer + sizeof(setup), *length);
  }
  return ESP_OK;
}

}  // namespace ups_hid
}  // namespace esphome
