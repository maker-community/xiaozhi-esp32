import os
import shlex
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main" / "boards" / "seeed" / "xiao-esp32s3-respeaker-xvf3800"


class ReSpeakerLedTest(unittest.TestCase):
    def test_breathing_transitions_audio_response_and_limits(self):
        driver = r"""
            #include <cassert>
            #include <cmath>
            #include <cstdint>
            #include <iostream>
            #include <limits>

            #include "assistant_led_animation.h"

            using Animation = respeaker::AssistantLedAnimation;
            using Frame = Animation::Frame;

            static int Energy(const Frame& frame) {
                int energy = 0;
                for (const auto& pixel : frame) {
                    energy += pixel.red + pixel.green + pixel.blue;
                    assert(pixel.red <= Animation::kMaxChannel);
                    assert(pixel.green <= Animation::kMaxChannel);
                    assert(pixel.blue <= Animation::kMaxChannel);
                }
                return energy;
            }

            static int Difference(const Frame& a, const Frame& b) {
                int difference = 0;
                for (size_t i = 0; i < a.size(); ++i) {
                    difference = std::max(difference,
                        std::abs(int(a[i].red) - int(b[i].red)));
                    difference = std::max(difference,
                        std::abs(int(a[i].green) - int(b[i].green)));
                    difference = std::max(difference,
                        std::abs(int(a[i].blue) - int(b[i].blue)));
                }
                return difference;
            }

            int main() {
                static_assert(Animation::kPixelCount == 12);
                respeaker::AudioLevel level;
                assert(level.Get(0) == 0.0f);
                level.Update(uint64_t{32768} * 32768, 1, 1.0f, 1000);
                assert(level.Get(1000) == 1.0f);
                assert(level.Get(1075) == 0.5f);
                assert(level.Get(1150) == 0.0f);
                level.Update(uint64_t{32768} * 32768, 1, 0.0f, 2000);
                assert(level.Get(2000) == 0.0f);
                level.Update(0, 0, 1.0f, 3000);
                assert(level.Get(3000) == 0.0f);
                const uint32_t wrap = std::numeric_limits<uint32_t>::max() - 50;
                level.Update(64000000, 1, 1.0f, wrap);
                assert(level.Get(wrap + 75) == 0.5f);
                assert(level.Get(wrap + 150) == 0.0f);

                const DeviceState states[] = {
                    kDeviceStateUnknown, kDeviceStateStarting, kDeviceStateWifiConfiguring,
                    kDeviceStateIdle, kDeviceStateConnecting, kDeviceStateListening,
                    kDeviceStateSpeaking, kDeviceStateNotifying, kDeviceStateUpgrading,
                    kDeviceStateActivating, kDeviceStateAudioTesting, kDeviceStateFatalError
                };
                int largest_step = 0;
                for (auto from : states) {
                    for (auto to : states) {
                        Animation animation;
                        Frame previous{};
                        for (uint32_t t = 0; t <= 2000; t += 25) {
                            previous = animation.Render(from, t, 0.6f, 0.7f);
                            Energy(previous);
                        }
                        const auto first = animation.Render(to, 2000, 0.6f, 0.7f);
                        assert(Difference(previous, first) == 0);
                        for (uint32_t t = 2025; t <= 6000; t += 25) {
                            const auto frame = animation.Render(to, t, 0.6f, 0.7f);
                            Energy(frame);
                            const int step = Difference(previous, frame);
                            largest_step = std::max(largest_step, step);
                            assert(step <= 8);
                            previous = frame;
                        }
                    }
                }

                Animation idle;
                int minimum = 10000, maximum = 0;
                for (uint32_t t = 0; t <= 10000; t += 25) {
                    const int energy = Energy(idle.Render(kDeviceStateIdle, t, 0.0f, 0.0f));
                    if (t >= 1000) {
                        minimum = std::min(minimum, energy);
                        maximum = std::max(maximum, energy);
                    }
                }
                assert(minimum > 0);
                assert(maximum - minimum >= 12);
                assert(maximum <= 100);

                for (auto state : {kDeviceStateListening, kDeviceStateSpeaking,
                                   kDeviceStateNotifying}) {
                    Animation quiet, active;
                    int quiet_energy = 0, active_energy = 0;
                    for (uint32_t t = 0; t <= 2000; t += 25) {
                        const auto a = quiet.Render(state, t, 0.0f, 0.0f);
                        const auto b = active.Render(state, t, 0.8f, 0.8f);
                        if (t >= 1000) {
                            quiet_energy += Energy(a);
                            active_energy += Energy(b);
                        }
                    }
                    assert(active_energy > quiet_energy * 2);
                    Frame released{};
                    for (uint32_t t = 2025; t <= 4000; t += 25) {
                        released = active.Render(state, t, 0.0f, 0.0f);
                    }
                    const auto silence = quiet.Render(state, 4000, 0.0f, 0.0f);
                    assert(Difference(released, silence) <= 1);
                }

                Animation speaking;
                Frame previous{};
                int changes = 0;
                for (uint32_t t = 0; t <= 6000; t += 25) {
                    auto frame = speaking.Render(kDeviceStateSpeaking, t, 0.0f, 0.5f);
                    if (t >= 1000 && Difference(previous, frame) != 0) {
                        ++changes;
                    }
                    previous = frame;
                }
                assert(changes > 40);

                Animation interrupted;
                Frame frame{};
                for (uint32_t t = 0; t <= 1500; t += 25) {
                    const auto before = interrupted.Render(kDeviceStateSpeaking, t, 0.0f, 0.8f);
                    frame = interrupted.Render(kDeviceStateListening, t, 0.8f, 0.0f);
                    assert(Difference(before, frame) == 0);
                    assert(Difference(frame, interrupted.Render(
                        kDeviceStateNotifying, t, 0.0f, 0.8f)) == 0);
                }

                Animation rollover;
                previous = {};
                for (uint32_t offset = 0; offset <= 3000; offset += 25) {
                    frame = rollover.Render(kDeviceStateSpeaking, wrap + offset, 0.0f, 0.7f);
                    assert(Difference(previous, frame) <= 8);
                    Energy(frame);
                    previous = frame;
                }
                std::cout << "144 state pairs checked; max 25ms channel step="
                          << largest_step << "; idle energy=" << minimum << ".." << maximum
                          << "; speaking changes=" << changes << "\n";
            }
        """
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            source = build_dir / "respeaker_led_test.cc"
            source.write_text(textwrap.dedent(driver), encoding="utf-8")
            executable = build_dir / (
                "respeaker_led_test.exe" if os.name == "nt" else "respeaker_led_test"
            )
            compiler = shlex.split(os.environ.get("CXX", "c++"))
            if Path(compiler[0]).stem.lower() == "cl":
                flags = [
                    "/nologo",
                    "/std:c++20",
                    "/EHsc",
                    "/W4",
                    "/WX",
                    f"/I{ROOT / 'main'}",
                    f"/I{BOARD}",
                    str(source),
                    f"/Fe:{executable}",
                ]
            else:
                flags = [
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'main'}",
                    f"-I{BOARD}",
                    str(source),
                    "-o",
                    str(executable),
                ]
            command = compiler + flags
            subprocess.run(command, check=True, cwd=build_dir)
            subprocess.run([executable], check=True, cwd=build_dir)


if __name__ == "__main__":
    unittest.main()
