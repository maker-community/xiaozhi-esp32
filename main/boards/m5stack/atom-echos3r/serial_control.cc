#include "serial_control.h"

#include <esp_log.h>

#include <cstring>

#define TAG "SerialControl"

SerialControl::SerialControl(uart_port_t uart_num, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud,
                             size_t line_buf_size)
    : uart_num_(uart_num),
      tx_pin_(tx_pin),
      rx_pin_(rx_pin),
      baud_(baud),
      line_buf_size_(line_buf_size) {}

SerialControl::~SerialControl() {
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    if (tx_mutex_ != nullptr) {
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
    }
    uart_driver_delete(uart_num_);
}

void SerialControl::SetLineHandler(std::function<void(const std::string&)> handler) {
    line_handler_ = std::move(handler);
}

void SerialControl::SetHelpHandler(std::function<void()> handler) {
    help_handler_ = std::move(handler);
}

void SerialControl::FlushInput() {
    // Drain whatever is already buffered in the UART RX ring buffer.
    uint8_t tmp[64];
    size_t n = 0;
    while (uart_read_bytes(uart_num_, tmp, sizeof(tmp), 0) > 0) {
        n += sizeof(tmp);
        if (n > 4096) {
            break;
        }
    }
    if (n > 0) {
        ESP_LOGI(TAG, "Flushed %u bytes of stale input", (unsigned)n);
    }
}

void SerialControl::Start() {
    // Install UART driver (RX ping-pong ring buffer, TX ring buffer).
    uart_config_t uart_config = {
        .baud_rate = baud_,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_config));
    ESP_ERROR_CHECK(
        uart_set_pin(uart_num_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(
        uart_driver_install(uart_num_, line_buf_size_ * 2, line_buf_size_ * 2, 0, nullptr, 0));

    tx_mutex_ = xSemaphoreCreateMutex();

    BaseType_t ret = xTaskCreate(TaskEntry, "serial_control", 4096, this, 5, &task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial control task");
    } else {
        ESP_LOGI(TAG, "Serial control started on UART%d @ %d baud", uart_num_, baud_);
    }
}

void SerialControl::TaskEntry(void* arg) { static_cast<SerialControl*>(arg)->Run(); }

void SerialControl::Run() {
    char buf[line_buf_size_];
    size_t used = 0;
    int len;

    while (true) {
        len = uart_read_bytes(uart_num_, buf + used, line_buf_size_ - 1 - used, pdMS_TO_TICKS(100));
        if (len < 0) {
            continue;
        }
        used += len;

        // Process complete lines delimited by '\n' (strip '\r').
        size_t start = 0;
        for (size_t i = 0; i < used; ++i) {
            if (buf[i] == '\n') {
                std::string line(buf + start, i - start);
                // Trim trailing '\r'
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                HandleLine(line);
                start = i + 1;
            }
        }
        // Keep the (possibly partial) remainder.
        if (start > 0 && start < used) {
            memmove(buf, buf + start, used - start);
            used -= start;
        } else if (start == used) {
            used = 0;
        } else if (used >= line_buf_size_ - 1) {
            // Buffer full without newline: drop it.
            used = 0;
        }
    }
}

void SerialControl::HandleLine(std::string& line) {
    if (line.empty()) {
        return;
    }
    // Trim trailing whitespace for convenience.
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    ESP_LOGI(TAG, "cmd: %s", line.c_str());

    if (line == "help" || line == "?") {
        if (help_handler_) {
            help_handler_();
        }
        return;
    }
    if (line_handler_) {
        line_handler_(line);
    }
}

void SerialControl::SendLine(const std::string& line) {
    if (tx_pin_ == GPIO_NUM_NC) {
        return;
    }
    std::string out = line + "\r\n";
    if (tx_mutex_ != nullptr) {
        xSemaphoreTake(tx_mutex_, portMAX_DELAY);
        uart_write_bytes(uart_num_, out.c_str(), out.size());
        xSemaphoreGive(tx_mutex_);
    } else {
        uart_write_bytes(uart_num_, out.c_str(), out.size());
    }
}
