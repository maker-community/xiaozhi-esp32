#include "signalr_client.h"

#ifdef CONFIG_ENABLE_SIGNALR_CLIENT

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <chrono>
#include "esp32_websocket_client.h"
#include "esp32_http_client.h"
#include "signalr_value.h"

#define TAG "SignalRClient"

SignalRClient& SignalRClient::GetInstance() {
    static SignalRClient instance;
    return instance;
}

SignalRClient::SignalRClient() {
}

SignalRClient::~SignalRClient() {
    Disconnect();
}

bool SignalRClient::Initialize(const std::string& hub_url, const std::string& token) {
    if (initialized_) {
        ESP_LOGW(TAG, "SignalR client already initialized");
        return true;
    }

    if (hub_url.empty()) {
        ESP_LOGE(TAG, "Hub URL cannot be empty");
        return false;
    }

    hub_url_ = hub_url;
    token_ = token;

    try {
        // Create hub connection builder
        auto builder = signalr::hub_connection_builder::create(hub_url_);

        // Set WebSocket factory
        builder.with_websocket_factory([token = token_](const signalr::signalr_client_config& config) {
            auto client = std::make_shared<signalr::esp32_websocket_client>(config);
            
            // Add authorization header if token provided
            if (!token.empty()) {
                std::string auth_header = token;
                // Add "Bearer " prefix if not present
                if (auth_header.find("Bearer ") != 0 && auth_header.find("bearer ") != 0) {
                    auth_header = "Bearer " + auth_header;
                }
                // Note: Header setting depends on esp32_websocket_client implementation
                // You may need to modify this based on the actual API
            }
            
            return client;
        });

        // Set HTTP client factory
        builder.with_http_client_factory([token = token_](const signalr::signalr_client_config& config) {
            return std::make_shared<signalr::esp32_http_client>(config);
        });

        // Enable automatic reconnection
        builder.with_automatic_reconnect();

        // Skip negotiation (direct WebSocket connection)
        builder.skip_negotiation(true);

        // Build connection
        connection_ = std::make_unique<signalr::hub_connection>(builder.build());

        // Log memory status before configuration
        ESP_LOGI(TAG, "Free heap after connection build: internal=%lu, PSRAM=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        // Tune timeouts to reduce false disconnects
        // NOTE: Do NOT call enable_auto_reconnect() here - it's already set via builder.with_automatic_reconnect()
        // Calling it twice can cause multiple scheduler instances to be created, consuming ~40KB extra RAM!
        signalr::signalr_client_config cfg;
        cfg.set_server_timeout(std::chrono::seconds(60));     // server expects 60s idle before dropping
        cfg.set_keepalive_interval(std::chrono::seconds(15));  // send ping every 15s
        cfg.set_handshake_timeout(std::chrono::seconds(30));   // generous handshake
        
        // IMPORTANT: Only set timeout parameters, auto-reconnect is already enabled by builder
        // cfg.enable_auto_reconnect(true);    // REMOVED - duplicate causes memory leak!
        // cfg.set_max_reconnect_attempts(-1); // REMOVED - handled by builder
        
        connection_->set_client_config(cfg);

        // Set disconnected callback to detect connection loss
        // Only set atomic flag here to avoid deadlock - actual reconnect happens in main loop
        connection_->set_disconnected([this](std::exception_ptr ex) {
            ESP_LOGW(TAG, "SignalR disconnected callback triggered, auto_reconnect=%d", 
                     auto_reconnect_enabled_.load(std::memory_order_acquire) ? 1 : 0);
            if (auto_reconnect_enabled_.load(std::memory_order_acquire)) {
                needs_reconnect_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "SignalR reconnect flag set");
                if (ex) {
                    try {
                        std::rethrow_exception(ex);
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "SignalR disconnect reason: %s", e.what());
                        last_error_ = e.what();
                    }
                }
            }
        });

        // Register Notification handler to confirm connection (like the example code)
        // Server sends "Notification" when client connects successfully
        connection_->on("Notification", [this](const std::vector<signalr::value>& args) {
            if (args.empty()) {
                return;
            }
            std::string message = args[0].as_string();
            ESP_LOGI(TAG, "🔔 Notification from server: %s", message.c_str());
            
            // Confirm connection is truly established
            if (!connection_confirmed_) {
                connection_confirmed_ = true;
                ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════╗");
                ESP_LOGI(TAG, "║  ✓✓✓ SIGNALR CONNECTION CONFIRMED BY SERVER! ✓✓✓    ║");
                ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════╝");
                ESP_LOGI(TAG, "Memory after connect: internal=%lu, min_free=%lu",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            }
        });

        initialized_ = true;
        ESP_LOGI(TAG, "SignalR client initialized with URL: %s", hub_url_.c_str());
        ESP_LOGI(TAG, "Memory after init: internal=%lu, PSRAM=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return true;

    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to initialize SignalR client: %s", e.what());
        return false;
    }
}

bool SignalRClient::Connect() {
    if (!initialized_) {
        ESP_LOGE(TAG, "SignalR client not initialized");
        return false;
    }

    if (!connection_) {
        ESP_LOGE(TAG, "Connection object is null");
        return false;
    }

    try {
        // Log memory status before connection attempt
        ESP_LOGI(TAG, "Connecting to SignalR hub...");
        ESP_LOGI(TAG, "Memory before connect: internal=%lu, PSRAM=%lu, min_free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        
        connection_lost_.store(false, std::memory_order_release);
        last_error_.clear();
        connecting_.store(true, std::memory_order_release);
        
        // Provide empty callback to satisfy API - keep work minimal to avoid deadlock
        connection_->start([this](std::exception_ptr ex) {
            connecting_.store(false, std::memory_order_release);
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    last_error_ = e.what();
                }
            }
        });
        
        // Wait a moment for connection to establish
        vTaskDelay(pdMS_TO_TICKS(100));
        
        if (IsConnected()) {
            ESP_LOGI(TAG, "Connected to SignalR hub, connection ID: %s", 
                    connection_->get_connection_id().c_str());
        } else {
            ESP_LOGW(TAG, "SignalR connection initiated, state: %s", GetConnectionState().c_str());
        }

        return true;

    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception during connect: %s", e.what());
        connecting_.store(false, std::memory_order_release);
        return false;
    }
}

