#include "application.h"
#include "assistant_led_animation.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "led/gpio_led.h"
#include "wifi_board.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <led_strip.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace {

constexpr char kTag[] = "XiaoReSpeakerXVF3800";
constexpr gpio_num_t kLedPin = GPIO_NUM_21;
constexpr gpio_num_t kLedStripPin = GPIO_NUM_1;
constexpr gpio_num_t kBootButtonPin = GPIO_NUM_0;
constexpr gpio_num_t kUserButtonPin = GPIO_NUM_4;
constexpr gpio_num_t kI2cSdaPin = GPIO_NUM_5;
constexpr gpio_num_t kI2cSclPin = GPIO_NUM_6;
constexpr gpio_num_t kI2sBclkPin = GPIO_NUM_8;
constexpr gpio_num_t kI2sWsPin = GPIO_NUM_7;
constexpr gpio_num_t kI2sDoutPin = GPIO_NUM_44;
constexpr gpio_num_t kI2sDinPin = GPIO_NUM_43;
constexpr uint8_t kXvf3800I2cAddress = 0x2C;
constexpr uint8_t kBq27220I2cAddress = 0x55;
constexpr uint16_t kBq27220DeviceNumber = 0x0220;
constexpr uint16_t kBq27220DeviceNumberCommand = 0x0001;
constexpr uint8_t kBq27220ControlRegister = 0x00;
constexpr uint8_t kBq27220VoltageRegister = 0x08;
constexpr uint8_t kBq27220CurrentRegister = 0x0C;
constexpr uint8_t kBq27220StateOfChargeRegister = 0x2C;
constexpr uint8_t kBq27220DesignCapacityRegister = 0x3C;
constexpr uint8_t kBq27220MacDataRegister = 0x40;
constexpr uint16_t kBatteryDesignCapacityMah = 1200;
constexpr int kI2cDeviceClockHz = 100 * 1000;
constexpr int kI2cTimeoutMs = 100;
constexpr int64_t kBatteryLogIntervalUs = 5000000;
#if CONFIG_RESPEAKER_I2S_EXTERNAL_CLOCK_48K
constexpr int kAudioSampleRate = 48000;
constexpr i2s_role_t kI2sRole = I2S_ROLE_SLAVE;
constexpr char kI2sRoleName[] = "slave (external clocks)";
#else
constexpr int kAudioSampleRate = 16000;
constexpr i2s_role_t kI2sRole = I2S_ROLE_MASTER;
constexpr char kI2sRoleName[] = "master";
#endif

#if CONFIG_RESPEAKER_I2S_CLOCK_PROBE_ONLY
[[noreturn]] void ProbeExternalI2sClock() {
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << kI2sBclkPin) | (1ULL << kI2sWsPin);
    config.mode = GPIO_MODE_INPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_LOGW(kTag, "CLOCK PROBE ONLY: GPIO8 BCLK and GPIO7 WS are inputs; I2S/chat disabled");
    ESP_LOGW(kTag, "Polling reports approximate WS frequency, not a precise clock measurement");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int previous_ws = gpio_get_level(kI2sWsPin);
        int previous_bclk = gpio_get_level(kI2sBclkPin);
        uint32_t ws_rising_edges = 0;
        uint32_t bclk_edges = 0;
        const int64_t start = esp_timer_get_time();
        int64_t elapsed = 0;
        do {
            const int ws = gpio_get_level(kI2sWsPin);
            const int bclk = gpio_get_level(kI2sBclkPin);
            ws_rising_edges += ws != 0 && previous_ws == 0;
            bclk_edges += bclk != previous_bclk;
            previous_ws = ws;
            previous_bclk = bclk;
            elapsed = esp_timer_get_time() - start;
        } while (elapsed < 20000);

        ESP_LOGI(kTag,
                 // %lld is a 64-bit conversion, which newlib-nano's printf does not have,
                 // and CONFIG_NEWLIB_NANO_FORMAT=y is in this project's sdkconfig.defaults.
                 // The do-while above bounds this window at 20000 us, so int is ample.
                 "CLOCK PROBE: window_us=%d ws_rises=%lu approx_ws_hz=%lu "
                 "sampled_bclk_edges=%lu WS=%d BCLK=%d",
                 static_cast<int>(elapsed), static_cast<unsigned long>(ws_rising_edges),
                 static_cast<unsigned long>(ws_rising_edges * 1000000ULL / elapsed),
                 static_cast<unsigned long>(bclk_edges), previous_ws, previous_bclk);
    }
}
#endif

