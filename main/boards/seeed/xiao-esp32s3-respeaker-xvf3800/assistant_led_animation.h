#ifndef XIAO_RESPEAKER_ASSISTANT_LED_ANIMATION_H_
#define XIAO_RESPEAKER_ASSISTANT_LED_ANIMATION_H_

#include "device_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace respeaker {

class AudioLevel {
public:
    void Update(uint64_t square_sum, size_t samples, float gain, uint32_t now_ms) {
        const float rms = samples == 0 ? 0.0f : std::sqrt(static_cast<float>(square_sum) / samples);
        const float level = std::sqrt(std::clamp(rms * gain / 8000.0f, 0.0f, 1.0f));
        level_.store(level, std::memory_order_relaxed);
        updated_ms_.store(now_ms, std::memory_order_release);
    }

    float Get(uint32_t now_ms) const {
        const uint32_t age = now_ms - updated_ms_.load(std::memory_order_acquire);
        if (age >= 150) {
            return 0.0f;
        }
        return level_.load(std::memory_order_relaxed) * (1.0f - static_cast<float>(age) / 150.0f);
    }

private:
    std::atomic<float> level_{0.0f};
    std::atomic<uint32_t> updated_ms_{0};
};

class AssistantLedAnimation {
public:
    static constexpr size_t kPixelCount = 12;
    static constexpr uint8_t kMaxChannel = 80;
    static constexpr uint32_t kTransitionMs = 450;

    struct Pixel {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
    };
    using Frame = std::array<Pixel, kPixelCount>;

    AssistantLedAnimation() {
        for (size_t i = 0; i < gamma_.size(); ++i) {
            gamma_[i] = static_cast<uint8_t>(
                std::lround(std::pow(static_cast<float>(i) / 255.0f, 2.2f) * kMaxChannel));
        }
    }

    Frame Render(DeviceState state, uint32_t now_ms, float input_level, float output_level) {
        if (!initialized_ || state != state_) {
            source_ = current_;
            state_ = state;
            changed_ms_ = now_ms;
        }
        const float dt =
            initialized_
                ? static_cast<float>(std::min(now_ms - previous_ms_, uint32_t{100})) / 1000.0f
                : 0.025f;
        initialized_ = true;
        previous_ms_ = now_ms;
        SmoothEnvelope(input_level, dt, input_envelope_);
        SmoothEnvelope(output_level, dt, output_envelope_);

        const uint32_t elapsed_ms = now_ms - changed_ms_;
        const float blend = Ease(std::min(static_cast<float>(elapsed_ms) / kTransitionMs, 1.0f));
        Frame frame{};
        for (size_t i = 0; i < kPixelCount; ++i) {
            const Color target = Target(state, elapsed_ms, static_cast<float>(i) / kPixelCount);
            current_[i] = Mix(source_[i], target, blend);
            frame[i] = {Correct(current_[i].red), Correct(current_[i].green),
                        Correct(current_[i].blue)};
        }
        return frame;
    }

private:
    struct Color {
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
    };

    static constexpr float kTau = 6.28318530718f;
    std::array<Color, kPixelCount> source_{};
    std::array<Color, kPixelCount> current_{};
    std::array<uint8_t, 256> gamma_{};
    DeviceState state_ = kDeviceStateUnknown;
    uint32_t changed_ms_ = 0;
    uint32_t previous_ms_ = 0;
    bool initialized_ = false;
    float input_envelope_ = 0.0f;
    float output_envelope_ = 0.0f;

    static float Ease(float x) { return x * x * (3.0f - 2.0f * x); }

    static float Pulse(uint32_t elapsed_ms, uint32_t period_ms, float phase = 0.0f) {
        const float cycles = static_cast<float>(elapsed_ms % period_ms) / period_ms + phase;
        return 0.5f - 0.5f * std::cos(kTau * cycles);
    }

    static Color Mix(Color a, Color b, float amount) {
        return {a.red + (b.red - a.red) * amount, a.green + (b.green - a.green) * amount,
                a.blue + (b.blue - a.blue) * amount};
    }

    static Color Scale(Color color, float brightness) {
        return {color.red * brightness, color.green * brightness, color.blue * brightness};
    }

    static void SmoothEnvelope(float level, float dt, float& envelope) {
        const float target = std::clamp(level, 0.0f, 1.0f);
        const float time_constant = target > envelope ? 0.055f : 0.250f;
        envelope += (target - envelope) * (1.0f - std::exp(-dt / time_constant));
    }

    uint8_t Correct(float channel) const {
        const size_t index =
            static_cast<size_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
        return gamma_[index];
    }

    Color Target(DeviceState state, uint32_t elapsed_ms, float position) const {
        constexpr Color blue{0.12f, 0.38f, 1.0f};
        constexpr Color cyan{0.05f, 0.90f, 1.0f};
        constexpr Color violet{0.65f, 0.20f, 1.0f};
        constexpr Color amber{1.0f, 0.48f, 0.06f};
        constexpr Color mint{0.10f, 1.0f, 0.55f};
        switch (state) {
            case kDeviceStateIdle:
                return Scale(blue, 0.12f + 0.10f * Pulse(elapsed_ms, 5000));
            case kDeviceStateStarting:
            case kDeviceStateConnecting: {
                const float orbit = Pulse(elapsed_ms, 2400, -position);
                return Scale(Mix(blue, cyan, orbit), 0.18f + 0.48f * orbit * orbit);
            }
            case kDeviceStateListening:
            case kDeviceStateAudioTesting: {
                const float focus = Pulse(0, 1, position * 2.0f);
                return Scale(Mix(blue, cyan, focus), 0.30f + 0.12f * Pulse(elapsed_ms, 2800) +
                                                         0.45f * input_envelope_ * focus);
            }
            case kDeviceStateSpeaking: {
                const float wave = Pulse(elapsed_ms, 3000, -position);
                const Color color =
                    Mix(Mix(blue, violet, wave), cyan, 0.35f * Pulse(elapsed_ms, 4000, position));
                return Scale(color, 0.25f + 0.10f * Pulse(elapsed_ms, 2400) +
                                        output_envelope_ * (0.35f + 0.25f * wave));
            }
            case kDeviceStateNotifying:
                return Scale(amber,
                             0.20f + 0.38f * Pulse(elapsed_ms, 2000) + 0.30f * output_envelope_);
            case kDeviceStateWifiConfiguring:
                return Scale(amber, 0.18f + 0.48f * Pulse(elapsed_ms, 3000));
            case kDeviceStateActivating:
                return Scale(violet, 0.18f + 0.48f * Pulse(elapsed_ms, 2600));
            case kDeviceStateUpgrading:
                return Scale(mint, 0.20f + 0.42f * Pulse(elapsed_ms, 2000, -position));
            case kDeviceStateFatalError:
                return Scale({1.0f, 0.04f, 0.02f}, 0.25f + 0.50f * Pulse(elapsed_ms, 1800));
            case kDeviceStateUnknown:
                return {};
        }
        return {};
    }
};

}  // namespace respeaker

#endif  // XIAO_RESPEAKER_ASSISTANT_LED_ANIMATION_H_
