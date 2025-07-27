#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include <wifi_station.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <cstring>
#include <string>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


#define TAG "LichuangDevBoard"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x01, 0x03);
        WriteReg(0x03, 0xf8);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557) 
        : BoxAudioCodec(i2c_bus, 
                       AUDIO_INPUT_SAMPLE_RATE, 
                       AUDIO_OUTPUT_SAMPLE_RATE,
                       AUDIO_I2S_GPIO_MCLK, 
                       AUDIO_I2S_GPIO_BCLK, 
                       AUDIO_I2S_GPIO_WS, 
                       AUDIO_I2S_GPIO_DOUT, 
                       AUDIO_I2S_GPIO_DIN,
                       GPIO_NUM_NC, 
                       AUDIO_CODEC_ES8311_ADDR, 
                       AUDIO_CODEC_ES7210_ADDR, 
                       AUDIO_INPUT_REFERENCE),
          pca9557_(pca9557) {
    }

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    LcdDisplay* display_;
    Pca9557* pca9557_;
    Esp32Camera* camera_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize PCA9557
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        pca9557_->SetOutputState(0, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_20_4,
                                        .icon_font = &font_awesome_20_4,
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
                                        .emoji_font = font_emoji_32_init(),
#else
                                        .emoji_font = font_emoji_64_init(),
#endif
                                    });
    }

    void InitializeTouch()
    {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
            .int_gpio_num = GPIO_NUM_NC, 
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 1,
                .mirror_x = 1,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        tp_io_config.scl_speed_hz = 400000;

        esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp);
        assert(tp);

        /* Add touch input (for selected screen) */
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(), 
            .handle = tp,
        };

        lvgl_port_add_touch(&touch_cfg);
    }

    void InitializeCamera() {
        // Open camera power
        pca9557_->SetOutputState(2, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;  // LEDC通道选择  用于生成XCLK时钟 但是S3不用
        config.ledc_timer = LEDC_TIMER_2; // LEDC timer选择  用于生成XCLK时钟 但是S3不用
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;   // 这里写-1 表示使用已经初始化的I2C接口
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }

    /*
        初始化UART用于外部设备控制
        使用GPIO10作为TXD，GPIO11作为RXD
        参考sparkbot的高效串口配置
    */
    void InitializeEchoUart() {
        uart_config_t uart_config = {
            .baud_rate = ECHO_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        int intr_alloc_flags = 0;

        ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));

        // 简化初始化，发送基础初始化命令
        SendUartMessage("INIT");
        
        ESP_LOGI(TAG, "UART初始化完成，TXD:%d, RXD:%d", UART_ECHO_TXD, UART_ECHO_RXD);
    }

    // 优化的串口命令发送方法，参考sparkbot只发送不接收的高效实现
    std::string SendUartCommand(const char* command_str, int timeout_ms = 1000) {
        if (!command_str) {
            return "ERROR: 命令为空";
        }

        // 发送命令，减少字符串拷贝
        uint8_t len = strlen(command_str);
        uart_write_bytes(ECHO_UART_PORT_NUM, command_str, len);
        
        // 可选：添加换行符，根据设备需求决定
        if (len > 0 && command_str[len-1] != '\n') {
            uart_write_bytes(ECHO_UART_PORT_NUM, "\r\n", 2);
        }
        
        // 等待发送完成
        uart_wait_tx_done(ECHO_UART_PORT_NUM, pdMS_TO_TICKS(100));
        
        // 参考sparkbot，只发送不接收，直接返回成功状态
        return "OK: 命令已发送";
    }

    // 简化的串口消息发送，参考sparkbot实现
    void SendUartMessage(const char * command_str) {
        if (!command_str) return;
        
        uint8_t len = strlen(command_str);
        uart_write_bytes(ECHO_UART_PORT_NUM, command_str, len);
        ESP_LOGI(TAG, "Sent command: %s", command_str);
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        ESP_LOGI(TAG, "开始注册Lichuang Dev MCP工具...");

        // 基础控制指令 - 参考sparkbot只发送不等待响应
        mcp_server.AddTool("self.device.send_command", "发送自定义命令到外部设备", 
            PropertyList({Property("command", kPropertyTypeString, "")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string command = properties["command"].value<std::string>();
                if (command.empty()) {
                    return "错误：命令不能为空";
                }
                
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                return "命令: " + command + " | 状态: " + response;
            });

        // Otto机器人标准命令实现
        
        // 初始化命令
        mcp_server.AddTool("self.device.init", "初始化机器人", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string response = SendUartCommand("INIT");  // 移除超时参数
                return "初始化完成 | 状态: " + response;
            });

        // 回到初始位置
        mcp_server.AddTool("self.device.home", "回到初始位置", 
            PropertyList({Property("hands_down", kPropertyTypeInteger, 1, 0, 1)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int hands_down = properties["hands_down"].value<int>();
                std::string command = "HOME " + std::to_string(hands_down);
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                return "回到初始位置 | 状态: " + response;
            });

        // 运动控制 - 参考sparkbot只发送不等待响应
        mcp_server.AddTool("self.device.walk", "机器人行走", 
            PropertyList({
                Property("steps", kPropertyTypeInteger, 2, 1, 10),
                Property("speed", kPropertyTypeInteger, 1000, 500, 3000),
                Property("direction", kPropertyTypeInteger, 1, -1, 1),  // 1=前进, -1=后退
                Property("amount", kPropertyTypeInteger, 30, 0, 50)     // 手臂摆动幅度
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                int direction = properties["direction"].value<int>();
                int amount = properties["amount"].value<int>();
                
                std::string command = "WALK " + std::to_string(steps) + " " + 
                                    std::to_string(speed) + " " + 
                                    std::to_string(direction) + " " + 
                                    std::to_string(amount);
                
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                std::string direction_str = (direction == 1) ? "前进" : "后退";
                return direction_str + std::to_string(steps) + "步 | 状态: " + response;
            });

        mcp_server.AddTool("self.device.move_forward", "设备前进", 
            PropertyList({
                Property("steps", kPropertyTypeInteger, 3, 1, 10),
                Property("speed", kPropertyTypeInteger, 1200, 500, 3000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                std::string command = "WALK " + std::to_string(steps) + " " + 
                                    std::to_string(speed) + " 1 30";
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                return "前进" + std::to_string(steps) + "步 | 状态: " + response;
            });

        mcp_server.AddTool("self.device.move_backward", "设备后退", 
            PropertyList({
                Property("steps", kPropertyTypeInteger, 3, 1, 10),
                Property("speed", kPropertyTypeInteger, 1200, 500, 3000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                std::string command = "WALK " + std::to_string(steps) + " " + 
                                    std::to_string(speed) + " -1 30";
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                return "后退" + std::to_string(steps) + "步 | 状态: " + response;
            });

        // 转向控制
        mcp_server.AddTool("self.device.turn", "机器人转向", 
            PropertyList({
                Property("steps", kPropertyTypeInteger, 1, 1, 5),
                Property("speed", kPropertyTypeInteger, 2000, 1000, 3000),
                Property("direction", kPropertyTypeInteger, 1, -1, 1),  // 1=左转, -1=右转
                Property("amount", kPropertyTypeInteger, 0, 0, 50)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                int direction = properties["direction"].value<int>();
                int amount = properties["amount"].value<int>();
                
                std::string command = "TURN " + std::to_string(steps) + " " + 
                                    std::to_string(speed) + " " + 
                                    std::to_string(direction) + " " + 
                                    std::to_string(amount);
                
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                std::string direction_str = (direction == 1) ? "左转" : "右转";
                return direction_str + std::to_string(steps) + "步 | 状态: " + response;
            });

        mcp_server.AddTool("self.device.turn_left", "设备左转", 
            PropertyList({
                Property("steps", kPropertyTypeInteger, 2, 1, 5),
                Property("speed", kPropertyTypeInteger, 2000, 1000, 3000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                std::string command = "TURN " + std::to_string(steps) + " " + 
                                    std::to_string(speed) + " 1 0";
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                return "左转" + std::to_string(steps) + "步 | 状态: " + response;
            });

        mcp_server.AddTool("self.device.turn_right", "设备右转", 
            PropertyList({
                Property("steps", kPropertyTypeInteger, 2, 1, 5),
                Property("speed", kPropertyTypeInteger, 2000, 1000, 3000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                std::string command = "TURN " + std::to_string(steps) + " " + 
                                    std::to_string(speed) + " -1 0";
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                return "右转" + std::to_string(steps) + "步 | 状态: " + response;
            });

        // 其他动作
        mcp_server.AddTool("self.device.jump", "机器人跳跃", 
            PropertyList({
                Property("steps", kPropertyTypeInteger, 1, 1, 3),
                Property("speed", kPropertyTypeInteger, 2000, 1000, 3000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                std::string command = "JUMP " + std::to_string(steps) + " " + std::to_string(speed);
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                return "跳跃" + std::to_string(steps) + "次 | 状态: " + response;
            });

        mcp_server.AddTool("self.device.swing", "机器人摇摆", 
            PropertyList({
                Property("steps", kPropertyTypeInteger, 1, 1, 5),
                Property("speed", kPropertyTypeInteger, 1000, 500, 2000),
                Property("height", kPropertyTypeInteger, 20, 10, 50)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                int height = properties["height"].value<int>();
                std::string command = "SWING " + std::to_string(steps) + " " + 
                                    std::to_string(speed) + " " + 
                                    std::to_string(height);
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                return "摇摆" + std::to_string(steps) + "次 | 状态: " + response;
            });

        // 手部动作
        mcp_server.AddTool("self.device.hands_up", "举手动作", 
            PropertyList({
                Property("speed", kPropertyTypeInteger, 1000, 500, 2000),
                Property("direction", kPropertyTypeInteger, 0, -1, 1)  // 0=双手, 1=左手, -1=右手
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed"].value<int>();
                int direction = properties["direction"].value<int>();
                std::string command = "HANDS_UP " + std::to_string(speed) + " " + std::to_string(direction);
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                
                std::string dir_str = (direction == 0) ? "双手" : ((direction == 1) ? "左手" : "右手");
                return dir_str + "举起 | 状态: " + response;
            });

        mcp_server.AddTool("self.device.hands_down", "放手动作", 
            PropertyList({
                Property("speed", kPropertyTypeInteger, 1000, 500, 2000),
                Property("direction", kPropertyTypeInteger, 0, -1, 1)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed"].value<int>();
                int direction = properties["direction"].value<int>();
                std::string command = "HANDS_DOWN " + std::to_string(speed) + " " + std::to_string(direction);
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                
                std::string dir_str = (direction == 0) ? "双手" : ((direction == 1) ? "左手" : "右手");
                return dir_str + "放下 | 状态: " + response;
            });

        mcp_server.AddTool("self.device.hand_wave", "挥手动作", 
            PropertyList({
                Property("speed", kPropertyTypeInteger, 1000, 500, 2000),
                Property("direction", kPropertyTypeInteger, 1, -1, 1)  // 1=左手, -1=右手, 0=双手
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed"].value<int>();
                int direction = properties["direction"].value<int>();
                std::string command = "HAND_WAVE " + std::to_string(speed) + " " + std::to_string(direction);
                std::string response = SendUartCommand(command.c_str());  // 移除超时参数
                
                std::string dir_str = (direction == 1) ? "左手" : ((direction == -1) ? "右手" : "双手");
                return dir_str + "挥手 | 状态: " + response;
            });

        // 停止和状态查询 - 参考sparkbot只发送不等待响应
        mcp_server.AddTool("self.device.stop", "设备停止", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string response = SendUartCommand("STOP");  // 移除超时参数
                return "停止命令已发送 | 状态: " + response;
            });

        mcp_server.AddTool("self.device.get_status", "获取设备状态", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string response = SendUartCommand("GET_STATUS");  // 移除超时参数
                return "设备状态 | 状态: " + response;
            });
            
        ESP_LOGI(TAG, "Lichuang Dev MCP工具注册完成，共注册%d个工具", 16);
    }

public:
    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeTouch();
        InitializeButtons();
        InitializeCamera();
        InitializeEchoUart();
        InitializeTools();

        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(LichuangDevBoard);