void ProbeXvf3800(i2c_master_bus_handle_t& bus) {
    if (bus == nullptr) {
        i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = kI2cSdaPin,
            .scl_io_num = kI2cSclPin,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = true,
                },
        };

        const esp_err_t err = i2c_new_master_bus(&config, &bus);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Could not initialize I2C bus: %s", esp_err_to_name(err));
            return;
        }
    }

    const esp_err_t err = i2c_master_probe(bus, kXvf3800I2cAddress, 200);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "XVF3800 detected at I2C address 0x%02X", kXvf3800I2cAddress);
    } else {
        ESP_LOGW(kTag, "XVF3800 not detected at I2C address 0x%02X: %s", kXvf3800I2cAddress,
                 esp_err_to_name(err));
    }
}

class XiaoEsp32S3ReSpeakerXvf3800Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t battery_i2c_device_ = nullptr;
    std::mutex battery_mutex_;
    int64_t last_battery_error_us_ = -kBatteryLogIntervalUs;
    uint16_t battery_gauge_design_capacity_mah_ = 0;
    uint16_t battery_voltage_mv_ = 0;
    int battery_current_ma_ = 0;
    Button boot_button_{kBootButtonPin};
    Button user_button_{kUserButtonPin};
    respeaker::AudioLevel input_level_;
    respeaker::AudioLevel output_level_;

    class BoardLed final : public Led {
    private:
        GpioLed onboard_led_{kLedPin, 1};
        led_strip_handle_t led_strip_ = nullptr;
        respeaker::AudioLevel& input_level_;
        respeaker::AudioLevel& output_level_;
        std::atomic<DeviceState> state_{kDeviceStateStarting};
        std::atomic<bool> stopping_{false};
        SemaphoreHandle_t stopped_ = nullptr;

        void Run() {
            respeaker::AssistantLedAnimation animation;
            TickType_t next_frame = xTaskGetTickCount();
            while (!stopping_.load()) {
                const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
                const auto frame = animation.Render(state_.load(), now_ms, input_level_.Get(now_ms),
                                                    output_level_.Get(now_ms));
                for (size_t i = 0; i < frame.size(); ++i) {
                    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip_, i, frame[i].red, frame[i].green,
                                                        frame[i].blue));
                }
                ESP_ERROR_CHECK(led_strip_refresh(led_strip_));
                vTaskDelayUntil(&next_frame, pdMS_TO_TICKS(25));
            }
            xSemaphoreGive(stopped_);
            vTaskDelete(nullptr);
        }

    public:
        BoardLed(respeaker::AudioLevel& input_level, respeaker::AudioLevel& output_level)
            : input_level_(input_level), output_level_(output_level) {
            led_strip_config_t strip_config = {};
            strip_config.strip_gpio_num = kLedStripPin;
            strip_config.max_leds = respeaker::AssistantLedAnimation::kPixelCount;
            strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
            strip_config.led_model = LED_MODEL_WS2812;
            led_strip_rmt_config_t rmt_config = {};
            rmt_config.resolution_hz = 10 * 1000 * 1000;
            rmt_config.flags.with_dma = true;
            ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
            ESP_ERROR_CHECK(led_strip_clear(led_strip_));
            stopped_ = xSemaphoreCreateBinary();
            ESP_ERROR_CHECK(stopped_ == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
            const BaseType_t result =
                xTaskCreate([](void* arg) { static_cast<BoardLed*>(arg)->Run(); }, "assistant_led",
                            4096, this, 1, nullptr);
            ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        }

        ~BoardLed() override {
            stopping_.store(true);
            xSemaphoreTake(stopped_, portMAX_DELAY);
            vSemaphoreDelete(stopped_);
            ESP_ERROR_CHECK(led_strip_clear(led_strip_));
            ESP_ERROR_CHECK(led_strip_del(led_strip_));
        }

        void OnStateChanged() override {
            state_.store(Application::GetInstance().GetDeviceState());
            onboard_led_.OnStateChanged();
        }
    };

    void HandleConversationButtonClick(const char* button_name) {
        ESP_LOGI(kTag, "%s clicked", button_name);
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    esp_err_t ReadBatteryWord(uint8_t reg, uint16_t& value) {
        uint8_t data[2] = {};
        const esp_err_t err = i2c_master_transmit_receive(battery_i2c_device_, &reg, sizeof(reg),
                                                          data, sizeof(data), kI2cTimeoutMs);
        if (err != ESP_OK) {
            return err;
        }
        value = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
        return ESP_OK;
    }

    esp_err_t VerifyBq27220Device() {
        uint16_t voltage_mv = 0;
        uint16_t state_of_charge = 0;
        uint16_t design_capacity_mah = 0;
        esp_err_t err = ReadBatteryWord(kBq27220VoltageRegister, voltage_mv);
        if (err == ESP_OK) {
            err = ReadBatteryWord(kBq27220StateOfChargeRegister, state_of_charge);
        }
        if (err == ESP_OK) {
            err = ReadBatteryWord(kBq27220DesignCapacityRegister, design_capacity_mah);
        }
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "BQ27220 initial telemetry read failed: %s", esp_err_to_name(err));
            return err;
        }
        if (voltage_mv < 1500 || voltage_mv > 6000 || state_of_charge > 100) {
            ESP_LOGW(kTag, "BQ27220 initial telemetry invalid: voltage=%u mV SOC=%u", voltage_mv,
                     state_of_charge);
            return ESP_ERR_NOT_SUPPORTED;
        }

        const uint8_t device_number_command[] = {
            kBq27220ControlRegister,
            static_cast<uint8_t>(kBq27220DeviceNumberCommand & 0xFF),
            static_cast<uint8_t>(kBq27220DeviceNumberCommand >> 8),
        };
        err = i2c_master_transmit(battery_i2c_device_, device_number_command,
                                  sizeof(device_number_command), kI2cTimeoutMs);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "BQ27220 device number request failed: %s", esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(15));

        uint16_t device_number = 0;
        err = ReadBatteryWord(kBq27220MacDataRegister, device_number);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "BQ27220 device number response failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(kTag, "BQ27220 identification: device=0x%04X voltage=%u mV SOC=%u design=%u mAh",
                 device_number, voltage_mv, state_of_charge, design_capacity_mah);
        if (device_number != kBq27220DeviceNumber) {
            ESP_LOGW(kTag, "Unexpected device at 0x%02X: expected 0x%04X, received 0x%04X",
                     kBq27220I2cAddress, kBq27220DeviceNumber, device_number);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (design_capacity_mah != kBatteryDesignCapacityMah) {
            ESP_LOGW(kTag,
                     "BQ27220 profile mismatch: design=%u mAh, expected=%u mAh; "
                     "reporting reference SOC only, gauge configuration unchanged",
                     design_capacity_mah, kBatteryDesignCapacityMah);
        }
        battery_gauge_design_capacity_mah_ = design_capacity_mah;
        return ESP_OK;
    }

    void InitializeBatteryMonitor() {
        if (i2c_bus_ == nullptr) {
            ESP_LOGW(kTag, "I2C bus unavailable; optional BQ27220 battery monitor disabled");
            return;
        }

        ESP_LOGI(kTag, "BQ27220 monitor v3: SDA=GPIO%d SCL=GPIO%d address=0x%02X probe=100 ms",
                 static_cast<int>(kI2cSdaPin), static_cast<int>(kI2cSclPin), kBq27220I2cAddress);
        const esp_err_t probe_err = i2c_master_probe(i2c_bus_, kBq27220I2cAddress, kI2cTimeoutMs);
        if (probe_err != ESP_OK) {
            if (probe_err == ESP_ERR_NOT_FOUND) {
                ESP_LOGI(kTag,
                         "Optional BQ27220 at 0x%02X did not ACK; continuing without battery data",
                         kBq27220I2cAddress);
            } else {
                ESP_LOGW(kTag,
                         "BQ27220 probe failed at 0x%02X: %s; check module power, SDA/SCL and "
                         "3.3 V pull-ups; continuing without battery data",
                         kBq27220I2cAddress, esp_err_to_name(probe_err));
            }
            return;
        }

        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = kBq27220I2cAddress,
            .scl_speed_hz = kI2cDeviceClockHz,
            .scl_wait_us = 0,
            .flags =
                {
                    .disable_ack_check = 0,
                },
        };
        esp_err_t err = i2c_master_bus_add_device(i2c_bus_, &config, &battery_i2c_device_);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Could not attach optional BQ27220 at 0x%02X: %s", kBq27220I2cAddress,
                     esp_err_to_name(err));
            battery_i2c_device_ = nullptr;
            return;
        }

        err = VerifyBq27220Device();
        if (err != ESP_OK) {
            ESP_LOGI(kTag, "No usable BQ27220 at 0x%02X; battery monitor disabled: %s",
                     kBq27220I2cAddress, esp_err_to_name(err));
            ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_bus_rm_device(battery_i2c_device_));
            battery_i2c_device_ = nullptr;
            return;
        }

        ESP_LOGI(kTag,
                 "BQ27220 detected at 0x%02X for %u mAh cell; using read-only monitoring "
                 "(reference SOC, battery profile unchanged)",
                 kBq27220I2cAddress, kBatteryDesignCapacityMah);
    }

    class AudioCodec final : public NoAudioCodec {
    private:
        std::vector<int32_t> rx_buffer_;
        std::vector<int32_t> tx_buffer_;
        respeaker::AudioLevel& input_level_;
        respeaker::AudioLevel& output_level_;
        int64_t last_rx_error_us_ = -5000000;
#if CONFIG_RESPEAKER_AUDIO_DIAGNOSTICS
        int64_t last_rx_report_us_ = 0;
        uint32_t rx_frames_ = 0;
        uint32_t rx_nonzero_ = 0;
        uint32_t rx_rails_ = 0;
        uint32_t rx_timeouts_ = 0;
        uint32_t rx_left_peak_ = 0;
        uint32_t rx_right_peak_ = 0;
        uint64_t rx_square_sum_ = 0;

        void ReportRx(const std::vector<int32_t>& raw, size_t frames) {
            const int64_t now = esp_timer_get_time();
            if (last_rx_report_us_ != 0 && now - last_rx_report_us_ < 5000000) {
                return;
            }
            const double rms =
                rx_frames_ == 0 ? 0.0 : std::sqrt(static_cast<double>(rx_square_sum_) / rx_frames_);
            ESP_LOGI(kTag,
                     "RX: frames=%lu left_peak32=%lu right_peak32=%lu "
                     "left_nonzero=%lu rails16=%lu rms16=%.1f timeouts=%lu (left slot, shift=16)",
                     static_cast<unsigned long>(rx_frames_),
                     static_cast<unsigned long>(rx_left_peak_),
                     static_cast<unsigned long>(rx_right_peak_),
                     static_cast<unsigned long>(rx_nonzero_), static_cast<unsigned long>(rx_rails_),
                     rms, static_cast<unsigned long>(rx_timeouts_));
            if (frames >= 4) {
                ESP_LOGI(kTag, "RX raw L/R: %08lx/%08lx %08lx/%08lx %08lx/%08lx %08lx/%08lx",
                         static_cast<unsigned long>(static_cast<uint32_t>(raw[0])),
                         static_cast<unsigned long>(static_cast<uint32_t>(raw[1])),
                         static_cast<unsigned long>(static_cast<uint32_t>(raw[2])),
                         static_cast<unsigned long>(static_cast<uint32_t>(raw[3])),
                         static_cast<unsigned long>(static_cast<uint32_t>(raw[4])),
                         static_cast<unsigned long>(static_cast<uint32_t>(raw[5])),
                         static_cast<unsigned long>(static_cast<uint32_t>(raw[6])),
                         static_cast<unsigned long>(static_cast<uint32_t>(raw[7])));
            }
            last_rx_report_us_ = now;
            rx_frames_ = rx_nonzero_ = rx_rails_ = rx_timeouts_ = 0;
            rx_left_peak_ = rx_right_peak_ = 0;
            rx_square_sum_ = 0;
        }
#endif

    public:
        AudioCodec(respeaker::AudioLevel& input_level, respeaker::AudioLevel& output_level)
            : input_level_(input_level), output_level_(output_level) {
            duplex_ = true;
            input_channels_ = 1;
            output_channels_ = 2;
            input_sample_rate_ = kAudioSampleRate;
            output_sample_rate_ = kAudioSampleRate;

            i2s_chan_config_t channel_config = {
                .id = XIAOZHI_I2S_PORT(0),
                .role = kI2sRole,
                .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
                .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
                .auto_clear_after_cb = true,
                .auto_clear_before_cb = false,
                .intr_priority = 0,
            };
            ESP_ERROR_CHECK(i2s_new_channel(&channel_config, &tx_handle_, &rx_handle_));

            i2s_std_config_t standard_config = {
                .clk_cfg =
                    {
                        .sample_rate_hz = kAudioSampleRate,
                        .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
                        .ext_clk_freq_hz = 0,
#endif
                        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                    },
                .slot_cfg =
                    {
                        .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
                        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                        .slot_mode = I2S_SLOT_MODE_STEREO,
                        .slot_mask = I2S_STD_SLOT_BOTH,
                        .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
                        .ws_pol = false,
                        .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
                        .left_align = true,
                        .big_endian = false,
                        .bit_order_lsb = false,
#endif
                    },
                .gpio_cfg =
                    {
                        .mclk = I2S_GPIO_UNUSED,
                        .bclk = kI2sBclkPin,
                        .ws = kI2sWsPin,
                        .dout = kI2sDoutPin,
                        .din = kI2sDinPin,
                        .invert_flags =
                            {
                                .mclk_inv = false,
                                .bclk_inv = false,
                                .ws_inv = false,
                            },
                    },
            };
            ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &standard_config));
            ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &standard_config));
            ESP_LOGI(kTag, "I2S: %s, %d Hz, 32-bit stereo bus; mono left-slot input", kI2sRoleName,
                     kAudioSampleRate);
        }

        bool InputData(std::vector<int16_t>& data) override {
            const int samples = Read(data.data(), data.size());
            data.resize(samples);
            return samples > 0;
        }

    protected:
        int Read(int16_t* dest, int samples) override {
            rx_buffer_.resize(static_cast<size_t>(samples) * 2);
            size_t bytes_read = 0;
            const esp_err_t err =
                i2s_channel_read(rx_handle_, rx_buffer_.data(), rx_buffer_.size() * sizeof(int32_t),
                                 &bytes_read, 200);
            if (err == ESP_ERR_TIMEOUT) {
#if CONFIG_RESPEAKER_AUDIO_DIAGNOSTICS
                ++rx_timeouts_;
#endif
                const int64_t now = esp_timer_get_time();
                if (now - last_rx_error_us_ >= 5000000) {
                    ESP_LOGW(kTag, "I2S read timed out; check XMOS power and clock configuration");
                    last_rx_error_us_ = now;
                }
            } else if (err != ESP_OK) {
                ESP_LOGE(kTag, "I2S read failed: %s", esp_err_to_name(err));
                return 0;
            }
            if (bytes_read % (2 * sizeof(int32_t)) != 0) {
                ESP_LOGE(kTag, "Incomplete I2S frame: %u bytes", static_cast<unsigned>(bytes_read));
                return 0;
            }

            const size_t frames = bytes_read / (2 * sizeof(int32_t));
            uint64_t square_sum = 0;
            for (size_t i = 0; i < frames; ++i) {
                const int32_t left = rx_buffer_[i * 2];
                const int32_t value = left >> 16;
                dest[i] = static_cast<int16_t>(value);
                square_sum += static_cast<uint64_t>(static_cast<int64_t>(value) * value);
#if CONFIG_RESPEAKER_AUDIO_DIAGNOSTICS
                const int32_t right = rx_buffer_[i * 2 + 1];
                const uint32_t left_peak =
                    static_cast<uint32_t>(left < 0 ? -static_cast<int64_t>(left) : left);
                const uint32_t right_peak =
                    static_cast<uint32_t>(right < 0 ? -static_cast<int64_t>(right) : right);
                rx_left_peak_ = std::max(rx_left_peak_, left_peak);
                rx_right_peak_ = std::max(rx_right_peak_, right_peak);
                rx_nonzero_ += left != 0;

                rx_rails_ += value == INT16_MIN || value == INT16_MAX;
                rx_square_sum_ += static_cast<uint64_t>(static_cast<int64_t>(value) * value);
#endif
            }
            input_level_.Update(square_sum, frames, 1.0f,
                                static_cast<uint32_t>(esp_timer_get_time() / 1000));
#if CONFIG_RESPEAKER_AUDIO_DIAGNOSTICS
            rx_frames_ += static_cast<uint32_t>(frames);
            ReportRx(rx_buffer_, frames);
#endif
            return static_cast<int>(frames);
        }

        int Write(const int16_t* data, int samples) override {
            std::lock_guard<std::mutex> lock(data_if_mutex_);
            tx_buffer_.resize(static_cast<size_t>(samples) * 2);
            const double volume_ratio = static_cast<double>(output_volume_) / 100.0;
            const int32_t volume_factor = static_cast<int32_t>(volume_ratio * volume_ratio * 65536);

            for (int i = 0; i < samples; ++i) {
                const int64_t scaled = static_cast<int64_t>(data[i]) * volume_factor;
                const int32_t sample =
                    static_cast<int32_t>(std::clamp<int64_t>(scaled, INT32_MIN, INT32_MAX));
                tx_buffer_[static_cast<size_t>(i) * 2] = sample;
                tx_buffer_[static_cast<size_t>(i) * 2 + 1] = sample;
            }

            constexpr size_t kFrameBytes = 2 * sizeof(int32_t);
            constexpr size_t kChunkBytes = AUDIO_CODEC_DMA_FRAME_NUM * kFrameBytes;
            constexpr int64_t kWriteBudgetUs = 500000;
            const int64_t deadline = esp_timer_get_time() + kWriteBudgetUs;
            const auto* bytes = reinterpret_cast<const uint8_t*>(tx_buffer_.data());
            const size_t total_bytes = tx_buffer_.size() * sizeof(int32_t);
            size_t written = 0;
            while (written < total_bytes) {
                const int64_t remaining_us = deadline - esp_timer_get_time();
                if (remaining_us <= 0) {
                    ESP_LOGE(kTag, "I2S write budget exceeded: %u/%u bytes; check XMOS clocks",
                             static_cast<unsigned>(written), static_cast<unsigned>(total_bytes));
                    break;
                }
                const uint32_t timeout_ms =
                    static_cast<uint32_t>(std::min<int64_t>(100, (remaining_us + 999) / 1000));
                const size_t chunk_bytes = std::min(kChunkBytes, total_bytes - written);
                size_t bytes_written = 0;
                const size_t first_frame = written / kFrameBytes;
                const esp_err_t err = i2s_channel_write(tx_handle_, bytes + written, chunk_bytes,
                                                        &bytes_written, timeout_ms);
                const size_t accepted_frames = bytes_written / kFrameBytes;
                if (accepted_frames > 0) {
                    uint64_t square_sum = 0;
                    for (size_t i = 0; i < accepted_frames; ++i) {
                        const int64_t sample = data[first_frame + i];
                        square_sum += static_cast<uint64_t>(sample * sample);
                    }
                    output_level_.Update(square_sum, accepted_frames,
                                         static_cast<float>(volume_ratio * volume_ratio),
                                         static_cast<uint32_t>(esp_timer_get_time() / 1000));
                }
                written += bytes_written;
                if (err != ESP_OK || bytes_written == 0) {
                    ESP_LOGE(kTag, "I2S write stopped: %s, %u/%u bytes; remaining audio dropped",
                             esp_err_to_name(err), static_cast<unsigned>(written),
                             static_cast<unsigned>(total_bytes));
                    break;
                }
            }
            return static_cast<int>(written / kFrameBytes);
        }
    };

