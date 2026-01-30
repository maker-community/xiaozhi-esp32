#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "keycloak_auth.h"

#ifdef HAVE_LVGL
#include "lcd_display.h"
#include "lvgl_display.h"
#ifndef CONFIG_IDF_TARGET_ESP32
#include "jpg/jpeg_to_image.h"
#endif
#endif

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }

#ifdef CONFIG_ENABLE_SIGNALR_CLIENT
            // Check if SignalR needs reconnection (non-blocking async approach)
            // The reconnection runs in a background task, so this is always safe to call
            {
                static int signalr_disconnect_detect_count = 0;
                auto& signalr = SignalRClient::GetInstance();
                if (signalr.IsInitialized()) {
                    // First, check if token is still valid before attempting reconnect
                    Settings token_storage("keycloak", false);
                    std::string token = token_storage.GetString("access_token", "");
                    int64_t expires_at = token_storage.GetInt("access_expires", 0);
                    
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    int64_t now = tv.tv_sec;
                    
                    bool token_valid = !token.empty() && expires_at > now;
                    
                    if (!token_valid) {
                        // Token expired or missing - destroy SignalR to save resources
                        ESP_LOGW(TAG, "Token expired or missing - destroying SignalR to save resources");
                        signalr.Reset();
                        signalr_disconnect_detect_count = 0;
                    } else {
                        // Token valid - proceed with reconnection check
                        // Pure polling: check if disconnected and not currently connecting
                        if (!signalr.IsConnected() && !signalr.IsConnecting()) {
                            signalr_disconnect_detect_count++;
                            // Wait for 2 consecutive checks (2 seconds) to confirm disconnect
                            if (signalr_disconnect_detect_count >= 2) {
                                signalr_disconnect_detect_count = 0;
                                // Request reconnection - this is non-blocking (runs in background task)
                                // No need to check device state since it doesn't block audio processing
                                signalr.RequestReconnect();
                            }
                        } else {
                            // Reset counter when connected or connecting
                            signalr_disconnect_detect_count = 0;
                        }
                    }
                }
            }
