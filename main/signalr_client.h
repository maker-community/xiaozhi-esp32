#ifndef SIGNALR_CLIENT_H
#define SIGNALR_CLIENT_H

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef CONFIG_ENABLE_SIGNALR_CLIENT

// Disable unknown pragma warnings for SignalR headers (MSVC specific pragmas)
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif

#include "hub_connection.h"
#include "hub_connection_builder.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

class SignalRClient {
public:
    static SignalRClient& GetInstance();

    // Delete copy constructor and assignment operator
    SignalRClient(const SignalRClient&) = delete;
    SignalRClient& operator=(const SignalRClient&) = delete;

    /**
     * Initialize the SignalR client with hub URL and optional token
     * @param hub_url The SignalR hub endpoint URL
     * @param token Optional bearer token for authentication
     * @return true if initialization successful
     */
    bool Initialize(const std::string& hub_url, const std::string& token = "");

    /**
     * Connect to the SignalR hub
     * @return true if connection started successfully (check callback for result)
     */
    bool Connect();

    /**
     * Disconnect from the SignalR hub
     */
    void Disconnect();

    /**
     * Reset the SignalR client state (disconnect and clear all stored tokens/URLs)
     * After calling this, Initialize() can be called again with new credentials
     */
    void Reset();

    /**
     * Reconnect to the SignalR hub (if previously initialized)
     * @return true if reconnection started successfully
     */
    bool Reconnect();

    /**
     * Check if the client has been initialized
     */
    bool IsInitialized() const;

    /**
     * Check if connected to the hub
     */
    bool IsConnected() const;

    /**
     * Get the connection state
     */
    std::string GetConnectionState() const;

    /**
     * Register a handler for custom messages from the server
     * Handler receives parsed JSON payload
     */
    void OnCustomMessage(std::function<void(const cJSON*)> callback);

    /**
     * Register a handler for connection state changes
     * Note: Callback should NOT call complex operations like Schedule()
     */
    void OnConnectionStateChanged(std::function<void(bool connected, const std::string& error)> callback);

    /**
     * Invoke a hub method with arguments
     * @param method_name The hub method name to invoke
     * @param args JSON array string of arguments (e.g., "[\"arg1\", 123]")
     * @param callback Optional callback for result
     */
    void InvokeHubMethod(const std::string& method_name, const std::string& args_json = "[]",
                        std::function<void(bool success, const std::string& result)> callback = nullptr);

    /**
     * Send a message to the hub (fire and forget)
     * @param method_name The hub method name
     * @param args JSON array string of arguments
     */
    void SendHubMessage(const std::string& method_name, const std::string& args_json = "[]");

    /**
     * Check if connection attempt is currently in progress
     */
    bool IsConnecting() const;

    /**
     * Attempt reconnection if not connected
     * Safe to call repeatedly - will skip if already connected or connecting
     * NOTE: This now runs in a background task and returns immediately (non-blocking)
     */
    void PerformReconnect();

    /**
     * Request a reconnection attempt via background task
     * This is completely non-blocking and safe to call from any context
     */
    void RequestReconnect();

private:
    SignalRClient();
    ~SignalRClient();

    std::unique_ptr<signalr::hub_connection> connection_;
    std::string hub_url_;
    std::string token_;
    bool initialized_ = false;
    bool connection_confirmed_ = false;  // Set true when server sends Notification
    std::atomic<bool> connecting_{false};
    std::atomic<bool> reconnect_requested_{false};  // Flag for async reconnect request
    
    // Background reconnection task (uses PSRAM for stack to save internal RAM)
    TaskHandle_t reconnect_task_handle_ = nullptr;
    StackType_t* reconnect_task_stack_ = nullptr;      // Task stack in PSRAM
    StaticTask_t* reconnect_task_buffer_ = nullptr;    // Task TCB in internal RAM
    std::atomic<bool> reconnect_task_running_{false};
    int64_t last_reconnect_attempt_time_ = 0;  // For backoff timing
    int reconnect_backoff_ms_ = 1000;  // Initial backoff delay
    static constexpr int MAX_RECONNECT_BACKOFF_MS = 30000;  // Max 30 seconds
    static constexpr int MIN_RECONNECT_INTERVAL_MS = 5000;  // Minimum 5 seconds between attempts
    static constexpr size_t RECONNECT_TASK_STACK_SIZE = 8192;  // 8KB stack in PSRAM (Connect() needs more stack)
    
    static void ReconnectTaskEntry(void* arg);
    void ReconnectTaskLoop();
    
    std::function<void(const cJSON*)> on_custom_message_;
    std::function<void(bool, const std::string&)> on_connection_state_changed_;

    // Helper to parse JSON array string to signalr::value vector
    std::vector<signalr::value> ParseJsonArray(const std::string& json_str);
};

#else // !CONFIG_ENABLE_SIGNALR_CLIENT

// Stub implementation when SignalR is disabled
class SignalRClient {
public:
    static SignalRClient& GetInstance() {
        static SignalRClient instance;
        return instance;
    }
    bool Initialize(const std::string&, const std::string& = "") { return false; }
    bool Connect() { return false; }
    void Disconnect() {}
    void Reset() {}
    bool Reconnect() { return false; }
    bool IsInitialized() const { return false; }
    bool IsConnected() const { return false; }
    bool IsConnecting() const { return false; }
    void PerformReconnect() {}
    void RequestReconnect() {}
    std::string GetConnectionState() const { return "disabled"; }
    void OnCustomMessage(std::function<void(const cJSON*)>) {}
    void OnConnectionStateChanged(std::function<void(bool, const std::string&)>) {}
    void InvokeHubMethod(const std::string&, const std::string& = "[]",
                        std::function<void(bool, const std::string&)> = nullptr) {}
    void SendHubMessage(const std::string&, const std::string& = "[]") {}
};

#endif // CONFIG_ENABLE_SIGNALR_CLIENT

#endif // SIGNALR_CLIENT_H
