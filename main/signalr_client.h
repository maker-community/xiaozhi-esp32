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
     * Get the last connection error message
     * Safe to call from any context
     */
    std::string GetLastConnectionError() const;

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

private:
    SignalRClient();
    ~SignalRClient();

    std::unique_ptr<signalr::hub_connection> connection_;
    std::string hub_url_;
    std::string token_;
    bool initialized_ = false;
    std::atomic<bool> connection_lost_{false};
    std::string last_error_;
    std::atomic<bool> connecting_{false};
    std::atomic<bool> reconnect_running_{false};
    TaskHandle_t reconnect_task_{nullptr};
    
    std::function<void(const cJSON*)> on_custom_message_;
    std::function<void(bool, const std::string&)> on_connection_state_changed_;

    // Helper to parse JSON array string to signalr::value vector
    std::vector<signalr::value> ParseJsonArray(const std::string& json_str);

    // Background reconnect loop
    void StartReconnectTask();
    static void ReconnectTaskThunk(void* param);
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
    bool IsConnected() const { return false; }
    std::string GetConnectionState() const { return "disabled"; }
    void OnCustomMessage(std::function<void(const cJSON*)>) {}
    void OnConnectionStateChanged(std::function<void(bool, const std::string&)>) {}
    void InvokeHubMethod(const std::string&, const std::string& = "[]",
                        std::function<void(bool, const std::string&)> = nullptr) {}
    void SendHubMessage(const std::string&, const std::string& = "[]") {}
};

#endif // CONFIG_ENABLE_SIGNALR_CLIENT

#endif // SIGNALR_CLIENT_H
