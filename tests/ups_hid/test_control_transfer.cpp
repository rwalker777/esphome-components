#include "../../components/ups_hid/usb_control_transfer.h"
#include <cassert>
#include <functional>
#include <iostream>

using esphome::ups_hid::usb_control_transfer;
int transfers = 0, semaphores = 0;
std::function<void()> wait_hook;
SemaphoreHandle_t xSemaphoreCreateBinary() {
  ++semaphores;
  return new TestSemaphore;
}
void vSemaphoreDelete(SemaphoreHandle_t sem) {
  --semaphores;
  delete sem;
}
void xSemaphoreGive(SemaphoreHandle_t sem) { sem->signaled = true; }
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t) {
  if (wait_hook) {
    auto hook = std::move(wait_hook);
    wait_hook = nullptr;
    hook();
  }
  return sem->signaled ? pdTRUE : 0;
}
esp_err_t usb_host_transfer_alloc(size_t size, int, usb_transfer_t **transfer) {
  ++transfers;
  *transfer = new usb_transfer_t;
  (*transfer)->data_buffer = new uint8_t[size]{};
  return ESP_OK;
}
esp_err_t usb_host_transfer_free(usb_transfer_t *t) {
  assert(!t->in_flight);
  --transfers;
  delete[] t->data_buffer;
  delete t;
  return ESP_OK;
}
void complete(usb_transfer_t *t, int actual, int status = USB_TRANSFER_STATUS_COMPLETED) {
  t->actual_num_bytes = actual;
  t->status = status;
  t->in_flight = false;
  t->callback(t);
}
int main() {
  auto pending = std::make_shared<std::atomic<bool>>(false);
  const uint8_t payload[] = {0x11, 3};
  usb_setup_packet_t write{0x21, 9, 0x311, 2, 0};
  size_t length = sizeof(payload);
  assert(usb_control_transfer(write, 8, nullptr, payload, &length, 10, pending,
                              [&](usb_transfer_t *t) {
                                auto *setup = reinterpret_cast<usb_setup_packet_t *>(t->data_buffer);
                                assert(setup->wValue == 0x311 && setup->wIndex == 2 && setup->wLength == 2);
                                assert(t->num_bytes == 10 && t->bEndpointAddress == 0);
                                assert(std::memcmp(t->data_buffer + 8, payload, 2) == 0);
                                complete(t, 10);
                                return ESP_OK;
                              }) == ESP_OK &&
         length == 2);
  assert(!*pending && transfers == 0 && semaphores == 0);

  // Reproduce timeout followed by completion after the caller has returned.
  usb_transfer_t *late = nullptr;
  length = 2;
  assert(usb_control_transfer(write, 8, nullptr, payload, &length, 10, pending, [&](usb_transfer_t *t) {
           late = t;
           t->in_flight = true;
           return ESP_OK;
         }) == ESP_ERR_TIMEOUT);
  assert(*pending && transfers == 1 && semaphores == 1 && length == 0);
  length = 2;
  assert(usb_control_transfer(write, 8, nullptr, payload, &length, 10, pending, [](usb_transfer_t *) {
           assert(false);
           return ESP_FAIL;
         }) == ESP_ERR_INVALID_STATE);
  complete(late, 10);
  assert(!*pending && transfers == 0 && semaphores == 0);

  // A new request can start after completion wakes the old waiter, before its
  // context is destroyed. The old destructor must not release the new gate.
  length = 2;
  assert(usb_control_transfer(write, 8, nullptr, payload, &length, 10, pending, [&](usb_transfer_t *t) {
           complete(t, 10);
           wait_hook = [&] {
             size_t next_length = 2;
             assert(
                 usb_control_transfer(write, 8, nullptr, payload, &next_length, 10, pending, [&](usb_transfer_t *next) {
                   late = next;
                   next->in_flight = true;
                   return ESP_OK;
                 }) == ESP_ERR_TIMEOUT);
           };
           return ESP_OK;
         }) == ESP_OK);
  assert(*pending && transfers == 1);
  complete(late, 10);

  uint8_t input[3]{};
  usb_setup_packet_t read{0xa1, 1, 0x332, 2, 0};
  length = 3;
  assert(usb_control_transfer(read, 8, input, nullptr, &length, 10, pending,
                              [](usb_transfer_t *t) {
                                assert(t->num_bytes == 16);  // IN storage padded to MPS, request length stays 3.
                                auto *setup = reinterpret_cast<usb_setup_packet_t *>(t->data_buffer);
                                assert(setup->wLength == 3);
                                t->data_buffer[8] = 0x32;
                                t->data_buffer[9] = 7;
                                t->data_buffer[10] = 9;
                                complete(t, 11);
                                return ESP_OK;
                              }) == ESP_OK &&
         length == 3 && input[0] == 0x32 && input[2] == 9);
  for (int failure : {0, 1, 2}) {
    length = 2;
    auto ret = usb_control_transfer(write, 8, nullptr, payload, &length, 10, pending, [failure](usb_transfer_t *t) {
      if (failure == 0) {
        return ESP_FAIL;  // Submit rejected: no callback.
      }
      complete(t, failure == 1 ? 9 : 10, failure == 2 ? 1 : 0);
      return ESP_OK;
    });
    assert(ret != ESP_OK && !*pending && transfers == 0 && semaphores == 0);
  }
  std::cout << "USB control transfer ownership checks passed\n";
}
