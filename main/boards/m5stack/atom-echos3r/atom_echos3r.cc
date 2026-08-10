#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "i2c_device.h"
#include "serial_control.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>

#define TAG "AtomEchoS3R"

class AtomEchoS3rBaseBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    LcdDisplay* display_ = nullptr;
    SerialControl serial_;

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
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void SendHelp() {
        serial_.SendLine("=== AtomEchoS3R (Verdure Buddy Rover) ===");
        serial_.SendLine("Commands (case-insensitive):");
        serial_.SendLine("  status                 - show device state & volume");
        serial_.SendLine("  toggle                 - toggle chat/listening");
        serial_.SendLine("  wake [word]            - trigger wake word");
        serial_.SendLine("  listen                 - start listening");
        serial_.SendLine("  stop                   - stop listening");
        serial_.SendLine("  volume [0-100]         - get/set output volume");
        serial_.SendLine("  screen <text>          - show message on LCD");
        serial_.SendLine("  backlight [0-100]      - get/set backlight");
        serial_.SendLine("  reboot                 - restart the device");
        serial_.SendLine("  help / ?               - this help");
    }

    static std::string ToLower(std::string s) {
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c += ('a' - 'A');
            }
        }
        return s;
    }

    void HandleSerialCommand(const std::string& line) {
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
            serial_.SendLine(buf);
            serial_.SendLine(GetDeviceStatusJson().c_str());
        } else if (cmd == "toggle") {
            app.ToggleChatState();
            serial_.SendLine("OK toggle");
        } else if (cmd == "wake") {
            std::string word = arg.empty() ? "你好小智" : arg;
            app.WakeWordInvoke(word);
            serial_.SendLine("OK wake");
        } else if (cmd == "listen") {
            app.StartListening();
            serial_.SendLine("OK listen");
        } else if (cmd == "stop") {
            app.StopListening();
            serial_.SendLine("OK stop");
        } else if (cmd == "volume") {
            if (arg.empty() && codec) {
                char buf[32];
                snprintf(buf, sizeof(buf), "volume=%d", codec->output_volume());
                serial_.SendLine(buf);
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
                serial_.SendLine(buf);
            }
        } else if (cmd == "screen") {
            if (display_ && !arg.empty()) {
                display_->ShowNotification(arg.c_str(), 5000);
                serial_.SendLine("OK screen");
            } else {
                serial_.SendLine("ERR empty text");
            }
        } else if (cmd == "backlight") {
            Backlight* bl = GetBacklight();
            if (!bl) {
                serial_.SendLine("ERR no backlight");
            } else if (arg.empty()) {
                char buf[32];
                snprintf(buf, sizeof(buf), "backlight=%d", bl->brightness());
                serial_.SendLine(buf);
            } else {
                int v = atoi(arg.c_str());
                if (v < 0)
                    v = 0;
                if (v > 100)
                    v = 100;
                bl->SetBrightness((uint8_t)v);
                char buf[32];
                snprintf(buf, sizeof(buf), "OK backlight=%d", v);
                serial_.SendLine(buf);
            }
        } else if (cmd == "reboot") {
            serial_.SendLine("OK rebooting");
            vTaskDelay(pdMS_TO_TICKS(100));
            app.Reboot();
        } else {
            serial_.SendLine("ERR unknown command (type help)");
        }
    }

    void InitializeSerial() {
        serial_.SetHelpHandler([this]() { SendHelp(); });
        serial_.SetLineHandler([this](const std::string& line) { HandleSerialCommand(line); });
        serial_.Start();
        SendHelp();
    }

public:
    AtomEchoS3rBaseBoard()
        : boot_button_(USER_BUTTON_GPIO),
          serial_(UART_NUM_1, SERIAL_CONTROL_TX_PIN, SERIAL_CONTROL_RX_PIN, SERIAL_CONTROL_BAUD) {
        InitializeI2c();
        I2cDetect();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeSerial();
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
};

DECLARE_BOARD(AtomEchoS3rBaseBoard);
