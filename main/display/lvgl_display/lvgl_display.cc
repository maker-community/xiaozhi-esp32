#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>
#include <qrcode.h>

#include "lvgl_display.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "jpg/image_to_jpeg.h"

#ifdef CONFIG_ENABLE_SIGNALR_CLIENT
#include "signalr_client.h"
#endif

#define TAG "Display"

// QR code context structure for callback
struct QRContext {
    LvglDisplay* display;
    const char* title;
    const char* subtitle;
    bool success;
};

// Static variable to pass context to C callback function
static QRContext* s_qr_context = nullptr;

LvglDisplay::LvglDisplay() {
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback = [](void *arg) {
            LvglDisplay *display = static_cast<LvglDisplay*>(arg);
            DisplayLockGuard lock(display);
            lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

LvglDisplay::~LvglDisplay() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if( low_battery_popup_ != nullptr ) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // Update icon if mute state changes
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_XMARK);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time
    if (app.GetDeviceState() == kDeviceStateIdle) {
        if (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now()) {
            // Set status to clock "HH:MM"
            time_t now = time(NULL);
            struct tm* tm = localtime(&now);
            // Check if the we have already set the time
            if (tm->tm_year >= 2025 - 1900) {
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", tm);
                SetStatus(time_str);
            } else {
                ESP_LOGW(TAG, "System time is not set, tm_year: %d", tm->tm_year);
            }
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    // Update battery icon
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, // 0-19%
                FONT_AWESOME_BATTERY_QUARTER,    // 20-39%
                FONT_AWESOME_BATTERY_HALF,    // 40-59%
                FONT_AWESOME_BATTERY_THREE_QUARTERS,    // 60-79%
                FONT_AWESOME_BATTERY_FULL, // 80-99%
                FONT_AWESOME_BATTERY_FULL, // 100%
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        if (low_battery_popup_ != nullptr) {
            if (strcmp(icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Show if low battery popup is hidden
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Hide if low battery popup is shown
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // Update network icon every 10 seconds
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

#ifdef CONFIG_ENABLE_SIGNALR_CLIENT
    // Update SignalR connection status icon
    if (signalr_label_ != nullptr) {
        auto& signalr = SignalRClient::GetInstance();
        const char* signalr_icon = nullptr;
        lv_color_t icon_color = lv_color_hex(0xFFFFFF);  // Default white
        
        bool is_connected = signalr.IsConnected();
        bool is_initialized = signalr.IsInitialized();
        
        // Determine icon based on actual connection state
        if (is_connected) {
            signalr_icon = FONT_AWESOME_CIRCLE_CHECK;
            icon_color = lv_color_hex(0x00FF00);  // Green
        } else if (is_initialized) {
            signalr_icon = FONT_AWESOME_CIRCLE_XMARK;
            icon_color = lv_color_hex(0xFF0000);  // Red
        } else {
            signalr_icon = "";  // Not initialized - hide icon
        }
        
        // Update icon if changed
        if (signalr_icon_ != signalr_icon) {
            DisplayLockGuard lock(this);
            signalr_icon_ = signalr_icon;
            lv_label_set_text(signalr_label_, signalr_icon_);
            
            if (strlen(signalr_icon) > 0) {  // Only set color if icon is visible
                lv_obj_set_style_text_color(signalr_label_, icon_color, 0);
            }
            
            ESP_LOGI(TAG, "SignalR status updated: %s (connected=%d, initialized=%d)",
                     is_connected ? "Connected" : (is_initialized ? "Disconnected" : "Hidden"),
                     is_connected, is_initialized);
        }
    }
#endif

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
}

void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret = image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w, draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
        [](void *arg, size_t index, const void *data, size_t len) -> size_t {
        std::string* output = static_cast<std::string*>(arg);
        if (data && len > 0) {
            output->append(static_cast<const char*>(data), len);
        }
        return len;
    }, &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}

void LvglDisplay::ShowQRCode(const char* data, const char* title, const char* subtitle) {
    ESP_LOGI(TAG, "========== SHOW QR CODE START ==========");
    ESP_LOGI(TAG, "QR Data: %s", data ? data : "(null)");
    ESP_LOGI(TAG, "Title: %s", title ? title : "(null)");
    ESP_LOGI(TAG, "Subtitle: %s", subtitle ? subtitle : "(null)");
    ESP_LOGI(TAG, "Memory before QR generation:");
    ESP_LOGI(TAG, "  Internal heap: %lu bytes", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "  PSRAM: %lu bytes", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    DisplayLockGuard lock(this);
    ESP_LOGI(TAG, "Display lock acquired");
    
    // Hide existing QR code if any
    if (qrcode_container_ != nullptr) {
        ESP_LOGI(TAG, "Hiding existing QR code container");
        lv_obj_delete(qrcode_container_);
        qrcode_container_ = nullptr;
        qrcode_obj_ = nullptr;
    }
    
    if (data == nullptr || strlen(data) == 0) {
        ESP_LOGW(TAG, "QR code data is empty or null, returning");
        return;
    }
    
    ESP_LOGI(TAG, "Generating QR code for URL (length=%d)...", strlen(data));
    
    // Create context for the callback
    QRContext context = {this, title, subtitle, false};
    
    // Generate QR code with display callback
    esp_qrcode_config_t cfg = {
        .display_func = [](esp_qrcode_handle_t qrcode) {
            // Get context from static variable (workaround for C function pointer limitation)
            QRContext* ctx = s_qr_context;
            if (ctx == nullptr || ctx->display == nullptr) {
                return;
            }
            
            LvglDisplay* disp = ctx->display;
            int qr_size = esp_qrcode_get_size(qrcode);
            ESP_LOGI(TAG, "QR code generated, size: %d", qr_size);
            
            // Create full-screen container with semi-transparent background
            disp->qrcode_container_ = lv_obj_create(lv_screen_active());
            lv_obj_set_size(disp->qrcode_container_, LV_HOR_RES, LV_VER_RES);
            lv_obj_set_pos(disp->qrcode_container_, 0, 0);
            lv_obj_set_style_bg_color(disp->qrcode_container_, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(disp->qrcode_container_, LV_OPA_90, 0);
            lv_obj_set_style_border_width(disp->qrcode_container_, 0, 0);
            lv_obj_set_style_radius(disp->qrcode_container_, 0, 0);
            lv_obj_set_style_pad_all(disp->qrcode_container_, 20, 0);
            lv_obj_set_flex_flow(disp->qrcode_container_, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(disp->qrcode_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            
            // Add title if provided
            if (ctx->title != nullptr && strlen(ctx->title) > 0) {
                lv_obj_t* title_label = lv_label_create(disp->qrcode_container_);
                lv_label_set_text(title_label, ctx->title);
                lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
                lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_pad_bottom(title_label, 10, 0);
            }
            
            // Calculate QR code display size
            int scale = 3; // Pixel scale factor
            int canvas_size = qr_size * scale;
            int max_size = (LV_HOR_RES < LV_VER_RES ? LV_HOR_RES : LV_VER_RES) - 100;
            
            // Adjust scale if too large
            while (canvas_size > max_size && scale > 1) {
                scale--;
                canvas_size = qr_size * scale;
            }
            
            disp->qrcode_obj_ = lv_canvas_create(disp->qrcode_container_);
            
            // Allocate buffer for canvas (RGB565 format for LVGL 9.x)
            size_t buf_size = canvas_size * canvas_size * sizeof(lv_color_t);
            ESP_LOGI(TAG, "Allocating canvas buffer: %d bytes (canvas: %dx%d, scale: %d)", 
                     buf_size, canvas_size, canvas_size, scale);
            
            lv_color_t* canvas_buf = (lv_color_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (canvas_buf == nullptr) {
                ESP_LOGW(TAG, "PSRAM allocation failed, trying internal RAM...");
                canvas_buf = (lv_color_t*)malloc(buf_size);
            }
            
            if (canvas_buf == nullptr) {
                ESP_LOGE(TAG, "❌ CRITICAL: Failed to allocate canvas buffer (%d bytes)!", buf_size);
                ESP_LOGE(TAG, "  Free internal: %lu, Free PSRAM: %lu", 
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                lv_obj_delete(disp->qrcode_container_);
                disp->qrcode_container_ = nullptr;
                disp->qrcode_obj_ = nullptr;
                ctx->success = false;
                return;
            }
            ESP_LOGI(TAG, "✓ Canvas buffer allocated successfully");
            
            // Set buffer to canvas first
            lv_canvas_set_buffer(disp->qrcode_obj_, canvas_buf, canvas_size, canvas_size, LV_COLOR_FORMAT_RGB565);
            
            // Fill with white background
            lv_canvas_fill_bg(disp->qrcode_obj_, lv_color_white(), LV_OPA_COVER);
            
            // Draw QR code modules using lv_canvas_set_px for correct pixel placement
            lv_color_t black = lv_color_black();
            for (int y = 0; y < qr_size; y++) {
                for (int x = 0; x < qr_size; x++) {
                    if (esp_qrcode_get_module(qrcode, x, y)) {
                        // Fill scaled block with black pixels using LVGL API
                        for (int dy = 0; dy < scale; dy++) {
                            for (int dx = 0; dx < scale; dx++) {
                                lv_canvas_set_px(disp->qrcode_obj_, x * scale + dx, y * scale + dy, black, LV_OPA_COVER);
                            }
                        }
                    }
                }
            }
            
            // Add white border around QR code
            lv_obj_set_style_bg_color(disp->qrcode_obj_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(disp->qrcode_obj_, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(disp->qrcode_obj_, 10, 0);
            
            // Add subtitle if provided
            if (ctx->subtitle != nullptr && strlen(ctx->subtitle) > 0) {
                lv_obj_t* subtitle_label = lv_label_create(disp->qrcode_container_);
                lv_label_set_text(subtitle_label, ctx->subtitle);
                lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(subtitle_label, LV_HOR_RES - 40);
                lv_obj_set_style_text_color(subtitle_label, lv_color_white(), 0);
                lv_obj_set_style_text_align(subtitle_label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_pad_top(subtitle_label, 10, 0);
            }
            
            ctx->success = true;
            ESP_LOGI(TAG, "✓ QR code displayed successfully!");
            ESP_LOGI(TAG, "Memory after QR display:");
            ESP_LOGI(TAG, "  Internal: %lu, PSRAM: %lu", 
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        },
        .max_qrcode_version = 10,
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
    };
    
    // Store context in static variable for callback access
    s_qr_context = &context;
    
    ESP_LOGI(TAG, "Calling esp_qrcode_generate()...");
    esp_err_t ret = esp_qrcode_generate(&cfg, data);
    ESP_LOGI(TAG, "esp_qrcode_generate() returned: %s (0x%x)", esp_err_to_name(ret), ret);
    
    // Clear static context
    s_qr_context = nullptr;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ esp_qrcode_generate FAILED: %s", esp_err_to_name(ret));
        return;
    }
    
    if (!context.success) {
        ESP_LOGE(TAG, "❌ QR code generation succeeded but display callback failed");
        ESP_LOGE(TAG, "  This may indicate memory allocation issues in the callback");
        return;
    }
    
    ESP_LOGI(TAG, "========== SHOW QR CODE COMPLETE ==========");
}

void LvglDisplay::HideQRCode() {
    ESP_LOGI(TAG, "HideQRCode() called");
    DisplayLockGuard lock(this);
    
    if (qrcode_container_ != nullptr) {
        lv_obj_delete(qrcode_container_);
        qrcode_container_ = nullptr;
        qrcode_obj_ = nullptr;
        ESP_LOGI(TAG, "QR code hidden");
    }
}
