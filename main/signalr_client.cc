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

    // 🔐 Build URL with token as query parameter (ASP.NET Core SignalR standard method)
    // This is the recommended way for WebSocket connections since setting Authorization
    // header in WebSocket upgrade request requires modifying esp32_websocket_client
    std::string final_hub_url = hub_url;
    
    if (!token.empty()) {
        ESP_LOGI(TAG, "========== SignalR Token Authentication ==========");
        ESP_LOGI(TAG, "Token provided: YES");
        ESP_LOGI(TAG, "Token length: %d characters", token.length());
        
        // Remove "Bearer " prefix if present (not needed in query string)
        std::string token_value = token;
        if (token_value.find("Bearer ") == 0) {
            token_value = token_value.substr(7);  // Remove "Bearer "
            ESP_LOGI(TAG, "Removed 'Bearer ' prefix from token");
        } else if (token_value.find("bearer ") == 0) {
            token_value = token_value.substr(7);  // Remove "bearer "
            ESP_LOGI(TAG, "Removed 'bearer ' prefix from token");
        }
        
        ESP_LOGI(TAG, "Token value length: %d", token_value.length());
        ESP_LOGI(TAG, "Token preview: %.30s...", token_value.c_str());
        
        // Append access_token as query parameter
        // ASP.NET Core SignalR Hub automatically checks this query parameter
        char separator = (hub_url.find('?') != std::string::npos) ? '&' : '?';
        final_hub_url = hub_url + separator + "access_token=" + token_value;
        
        ESP_LOGI(TAG, "✓ Token appended to URL as query parameter");
        ESP_LOGI(TAG, "Final URL format: %s?access_token=...", hub_url.c_str());
        ESP_LOGI(TAG, "==================================================");
    } else {
        ESP_LOGW(TAG, "⚠️ SignalR initialized WITHOUT authentication token");
        ESP_LOGW(TAG, "Connection will be established without authorization.");
        ESP_LOGW(TAG, "Server may reject the connection if authentication is required.");
    }

    hub_url_ = final_hub_url;  // Store the final URL with token
    token_ = token;

    try {
        // Create hub connection builder
        auto builder = signalr::hub_connection_builder::create(final_hub_url);

        // Set WebSocket factory
        // Note: Token is already in the URL as query parameter, no need to set headers
        builder.with_websocket_factory([](const signalr::signalr_client_config& config) {
            ESP_LOGI(TAG, "[WebSocket Factory] Creating WebSocket client");
            ESP_LOGI(TAG, "[WebSocket Factory] Token is in URL query string: ?access_token=...");
            
            auto client = std::make_shared<signalr::esp32_websocket_client>(config);
            return client;
        });

        // Set HTTP client factory
        builder.with_http_client_factory([token = token_](const signalr::signalr_client_config& config) {
            return std::make_shared<signalr::esp32_http_client>(config);
        });

        // NOTE: Do NOT use builder.with_automatic_reconnect() - it has race condition bugs!
        // We use application-layer reconnection via needs_reconnect_ flag instead.

        // Skip negotiation (direct WebSocket connection)
        builder.skip_negotiation(true);

        // Build connection
        connection_ = std::make_unique<signalr::hub_connection>(builder.build());

        // Log memory status before configuration
        ESP_LOGI(TAG, "Free heap after connection build: internal=%lu, PSRAM=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        // Tune timeouts to reduce false disconnects
        signalr::signalr_client_config cfg;
        cfg.set_server_timeout(std::chrono::seconds(60));     // server expects 60s idle before dropping
        cfg.set_keepalive_interval(std::chrono::seconds(15));  // send ping every 15s
        cfg.set_handshake_timeout(std::chrono::seconds(30));   // generous handshake
        
        // IMPORTANT: Disable library's auto-reconnect! It has race condition bugs that cause crashes.
        // We use our own application-layer reconnection logic via needs_reconnect_ flag instead.
        // The app layer reconnect is more stable and gives us better control over timing.
        cfg.enable_auto_reconnect(false);
        
        connection_->set_client_config(cfg);

        // Set disconnected callback - just log for debugging
        // Actual reconnect is handled by application-layer polling (checks IsConnected periodically)
        connection_->set_disconnected([](std::exception_ptr ex) {
            ESP_LOGW(TAG, "SignalR disconnected callback triggered");
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    ESP_LOGW(TAG, "SignalR disconnect reason: %s", e.what());
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

        // Register handler for "CustomMessage" hub method
        // NOTE: Must register BEFORE connecting, otherwise handler won't be triggered
        connection_->on("CustomMessage", [this](const std::vector<signalr::value>& args) {
            if (args.empty()) {
                ESP_LOGW(TAG, "Received empty CustomMessage");
                return;
            }

            try {
                // Parse the first argument as JSON string
                std::string json_str = args[0].as_string();
                ESP_LOGI(TAG, "📨 Received CustomMessage: %s", json_str.c_str());

                auto root = cJSON_Parse(json_str.c_str());
                if (root) {
                    if (on_custom_message_) {
                        on_custom_message_(root);
                    } else {
                        ESP_LOGW(TAG, "CustomMessage callback not set");
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGE(TAG, "Failed to parse CustomMessage JSON");
                }
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Exception handling CustomMessage: %s", e.what());
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
        
        connecting_.store(true, std::memory_order_release);
        
        // Provide empty callback to satisfy API - keep work minimal to avoid deadlock
        connection_->start([this](std::exception_ptr ex) {
            connecting_.store(false, std::memory_order_release);
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "Connection failed: %s", e.what());
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

void SignalRClient::Reset() {
    ESP_LOGI(TAG, "Resetting SignalR client state...");
    
    // Disconnect if connected
    Disconnect();
    
    // Wait a bit for disconnect to complete
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Clear stored URL (contains token as query parameter) and token
    hub_url_.clear();
    token_.clear();
    
    // Reset initialization flags
    initialized_ = false;
    connection_confirmed_ = false;
    connecting_.store(false, std::memory_order_release);
    
    // Reset connection object
    connection_.reset();
    
    // Clear callbacks (optional, but ensures clean state)
    on_custom_message_ = nullptr;
    on_connection_state_changed_ = nullptr;
    
    ESP_LOGI(TAG, "SignalR client reset complete - can be re-initialized");
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

void SignalRClient::PerformReconnect() {
    // Application-layer reconnection logic.
    // Called from main loop when polling detects disconnection.
    // Connect() is non-blocking (starts async connection and returns immediately).
    
    if (!initialized_) {
        ESP_LOGW(TAG, "SignalR not initialized, skipping reconnect");
        return;
    }
    
    if (connecting_.load(std::memory_order_acquire)) {
        ESP_LOGD(TAG, "SignalR already connecting, skipping");
        return;
    }
    
    if (IsConnected()) {
        ESP_LOGD(TAG, "SignalR already connected, skipping reconnect");
        return;
    }
    
    ESP_LOGI(TAG, "SignalR performing reconnection...");
    
    if (!Connect()) {
        ESP_LOGW(TAG, "SignalR reconnection failed to start");
    } else {
        ESP_LOGI(TAG, "SignalR reconnection initiated");
    }
}

bool SignalRClient::IsInitialized() const {
    return initialized_;
}

bool SignalRClient::IsConnecting() const {
    if (connecting_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!connection_) {
        return false;
    }
    return connection_->get_connection_state() == signalr::connection_state::connecting;
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
    ESP_LOGI(TAG, "CustomMessage callback registered");
}

void SignalRClient::OnConnectionStateChanged(
    std::function<void(bool connected, const std::string& error)> callback) {
    on_connection_state_changed_ = callback;
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
