#include "signalr_client.h"

#ifdef CONFIG_ENABLE_SIGNALR_CLIENT

#include <esp_log.h>
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

        // Build connection
        connection_ = std::make_unique<signalr::hub_connection>(builder.build());

        // Set disconnected callback
        connection_->set_disconnected([this](std::exception_ptr ex) {
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "Disconnected with error: %s", e.what());
                    if (on_connection_state_changed_) {
                        on_connection_state_changed_(false, e.what());
                    }
                }
            } else {
                ESP_LOGI(TAG, "Disconnected gracefully");
                if (on_connection_state_changed_) {
                    on_connection_state_changed_(false, "");
                }
            }
        });

        initialized_ = true;
        ESP_LOGI(TAG, "SignalR client initialized with URL: %s", hub_url_.c_str());
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
        ESP_LOGI(TAG, "Connecting to SignalR hub...");
        
        connection_->start([this](std::exception_ptr ex) {
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "Connection failed: %s", e.what());
                    if (on_connection_state_changed_) {
                        on_connection_state_changed_(false, e.what());
                    }
                }
            } else {
                ESP_LOGI(TAG, "Connected to SignalR hub, connection ID: %s", 
                        connection_->get_connection_id().c_str());
                if (on_connection_state_changed_) {
                    on_connection_state_changed_(true, "");
                }
            }
        });

        return true;

    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception during connect: %s", e.what());
        return false;
    }
}

void SignalRClient::Disconnect() {
    if (!connection_) {
        return;
    }

    try {
        ESP_LOGI(TAG, "Disconnecting from SignalR hub...");
        connection_->stop([](std::exception_ptr ex) {
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "Error during disconnect: %s", e.what());
                }
            }
        });
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception during disconnect: %s", e.what());
    }
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
