#include "transport_esp32.h"
#include "hid_power_device.h"
#ifdef USE_ESP32
#include "usb_control_transfer.h"
#endif
#include "constants_ups.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#ifdef USE_ESP32

namespace esphome {
namespace ups_hid {

static const char *const ESP32_USB_TAG = "ups_hid.esp32_usb";

// USB HID Class defines
#ifndef USB_CLASS_HID
#define USB_CLASS_HID 0x03
#endif

Esp32UsbTransport::Esp32UsbTransport() {
    memset(&device_, 0, sizeof(device_));
}

Esp32UsbTransport::~Esp32UsbTransport() {
    deinitialize();
}

esp_err_t Esp32UsbTransport::initialize() {
    std::lock_guard<std::mutex> lock(device_mutex_);
    
    if (initialized_.load()) {
        return ESP_OK;
    }
    
    ESP_LOGI(ESP32_USB_TAG, "Initializing ESP32 USB transport");
    
    esp_err_t ret = setup_usb_host();
    if (ret != ESP_OK) {
        set_last_error("Failed to setup USB host: " + std::string(esp_err_to_name(ret)));
        return ret;
    }
    
    // Register USB client for device events - connection will be asynchronous  
    ret = find_and_open_device();
    if (ret != ESP_OK) {
        teardown_usb_host();
        return ret;
    }
    
    initialized_ = true;
    
    ESP_LOGI(ESP32_USB_TAG, "ESP32 USB transport initialized - waiting for USB device connection events");
    
    return ESP_OK;
}

esp_err_t Esp32UsbTransport::deinitialize() {
    std::lock_guard<std::mutex> lock(device_mutex_);
    
    if (!initialized_.load()) {
        return ESP_OK;
    }
    
    ESP_LOGI(ESP32_USB_TAG, "Deinitializing ESP32 USB transport");
    
    connected_ = false;
    initialized_ = false;
    
    esp_err_t ret = teardown_usb_host();
    if (ret != ESP_OK) {
        ESP_LOGW(ESP32_USB_TAG, "USB teardown had issues: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

bool Esp32UsbTransport::is_connected() const {
    return connected_.load() && initialized_.load() && recovery_state_.load() == RecoveryState::IDLE;
}

esp_err_t Esp32UsbTransport::request_reenumeration() {
    std::lock_guard<std::mutex> lock(device_mutex_);
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    auto state = recovery_state_.load();
    if (state == RecoveryState::IDLE || state == RecoveryState::RETRY_OFF) {
        recovery_state_ = RecoveryState::REQUESTED;
    } else if (state == RecoveryState::RETRY_ON) {
        recovery_state_ = RecoveryState::PORT_OFF;
    } else {
        return ESP_ERR_INVALID_STATE;
    }
    connected_ = false;
    ++connection_id_;  // Reject controls bound to the old descriptor immediately.
    return ESP_OK;
}

void Esp32UsbTransport::process_reenumeration() {
    // Run after library events drain, with both USB event tasks alive. Port
    // disconnect flushes even a transfer whose caller timed out; its callback
    // still owns the buffer. Never emulate completion or clear its pending gate.
    auto state = recovery_state_.load();
    if (state == RecoveryState::IDLE || state == RecoveryState::RETRY_OFF || state == RecoveryState::RETRY_ON) {
        return;
    }
    uint32_t now = millis();
    if (now - recovery_step_at_ < PORT_RECOVERY_DELAY_MS) {
        return;
    }
    recovery_step_at_ = now;
    if (recovery_state_ == RecoveryState::REQUESTED) {
        esp_err_t ret = usb_host_lib_set_root_port_power(false);
        if (ret == ESP_OK) {
            recovery_state_ = RecoveryState::PORT_OFF;
            ESP_LOGW(ESP32_USB_TAG, "USB root port disabled; waiting for transfer/device cleanup");
        } else {
            recovery_state_ = RecoveryState::RETRY_OFF;
            ESP_LOGW(ESP32_USB_TAG, "Could not disable USB root port: %s", esp_err_to_name(ret));
        }
        return;
    }
    // An open handle retains a device reference. The library handler drains
    // device-free and root-port recovery work before returning to this task.
    usb_host_lib_info_t info{};
    if (usb_host_lib_info(&info) != ESP_OK || info.num_devices != 0) {
        return;
    }
    esp_err_t ret = usb_host_lib_set_root_port_power(true);
    if (ret == ESP_OK) {
        recovery_state_ = RecoveryState::IDLE;
        ESP_LOGI(ESP32_USB_TAG, "USB root port enabled; waiting for fresh enumeration");
    } else {
        recovery_state_ = RecoveryState::RETRY_ON;
        ESP_LOGW(ESP32_USB_TAG, "Could not enable USB root port: %s", esp_err_to_name(ret));
    }
}

uint16_t Esp32UsbTransport::get_vendor_id() const {
    std::lock_guard<std::mutex> lock(device_mutex_);
    return device_.vendor_id;
}

uint16_t Esp32UsbTransport::get_product_id() const {
    std::lock_guard<std::mutex> lock(device_mutex_);
    return device_.product_id;
}

esp_err_t Esp32UsbTransport::control_transfer(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                                              uint8_t *input, size_t *length, uint32_t timeout_ms,
                                              const uint8_t *output, uint32_t expected_connection) {
    size_t packet_size;
    uint32_t connection;
    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        if (!is_connected() || !device_.dev_hdl || (expected_connection && connection_id_ != expected_connection)) {
            return ESP_ERR_INVALID_STATE;
        }
        const usb_device_desc_t *descriptor = nullptr;
        esp_err_t ret = usb_host_get_device_descriptor(device_.dev_hdl, &descriptor);
        if (ret != ESP_OK) {
            return ret;
        }
        connection = connection_id_;
        packet_size = descriptor->bMaxPacketSize0;
    }
    usb_setup_packet_t setup{};
    setup.bmRequestType = request_type;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    return usb_control_transfer(setup, packet_size, input, output, length, timeout_ms, control_pending_,
                                [this, connection](usb_transfer_t *transfer) {
                                    std::lock_guard<std::mutex> lock(device_mutex_);
                                    if (!is_connected() || !device_.dev_hdl || connection_id_ != connection) {
                                        return ESP_ERR_INVALID_STATE;
                                    }
                                    transfer->device_handle = device_.dev_hdl;
                                    return usb_host_transfer_submit_control(device_.client_hdl, transfer);
                                });
}

esp_err_t Esp32UsbTransport::hid_get_report(uint8_t report_type, uint8_t report_id, uint8_t *data, size_t *data_len,
                                            uint32_t timeout_ms) {
    if (!data_len || *data_len > hid_power::MAX_REPORT_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t interface_number;
    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        interface_number = device_.interface_num;
    }
    esp_err_t ret = control_transfer(0xa1, 0x01, (uint16_t(report_type) << 8) | report_id, interface_number, data,
                                     data_len, timeout_ms);
    if (ret == ESP_OK) {
        ESP_LOGV(ESP32_USB_TAG, "GET_REPORT type=%u ID=0x%02X length=%zu", report_type, report_id, *data_len);
        ESP_LOGVV(ESP32_USB_TAG, "Raw report: %s", format_hex_pretty(data, *data_len).c_str());
    }
    return ret;
}

esp_err_t Esp32UsbTransport::get_hid_report_descriptor(std::vector<uint8_t> &descriptor, uint32_t timeout_ms) {
    descriptor.clear();
    size_t length;
    uint8_t interface_number;
    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        if (!connected_ || !device_.dev_hdl) {
            return ESP_ERR_INVALID_STATE;
        }
        const usb_config_desc_t *config = nullptr;
        esp_err_t ret = usb_host_get_active_config_descriptor(device_.dev_hdl, &config);
        if (ret != ESP_OK) {
            return ret;
        }
        interface_number = device_.interface_num;
        length = hid_power::report_descriptor_length(reinterpret_cast<const uint8_t *>(config), config->wTotalLength,
                                                     interface_number);
    }
    if (!length) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    descriptor.resize(length);
    size_t actual = length;
    // Standard interface GET_DESCRIPTOR(Report), not class GET_REPORT.
    esp_err_t ret = control_transfer(0x81, USB_B_REQUEST_GET_DESCRIPTOR, 0x2200, interface_number, descriptor.data(),
                                     &actual, timeout_ms);
    if (ret != ESP_OK || actual != length) {
        descriptor.clear();
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t Esp32UsbTransport::hid_set_report(uint8_t report_type, uint8_t report_id, const uint8_t *data,
                                            size_t data_len, uint32_t timeout_ms, uint32_t expected_connection) {
    if (!data || !data_len || data_len > hid_power::MAX_REPORT_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t interface_number;
    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        interface_number = device_.interface_num;
    }
    size_t actual = data_len;
    ESP_LOGVV(ESP32_USB_TAG, "SET_REPORT type=%u ID=0x%02X payload=%s", report_type, report_id,
              format_hex_pretty(data, data_len).c_str());
    esp_err_t ret = control_transfer(0x21, 0x09, (uint16_t(report_type) << 8) | report_id, interface_number, nullptr,
                                     &actual, timeout_ms, data, expected_connection);
    ESP_LOGD(ESP32_USB_TAG, "SET_REPORT type=%u ID=0x%02X result=%s", report_type, report_id, esp_err_to_name(ret));
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(ESP32_USB_TAG, "SET_REPORT ID=0x%02X timed out; command outcome unknown, not retried", report_id);
    }
    return ret;
}

esp_err_t Esp32UsbTransport::get_string_descriptor(uint8_t string_index, 
                                                 std::string& result) {
    result.clear();
    // The descriptor length is one byte. Reuse the same ownership and pending
    // gate as HID reads so recovery also waits for legacy-protocol string reads.
    uint8_t bytes[255];
    size_t length = sizeof(bytes);
    const uint16_t language_id = 0x0409; // English US
    const uint8_t bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | 
                                 USB_BM_REQUEST_TYPE_TYPE_STANDARD | 
                                 USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    const uint16_t wValue = (USB_B_DESCRIPTOR_TYPE_STRING << 8) | string_index;
    esp_err_t ret = control_transfer(bmRequestType, USB_B_REQUEST_GET_DESCRIPTOR, wValue, language_id,
                                    bytes, &length, timing::USB_CONTROL_TRANSFER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    if (length < 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (bytes[1] != USB_B_DESCRIPTOR_TYPE_STRING || bytes[0] < 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t end = std::min(size_t(bytes[0]), length);
    for (size_t i = 2; i + 1 < end; i += 2) {
        uint16_t character = bytes[i] | (uint16_t(bytes[i + 1]) << 8);
        if (character > 0 && character < 128) {
            result += static_cast<char>(character);
        } else if (character >= 128) {
            result += '?';
        }
    }
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
        result.pop_back();
    }
    ESP_LOGI(ESP32_USB_TAG, "USB string descriptor %d: \"%s\"", string_index, result.c_str());
    return ESP_OK;
}

std::string Esp32UsbTransport::get_last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

// Private methods implementation

void Esp32UsbTransport::set_last_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
    ESP_LOGW(ESP32_USB_TAG, "%s", error.c_str());
}

esp_err_t Esp32UsbTransport::setup_usb_host() {
    // Create USB library task - USB Host installation happens inside the task
    if (!usb_tasks_running_.load()) {
        usb_tasks_running_ = true;
        
        // Create USB Host Library task first
        BaseType_t task_created = xTaskCreate(usb_lib_task, "usb_lib_task", 4096, this, 2, &usb_lib_task_handle_);
        if (task_created != pdTRUE) {
            ESP_LOGE(ESP32_USB_TAG, "Failed to create USB Host Library task");
            usb_tasks_running_ = false;
            return ESP_FAIL;
        }
        
        // Give USB Host Library task time to initialize
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Create USB client task
        task_created = xTaskCreate(usb_client_task, "usb_client_task", 6144, this, 3, &usb_client_task_handle_);
        if (task_created != pdTRUE) {
            ESP_LOGE(ESP32_USB_TAG, "Failed to create USB client task");
            usb_tasks_running_ = false;
            return ESP_FAIL;
        }
        
        ESP_LOGI(ESP32_USB_TAG, "USB Host tasks created successfully");
    }
    
    return ESP_OK;
}

esp_err_t Esp32UsbTransport::teardown_usb_host() {
    // Signal tasks to stop (they will self-terminate and handle USB cleanup)
    if (usb_tasks_running_.load()) {
        ESP_LOGI(ESP32_USB_TAG, "Stopping USB Host tasks...");
        usb_tasks_running_ = false;
        
        // Wait for tasks to self-terminate
        vTaskDelay(pdMS_TO_TICKS(500)); // Give tasks time to exit cleanly
        
        usb_client_task_handle_ = nullptr;
        usb_lib_task_handle_ = nullptr;
        
        ESP_LOGI(ESP32_USB_TAG, "USB Host tasks stopped");
    }
    
    // Release interface and device
    if (device_.dev_hdl) {
        ESP_LOGI(ESP32_USB_TAG, "Cleaning up device resources");
        usb_host_interface_release(device_.client_hdl, device_.dev_hdl, device_.interface_num);
        usb_host_device_close(device_.client_hdl, device_.dev_hdl);
        device_.dev_hdl = nullptr;
    }
    
    if (device_.client_hdl) {
        ESP_LOGI(ESP32_USB_TAG, "Deregistering USB client");
        usb_host_client_deregister(device_.client_hdl);
        device_.client_hdl = nullptr;
    }
    
    // USB Host uninstallation happens inside usb_lib_task now
    return ESP_OK;
}

esp_err_t Esp32UsbTransport::find_and_open_device() {
    // Register USB client in asynchronous mode for event-driven device detection
    usb_host_client_config_t client_config = {
        .is_synchronous = false,  // Use asynchronous mode for USB_HOST_CLIENT_EVENT_NEW_DEV events
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = usb_client_event_callback,
            .callback_arg = this
        }
    };
    
    esp_err_t ret = usb_host_client_register(&client_config, &device_.client_hdl);
    if (ret != ESP_OK) {
        set_last_error("Client register failed: " + std::string(esp_err_to_name(ret)));
        return ret;
    }
    
    ESP_LOGI(ESP32_USB_TAG, "USB client registered (handle=0x%p), waiting for device connection events...", 
             device_.client_hdl);
    
    // Force immediate device enumeration check in addition to event-driven detection
    ESP_LOGI(ESP32_USB_TAG, "Performing immediate device enumeration check...");
    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay for USB stack to stabilize
    
    int num_dev = 10;
    uint8_t dev_addr_list[10];
    esp_err_t enum_ret = usb_host_device_addr_list_fill(num_dev, dev_addr_list, &num_dev);
    if (enum_ret == ESP_OK && num_dev > 0) {
        ESP_LOGI(ESP32_USB_TAG, "Found %d existing USB devices during initial enumeration:", num_dev);
        for (int i = 0; i < num_dev; i++) {
            ESP_LOGI(ESP32_USB_TAG, "  Attempting to handle existing device at address %d", dev_addr_list[i]);
            handle_new_device(dev_addr_list[i]);
        }
    } else {
        ESP_LOGI(ESP32_USB_TAG, "No existing USB devices found - waiting for connection events");
    }
    
    return ESP_OK;
}

esp_err_t Esp32UsbTransport::claim_interface() {
    const usb_config_desc_t *config_desc;
    esp_err_t ret = usb_host_get_active_config_descriptor(device_.dev_hdl, &config_desc);
    if (ret != ESP_OK) {
        set_last_error("Failed to get config descriptor");
        return ret;
    }
    
    // Find HID interface
    const usb_intf_desc_t *intf_desc = nullptr;
    int offset = 0;
    
    for (int i = 0; i < config_desc->bNumInterfaces; i++) {
        intf_desc = usb_parse_interface_descriptor(config_desc, i, 0, &offset);
        if (intf_desc && intf_desc->bInterfaceClass == USB_CLASS_HID) {
            device_.interface_num = intf_desc->bInterfaceNumber;
            break;
        }
    }
    
    if (!intf_desc || intf_desc->bInterfaceClass != USB_CLASS_HID) {
        set_last_error("No HID interface found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ret = usb_host_interface_claim(device_.client_hdl, device_.dev_hdl, 
                                  device_.interface_num, 0);
    if (ret != ESP_OK) {
        set_last_error("Failed to claim interface: " + std::string(esp_err_to_name(ret)));
        return ret;
    }
    interface_claimed_ = true;
    
    return ESP_OK;
}

void Esp32UsbTransport::usb_client_event_callback(const usb_host_client_event_msg_t* event_msg, void* arg) {
    Esp32UsbTransport* transport = static_cast<Esp32UsbTransport*>(arg);
    
    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            ESP_LOGI(ESP32_USB_TAG, "New USB device detected: address=%d", event_msg->new_dev.address);
            transport->handle_new_device(event_msg->new_dev.address);
            break;
            
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            ESP_LOGI(ESP32_USB_TAG, "USB device disconnected");
            transport->handle_device_gone(event_msg->dev_gone.dev_hdl);
            break;
            
        default:
            ESP_LOGW(ESP32_USB_TAG, "Unhandled USB client event: %d", event_msg->event);
            break;
    }
}

void Esp32UsbTransport::handle_new_device(uint8_t dev_addr) {
    ESP_LOGI(ESP32_USB_TAG, "Handling new USB device at address %d", dev_addr);
    
    std::lock_guard<std::mutex> lock(device_mutex_);

    if (recovery_state_ != RecoveryState::IDLE) {
        return;  // A queued pre-reset NEW_DEV must not resurrect the old session.
    }
    
    // Check if we already have a device connected
    if (device_.dev_hdl != nullptr) {
        ESP_LOGW(ESP32_USB_TAG, "Device already connected - skipping new device at address %d", dev_addr);
        return;
    }
    
    // Open the device
    esp_err_t ret = usb_host_device_open(device_.client_hdl, dev_addr, &device_.dev_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(ESP32_USB_TAG, "Failed to open device at address %d: %s", dev_addr, esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(ESP32_USB_TAG, "Successfully opened USB device at address %d", dev_addr);
    
    // Get device information
    usb_device_info_t dev_info;
    ret = usb_host_device_info(device_.dev_hdl, &dev_info);
    if (ret != ESP_OK) {
        ESP_LOGE(ESP32_USB_TAG, "Failed to get device info: %s", esp_err_to_name(ret));
        usb_host_device_close(device_.client_hdl, device_.dev_hdl);
        device_.dev_hdl = nullptr;
        return;
    }
    
    device_.address = dev_addr;
    device_.speed = dev_info.speed;
    
    // Get device descriptor to extract VID/PID
    const usb_device_desc_t* device_desc;
    ret = usb_host_get_device_descriptor(device_.dev_hdl, &device_desc);
    if (ret == ESP_OK) {
        device_.vendor_id = device_desc->idVendor;
        device_.product_id = device_desc->idProduct;
        
        ESP_LOGI(ESP32_USB_TAG, "USB device opened: VID=0x%04X, PID=0x%04X, Speed=%d", 
                 device_.vendor_id, device_.product_id, dev_info.speed);
        
        // Check if this is a UPS device (HID class)
        if (device_desc->bDeviceClass == USB_CLASS_HID || 
            device_desc->bDeviceClass == 0x00) { // Device class defined at interface level
            
            // HID reports and descriptors use the default control endpoint.
            ret = claim_interface();
            if (ret == ESP_OK) {
                ++connection_id_;
                connected_ = true;
                ESP_LOGI(ESP32_USB_TAG, "UPS device successfully configured and ready");
                return;
            }
        } else {
            ESP_LOGW(ESP32_USB_TAG, "Connected device is not a HID device (class=0x%02X)", device_desc->bDeviceClass);
        }
    }
    
    // Clean up on failure
    usb_host_device_close(device_.client_hdl, device_.dev_hdl);
    device_.dev_hdl = nullptr;
}

void Esp32UsbTransport::handle_device_gone(usb_device_handle_t dev_hdl) {
    ESP_LOGI(ESP32_USB_TAG, "Handling USB device disconnection");
    
    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        if (device_.dev_hdl == dev_hdl) {
            connected_ = false;
            device_gone_ = true;
        }
    }
    // Close now when possible so a NEW_DEV in the same event batch can open.
    // A still-pending transfer defers cleanup to the next client-task iteration.
    close_disconnected_device();
}

void Esp32UsbTransport::close_disconnected_device() {
    std::lock_guard<std::mutex> lock(device_mutex_);
    if (!device_gone_ || control_pending_->load()) {
        return;
    }
    if (interface_claimed_) {
        esp_err_t ret = usb_host_interface_release(device_.client_hdl, device_.dev_hdl, device_.interface_num);
        if (ret != ESP_OK) {
            return;
        }
        interface_claimed_ = false;
    }
    if (usb_host_device_close(device_.client_hdl, device_.dev_hdl) != ESP_OK) {
        return;
    }
    auto client = device_.client_hdl;
    device_ = UsbDevice{};
    device_.client_hdl = client;
    device_gone_ = false;
    ESP_LOGI(ESP32_USB_TAG, "USB device disconnected and cleaned up");
}

void Esp32UsbTransport::usb_lib_task(void* arg) {
    Esp32UsbTransport* transport = static_cast<Esp32UsbTransport*>(arg);
    
    ESP_LOGI(ESP32_USB_TAG, "USB Host Library task starting...");
    
    // Initialize USB Host library inside task (following working prototype)
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1
    };

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(ESP32_USB_TAG, "USB Host install failed: %s", esp_err_to_name(ret));
        transport->usb_tasks_running_ = false; // Stop other tasks
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(ESP32_USB_TAG, "USB Host library installed successfully");
    
    // Main USB Host event loop - following working prototype pattern
    bool has_clients = true;
    bool has_devices = false;
    
    while (has_clients && transport->usb_tasks_running_.load()) {
        uint32_t event_flags = 0;
        ret = usb_host_lib_handle_events(pdMS_TO_TICKS(PORT_RECOVERY_DELAY_MS), &event_flags);
        
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGE(ESP32_USB_TAG, "USB Host event handling failed: %s", esp_err_to_name(ret));
            continue;
        }
        transport->process_reenumeration();

        // Handle USB Host events (following ESP-IDF v5.4 patterns)
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(ESP32_USB_TAG, "No more USB clients");
            if (usb_host_device_free_all() == ESP_OK) {
                ESP_LOGI(ESP32_USB_TAG, "All devices freed");
                has_clients = false;
            } else {
                ESP_LOGI(ESP32_USB_TAG, "Waiting for devices to be freed");
                has_devices = true;
            }
        }
        
        if (has_devices && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) {
            ESP_LOGI(ESP32_USB_TAG, "All devices freed");
            has_clients = false;
        }
    }
    
    // Cleanup USB Host library
    ESP_LOGI(ESP32_USB_TAG, "Uninstalling USB Host library");
    usb_host_uninstall();
    
    ESP_LOGI(ESP32_USB_TAG, "USB Host Library task ending");
    vTaskDelete(nullptr);
}

void Esp32UsbTransport::usb_client_task(void* arg) {
    Esp32UsbTransport* transport = static_cast<Esp32UsbTransport*>(arg);
    
    ESP_LOGI(ESP32_USB_TAG, "USB client task started");
    
    while (transport->usb_tasks_running_.load()) {
        if (transport->device_.client_hdl) {
            // Use shorter timeout for more responsive event processing
            esp_err_t ret = usb_host_client_handle_events(transport->device_.client_hdl, timing::USB_CLIENT_EVENT_TIMEOUT_MS);
            
            if (ret == ESP_ERR_TIMEOUT) {
                // Timeout is normal - continue processing
            } else if (ret != ESP_OK) {
                ESP_LOGW(ESP32_USB_TAG, "USB client event handling failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(100)); // Brief delay on error
            }
        } else {
            ESP_LOGD(ESP32_USB_TAG, "No USB client handle available");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        transport->close_disconnected_device();
        
        // Small delay to prevent busy-waiting
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    ESP_LOGI(ESP32_USB_TAG, "USB client task stopping");
    vTaskDelete(nullptr);
}

} // namespace ups_hid
} // namespace esphome

#endif  // USE_ESP32