#endif
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    } else {
#ifdef CONFIG_ENABLE_SIGNALR_CLIENT
        // Network restored - polling will detect disconnect and reconnect automatically
        auto& signalr = SignalRClient::GetInstance();
        if (signalr.IsInitialized() && !signalr.IsConnected()) {
            ESP_LOGI(TAG, "Network restored, SignalR will reconnect via polling (state=%s)", 
                     signalr.GetConnectionState().c_str());
        }
#endif
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

#ifdef CONFIG_ENABLE_SIGNALR_CLIENT
    // Disconnect SignalR when network is lost
    // Polling will not reconnect while network is down (IsConnecting check prevents rapid retries)
    auto& signalr = SignalRClient::GetInstance();
    if (signalr.IsInitialized()) {
        ESP_LOGI(TAG, "Disconnecting SignalR due to network loss");
        signalr.Disconnect();
    }
#endif

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Play the success sound to indicate the device is ready
    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Initialize SignalR client (if enabled)
    InitializeSignalR();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::InitializeSignalR() {
#ifdef CONFIG_ENABLE_SIGNALR_CLIENT
    // 优先从运行时配置读取，如果为空则使用编译时配置
    Settings settings("signalr", false);
    std::string hub_url = settings.GetString("hub_url");
    
    if (hub_url.empty()) {
        // 使用编译时配置的默认值
#ifdef CONFIG_SIGNALR_HUB_URL
        hub_url = CONFIG_SIGNALR_HUB_URL;
#endif
    }
    
    if (hub_url.empty()) {
        ESP_LOGI(TAG, "SignalR not configured, skipping");
        return;
    }
    
    ESP_LOGI(TAG, "SignalR Hub URL: %s", hub_url.c_str());
    
    // 直接从 NVS 读取已保存的 access token（不依赖 Keycloak 配置）
    ESP_LOGI(TAG, "========== Loading Saved Token ==========");
    Settings token_storage("keycloak", false);
    std::string token = token_storage.GetString("access_token", "");
    int64_t expires_at = token_storage.GetInt("access_expires", 0);
    
    if (!token.empty()) {
        // 检查token是否过期
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t now = tv.tv_sec;
        
        if (expires_at > now) {
            ESP_LOGI(TAG, "✅ Found valid saved token");
            ESP_LOGI(TAG, "Token length: %d characters", token.length());
            ESP_LOGI(TAG, "Token expires in: %lld seconds", (long long)(expires_at - now));
        } else {
            ESP_LOGW(TAG, "⚠️ Saved token has expired, clearing...");
            token.clear();
        }
    } else {
        ESP_LOGW(TAG, "No saved token found");
    }
    ESP_LOGI(TAG, "==========================================");
    
    // 🔒 Only initialize SignalR if we have a valid token
    // Without token, SignalR functionality is limited/useless, so skip to save resources
    if (token.empty()) {
        ESP_LOGI(TAG, "No valid token - skipping SignalR initialization to save resources");
        ESP_LOGI(TAG, "SignalR will be initialized after successful login");
        return;
    }
    
    auto& signalr = SignalRClient::GetInstance();
    
    if (!signalr.Initialize(hub_url, token)) {
        ESP_LOGE(TAG, "Failed to initialize SignalR client");
        return;
    }
    
    // Register custom message handler
    signalr.OnCustomMessage([this](const cJSON* payload) {
        if (!payload) return;
        
        char* json_str = cJSON_PrintUnformatted(payload);
        if (json_str) {
            Schedule([this, message = std::string(json_str)]() {
                HandleSignalRMessage(message);
            });
            cJSON_free(json_str);
        }
    });
    
    // Note: SignalR disconnection is handled internally via atomic flags
    // No callback registration needed to avoid deadlock issues
    
    // Connect to SignalR hub
    if (!signalr.Connect()) {
        ESP_LOGE(TAG, "Failed to connect to SignalR hub");
    }
#else
    ESP_LOGI(TAG, "SignalR client is disabled");
#endif
}

void Application::HandleSignalRMessage(const std::string& message) {
    ESP_LOGI(TAG, "Handling SignalR message: %s", message.c_str());
    
    auto root = cJSON_Parse(message.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse SignalR message JSON");
        return;
    }
    
    auto display = Board::GetInstance().GetDisplay();
    
    // Check message action/type
    auto action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "notification") == 0) {
            // Handle notification
            // JSON: {"action":"notification", "title":"标题", "content":"内容", "emotion":"bell", "sound":"popup"}
            auto title = cJSON_GetObjectItem(root, "title");
            auto content = cJSON_GetObjectItem(root, "content");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            auto sound = cJSON_GetObjectItem(root, "sound");
            
            const char* title_str = cJSON_IsString(title) ? title->valuestring : Lang::Strings::INFO;
            const char* content_str = cJSON_IsString(content) ? content->valuestring : "";
            const char* emotion_str = cJSON_IsString(emotion) ? emotion->valuestring : "bell";
            
            // Select sound based on "sound" field
            std::string_view sound_view = Lang::Sounds::OGG_POPUP;
            if (cJSON_IsString(sound)) {
                if (strcmp(sound->valuestring, "success") == 0) {
                    sound_view = Lang::Sounds::OGG_SUCCESS;
                } else if (strcmp(sound->valuestring, "vibration") == 0) {
                    sound_view = Lang::Sounds::OGG_VIBRATION;
                } else if (strcmp(sound->valuestring, "exclamation") == 0) {
                    sound_view = Lang::Sounds::OGG_EXCLAMATION;
                } else if (strcmp(sound->valuestring, "low_battery") == 0) {
                    sound_view = Lang::Sounds::OGG_LOW_BATTERY;
                } else if (strcmp(sound->valuestring, "none") == 0) {
                    sound_view = "";
                }
                // default: popup
            }
            
            Alert(title_str, content_str, emotion_str, sound_view);
            
        } else if (strcmp(action->valuestring, "command") == 0) {
            // Handle command
            // JSON: {"action":"command", "command":"reboot|wake|listen|stop"}
            auto cmd = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(cmd)) {
                if (strcmp(cmd->valuestring, "reboot") == 0) {
                    Reboot();
                } else if (strcmp(cmd->valuestring, "wake") == 0) {
                    // Trigger wake word detection
                    xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
                } else if (strcmp(cmd->valuestring, "listen") == 0) {
                    StartListening();
                } else if (strcmp(cmd->valuestring, "stop") == 0) {
                    StopListening();
                } else {
                    ESP_LOGW(TAG, "Unknown SignalR command: %s", cmd->valuestring);
                }
            }
            
        } else if (strcmp(action->valuestring, "display") == 0) {
            // Display custom content
            // JSON: {"action":"display", "content":"文本内容", "role":"system"}
            auto content = cJSON_GetObjectItem(root, "content");
            auto role = cJSON_GetObjectItem(root, "role");
            const char* role_str = cJSON_IsString(role) ? role->valuestring : "system";
            if (cJSON_IsString(content)) {
                display->SetChatMessage(role_str, content->valuestring);
            }
            
        } else if (strcmp(action->valuestring, "emotion") == 0) {
            // Change emotion/expression
            // JSON: {"action":"emotion", "emotion":"happy"}
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                display->SetEmotion(emotion->valuestring);
            }
            
        } else if (strcmp(action->valuestring, "image") == 0) {
            // Display image from URL
            // JSON: {"action":"image", "url":"https://example.com/image.jpg"}
            auto url = cJSON_GetObjectItem(root, "url");
            if (cJSON_IsString(url)) {
                HandleSignalRImageMessage(url->valuestring);
            } else {
                ESP_LOGW(TAG, "Image action requires 'url' field");
            }
            
        } else if (strcmp(action->valuestring, "audio") == 0) {
            // Play audio from URL (OGG format)
            // JSON: {"action":"audio", "url":"https://example.com/sound.ogg"}
            auto url = cJSON_GetObjectItem(root, "url");
            if (cJSON_IsString(url)) {
                HandleSignalRAudioMessage(url->valuestring);
            } else {
                ESP_LOGW(TAG, "Audio action requires 'url' field");
            }
            
        } else if (strcmp(action->valuestring, "qrcode") == 0) {
            // Show QR code
            // JSON: {"action":"qrcode", "data":"https://...", "title":"标题", "subtitle":"副标题"}
            auto data = cJSON_GetObjectItem(root, "data");
            auto title = cJSON_GetObjectItem(root, "title");
            auto subtitle = cJSON_GetObjectItem(root, "subtitle");
            if (cJSON_IsString(data)) {
                const char* title_str = cJSON_IsString(title) ? title->valuestring : nullptr;
                const char* subtitle_str = cJSON_IsString(subtitle) ? subtitle->valuestring : nullptr;
                display->ShowQRCode(data->valuestring, title_str, subtitle_str);
            } else {
                ESP_LOGW(TAG, "QRCode action requires 'data' field");
            }
            
        } else if (strcmp(action->valuestring, "hide_qrcode") == 0) {
            // Hide QR code
            // JSON: {"action":"hide_qrcode"}
            display->HideQRCode();
            
        } else {
            // Default: display as system message
            char* display_str = cJSON_Print(root);
            if (display_str) {
                display->SetChatMessage("system", display_str);
                cJSON_free(display_str);
            }
        }
    } else {
        // No action specified, display raw message
        char* display_str = cJSON_Print(root);
        if (display_str) {
            display->SetChatMessage("system", display_str);
            cJSON_free(display_str);
        }
    }
    
    cJSON_Delete(root);
}

