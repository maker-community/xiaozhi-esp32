#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "i2c_device.h"
#include "mcp_server.h"
#include "serial_control.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <cJSON.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <utility>

#define TAG "AtomEchoS3R"

class RoverLcdDisplay : public SpiLcdDisplay {
public:
    RoverLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                    int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                    bool swap_xy, std::function<void(const char*)> emotion_handler)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy),
          emotion_handler_(std::move(emotion_handler)) {}

    void SetEmotion(const char* emotion) override {
        SpiLcdDisplay::SetEmotion(emotion);
        if (emotion != nullptr && emotion_handler_) {
            emotion_handler_(emotion);
        }
    }

private:
    std::function<void(const char*)> emotion_handler_;
};

class AtomEchoS3rBaseBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button user_button_;
    LcdDisplay* display_ = nullptr;
    SerialControl serial_;  // Grove 口 -> 下位机 ESP32-C3 电机板

    // ---- 下位机状态缓存 (串口任务写入, MCP/主任务读取) ----
    std::mutex motor_mutex_;
    int battery_level_ = -1;
    bool battery_charging_ = false;
    bool battery_discharging_ = false;
    int motor_speed_[2] = {0, 0};
    uint8_t rgb_[3] = {0, 0, 0};
    esp_timer_handle_t battery_timer_ = nullptr;
    volatile bool motor_board_ready_ = false;  // 收到 READY / 首个有效响应后置位

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        ESP_LOGI(TAG, "Initialize LCD Display (ST7789 %dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RESET_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        // NOTE: offset/gap is handled by SpiLcdDisplay via lv_display_set_offset;
        // do NOT also call esp_lcd_panel_set_gap here or the image is shifted twice.
        display_ = new RoverLcdDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, [this](const char* emotion) {
                std::string value = ToLower(emotion);
                if (value == "happy" || value == "laughing" || value == "loving" ||
                    value == "winking" || value == "cool" || value == "relaxed" ||
                    value == "delicious" || value == "kissy" || value == "confident" ||
                    value == "silly") {
                    value = "happy";
                } else if (value == "thinking" || value == "confused") {
                    value = "thinking";
                } else if (value == "sad" || value == "angry" || value == "crying" ||
                           value == "embarrassed" || value == "surprised" || value == "shocked") {
                    value = "warning";
                } else {
                    value = "off";
                }
                SetLedEmotion(value);
            });
    }

    void InitializeButtons() {
        user_button_.OnPressDown([this]() {
            bool moving = false;
            {
                std::lock_guard<std::mutex> lock(motor_mutex_);
                moving = motor_speed_[0] != 0 || motor_speed_[1] != 0;
            }
            if (!moving) {
                return;
            }

            // 急停不等待 READY 握手，保证按钮按下后立即把停止命令发出。
            serial_.SendLine("stop");
            {
                std::lock_guard<std::mutex> lock(motor_mutex_);
                motor_speed_[0] = 0;
                motor_speed_[1] = 0;
            }
            ESP_LOGI(TAG, "[BUTTON] emergency stop");
        });

        user_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    // 上位机自身命令帮助 -> 仅输出到日志(USB 串口), 不要发到 Grove(下位机通道)
    void SendHelp() {
        ESP_LOGI(TAG, "=== AtomEchoS3R (Verdure Buddy Rover) ===");
        ESP_LOGI(TAG, "Commands (case-insensitive):");
        ESP_LOGI(TAG, "  status / toggle / wake / listen / stop");
        ESP_LOGI(TAG, "  volume [0-100] / screen <text> / backlight [0-100]");
        ESP_LOGI(TAG, "  motor <0|1> <fwd|rev|stop> [0-100] / battery / rgb / chassis");
        ESP_LOGI(TAG, "  reboot / help / ?");
    }

    static std::string ToLower(std::string s) {
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c += ('a' - 'A');
            }
        }
        return s;
    }

    // ---- 下位机通信 (ESP32-C3 电机板, 协议参考 esp32c3_kb_motor.ino) ----
    // 共电源复位不同步: 发送前确保下位机已上线(收到过 READY/响应)
    // 等待窗口较长(READY_MAX_PINGS 次 x PING_RETRY_MS), 覆盖下位机 setup(BLE/I2C)耗时
    void EnsureMotorBoardReady() {
        if (motor_board_ready_) {
            return;
        }
        for (int i = 0; i < MOTOR_BOARD_READY_MAX_PINGS && !motor_board_ready_; i++) {
            ESP_LOGI(TAG, "[UART TX] ping (waiting for motor board READY, %d/%d)", i,
                     MOTOR_BOARD_READY_MAX_PINGS);
            serial_.SendLine("ping");
            vTaskDelay(pdMS_TO_TICKS(MOTOR_BOARD_PING_RETRY_MS));
        }
        if (!motor_board_ready_) {
            ESP_LOGW(TAG, "Motor board did not answer READY in time; proceeding anyway");
        }
    }

    void SendMotorCommand(int id, const char* dir, int speed) {
        EnsureMotorBoardReady();
        if (speed < 0)
            speed = 0;
        if (speed > 100)
            speed = 100;
        char buf[64];
        snprintf(buf, sizeof(buf), "motor %d %s %d", id, dir, MOTOR_PWM_SCALE(speed));
        ESP_LOGI(TAG, "[UART TX] %s", buf);
        serial_.SendLine(buf);
    }

    bool DriveAction(const std::string& action, int speed) {
        speed = std::max(MOTOR_SPEED_MIN, std::min(MOTOR_SPEED_MAX, speed));
        EnsureMotorBoardReady();

        char command[48];
        int left = 0;
        int right = 0;
        if (action == "forward") {
            snprintf(command, sizeof(command), "forward %d", speed);
            left = right = speed;
        } else if (action == "back") {
            snprintf(command, sizeof(command), "back %d", speed);
            left = right = -speed;
        } else if (action == "turn_left") {
            snprintf(command, sizeof(command), "turn left %d", speed);
            left = -speed;
            right = speed;
        } else if (action == "turn_right") {
            snprintf(command, sizeof(command), "turn right %d", speed);
            left = speed;
            right = -speed;
        } else {
            snprintf(command, sizeof(command), "stop");
        }

        ESP_LOGI(TAG, "[CHASSIS] %s", command);
        std::string response;
        if (!serial_.SendCommand(command, "OK drive", response)) {
            ESP_LOGW(TAG, "[CHASSIS] command failed or timed out: %s", command);
            return false;
        }
        std::lock_guard<std::mutex> lock(motor_mutex_);
        motor_speed_[0] = left;
        motor_speed_[1] = right;
        return true;
    }

    // 直接发 battery 查询 (无阻塞, 可在 esp_timer 回调中调用)
    void QueryBattery() {
        ESP_LOGI(TAG, "[UART TX] battery");
        serial_.SendLine("battery");
    }

    void SetRgb(int r, int g, int b) {
        EnsureMotorBoardReady();
        if (r < 0)
            r = 0;
        if (r > 255)
            r = 255;
        if (g < 0)
            g = 0;
        if (g > 255)
            g = 255;
        if (b < 0)
            b = 0;
        if (b > 255)
            b = 255;
        {
            std::lock_guard<std::mutex> lock(motor_mutex_);
            rgb_[0] = (uint8_t)r;
            rgb_[1] = (uint8_t)g;
            rgb_[2] = (uint8_t)b;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "rgb %d %d %d", r, g, b);
        ESP_LOGI(TAG, "[UART TX] %s", buf);
        serial_.SendLine(buf);
    }

    bool SetLedMode(const std::string& mode, int r = 0, int g = 0, int b = 0,
                    int period_ms = 1000) {
        EnsureMotorBoardReady();
        r = std::max(0, std::min(255, r));
        g = std::max(0, std::min(255, g));
        b = std::max(0, std::min(255, b));
        period_ms = std::max(200, std::min(10000, period_ms));

        char buf[96];
        if (mode == "on" || mode == "off") {
            snprintf(buf, sizeof(buf), "led %s", mode.c_str());
        } else if (mode == "rgb") {
            snprintf(buf, sizeof(buf), "rgb %d %d %d", r, g, b);
        } else {
            snprintf(buf, sizeof(buf), "led %s %d %d %d %d", mode.c_str(), r, g, b, period_ms);
        }
        ESP_LOGI(TAG, "[UART TX] %s", buf);
        const char* response_prefix = mode == "rgb" ? "OK rgb" : "OK led";
        std::string response;
        if (!serial_.SendCommand(buf, response_prefix, response)) {
            ESP_LOGW(TAG, "[LIGHT] command failed or timed out: %s", buf);
            return false;
        }

        if (mode == "off") {
            std::lock_guard<std::mutex> lock(motor_mutex_);
            rgb_[0] = rgb_[1] = rgb_[2] = 0;
        } else if (mode == "rgb") {
            std::lock_guard<std::mutex> lock(motor_mutex_);
            rgb_[0] = static_cast<uint8_t>(r);
            rgb_[1] = static_cast<uint8_t>(g);
            rgb_[2] = static_cast<uint8_t>(b);
        }
        return true;
    }

    bool SetLedEmotion(const std::string& emotion, bool wait_for_response = false) {
        ESP_LOGI(TAG, "[UART TX] led emotion %s", emotion.c_str());
        std::string command = "led emotion " + emotion;
        if (!wait_for_response) {
            serial_.SendLine(command);
            return true;
        }
        EnsureMotorBoardReady();
        std::string response;
        if (!serial_.SendCommand(command, "OK led emotion", response)) {
            ESP_LOGW(TAG, "[LIGHT] emotion command failed or timed out: %s", emotion.c_str());
            return false;
        }
        return true;
    }

    // 解析下位机回报行: battery level=.. charging=.. discharging=..
    //                    status JSON / OK ... / EVT ... / READY
    void HandleMotorBoardLine(const std::string& line) {
        // 收到下位机的 READY 或任何有效响应, 即视为下位机已上线并进入 loop
        if (line.rfind("READY", 0) == 0 || line.rfind("OK ", 0) == 0 ||
            line.rfind("EVT ", 0) == 0 || line.rfind("battery ", 0) == 0 ||
            (!line.empty() && line[0] == '{')) {
            motor_board_ready_ = true;
        }
        ESP_LOGI(TAG, "[MOTOR BOARD] %s", line.c_str());

        if (line.rfind("battery ", 0) == 0) {
            int level = -1;
            int charging = 0, discharging = 0;
            if (sscanf(line.c_str(), "battery level=%d charging=%d discharging=%d", &level,
                       &charging, &discharging) >= 1 &&
                level >= 0) {
                std::lock_guard<std::mutex> lock(motor_mutex_);
                battery_level_ = level;
                battery_charging_ = charging != 0;
                battery_discharging_ = discharging != 0;
                ESP_LOGI(TAG, "[UART RX] battery level=%d charging=%d discharging=%d", level,
                         charging, discharging);
            } else {
                ESP_LOGW(TAG, "[UART RX] unparsed battery line: %s", line.c_str());
            }
            return;
        }
        if (line.rfind("OK ", 0) == 0 || line.rfind("ERR ", 0) == 0 || line.rfind("EVT ", 0) == 0 ||
            line.rfind("READY", 0) == 0) {
            ESP_LOGI(TAG, "[UART RX] %s", line.c_str());
            return;
        }
        if (!line.empty() && line[0] == '{') {
            ESP_LOGI(TAG, "[UART RX] status json: %s", line.c_str());
            // 下位机 status 完整 JSON: {battery,motor,rgb,keys,ble}
            cJSON* root = cJSON_Parse(line.c_str());
            if (root != nullptr) {
                std::lock_guard<std::mutex> lock(motor_mutex_);
                cJSON* battery = cJSON_GetObjectItem(root, "battery");
                if (cJSON_IsObject(battery)) {
                    cJSON* level = cJSON_GetObjectItem(battery, "level");
                    cJSON* charging = cJSON_GetObjectItem(battery, "charging");
                    cJSON* discharging = cJSON_GetObjectItem(battery, "discharging");
                    if (cJSON_IsNumber(level)) {
                        battery_level_ = level->valueint;
                    }
                    if (cJSON_IsNumber(charging)) {
                        battery_charging_ = charging->valueint != 0;
                    }
                    if (cJSON_IsNumber(discharging)) {
                        battery_discharging_ = discharging->valueint != 0;
                    }
                }
                cJSON* motor = cJSON_GetObjectItem(root, "motor");
                if (cJSON_IsObject(motor)) {
                    cJSON* m0 = cJSON_GetObjectItem(motor, "m0");
                    cJSON* m1 = cJSON_GetObjectItem(motor, "m1");
                    if (cJSON_IsNumber(m0)) {
                        motor_speed_[0] = m0->valueint;
                    }
                    if (cJSON_IsNumber(m1)) {
                        motor_speed_[1] = m1->valueint;
                    }
                }
                cJSON* rgb = cJSON_GetObjectItem(root, "rgb");
                if (cJSON_IsArray(rgb) && cJSON_GetArraySize(rgb) == 3) {
                    for (int i = 0; i < 3; i++) {
                        cJSON* item = cJSON_GetArrayItem(rgb, i);
                        if (cJSON_IsNumber(item)) {
                            rgb_[i] = (uint8_t)item->valueint;
                        }
                    }
                }
                cJSON_Delete(root);
            }
            return;
        }
        ESP_LOGI(TAG, "motor board: %s", line.c_str());
    }

    void HandleSerialCommand(const std::string& line) {
        // 先尝试解析下位机回报 (来自电机板)
        if (line.rfind("battery ", 0) == 0 || line.rfind("OK ", 0) == 0 ||
            line.rfind("ERR ", 0) == 0 || line.rfind("EVT ", 0) == 0 ||
            line.rfind("READY", 0) == 0 || (!line.empty() && line[0] == '{')) {
            HandleMotorBoardLine(line);
            return;
        }

        // 下位机的帮助/状态文本 (来自 esp32c3_kb_motor.ino 的 printHelp) 直接忽略,
        // 不要当作未知命令回 ERR, 否则会与下位机形成垃圾回环淹没串口。
        if (line.empty() || line[0] == '=' || line.rfind("Commands", 0) == 0 || line[0] == ' ' ||
            line[0] == '\t') {
            ESP_LOGI(TAG, "[MOTOR BOARD INFO] %s", line.c_str());
            return;
        }

        ESP_LOGI(TAG, "[HOST CMD] %s", line.c_str());
        std::string cmd = line;
        // Split command and arguments on first space.
        std::string arg;
        size_t sp = cmd.find(' ');
        if (sp != std::string::npos) {
            arg = cmd.substr(sp + 1);
            cmd = cmd.substr(0, sp);
        }
        cmd = ToLower(cmd);

        auto& app = Application::GetInstance();
        AudioCodec* codec = GetAudioCodec();

        if (cmd == "status") {
            char buf[64];
            snprintf(buf, sizeof(buf), "state=%d volume=%d%%", (int)app.GetDeviceState(),
                     codec ? codec->output_volume() : -1);
            ESP_LOGI(TAG, "[HOST] %s", buf);
            ESP_LOGI(TAG, "[HOST] %s", GetDeviceStatusJson().c_str());
        } else if (cmd == "toggle") {
            app.ToggleChatState();
            ESP_LOGI(TAG, "[HOST] OK toggle");
        } else if (cmd == "wake") {
            std::string word = arg.empty() ? "你好小智" : arg;
            app.WakeWordInvoke(word);
            ESP_LOGI(TAG, "[HOST] OK wake");
        } else if (cmd == "listen") {
            app.StartListening();
            ESP_LOGI(TAG, "[HOST] OK listen");
        } else if (cmd == "stop") {
            app.StopListening();
            ESP_LOGI(TAG, "[HOST] OK stop");
        } else if (cmd == "volume") {
            if (arg.empty() && codec) {
                char buf[32];
                snprintf(buf, sizeof(buf), "volume=%d", codec->output_volume());
                ESP_LOGI(TAG, "[HOST] %s", buf);
            } else {
                int v = atoi(arg.c_str());
                if (v < 0)
                    v = 0;
                if (v > 100)
                    v = 100;
                if (codec) {
                    codec->SetOutputVolume(v);
                }
                char buf[32];
                snprintf(buf, sizeof(buf), "OK volume=%d", v);
                ESP_LOGI(TAG, "[HOST] %s", buf);
            }
        } else if (cmd == "screen") {
            if (display_ && !arg.empty()) {
                display_->ShowNotification(arg.c_str(), 5000);
                ESP_LOGI(TAG, "[HOST] OK screen");
            } else {
                ESP_LOGI(TAG, "[HOST] ERR empty text");
            }
        } else if (cmd == "backlight") {
            Backlight* bl = GetBacklight();
            if (!bl) {
                ESP_LOGI(TAG, "[HOST] ERR no backlight");
            } else if (arg.empty()) {
                char buf[32];
                snprintf(buf, sizeof(buf), "backlight=%d", bl->brightness());
                ESP_LOGI(TAG, "[HOST] %s", buf);
            } else {
                int v = atoi(arg.c_str());
                if (v < 0)
                    v = 0;
                if (v > 100)
                    v = 100;
                bl->SetBrightness((uint8_t)v);
                char buf[32];
                snprintf(buf, sizeof(buf), "OK backlight=%d", v);
                ESP_LOGI(TAG, "[HOST] %s", buf);
            }
        } else if (cmd == "motor") {
            int id = -1, speed = MOTOR_SPEED_MAX;
            char dir[8] = {0};
            if (sscanf(arg.c_str(), "%d %7s %d", &id, dir, &speed) < 2) {
                ESP_LOGI(TAG, "[HOST] ERR motor usage: motor <0|1> <fwd|rev|stop> [0-100]");
                return;
            }
            if (id != 0 && id != 1) {
                ESP_LOGI(TAG, "[HOST] ERR bad motor id");
                return;
            }
            std::string d = ToLower(dir);
            if (d == "stop")
                SendMotorCommand(id, "stop", 0);
            else if (d == "fwd")
                SendMotorCommand(id, "fwd", speed);
            else if (d == "rev")
                SendMotorCommand(id, "rev", speed);
            else {
                ESP_LOGI(TAG, "[HOST] ERR bad motor dir");
                return;
            }
            ESP_LOGI(TAG, "[HOST] OK motor %d", id);
        } else if (cmd == "chassis") {
            // 转发给下位机并解析完整状态 JSON
            EnsureMotorBoardReady();
            serial_.SendLine("status");
        } else if (cmd == "battery") {
            QueryBattery();
        } else if (cmd == "rgb") {
            int r = -1, g = -1, b = -1;
            if (sscanf(arg.c_str(), "%d %d %d", &r, &g, &b) == 3) {
                SetRgb(r, g, b);
                ESP_LOGI(TAG, "[HOST] OK rgb");
            } else {
                ESP_LOGI(TAG, "[HOST] ERR rgb usage: rgb <r> <g> <b> (0-255)");
            }
        } else if (cmd == "reboot") {
            ESP_LOGI(TAG, "[HOST] OK rebooting");
            vTaskDelay(pdMS_TO_TICKS(100));
            app.Reboot();
        } else {
            // 未知命令不再回 ERR 到下位机(避免回环), 只记日志
            ESP_LOGI(TAG, "[HOST] ERR unknown command: %s", line.c_str());
        }
    }

    void InitializeSerial() {
        serial_.SetHelpHandler([this]() { SendHelp(); });
        serial_.SetLineHandler([this](const std::string& line) { HandleSerialCommand(line); });
        serial_.Start();
        // 帮助文本延迟到下位机就绪后再发, 避免共电源启动瞬间干扰
    }

    static void OnBatteryTimer(void* arg) {
        static_cast<AtomEchoS3rBaseBoard*>(arg)->QueryBattery();
    }

    // 启动握手任务: 等共电源复位稳定后, 先确认下位机 READY, 再启动周期电池查询。
    // 必须在 FreeRTOS 任务里做(需要 vTaskDelay), 不能在 esp_timer 回调里阻塞。
    static void StartupHandshakeTask(void* arg) {
        auto* board = static_cast<AtomEchoS3rBaseBoard*>(arg);
        ESP_LOGI(TAG, "Motor board startup delay %d ms elapsed, handshaking...",
                 MOTOR_BOARD_STARTUP_DELAY_MS);
        board->SendHelp();               // 仅输出到日志, 不发到下位机
        board->EnsureMotorBoardReady();  // ping 探活, 等下位机 READY/响应
        board->QueryBattery();
        if (board->battery_timer_ != nullptr) {
            esp_timer_start_periodic(board->battery_timer_, BATTERY_QUERY_INTERVAL_MS * 1000);
        }
        ESP_LOGI(TAG, "Battery periodic query started (every %d ms)", BATTERY_QUERY_INTERVAL_MS);
        vTaskDelete(nullptr);
    }

    void InitializeBatteryTimer() {
        esp_timer_create_args_t args = {};
        args.callback = OnBatteryTimer;
        args.arg = this;
        args.name = "battery_query";
        if (esp_timer_create(&args, &battery_timer_) != ESP_OK) {
            return;
        }
        // 等上下位机共电源复位稳定后再开始周期查询, 避免早期命令丢失
        if (xTaskCreate(StartupHandshakeTask, "motor_handshake", 4096, this, 5, nullptr) !=
            pdPASS) {
            ESP_LOGE(TAG, "Failed to create motor handshake task");
        }
    }

    // ---- MCP 工具: 使用少量高语义工具覆盖小车协议 ----
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        auto add_chassis_action = [this, &mcp_server](const char* name, const char* description,
                                                      const char* action) {
            mcp_server.AddTool(
                name, description,
                PropertyList({Property("speed", kPropertyTypeInteger, MOTOR_SPEED_DEFAULT,
                                       MOTOR_SPEED_MIN, MOTOR_SPEED_MAX)}),
                [this, action](const PropertyList& properties) -> ReturnValue {
                    return DriveAction(action, properties["speed"].value<int>());
                });
        };
        add_chassis_action("self.chassis.go_forward", "控制小车向前行走。", "forward");
        add_chassis_action("self.chassis.go_back", "控制小车向后行走。", "back");
        add_chassis_action("self.chassis.turn_left", "控制小车原地左转。", "turn_left");
        add_chassis_action("self.chassis.turn_right", "控制小车原地右转。", "turn_right");
        mcp_server.AddTool(
            "self.chassis.stop", "立即停止小车。", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { return DriveAction("stop", 0); });

        mcp_server.AddTool("self.led.set_color", "设置小车 RGB 灯颜色。",
                           PropertyList({Property("red", kPropertyTypeInteger, 0, 0, 255),
                                         Property("green", kPropertyTypeInteger, 0, 0, 255),
                                         Property("blue", kPropertyTypeInteger, 0, 0, 255)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               return SetLedMode("rgb", properties["red"].value<int>(),
                                                 properties["green"].value<int>(),
                                                 properties["blue"].value<int>());
                           });
        mcp_server.AddTool(
            "self.led.off", "关闭小车 RGB 灯。", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { return SetLedMode("off"); });
        mcp_server.AddTool("self.led.set_emotion",
                           "设置小车灯光情绪。可选 "
                           "happy、listening、thinking、charging、working、warning、error、off。",
                           PropertyList({Property("emotion", kPropertyTypeString, "off")}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               return SetLedEmotion(
                                   ToLower(properties["emotion"].value<std::string>()), true);
                           });

        auto get_chassis_status = [this](const PropertyList&) -> ReturnValue {
            ESP_LOGI(TAG, "[MCP] chassis.get_status");
            EnsureMotorBoardReady();
            std::string response;
            if (!serial_.SendCommand("status", "{", response)) {
                return std::string("Motor board status request timed out");
            }
            return response;
        };
        mcp_server.AddTool("self.chassis.get_status", "获取小车电机、电池、灯光和 BLE 状态。",
                           PropertyList(), get_chassis_status);

        mcp_server.AddTool(
            "self.rover.action",
            "控制小车动作。action 可选 forward、back、turn_left、turn_right、stop；speed 为 80-100 "
            "的安全百分比。",
            PropertyList({Property("action", kPropertyTypeString, "stop"),
                          Property("speed", kPropertyTypeInteger, MOTOR_SPEED_DEFAULT,
                                   MOTOR_SPEED_MIN, MOTOR_SPEED_MAX)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = ToLower(properties["action"].value<std::string>());
                int speed = properties["speed"].value<int>();
                ESP_LOGI(TAG, "[MCP] rover.action action=%s speed=%d", action.c_str(), speed);

                if (action != "forward" && action != "back" && action != "turn_left" &&
                    action != "turn_right" && action != "stop") {
                    return std::string("Unsupported action: ") + action;
                }
                return DriveAction(action, speed);
            });

        mcp_server.AddTool(
            "self.rover.light",
            "控制小车灯光。mode 可选 off、on、rgb、breathe、blink、emotion；emotion 可选 "
            "happy、listening、thinking、charging、working、warning、error、off。",
            PropertyList({Property("mode", kPropertyTypeString, "off"),
                          Property("emotion", kPropertyTypeString, "off"),
                          Property("red", kPropertyTypeInteger, 0, 0, 255),
                          Property("green", kPropertyTypeInteger, 0, 0, 255),
                          Property("blue", kPropertyTypeInteger, 0, 0, 255),
                          Property("period_ms", kPropertyTypeInteger, 1000, 200, 10000)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string mode = ToLower(properties["mode"].value<std::string>());
                std::string emotion = ToLower(properties["emotion"].value<std::string>());
                int r = properties["red"].value<int>();
                int g = properties["green"].value<int>();
                int b = properties["blue"].value<int>();
                int period = properties["period_ms"].value<int>();
                ESP_LOGI(TAG, "[MCP] rover.light mode=%s emotion=%s rgb=(%d,%d,%d) period=%d",
                         mode.c_str(), emotion.c_str(), r, g, b, period);

                if (mode == "emotion") {
                    return SetLedEmotion(emotion, true);
                } else if (mode == "off" || mode == "on" || mode == "rgb" || mode == "breathe" ||
                           mode == "blink") {
                    return SetLedMode(mode, r, g, b, period);
                } else {
                    return std::string("Unsupported light mode: ") + mode;
                }
            });

        mcp_server.AddTool("self.rover.get_status",
                           "获取小车实时状态，包括电机、电池、灯光、速度限制和 BLE 状态。",
                           PropertyList(), get_chassis_status);
    }

public:
    AtomEchoS3rBaseBoard()
        : user_button_(USER_BUTTON_GPIO),
          serial_(UART_NUM_1, SERIAL_CONTROL_TX_PIN, SERIAL_CONTROL_RX_PIN, SERIAL_CONTROL_BAUD) {
        InitializeI2c();
        I2cDetect();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeSerial();
        // 丢弃共电源上电瞬间串口收到的杂散/半行数据
        serial_.FlushInput();
        InitializeBatteryTimer();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_GPIO_PA, AUDIO_CODEC_ES8311_ADDR, false);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    // 电池电量来自下位机 (BQ27220 via 串口), 由电池查询定时器刷新
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        std::lock_guard<std::mutex> lock(motor_mutex_);
        if (battery_level_ < 0) {
            return false;
        }
        level = battery_level_;
        charging = battery_charging_;
        discharging = battery_discharging_;
        return true;
    }
};

DECLARE_BOARD(AtomEchoS3rBaseBoard);