void SignalRClient::Disconnect() {
    if (!connection_) {
        return;
    }

    try {
        ESP_LOGI(TAG, "Disconnecting from SignalR hub...");
        connecting_.store(false, std::memory_order_release);
        // Provide empty callback to satisfy API - do NOTHING inside to avoid deadlock
        connection_->stop([](std::exception_ptr) {
            // Empty - do not log or call any functions here
        });
        ESP_LOGI(TAG, "Disconnect initiated");
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception during disconnect: %s", e.what());
    }
}

bool SignalRClient::Reconnect() {
    if (!initialized_) {
        ESP_LOGW(TAG, "SignalR client not initialized, cannot reconnect");
        return false;
    }

    // If already connected, skip reconnection
    if (IsConnected()) {
        ESP_LOGI(TAG, "SignalR already connected, skipping reconnect");
        return true;
    }

    // If currently connecting, skip
    if (connecting_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "SignalR connection already in progress");
        return true;
    }

    ESP_LOGI(TAG, "Attempting SignalR reconnection...");
    return Connect();
}

bool SignalRClient::NeedsReconnect() const {
    return needs_reconnect_.load(std::memory_order_acquire);
}

void SignalRClient::PerformReconnect() {
    // NOTE: This method is DEPRECATED. The SignalR library handles reconnection
    // internally in a dedicated FreeRTOS task (see hub_connection_impl.cpp).
    // This method should NOT be called from the main loop as it can block.
    // 
    // If you need to trigger a reconnection, use the library's built-in
    // auto-reconnect feature by calling SetAutoReconnectEnabled(true).
    
    if (!needs_reconnect_.load(std::memory_order_acquire)) {
        return;
    }
    
    // Clear the flag - the library's internal reconnect will handle this
    needs_reconnect_.store(false, std::memory_order_release);
    
    if (!initialized_) {
        ESP_LOGW(TAG, "SignalR not initialized, skipping reconnect");
        return;
    }
    
    if (connecting_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "SignalR already connecting, internal auto-reconnect will handle");
        return;
    }
    
    // Check if already connected (library may have reconnected automatically)
    if (IsConnected()) {
        ESP_LOGI(TAG, "SignalR already connected via internal auto-reconnect");
        return;
    }
    
    // Log for debugging - the library's internal mechanism handles actual reconnection
    ESP_LOGD(TAG, "SignalR needs_reconnect flag was set, library auto-reconnect should handle");
}

void SignalRClient::SetAutoReconnectEnabled(bool enabled) {
    auto_reconnect_enabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        needs_reconnect_.store(false, std::memory_order_release);
    }
}

bool SignalRClient::IsInitialized() const {
    return initialized_;
}

bool SignalRClient::IsConnected() const {
    if (!connection_) {
        return false;
    }
    return connection_->get_connection_state() == signalr::connection_state::connected;
}