void Application::HandleSignalRImageMessage(const char* url) {
#ifdef HAVE_LVGL
    ESP_LOGI(TAG, "Downloading image from: %s", url);
    // Pause audio processing and wake-word detection to avoid AFE ringbuffer overflow
    bool was_processor_running = audio_service_.IsAudioProcessorRunning();
    bool was_wake_running = audio_service_.IsWakeWordRunning();
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    // Wait for playback queue to drain before heavy work (decoding/download)
    audio_service_.WaitForPlaybackQueueEmpty();
    // Scope guard: restore previous audio state on any exit path
    struct ScopeGuard { std::function<void()> f; ScopeGuard(std::function<void()> fn): f(fn) {} ~ScopeGuard(){ if (f) f(); } };
    ScopeGuard __restore_audio([&](){
        if (was_processor_running) audio_service_.EnableVoiceProcessing(true);
        if (was_wake_running) audio_service_.EnableWakeWordDetection(true);
    });
    
    std::string current_url = url;
    int max_redirects = 5;
    int redirect_count = 0;
    
    while (redirect_count < max_redirects) {
        // Use longer timeout (30 seconds) for image downloads
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(30);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            return;
        }
        
        // Tell server we only accept JPEG/PNG (avoid WebP which we can't decode)
        http->SetHeader("Accept", "image/jpeg, image/png, image/*;q=0.9");
        
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open URL: %s", current_url.c_str());
            return;
        }
        
        int status_code = http->GetStatusCode();
        
        // Handle redirects (301, 302, 303, 307, 308)
        if (status_code >= 300 && status_code < 400) {
            std::string location = http->GetResponseHeader("Location");
            http->Close();
            
            if (location.empty()) {
                ESP_LOGE(TAG, "Redirect response missing Location header");
                return;
            }
            
            // Handle relative URLs
            if (location[0] == '/') {
                size_t pos = current_url.find("://");
                if (pos != std::string::npos) {
                    pos = current_url.find('/', pos + 3);
                    if (pos != std::string::npos) {
                        location = current_url.substr(0, pos) + location;
                    }
                }
            }
            
            ESP_LOGI(TAG, "Following redirect (%d) to: %s", status_code, location.c_str());
            current_url = location;
            redirect_count++;
            continue;
        }
        
        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP error: %d", status_code);
            http->Close();
            return;
        }
        
        size_t content_length = http->GetBodyLength();
        const size_t max_image_size = 2 * 1024 * 1024; // Max 2MB (need enough PSRAM for decoding)
        if (content_length == 0) {
            ESP_LOGE(TAG, "Empty response (content_length=0)");
            http->Close();
            return;
        }
        if (content_length > max_image_size) {
            ESP_LOGE(TAG, "Image too large: %d bytes (max %d bytes). Please compress the image.", 
                     content_length, max_image_size);
            http->Close();
            return;
        }
        
        // Allocate memory for image data
        uint8_t* data = (uint8_t*)heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (data == nullptr) {
            data = (uint8_t*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
        }
        if (data == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for image: %d bytes", content_length);
            http->Close();
            return;
        }
        
        // Download image data
        size_t total_read = 0;
        while (total_read < content_length) {
            int ret = http->Read((char*)data + total_read, content_length - total_read);
            if (ret < 0) {
                ESP_LOGE(TAG, "Failed to read image data");
                heap_caps_free(data);
                http->Close();
                return;
            }
            if (ret == 0) {
                break;
            }
            total_read += ret;
        }
        http->Close();
        
        ESP_LOGI(TAG, "Image downloaded: %d bytes", total_read);
        
        // Check image format by magic bytes
        bool is_jpeg = (total_read >= 2 && data[0] == 0xFF && data[1] == 0xD8);
        bool is_png = (total_read >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47);
        bool is_webp = (total_read >= 12 && data[0] == 0x52 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x46 
                        && data[8] == 0x57 && data[9] == 0x45 && data[10] == 0x42 && data[11] == 0x50); // RIFF....WEBP
        
        const char* format = is_jpeg ? "JPEG" : (is_png ? "PNG" : (is_webp ? "WebP" : "unknown"));
        ESP_LOGI(TAG, "Image format: %s", format);
        
        // WebP is not supported
        if (is_webp) {
            ESP_LOGE(TAG, "WebP format is not supported. Please use JPEG or PNG images.");
            heap_caps_free(data);
            return;
        }
        
        auto display = Board::GetInstance().GetDisplay();
        auto lcd_display = static_cast<LcdDisplay*>(display);
        
#ifndef CONFIG_IDF_TARGET_ESP32
        if (is_jpeg) {
            // Try to decode JPEG to RGB565 using ESP decoder
            uint8_t* decoded_data = nullptr;
            size_t decoded_len = 0;
            size_t width = 0, height = 0, stride = 0;
            
            esp_err_t ret = jpeg_to_image(data, total_read, &decoded_data, &decoded_len, &width, &height, &stride);
            
            if (ret == ESP_OK && decoded_data != nullptr) {
                heap_caps_free(data); // Free original JPEG data
                ESP_LOGI(TAG, "JPEG decoded: %dx%d", width, height);
                auto image = std::make_unique<LvglAllocatedImage>(decoded_data, decoded_len, width, height, stride, LV_COLOR_FORMAT_RGB565);
                lcd_display->SetPreviewImage(std::move(image));
                return;
            }
            
            // ESP decoder failed - do NOT fallback to LVGL decoder as it doesn't support raw JPEG
            ESP_LOGE(TAG, "JPEG decoding failed (%s), cannot display image", esp_err_to_name(ret));
            if (decoded_data) heap_caps_free(decoded_data);
            heap_caps_free(data);
            return;
        }
#else
        // On ESP32, we don't have hardware JPEG decoder
        if (is_jpeg) {
            ESP_LOGE(TAG, "JPEG images not supported on ESP32 (no hardware decoder)");
            heap_caps_free(data);
            return;
        }
#endif
        // Try to use LVGL's built-in image decoder for non-JPEG formats (PNG, etc.)
        try {
            ESP_LOGI(TAG, "Creating LvglAllocatedImage for non-JPEG image (%d bytes)...", total_read);
            auto image = std::make_unique<LvglAllocatedImage>(data, total_read);
            ESP_LOGI(TAG, "LvglAllocatedImage created, calling SetPreviewImage...");
            lcd_display->SetPreviewImage(std::move(image));
            ESP_LOGI(TAG, "SetPreviewImage completed");
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "Failed to create image: %s", e.what());
            heap_caps_free(data);
        }
        return;
    }
    
    ESP_LOGE(TAG, "Too many redirects");
