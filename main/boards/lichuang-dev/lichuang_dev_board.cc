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
        参考otto_serial_test.py的串口配置
    */
    void InitializeEchoUart() {
        ESP_LOGI(TAG, "开始初始化UART串口通讯...");
        
        uart_config_t uart_config = {
            .baud_rate = ECHO_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        int intr_alloc_flags = 0;

        ESP_LOGI(TAG, "UART配置 - 波特率: %d, 数据位: 8, 停止位: 1, 校验: 无", ECHO_UART_BAUD_RATE);
        
        ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
        ESP_LOGI(TAG, "UART驱动安装完成，接收缓冲区大小: %d字节", BUF_SIZE * 2);
        
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_LOGI(TAG, "UART参数配置完成");
        
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));
        ESP_LOGI(TAG, "UART引脚配置完成 - TXD: GPIO%d, RXD: GPIO%d", UART_ECHO_TXD, UART_ECHO_RXD);

        // 发送初始化命令并等待响应
        ESP_LOGI(TAG, "发送初始化命令到外部设备...");
        std::string init_response = SendUartCommand("INIT", 5000);
        
        if (init_response.find("TIMEOUT") != std::string::npos) {
            ESP_LOGW(TAG, "外部设备初始化超时，可能设备未连接或未准备好");
        } else if (init_response.find("OK") != std::string::npos) {
            ESP_LOGI(TAG, "外部设备初始化成功");
        } else {
            ESP_LOGW(TAG, "外部设备响应异常: %s", init_response.c_str());
        }
        
        ESP_LOGI(TAG, "✅ UART初始化完成，TXD:%d, RXD:%d", UART_ECHO_TXD, UART_ECHO_RXD);
    }

    std::string SendUartCommand(const char* command_str, int timeout_ms = 2000) {
        if (!command_str) {
            ESP_LOGE(TAG, "命令字符串为空");
            return "ERROR: 命令为空";
        }

        // 清空接收缓冲区
        uart_flush(ECHO_UART_PORT_NUM);
        
        // 构造带换行符的命令
        std::string full_command = std::string(command_str) + "\r\n";
        uint8_t len = full_command.length();
        
        ESP_LOGI(TAG, "发送UART命令: [%s] (长度: %d)", command_str, len);
        
        // 发送命令
        int written = uart_write_bytes(ECHO_UART_PORT_NUM, full_command.c_str(), len);
        if (written != len) {
            ESP_LOGE(TAG, "UART写入失败，期望: %d, 实际: %d", len, written);
            return "ERROR: 写入失败";
        }
        
        // 等待数据发送完成
        ESP_ERROR_CHECK(uart_wait_tx_done(ECHO_UART_PORT_NUM, pdMS_TO_TICKS(1000)));
        ESP_LOGD(TAG, "命令发送完成，等待响应...");
        
        // 等待响应
        std::string response;
        uint8_t data[256];
        int total_len = 0;
        TickType_t start_time = xTaskGetTickCount();
        TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
        
        while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
            int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, sizeof(data) - 1, pdMS_TO_TICKS(100));
            
            if (len > 0) {
                data[len] = '\0';
                response += (char*)data;
                total_len += len;
                
                ESP_LOGD(TAG, "接收到数据片段: [%s] (长度: %d)", (char*)data, len);
                
                // 检查是否接收到完整响应（以\n结尾）
                if (response.find('\n') != std::string::npos) {
                    break;
                }
                
                // 防止缓冲区溢出
                if (total_len >= 512) {
                    ESP_LOGW(TAG, "响应数据过长，截断处理");
                    break;
                }
            }
        }
        
        // 清理响应字符串
        if (!response.empty()) {
            // 移除换行符和回车符
            response.erase(std::remove(response.begin(), response.end(), '\r'), response.end());
            response.erase(std::remove(response.begin(), response.end(), '\n'), response.end());
            
            ESP_LOGI(TAG, "接收到响应: [%s] (总长度: %d)", response.c_str(), total_len);
            
            // 分析响应状态
            if (response.find("OK:") == 0) {
                ESP_LOGI(TAG, "✅ 命令执行成功");
            } else if (response.find("ERROR:") == 0) {
                ESP_LOGE(TAG, "❌ 命令执行失败: %s", response.c_str());
            } else if (response.empty()) {
                ESP_LOGW(TAG, "⚠️ 无响应或超时");
                response = "TIMEOUT: 无响应";
            } else {
                ESP_LOGI(TAG, "📝 收到数据: %s", response.c_str());
            }
        } else {
            ESP_LOGW(TAG, "⚠️ 超时无响应 (超时时间: %dms)", timeout_ms);
            response = "TIMEOUT: 无响应";
        }
        
        return response;
    }

    void SendUartMessage(const char * command_str) {
        SendUartCommand(command_str, 1000);  // 保持向后兼容
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        ESP_LOGI(TAG, "开始注册Lichuang Dev MCP工具...");

        // 基础控制指令 - 支持自定义命令
        mcp_server.AddTool("self.device.send_command", "发送自定义命令到外部设备", 
            PropertyList({Property("command", kPropertyTypeString, "")}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string command = properties["command"].value<std::string>();
                if (command.empty()) {
                    return "错误：命令不能为空";
                }
                
                std::string response = SendUartCommand(command.c_str(), 3000);
                return "命令: " + command + " | 响应: " + response;
            });

        // Otto机器人标准命令实现
        
        // 初始化命令
        mcp_server.AddTool("self.device.init", "初始化机器人", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string response = SendUartCommand("INIT", 3000);
                return "初始化完成 | 响应: " + response;
            });

        // 回到初始位置
        mcp_server.AddTool("self.device.home", "回到初始位置", 
            PropertyList({Property("hands_down", kPropertyTypeInteger, 1, 0, 1)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int hands_down = properties["hands_down"].value<int>();
                std::string command = "HOME " + std::to_string(hands_down);
                std::string response = SendUartCommand(command.c_str(), 3000);
                return "回到初始位置 | 响应: " + response;
            });

        // 运动控制
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
                
                std::string response = SendUartCommand(command.c_str(), 5000);
                std::string direction_str = (direction == 1) ? "前进" : "后退";
                return direction_str + std::to_string(steps) + "步 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 5000);
                return "前进" + std::to_string(steps) + "步 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 5000);
                return "后退" + std::to_string(steps) + "步 | 响应: " + response;
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
                
                std::string response = SendUartCommand(command.c_str(), 5000);
                std::string direction_str = (direction == 1) ? "左转" : "右转";
                return direction_str + std::to_string(steps) + "步 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 5000);
                return "左转" + std::to_string(steps) + "步 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 5000);
                return "右转" + std::to_string(steps) + "步 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 4000);
                return "跳跃" + std::to_string(steps) + "次 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 4000);
                return "摇摆" + std::to_string(steps) + "次 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 3000);
                
                std::string dir_str = (direction == 0) ? "双手" : ((direction == 1) ? "左手" : "右手");
                return dir_str + "举起 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 3000);
                
                std::string dir_str = (direction == 0) ? "双手" : ((direction == 1) ? "左手" : "右手");
                return dir_str + "放下 | 响应: " + response;
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
                std::string response = SendUartCommand(command.c_str(), 4000);
                
                std::string dir_str = (direction == 1) ? "左手" : ((direction == -1) ? "右手" : "双手");
                return dir_str + "挥手 | 响应: " + response;
            });

        // 停止和状态查询
        mcp_server.AddTool("self.device.stop", "设备停止", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string response = SendUartCommand("STOP", 2000);
                return "停止命令已发送 | 响应: " + response;
            });

        mcp_server.AddTool("self.device.get_status", "获取设备状态", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string response = SendUartCommand("GET_STATUS", 3000);
                return "设备状态 | 响应: " + response;
            });

        // 舵机控制 - 兼容旧接口
        mcp_server.AddTool("self.device.move_servo", "控制单个舵机", 
            PropertyList({
                Property("servo", kPropertyTypeInteger, 1, 1, 8),
                Property("position", kPropertyTypeInteger, 90, 0, 180)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int servo = properties["servo"].value<int>();
                int position = properties["position"].value<int>();
                std::string command = "SERVO_MOVE " + std::to_string(servo) + " " + std::to_string(position);
                std::string response = SendUartCommand(command.c_str(), 2000);
                return "舵机" + std::to_string(servo) + "移动到" + std::to_string(position) + "° | 响应: " + response;
            });

        // 摄像头控制 - 参考esp-sparkbot的实现
        mcp_server.AddTool("self.camera.set_camera_flipped", "翻转摄像头图像方向", PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
                Settings settings("lichuang_dev", true);
                bool flipped = !static_cast<bool>(settings.GetInt("camera-flipped", 0));
                
                camera_->SetHMirror(flipped);
                camera_->SetVFlip(flipped);
                
                settings.SetInt("camera-flipped", flipped ? 1 : 0);
                
                ESP_LOGI(TAG, "摄像头翻转状态设置为: %s", flipped ? "已翻转" : "正常");
                return "摄像头翻转状态: " + std::string(flipped ? "已翻转" : "正常");
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
