/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "keycloak_auth.h"
#include "signalr_client.h"
#include "assets/lang_config.h"

#define TAG "MCP"

// Keycloak登录任务静态资源（文件级别，任务内可访问）
static StackType_t* s_login_task_stack = nullptr;
static StaticTask_t* s_login_task_buffer = nullptr;
static volatile bool s_login_task_running = false;
static volatile bool s_login_task_cancelled = false;  // 取消标志
static TaskHandle_t s_login_task_handle = nullptr;    // 任务句柄
static const size_t LOGIN_STACK_SIZE = 8192;

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }

    // Keycloak authentication (unified tool)
    ESP_LOGI(TAG, "Adding Keycloak authentication tool...");
    AddTool("keycloak",
        "Keycloak authentication management. Use this tool when user wants to:\n"
        "- Check login status or ask 'am I logged in?'\n"
        "- Login to Keycloak account (shows QR code on device screen)\n"
        "- Logout or sign out from account\n"
        "- Cancel an ongoing login process\n"
        "\n"
        "Actions:\n"
        "- 'check': Returns whether user is currently authenticated\n"
        "- 'login': Starts OAuth2 device flow, displays QR code and user code on device, waits for user to authorize on phone/computer\n"
        "- 'logout': Clears authentication tokens\n"
        "- 'cancel': Cancels the ongoing login process and hides QR code\n"
        "\n"
        "Server: https://auth.verdure-hiro.cn/ (realm: maker-community)",
        PropertyList({
            Property("action", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            
            // 从配置读取服务器信息
            Settings settings("keycloak", false);
            std::string server_url = settings.GetString("server_url", "https://auth.verdure-hiro.cn/");
            std::string realm = settings.GetString("realm", "maker-community");
            std::string client_id = settings.GetString("client_id", "verdure-assistant");
            
            if (action == "check") {
                KeycloakAuth auth(server_url, realm, client_id);
                cJSON* result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "action", "check");
                bool is_authenticated = auth.IsAuthenticated();
                cJSON_AddBoolToObject(result, "authenticated", is_authenticated);
                
                if (is_authenticated) {
                    auto token = auth.GetAccessToken();
                    ESP_LOGI(TAG, "User is authenticated. Token length: %d", token.length());
                    ESP_LOGI(TAG, "Access token: %.50s...", token.c_str());
                    
                    cJSON_AddStringToObject(result, "status", "logged_in");
                    cJSON_AddStringToObject(result, "message", "You are currently logged in to Keycloak.");
                } else {
                    ESP_LOGI(TAG, "User is not authenticated");
                    cJSON_AddStringToObject(result, "status", "not_logged_in");
                    cJSON_AddStringToObject(result, "message", "You are not logged in. Please use action=login to authenticate.");
                }
                return result;
                
            } else if (action == "login") {
                // 打印登录前的内存状态
                ESP_LOGI(TAG, "========== KEYCLOAK LOGIN START ==========");
                ESP_LOGI(TAG, "Memory BEFORE login:");
                ESP_LOGI(TAG, "  Internal heap: %lu bytes free", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                ESP_LOGI(TAG, "  PSRAM: %lu bytes free", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                ESP_LOGI(TAG, "  Min ever free: %lu bytes", (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
                
                // 在主线程检查登录状态（NVS读取）
                ESP_LOGI(TAG, "Checking existing authentication status (NVS read on main thread)...");
                KeycloakAuth auth_check(server_url, realm, client_id);
                if (auth_check.IsAuthenticated()) {
                    ESP_LOGI(TAG, "User is already authenticated!");
                    cJSON* result = cJSON_CreateObject();
                    cJSON_AddStringToObject(result, "action", "login");
                    cJSON_AddStringToObject(result, "status", "already_logged_in");
                    cJSON_AddStringToObject(result, "message", "You are already logged in. Use action=logout first if you want to re-login.");
                    return result;
                }
                
                // 登录上下文结构体
                struct LoginContext {
                    std::string server_url;
                    std::string realm;
                    std::string client_id;
                };
                
                // 使用文件级别的静态变量 s_login_task_stack, s_login_task_buffer, s_login_task_running, LOGIN_STACK_SIZE
                
                // 检查是否有登录任务正在运行
                if (s_login_task_running) {
                    ESP_LOGW(TAG, "⚠️ Login task is already running, please wait...");
                    cJSON* result = cJSON_CreateObject();
                    cJSON_AddStringToObject(result, "action", "login");
                    cJSON_AddStringToObject(result, "status", "in_progress");
                    cJSON_AddStringToObject(result, "message", "Login is already in progress. Please wait for the current login to complete or timeout.");
                    return result;
                }
                
                if (s_login_task_stack == nullptr) {
                    s_login_task_stack = (StackType_t*)heap_caps_malloc(LOGIN_STACK_SIZE, MALLOC_CAP_SPIRAM);
                    if (s_login_task_stack == nullptr) {
                        ESP_LOGE(TAG, "❌ Failed to allocate task stack from PSRAM!");
                        throw std::runtime_error("Failed to allocate login task stack");
                    }
                    ESP_LOGI(TAG, "✓ Login task stack allocated from PSRAM (reusable)");
                }
                
                if (s_login_task_buffer == nullptr) {
                    s_login_task_buffer = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                    if (s_login_task_buffer == nullptr) {
                        ESP_LOGE(TAG, "❌ Failed to allocate task buffer!");
                        throw std::runtime_error("Failed to allocate login task buffer");
                    }
                    ESP_LOGI(TAG, "✓ Login task buffer allocated from internal RAM (reusable)");
                }
                
                // 标记任务开始运行，清除取消标志
                s_login_task_running = true;
                s_login_task_cancelled = false;
                
                // 创建任务参数（在堆上，任务结束后通过Schedule释放）
                auto* ctx = new LoginContext{server_url, realm, client_id};
                
                // 创建PSRAM栈任务执行HTTP操作
                // 注意：此任务不能访问NVS/Flash！
                TaskHandle_t task_handle = xTaskCreateStatic(
                    [](void* arg) {
                        auto* ctx = static_cast<LoginContext*>(arg);
                        ESP_LOGI(TAG, "[LOGIN TASK] Started on PSRAM stack");
                        ESP_LOGI(TAG, "[LOGIN TASK] Stack high water mark: %lu", 
                                 (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
                        
                        auto& board = Board::GetInstance();
                        auto& app = Application::GetInstance();
                        
                        // ===== 步骤1: 请求设备码 (HTTP, 不访问Flash) =====
                        ESP_LOGI(TAG, "[LOGIN TASK] Requesting device code...");
                        auto http = board.GetNetwork()->CreateHttp(5);  // 5秒超时，快速失败重试
                        if (http == nullptr) {
                            ESP_LOGE(TAG, "[LOGIN TASK] ❌ Failed to create HTTP client!");
                            s_login_task_running = false;  // 清除运行标志
                            app.Schedule([ctx]() {
                                Application::GetInstance().Alert(Lang::Strings::LOGIN_ERROR, 
                                    Lang::Strings::LOGIN_ERROR_START_FAILED, "triangle_exclamation", "");
                                // Alert将在几秒后自动消失或被下一个操作覆盖
                                delete ctx;
                            });
                            vTaskDelete(nullptr);
                            return;
                        }
                        
                        // 确保server_url末尾没有斜杠
                        std::string base_url = ctx->server_url;
                        while (!base_url.empty() && base_url.back() == '/') {
                            base_url.pop_back();
                        }
                        std::string device_auth_url = base_url + "/realms/" + ctx->realm + 
                                                      "/protocol/openid-connect/auth/device";
                        std::string post_data = "client_id=" + ctx->client_id;
                        
                        ESP_LOGI(TAG, "[LOGIN TASK] Base URL (trimmed): %s", base_url.c_str());
                        
                        http->SetHeader("Content-Type", "application/x-www-form-urlencoded");
                        
                        ESP_LOGI(TAG, "[LOGIN TASK] POST %s", device_auth_url.c_str());
                        if (!http->Open("POST", device_auth_url)) {
                            ESP_LOGE(TAG, "[LOGIN TASK] ❌ Failed to open HTTP connection!");
                            s_login_task_running = false;  // 清除运行标志
                            app.Schedule([ctx]() {
                                Application::GetInstance().Alert(Lang::Strings::LOGIN_ERROR,
                                    Lang::Strings::LOGIN_ERROR_START_FAILED, "triangle_exclamation", "");
                                delete ctx;
                            });
                            vTaskDelete(nullptr);
                            return;
                        }
                        
                        http->Write(post_data.c_str(), post_data.size());
                        http->Write("", 0);
                        
                        int status_code = http->GetStatusCode();
                        ESP_LOGI(TAG, "[LOGIN TASK] Response status: %d", status_code);
                        
                        if (status_code != 200) {
                            std::string error_body = http->ReadAll();
                            ESP_LOGE(TAG, "[LOGIN TASK] ❌ Device code request failed: %s", error_body.c_str());
                            http->Close();
                            s_login_task_running = false;  // 清除运行标志
                            app.Schedule([ctx]() {
                                Application::GetInstance().Alert(Lang::Strings::LOGIN_ERROR,
                                    Lang::Strings::LOGIN_ERROR_START_FAILED, "triangle_exclamation", "");
                                delete ctx;
                            });
                            vTaskDelete(nullptr);
                            return;
                        }
                        
                        std::string json_response = http->ReadAll();
                        http->Close();
                        ESP_LOGI(TAG, "[LOGIN TASK] ✓ Device code response: %s", json_response.c_str());
                        
                        // 解析响应
                        cJSON* root = cJSON_Parse(json_response.c_str());
                        if (!root) {
                            ESP_LOGE(TAG, "[LOGIN TASK] ❌ Failed to parse JSON response!");
                            s_login_task_running = false;  // 清除运行标志
                            app.Schedule([ctx]() {
                                Application::GetInstance().Alert(Lang::Strings::LOGIN_ERROR,
                                    Lang::Strings::LOGIN_ERROR_START_FAILED, "triangle_exclamation", "");
                                delete ctx;
                            });
                            vTaskDelete(nullptr);
                            return;
                        }
                        
                        // 安全获取JSON字段，检查空指针
                        cJSON* j_device_code = cJSON_GetObjectItem(root, "device_code");
                        cJSON* j_user_code = cJSON_GetObjectItem(root, "user_code");
                        cJSON* j_verification_uri = cJSON_GetObjectItem(root, "verification_uri");
                        cJSON* j_expires_in = cJSON_GetObjectItem(root, "expires_in");
                        cJSON* j_interval = cJSON_GetObjectItem(root, "interval");
                        
                        if (!j_device_code || !cJSON_IsString(j_device_code) ||
                            !j_user_code || !cJSON_IsString(j_user_code) ||
                            !j_verification_uri || !cJSON_IsString(j_verification_uri) ||
                            !j_expires_in || !cJSON_IsNumber(j_expires_in)) {
                            ESP_LOGE(TAG, "[LOGIN TASK] ❌ Invalid device code response format!");
                            cJSON_Delete(root);
                            s_login_task_running = false;
                            app.Schedule([ctx]() {
                                Application::GetInstance().Alert(Lang::Strings::LOGIN_ERROR,
                                    Lang::Strings::LOGIN_ERROR_START_FAILED, "triangle_exclamation", "");
                                delete ctx;
                            });
                            vTaskDelete(nullptr);
                            return;
                        }
                        
                        std::string device_code = j_device_code->valuestring;
                        std::string user_code = j_user_code->valuestring;
                        std::string verification_uri = j_verification_uri->valuestring;
                        int expires_in = j_expires_in->valueint;
                        int interval = (j_interval && cJSON_IsNumber(j_interval)) ? j_interval->valueint : 5;
                        
                        cJSON* uri_complete = cJSON_GetObjectItem(root, "verification_uri_complete");
                        std::string display_url = (uri_complete && cJSON_IsString(uri_complete)) 
                                                  ? uri_complete->valuestring 
                                                  : verification_uri;
                        cJSON_Delete(root);
                        
                        ESP_LOGI(TAG, "[LOGIN TASK] User Code: %s", user_code.c_str());
                        ESP_LOGI(TAG, "[LOGIN TASK] Verification URI: %s", display_url.c_str());
                        ESP_LOGI(TAG, "[LOGIN TASK] Expires in: %d seconds", expires_in);
                        
                        // ===== 步骤2: 在主线程显示二维码 =====
                        std::string user_code_copy = user_code;
                        std::string display_url_copy = display_url;
                        app.Schedule([user_code_copy, display_url_copy]() {
                            auto display = Board::GetInstance().GetDisplay();
                            if (display != nullptr) {
                                char subtitle[64];
                                snprintf(subtitle, sizeof(subtitle), Lang::Strings::LOGIN_USER_CODE, user_code_copy.c_str());
                                display->ShowQRCode(display_url_copy.c_str(), Lang::Strings::LOGIN_QR_TITLE, subtitle);
                                ESP_LOGI(TAG, "[MAIN] ✓ QR code displayed");
                            }
                        });
                        
                        // ===== 步骤3: 轮询token (HTTP, 不访问Flash) =====
                        // 确保base_url末尾没有斜杠
                        std::string token_base_url = ctx->server_url;
                        while (!token_base_url.empty() && token_base_url.back() == '/') {
                            token_base_url.pop_back();
                        }
                        std::string token_url = token_base_url + "/realms/" + ctx->realm + 
                                               "/protocol/openid-connect/token";
                        std::string token_post = "grant_type=urn:ietf:params:oauth:grant-type:device_code&"
                                                "client_id=" + ctx->client_id + "&"
                                                "device_code=" + device_code;
                        
                        int max_attempts = expires_in / interval;
                        bool success = false;
                        bool cancelled = false;
                        std::string access_token, refresh_token;
                        int token_expires_in = 0, refresh_expires_in = 0;
                        
                        for (int i = 0; i < max_attempts; i++) {
                            // 检查是否被取消
                            if (s_login_task_cancelled) {
                                ESP_LOGI(TAG, "[LOGIN TASK] Login cancelled by user");
                                cancelled = true;
                                break;
                            }
                            
                            if (i > 0) {
                                // 分段睡眠，以便更快响应取消
                                for (int j = 0; j < interval * 10; j++) {
                                    if (s_login_task_cancelled) {
                                        ESP_LOGI(TAG, "[LOGIN TASK] Login cancelled during wait");
                                        cancelled = true;
                                        break;
                                    }
                                    vTaskDelay(pdMS_TO_TICKS(100));
                                }
                                if (cancelled) break;
                            }
                            
                            ESP_LOGD(TAG, "[LOGIN TASK] Polling token... attempt %d/%d", i + 1, max_attempts);
                            
                            auto poll_http = board.GetNetwork()->CreateHttp(5);  // 5秒超时
                            if (!poll_http) continue;
                            
                            poll_http->SetHeader("Content-Type", "application/x-www-form-urlencoded");
                            if (!poll_http->Open("POST", token_url)) {
                                continue;
                            }
                            
                            poll_http->Write(token_post.c_str(), token_post.size());
                            poll_http->Write("", 0);
                            
                            int poll_status = poll_http->GetStatusCode();
                            std::string poll_response = poll_http->ReadAll();
                            poll_http->Close();
                            
                            if (poll_status == 200) {
                                // 成功获取token
                                cJSON* token_json = cJSON_Parse(poll_response.c_str());
                                if (token_json) {
                                    auto at = cJSON_GetObjectItem(token_json, "access_token");
                                    auto rt = cJSON_GetObjectItem(token_json, "refresh_token");
                                    auto ei = cJSON_GetObjectItem(token_json, "expires_in");
                                    auto rei = cJSON_GetObjectItem(token_json, "refresh_expires_in");
                                    
                                    if (at) access_token = at->valuestring;
                                    if (rt) refresh_token = rt->valuestring;
                                    if (ei) token_expires_in = ei->valueint;
                                    if (rei) refresh_expires_in = rei->valueint;
                                    
                                    cJSON_Delete(token_json);
                                    success = true;
                                    ESP_LOGI(TAG, "[LOGIN TASK] ✓ Token obtained! Length: %d", access_token.length());
                                    break;
                                }
                            } else if (poll_status == 400) {
                                // 检查是否是authorization_pending
                                cJSON* err_json = cJSON_Parse(poll_response.c_str());
                                if (err_json) {
                                    auto error = cJSON_GetObjectItem(err_json, "error");
                                    if (error && cJSON_IsString(error)) {
                                        std::string err_str = error->valuestring;
                                        cJSON_Delete(err_json);
                                        if (err_str == "authorization_pending" || err_str == "slow_down") {
                                            continue;  // 继续轮询
                                        }
                                    } else {
                                        cJSON_Delete(err_json);
                                    }
                                }
                                // 其他400错误，退出
                                ESP_LOGE(TAG, "[LOGIN TASK] ❌ Token request failed: %s", poll_response.c_str());
                                break;
                            }
                        }
                        
                        // ===== 步骤4: 通过Schedule在主线程保存token到NVS =====
                        if (cancelled) {
                            // 用户取消了登录，隐藏二维码即可，不需要显示额外提示
                            app.Schedule([]() {
                                auto display = Board::GetInstance().GetDisplay();
                                if (display) display->HideQRCode();
                                ESP_LOGI(TAG, "[MAIN] Login cancelled, QR code hidden");
                            });
                        } else if (success) {
                            // 捕获所有需要的数据
                            std::string server = ctx->server_url;
                            std::string realm = ctx->realm;
                            std::string client = ctx->client_id;
                            
                            app.Schedule([server, realm, client, access_token, refresh_token, 
                                         token_expires_in, refresh_expires_in]() {
                                ESP_LOGI(TAG, "[MAIN] Saving tokens to NVS...");
                                
                                // 在主线程创建KeycloakAuth并保存token
                                KeycloakAuth auth(server, realm, client);
                                KeycloakAuth::TokenResponse token_resp;
                                token_resp.access_token = access_token;
                                token_resp.refresh_token = refresh_token;
                                token_resp.token_type = "Bearer";
                                token_resp.expires_in = token_expires_in;
                                token_resp.refresh_expires_in = refresh_expires_in;
                                auth.SaveTokens(token_resp);
                                
                                // 隐藏二维码并显示成功
                                auto display = Board::GetInstance().GetDisplay();
                                if (display) display->HideQRCode();
                                
                                Application::GetInstance().Alert(Lang::Strings::LOGIN_SUCCESS, 
                                    Lang::Strings::LOGIN_SUCCESS_MESSAGE, "check_circle", "");
                                
                                // Re-initialize SignalR with new token
#ifdef CONFIG_ENABLE_SIGNALR_CLIENT
                                {
                                    auto& signalr = SignalRClient::GetInstance();
                                    // Reset first to clear old state (if any)
                                    if (signalr.IsInitialized()) {
                                        ESP_LOGI(TAG, "Resetting SignalR to use new token");
                                        signalr.Reset();
                                    }
                                    // Re-initialize SignalR with the new token
                                    Application::GetInstance().InitializeSignalR();
                                }
#endif
                                
                                // 使用定时器在3秒后清除Alert（非阻塞）
                                esp_timer_handle_t dismiss_timer;
                                esp_timer_create_args_t timer_args = {
                                    .callback = [](void* arg) {
                                        Application::GetInstance().Schedule([]() {
                                            Application::GetInstance().DismissAlert();
                                        });
                                        // 删除定时器自身
                                        esp_timer_delete((esp_timer_handle_t)arg);
                                    },
                                    .arg = nullptr,
                                    .dispatch_method = ESP_TIMER_TASK,
                                    .name = "login_alert_dismiss",
                                    .skip_unhandled_events = true
                                };
                                if (esp_timer_create(&timer_args, &dismiss_timer) == ESP_OK) {
                                    // 将timer handle传给回调，以便删除自身
                                    timer_args.arg = dismiss_timer;
                                    esp_timer_start_once(dismiss_timer, 3000000);  // 3秒 = 3000000微秒
                                }
                                
                                ESP_LOGI(TAG, "[MAIN] ✓ Login completed successfully!");
                            });
                        } else {
                            app.Schedule([]() {
                                auto display = Board::GetInstance().GetDisplay();
                                if (display) display->HideQRCode();
                                
                                Application::GetInstance().Alert(Lang::Strings::LOGIN_TIMEOUT, 
                                    Lang::Strings::LOGIN_TIMEOUT_MESSAGE, "triangle_exclamation", "");
                                
                                // 使用定时器在3秒后清除Alert（非阻塞）
                                esp_timer_handle_t dismiss_timer;
                                esp_timer_create_args_t timer_args = {
                                    .callback = [](void* arg) {
                                        Application::GetInstance().Schedule([]() {
                                            Application::GetInstance().DismissAlert();
                                        });
                                        esp_timer_delete((esp_timer_handle_t)arg);
                                    },
                                    .arg = nullptr,
                                    .dispatch_method = ESP_TIMER_TASK,
                                    .name = "login_timeout_dismiss",
                                    .skip_unhandled_events = true
                                };
                                if (esp_timer_create(&timer_args, &dismiss_timer) == ESP_OK) {
                                    timer_args.arg = dismiss_timer;
                                    esp_timer_start_once(dismiss_timer, 3000000);
                                }
                                
                                ESP_LOGW(TAG, "[MAIN] Login timeout or failed");
                            });
                        }
                        
                        // 清理上下文
                        delete ctx;
                        
                        // 清除运行标志和任务句柄
                        s_login_task_running = false;
                        s_login_task_handle = nullptr;
                        
                        ESP_LOGI(TAG, "[LOGIN TASK] Task ending, stack high water mark: %lu",
                                 (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
                        vTaskDelete(nullptr);
                    },
                    "keycloak_login",
                    LOGIN_STACK_SIZE / sizeof(StackType_t),
                    ctx,
                    5,  // 优先级
                    s_login_task_stack,
                    s_login_task_buffer
                );
                
                if (task_handle == nullptr) {
                    s_login_task_running = false;  // 清除运行标志
                    delete ctx;
                    ESP_LOGE(TAG, "❌ Failed to create login task!");
                    throw std::runtime_error("Failed to create login task");
                }
                
                // 保存任务句柄以便取消
                s_login_task_handle = task_handle;
                
                ESP_LOGI(TAG, "✓ Login task created on PSRAM stack (handle: %p)", task_handle);
                
                cJSON* result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "action", "login");
                cJSON_AddStringToObject(result, "status", "started");
                cJSON_AddStringToObject(result, "message", "Login process started. Please scan the QR code displayed on the device screen.");
                return result;
                
            } else if (action == "cancel") {
                // 取消正在进行的登录
                if (!s_login_task_running) {
                    ESP_LOGI(TAG, "No login in progress to cancel");
                    cJSON* result = cJSON_CreateObject();
                    cJSON_AddStringToObject(result, "action", "cancel");
                    cJSON_AddBoolToObject(result, "success", false);
                    cJSON_AddStringToObject(result, "status", "no_login_in_progress");
                    cJSON_AddStringToObject(result, "message", "There is no login in progress to cancel.");
                    return result;
                }
                
                ESP_LOGI(TAG, "Cancelling login...");
                s_login_task_cancelled = true;
                
                // 立即隐藏二维码
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    display->HideQRCode();
                }
                
                // 任务会在下次检查取消标志时自行退出
                cJSON* result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "action", "cancel");
                cJSON_AddBoolToObject(result, "success", true);
                cJSON_AddStringToObject(result, "status", "cancelled");
                cJSON_AddStringToObject(result, "message", "Login process has been cancelled.");
                return result;
                
            } else if (action == "logout") {
                KeycloakAuth auth(server_url, realm, client_id);
                bool was_logged_in = auth.IsAuthenticated();
                auth.ClearTokens();
                
                // Also reset SignalR client to clear any stored token in URL
                // This ensures reconnection will not use the old token
#ifdef CONFIG_ENABLE_SIGNALR_CLIENT
                auto& signalr = SignalRClient::GetInstance();
                if (signalr.IsInitialized()) {
                    ESP_LOGI(TAG, "Resetting SignalR client to clear stored token");
                    signalr.Reset();
                }
#endif
                
                ESP_LOGI(TAG, "User logged out successfully (was authenticated: %s)", was_logged_in ? "yes" : "no");
                
                cJSON* result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "action", "logout");
                cJSON_AddBoolToObject(result, "success", true);
                cJSON_AddStringToObject(result, "status", "logged_out");
                if (was_logged_in) {
                    cJSON_AddStringToObject(result, "message", "You have been logged out successfully. All authentication tokens have been cleared.");
                } else {
                    cJSON_AddStringToObject(result, "message", "You were not logged in. No tokens to clear.");
                }
                return result;
                
            } else {
                throw std::runtime_error("Invalid action. Use: check, login, logout, or cancel");
            }
        });
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