public:
    XiaoEsp32S3ReSpeakerXvf3800Board() {
        ProbeXvf3800(i2c_bus_);
        InitializeBatteryMonitor();
#if CONFIG_RESPEAKER_I2S_CLOCK_PROBE_ONLY
        ProbeExternalI2sClock();
#endif

        boot_button_.OnClick([this]() { HandleConversationButtonClick("XIAO BOOT"); });
        user_button_.OnClick([this]() { HandleConversationButtonClick("XIAO D3"); });
    }

    AudioCodec* GetAudioCodec() override {
        static AudioCodec codec(input_level_, output_level_);
        return &codec;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        std::lock_guard<std::mutex> lock(battery_mutex_);
        if (battery_i2c_device_ == nullptr) {
            return false;
        }

        uint16_t voltage_mv = 0;
        uint16_t current_raw = 0;
        uint16_t state_of_charge = 0;
        esp_err_t err = ReadBatteryWord(kBq27220VoltageRegister, voltage_mv);
        if (err == ESP_OK) {
            err = ReadBatteryWord(kBq27220CurrentRegister, current_raw);
        }
        if (err == ESP_OK) {
            err = ReadBatteryWord(kBq27220StateOfChargeRegister, state_of_charge);
        }
        if (err != ESP_OK || voltage_mv < 1500 || voltage_mv > 6000 || state_of_charge > 100) {
            const int64_t now = esp_timer_get_time();
            if (now - last_battery_error_us_ >= kBatteryLogIntervalUs) {
                ESP_LOGW(kTag,
                         "BQ27220 telemetry read failed or invalid: err=%s voltage=%u mV SOC=%u",
                         esp_err_to_name(err), voltage_mv, state_of_charge);
                last_battery_error_us_ = now;
            }
            return false;
        }

        const int16_t current_ma = static_cast<int16_t>(current_raw);
        level = static_cast<int>(state_of_charge);
        charging = current_ma > 0;
        discharging = current_ma < 0;
        battery_voltage_mv_ = voltage_mv;
        battery_current_ma_ = current_ma;
        return true;
    }

    std::string GetDeviceStatusJson() override {
        const std::string status = WifiBoard::GetDeviceStatusJson();
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(status.c_str()),
                                                             cJSON_Delete);
        if (!root) {
            ESP_LOGE(kTag, "Could not parse device status for battery diagnostics");
            return R"({"error":"Could not parse device status"})";
        }
        auto* battery = cJSON_GetObjectItemCaseSensitive(root.get(), "battery");
        if (!cJSON_IsObject(battery)) {
            return status;
        }

        std::lock_guard<std::mutex> lock(battery_mutex_);
        if (!cJSON_AddNumberToObject(battery, "voltage_mv", battery_voltage_mv_) ||
            !cJSON_AddNumberToObject(battery, "current_ma", battery_current_ma_) ||
            !cJSON_AddNumberToObject(battery, "gauge_design_capacity_mah",
                                     battery_gauge_design_capacity_mah_) ||
            !cJSON_AddNumberToObject(battery, "expected_capacity_mah", kBatteryDesignCapacityMah) ||
            !cJSON_AddBoolToObject(
                battery, "capacity_matches",
                battery_gauge_design_capacity_mah_ == kBatteryDesignCapacityMah) ||
            !cJSON_AddStringToObject(battery, "soc_accuracy", "unverified") ||
            !cJSON_AddStringToObject(
                battery, "soc_note",
                "Battery level is the gauge's reference estimate, not a calibrated percentage. "
                "The cell profile has not been verified, even if design capacity matches.")) {
            ESP_LOGE(kTag, "Could not allocate battery diagnostics JSON");
            return R"({"error":"Could not allocate battery diagnostics"})";
        }
        std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(root.get()),
                                                          cJSON_free);
        if (!json) {
            ESP_LOGE(kTag, "Could not serialize battery diagnostics");
            return R"({"error":"Could not serialize battery diagnostics"})";
        }
        return std::string(json.get());
    }

    Led* GetLed() override {
        static BoardLed led(input_level_, output_level_);
        return &led;
    }
};

}  // namespace

DECLARE_BOARD(XiaoEsp32S3ReSpeakerXvf3800Board);
