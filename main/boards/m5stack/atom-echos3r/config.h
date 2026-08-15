#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// AtomEchoS3R Board configuration
// Verdure Buddy Rover variant: 增加 1.47" ST7789 172x320 屏幕 + Grove 串口控制

#include <driver/gpio.h>

#define AUDIO_INPUT_REFERENCE true
#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_11
#define AUDIO_I2S_GPIO_WS GPIO_NUM_3
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_17
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_4
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_48

#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_45
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_0
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_GPIO_PA GPIO_NUM_18

#define BUILTIN_LED_GPIO GPIO_NUM_NC
#define USER_BUTTON_GPIO GPIO_NUM_41
#define VOLUME_UP_BUTTON_GPIO GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

// ---- Display: 1.47" ST7789 172x320 SPI (底部 GPIO G5/G6/G7/G8/G38/G39) ----
#define DISPLAY_CS_PIN GPIO_NUM_5
#define DISPLAY_MOSI_PIN GPIO_NUM_6
#define DISPLAY_SCLK_PIN GPIO_NUM_7
#define DISPLAY_DC_PIN GPIO_NUM_8
#define DISPLAY_RESET_PIN GPIO_NUM_38
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_39

#define DISPLAY_SPI_SCLK_HZ (40 * 1000 * 1000)
#define DISPLAY_SPI_MODE 0

#define LCD_TYPE_ST7789_SERIAL
// 横屏 320x172（swap_xy 把 172x320 竖屏面板旋转为横屏）
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 172
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY true
#define DISPLAY_INVERT_COLOR true
#define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
// ST7789 GRAM 240x320，横屏后宽(320)用满 GRAM 高，高(172)对应 GRAM 宽居中段
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 34
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// ---- Serial command control (Grove G1/G2 = GPIO1/GPIO2) ----
// 下位机: ESP32-C3 电机板 (esp32c3_kb_motor.ino, DRV8833 + BQ27220 + RGB)
#define SERIAL_CONTROL_TX_PIN GPIO_NUM_1  // G1
#define SERIAL_CONTROL_RX_PIN GPIO_NUM_2  // G2
#define SERIAL_CONTROL_BAUD 115200

// ---- Chassis / Light / Battery (与下位机协议对应) ----
#define MOTOR_SPEED_MAX 80
#define MOTOR_SPEED_DEFAULT 70
#define MOTOR_SPEED_MIN 0
// MCP 速度(0-100) -> 下位机 PWM(0-255) 缩放
#define MOTOR_PWM_SCALE(x) (((x) * 255) / 100)

// 下位机主动上报电池周期(ms), 上位机也按其节奏轮询
#define BATTERY_QUERY_INTERVAL_MS 5000

// ---- 上电时序加固: 上下位机共电源复位不同步 ----
// 上位机启动后延迟多久才开始与下位机通信(ms)。
// 下位机 setup() 含 BLE 键盘/BQ27220 初始化, 可能耗时数秒, 需留足时间。
#define MOTOR_BOARD_STARTUP_DELAY_MS 8000
// 发送关键命令前用 ping 探活, 失败等待间隔(ms)
#define MOTOR_BOARD_PING_RETRY_MS 1000
// 等待下位机 READY 的最大探活次数 (超过则强制继续, 避免永久卡住)
#define MOTOR_BOARD_READY_MAX_PINGS 25

#endif  // _BOARD_CONFIG_H_
