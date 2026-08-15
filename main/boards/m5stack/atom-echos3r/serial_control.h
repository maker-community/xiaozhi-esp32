#pragma once

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <mutex>
#include <string>

/**
 * @brief Simple newline-terminated UART command console.
 *
 * Runs a dedicated FreeRTOS task that reads whole lines from a UART port and
 * invokes a user-supplied handler for each line. The handler runs in the
 * serial task context, so it must not block on the main/audio tasks; use
 * Application::Schedule()/ToggleChatState() (which are thread-safe) when
 * driving application behavior.
 *
 * A "help" line ("?" or "help") triggers the registered help callback.
 */
class SerialControl {
public:
    /**
     * @param uart_num UART peripheral number (e.g. UART_NUM_1).
     * @param tx_pin   TX GPIO (may be GPIO_NUM_NC to disable TX).
     * @param rx_pin   RX GPIO (may be GPIO_NUM_NC to disable RX).
     * @param baud     Baud rate, e.g. 115200.
     * @param line_buf_size Max command line length.
     */
    SerialControl(uart_port_t uart_num, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud,
                  size_t line_buf_size = 128);
    ~SerialControl();

    SerialControl(const SerialControl&) = delete;
    SerialControl& operator=(const SerialControl&) = delete;

    /// Start the receive task.
    void Start();

    /// Discard any buffered / in-flight input (e.g. garbage received while the
    /// peer was still resetting). Safe to call before/after Start().
    void FlushInput();

    /// Send a single line (adds \r\n). Thread-safe.
    void SendLine(const std::string& line);

    /// Send one command and wait for a response with the requested prefix or ERR.
    bool SendCommand(const std::string& line, const std::string& response_prefix,
                     std::string& response, uint32_t timeout_ms = 1000);

    /// Called once per received command line (without trailing newline).
    void SetLineHandler(std::function<void(const std::string& line)> handler);

    /// Called when the user sends "help" or "?".
    void SetHelpHandler(std::function<void()> handler);

private:
    static void TaskEntry(void* arg);
    void Run();
    void HandleLine(std::string& line);

    uart_port_t uart_num_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    int baud_;
    size_t line_buf_size_;
    TaskHandle_t task_handle_ = nullptr;
    SemaphoreHandle_t tx_mutex_ = nullptr;
    SemaphoreHandle_t command_mutex_ = nullptr;
    SemaphoreHandle_t response_ready_ = nullptr;
    std::mutex response_mutex_;
    bool response_waiting_ = false;
    std::string response_prefix_;
    std::string response_line_;
    std::function<void(const std::string&)> line_handler_;
    std::function<void()> help_handler_;
};