#else
    ESP_LOGW(TAG, "Image display not supported (LVGL disabled)");
#endif
}

void Application::HandleSignalRAudioMessage(const char* url) {
    ESP_LOGI(TAG, "Downloading audio from: %s", url);
    
    std::string current_url = url;
    int max_redirects = 5;
    int redirect_count = 0;
    
    while (redirect_count < max_redirects) {
        // Use longer timeout (30 seconds) for audio downloads
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(30);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            return;
        }
        
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open URL: %s", current_url.c_str());
            return;
        }
        
        int status_code = http->GetStatusCode();
        
        // Handle redirects (301, 302, 303, 307, 308)
        if (status_code >= 300 && status_code < 400) {
            std::string location = http->GetResponseHeader("Location");
            http->Close();
            
            if (location.empty()) {
                ESP_LOGE(TAG, "Redirect response missing Location header");
                return;
            }
            
            // Handle relative URLs
            if (location[0] == '/') {
                size_t pos = current_url.find("://");
                if (pos != std::string::npos) {
                    pos = current_url.find('/', pos + 3);
                    if (pos != std::string::npos) {
                        location = current_url.substr(0, pos) + location;
                    }
                }
            }
            
            ESP_LOGI(TAG, "Following redirect (%d) to: %s", status_code, location.c_str());
            current_url = location;
            redirect_count++;
            continue;
        }
        
        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP error: %d", status_code);
            http->Close();
            return;
        }
        
        size_t content_length = http->GetBodyLength();
        if (content_length == 0 || content_length > 512 * 1024) { // Max 512KB for audio
            ESP_LOGE(TAG, "Invalid audio content length: %d", content_length);
            http->Close();
            return;
        }
        
        // Read audio data into string
        std::string audio_data;
        audio_data.reserve(content_length);
        
        char buffer[1024];
        size_t total_read = 0;
        while (total_read < content_length) {
            int ret = http->Read(buffer, std::min(sizeof(buffer), content_length - total_read));
            if (ret < 0) {
                ESP_LOGE(TAG, "Failed to read audio data");
                http->Close();
                return;
            }
            if (ret == 0) {
                break;
            }
            audio_data.append(buffer, ret);
            total_read += ret;
        }
        http->Close();
        
        ESP_LOGI(TAG, "Audio downloaded: %d bytes", total_read);
        
        // Play the audio (OGG format)
        audio_service_.PlaySound(std::string_view(audio_data.data(), audio_data.size()));
        return;
    }
    
    ESP_LOGE(TAG, "Too many redirects");
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }
        }

        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }
        }

        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [display](int progress, size_t speed) {
        std::thread([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
        }).detach();
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