std::string SignalRClient::GetConnectionState() const {
    if (!connection_) {
        return "not_initialized";
    }

    switch (connection_->get_connection_state()) {
        case signalr::connection_state::connecting:
            return "connecting";
        case signalr::connection_state::connected:
            return "connected";
        case signalr::connection_state::disconnected:
            return "disconnected";
        default:
            return "unknown";
    }
}

void SignalRClient::OnCustomMessage(std::function<void(const cJSON*)> callback) {
    on_custom_message_ = callback;

    if (!connection_) {
        ESP_LOGW(TAG, "Cannot register handler: connection not initialized");
        return;
    }

    // Register handler for "CustomMessage" hub method
    connection_->on("CustomMessage", [this](const std::vector<signalr::value>& args) {
        if (args.empty()) {
            ESP_LOGW(TAG, "Received empty CustomMessage");
            return;
        }

        try {
            // Parse the first argument as JSON string
            std::string json_str = args[0].as_string();
            ESP_LOGI(TAG, "Received CustomMessage: %s", json_str.c_str());

            auto root = cJSON_Parse(json_str.c_str());
            if (root) {
                if (on_custom_message_) {
                    on_custom_message_(root);
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "Failed to parse CustomMessage JSON");
            }
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "Exception handling CustomMessage: %s", e.what());
        }
    });
}

void SignalRClient::OnConnectionStateChanged(
    std::function<void(bool connected, const std::string& error)> callback) {
    on_connection_state_changed_ = callback;
}

std::string SignalRClient::GetLastConnectionError() const {
    return last_error_;
}

void SignalRClient::InvokeHubMethod(const std::string& method_name, 
                                    const std::string& args_json,
                                    std::function<void(bool success, const std::string& result)> callback) {
    if (!connection_ || !IsConnected()) {
        ESP_LOGE(TAG, "Cannot invoke method: not connected");
        if (callback) {
            callback(false, "Not connected");
        }
        return;
    }

    try {
        auto args = ParseJsonArray(args_json);
        
        connection_->invoke(method_name, args, 
            [callback, method_name](const signalr::value& result, std::exception_ptr ex) {
                if (ex) {
                    try {
                        std::rethrow_exception(ex);
                    } catch (const std::exception& e) {
                        ESP_LOGE(TAG, "Method '%s' failed: %s", method_name.c_str(), e.what());
                        if (callback) {
                            callback(false, e.what());
                        }
                    }
                } else {
                    std::string result_str;
                    try {
                        result_str = result.as_string();
                    } catch (...) {
                        result_str = "success";
                    }
                    ESP_LOGI(TAG, "Method '%s' succeeded: %s", method_name.c_str(), result_str.c_str());
                    if (callback) {
                        callback(true, result_str);
                    }
                }
            });

    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception invoking method '%s': %s", method_name.c_str(), e.what());
        if (callback) {
            callback(false, e.what());
        }
    }
}

void SignalRClient::SendHubMessage(const std::string& method_name, const std::string& args_json) {
    if (!connection_ || !IsConnected()) {
        ESP_LOGE(TAG, "Cannot send message: not connected");
        return;
    }

    try {
        auto args = ParseJsonArray(args_json);
        
        connection_->send(method_name, args, [method_name](std::exception_ptr ex) {
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "Send '%s' failed: %s", method_name.c_str(), e.what());
                }
            } else {
                ESP_LOGI(TAG, "Send '%s' succeeded", method_name.c_str());
            }
        });

    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception sending message '%s': %s", method_name.c_str(), e.what());
    }
}

std::vector<signalr::value> SignalRClient::ParseJsonArray(const std::string& json_str) {
    std::vector<signalr::value> result;

    auto root = cJSON_Parse(json_str.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON args, using empty array");
        return result;
    }

    if (cJSON_IsArray(root)) {
        int size = cJSON_GetArraySize(root);
        for (int i = 0; i < size; i++) {
            auto item = cJSON_GetArrayItem(root, i);
            
            if (cJSON_IsString(item)) {
                result.push_back(signalr::value(item->valuestring));
            } else if (cJSON_IsNumber(item)) {
                result.push_back(signalr::value(item->valuedouble));
            } else if (cJSON_IsBool(item)) {
                result.push_back(signalr::value(static_cast<bool>(cJSON_IsTrue(item))));
            } else if (cJSON_IsNull(item)) {
                result.push_back(signalr::value(nullptr));
            } else if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
                // Convert object/array to JSON string
                char* json_str = cJSON_PrintUnformatted(item);
                result.push_back(signalr::value(json_str));
                cJSON_free(json_str);
            }
        }
    }

    cJSON_Delete(root);
    return result;
}

#endif // CONFIG_ENABLE_SIGNALR_CLIENT
