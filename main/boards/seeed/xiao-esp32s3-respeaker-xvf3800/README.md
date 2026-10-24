# Seeed reSpeaker XVF3800 with XIAO ESP32-S3

[English](README.md) | [简体中文](README_zh.md)

This board implementation provides two I2S clock profiles for the circular
reSpeaker XVF3800 USB 4-Mic Array and the split-board reSpeaker Flex Circular
with a fitted XIAO ESP32-S3. It does not require a display, camera or SD card.
It does not target the ReSpeaker Lite / USB V3, which uses the XMOS XU316.

## Official hardware and firmware references

- [reSpeaker Flex with XIAO ESP32-S3](https://wiki.seeedstudio.com/respeaker_flex_xiao_introduction/):
  core board, circular/linear microphone arrays, headers and buttons.
- [Flex header pinout](https://files.seeedstudio.com/wiki/reSpeaker_flex/header_pinout.jpg):
  distinguish XIAO pins from XMOS GPIO headers.
- [XVF3800 USB Array with XIAO](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/):
  the integrated USB Array product and its separate XMOS firmware.
- [USB Array I2S example](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_i2s/):
  16 kHz, two 32-bit slots, XIAO master, and the audio pins used below.
- [Flex I2S recording/playback example](https://wiki.seeedstudio.com/respeaker_flex_xiao_record_playback/):
  note that the text describes a host master while the code sets
  `is_master = false`; do not infer the clock role from this example alone.
- [Official Flex firmware repository](https://github.com/respeaker/reSpeaker_Flex/tree/main/xmos_firmwares):
  image naming, microphone geometry, sample rate and clock-role information.
  Its downloadable I2S images are described as XMOS slave images. They are
  not necessarily equivalent to firmware preinstalled on an individual unit.

The XMOS and ESP32-S3 run separate firmware. Flashing Xiaozhi to the XIAO
does not install or change XMOS firmware. Select XMOS firmware for the exact
product, microphone geometry and required I2S mode; do not interchange Lite,
USB Array and Flex images just because the connectors look similar.

## Choose the clock profile before building

| Variant (`--name`) | ESP32-S3 role | I2S rate | Required XMOS mode |
| --- | --- | --- | --- |
| `xiao-esp32s3-respeaker-xvf3800` | Master | 16 kHz | Slave, 16 kHz |
| `xiao-esp32s3-respeaker-flex-circular-48k` | Slave | 48 kHz | Master, 48 kHz |

The second profile is based on hardware testing of one Flex Circular unit
with a preinstalled XMOS image of unknown version. With the XIAO clock pins
configured as inputs, WS was observed at approximately 48 kHz. Capture,
WakeNet and cloud conversation then worked with the XIAO as a 48 kHz slave.
This is an observation about that unit, not a claim that all Flex factory
firmware has the same clock role or sample rate.

In external-clock mode, Xiaozhi's existing audio service resamples microphone
audio to 16 kHz and playback to 48 kHz. The 48 kHz profile must not be used
with a downloadable XMOS slave image or with a 16 kHz clock source.

## Connections and controls

| Signal | XIAO ESP32-S3 GPIO |
| --- | --- |
| I2S BCLK | 8 |
| I2S WS/LRCLK | 7 |
| I2S ESP32-S3 output to XVF3800 | 44 |
| I2S XVF3800 output to ESP32-S3 | 43 |
| I2C SDA | 5 |
| I2C SCL | 6 |
| Ground | Ground |

These signals are already wired on the supported preassembled XIAO products.
For separate wiring, use common ground and 3.3 V-compatible logic. Follow the
official board's power and speaker instructions; this pin table is not a
power-supply wiring guide.

The codec receives two 32-bit I2S slots and selects the left slot as one
processed microphone channel. It converts signed 32-bit PCM to 16-bit PCM
without additional gain. It does not treat the processed stereo output as
two raw microphones for ESP-SR BSS. Mono playback is duplicated into both
slots for the board's audio codec/amplifier.

- **XIAO BOOT (GPIO0):** click to start/stop a conversation; during startup,
  click to enter Wi-Fi configuration.
- **XIAO RST:** resets the ESP32-S3.
- **XIAO D3 (GPIO4):** connect a button to GND to control conversation state;
  during startup, it enters Wi-Fi configuration, matching the onboard BOOT
  button's application behavior. It is not a hardware reset or ROM BOOT button.
- **XIAO D0 (GPIO1):** supports an optional 12-pixel WS2812 assistant ring,
  with effects described below. Use a suitably rated 5 V supply with common ground;
  do not power the strip from a GPIO. A 5 V-compatible level shifter between
  GPIO1 and the strip's DIN is recommended.
- **XIAO D2 (GPIO3):** unused. GPIO3 is a strapping-related pin; verify that an
  attached button circuit will not alter its level during reset.
- **Optional BQ27220 fuel gauge:** connect it to the core board's `I2C_SDA`
  (D4/GPIO5), `I2C_SCL` (D5/GPIO6), and common ground. Firmware reuses the
  XVF3800 I2C bus and only probes address `0x55` and reads the device number,
  voltage, current, SOC and design capacity. The battery's 4440 mWh / 3.7 V
  rating corresponds to about 1200 mAh. A matching device number and valid
  telemetry enable reporting; a design-capacity mismatch only warns, without
  disabling monitoring. Absence or failed reads omit battery data without blocking boot.
  `self.get_device_status` includes reference SOC, charging, voltage (mV), current
  (mA, positive for charging), gauge/expected capacities and a capacity-match flag.
  `soc_accuracy` is always `unverified`; `soc_note` warns that SOC is not calibrated.
  Matching capacity does not verify the cell curve. Do not use this reference
  percentage for accurate runtime prediction or automatic shutdown.
- Startup marker `BQ27220 monitor v3` reports SDA/SCL, address and the probe
  timeout in milliseconds. `did not ACK` means the address did not respond;
  `probe failed ... ESP_ERR_TIMEOUT` is a timeout, not proof that the module is
  absent. Check power, common ground, wiring and 3.3 V pull-ups.
  Identification logs include the device number, voltage, SOC and actual design
  capacity; a capacity mismatch has its own warning. The device-number query sends
  `Control(0x0001)` and expects `0x0220`, without changing the cell configuration.
  Detection happens only at startup: connect with power off and reboot.
  Hot-plugging and automatic discovery after boot are not supported.
- SOC accuracy depends on the cell chemistry, capacity and CEDV profile already
  configured in the gauge. This driver **does not write or change gauge
  configuration**; ensure the module is configured for the actual cell.
  I2C logic must be 3.3 V compatible; do not let an external module pull SDA/SCL
  up to 5 V. BQ27220 is not a charger or protection circuit; wire cell sensing,
  protection and power according to the gauge module and battery specifications.
- **Flex core-board RST and SafeMode/Boot:** act on the XMOS, not the XIAO
  conversation button. They are not connected by this driver as user keys.
- **Status LEDs:** use the active-low XIAO GPIO21 LED and optional D0 WS2812
  strip. The onboard LED retains its original effects; the external ring uses
  smooth animation. Neither controls the XMOS array's LEDs.

### External assistant ring

| State | Effect |
| --- | --- |
| Idle | Very dim blue breathing over 5 seconds, instead of turning completely off |
| Starting / connecting | Slowly orbiting blue/cyan glow |
| Listening / audio testing | Blue/cyan breathing with symmetric highlights responding to capture energy, not direction |
| Speaking | Blue/cyan/violet waves, brightness following audio accepted by I2S |
| Notification playback | Warm amber breathing, also responsive to playback energy |
| Wi-Fi setup | Amber breathing over 3 seconds |
| Activation | Slow violet breathing |
| Upgrade | Orbiting mint glow, not a percentage-progress indicator |
| Fatal error | Slow red breathing, without rapid flashing |

A dedicated low-priority task renders at approximately 40 fps. State changes
ease from the current colors over 450 ms; repeated state notifications do not
restart the animation. Gamma is 2.2, with each PWM channel capped at 80/255.
This is not current sensing or hardware current limiting; size the power supply
according to the strip specification.
Energy is measured only from this board's captured PCM or samples accepted by
I2S, including the playback volume setting, and decays when audio stops.
It is not sample-accurate synchronization, direction finding or emotion detection.
Generic notification state cannot distinguish individual warning reasons.
No new sounds are added and existing audio content is unchanged.
The external ring, button and audio response still require physical validation.

Design references: [Home Assistant Voice](https://www.home-assistant.io/voice-pe/)
and [Adafruit gamma correction](https://learn.adafruit.com/led-tricks-gamma-correction/the-quick-fix).
The palette, timing and brightness cap are board-specific design choices,
not an exact reproduction of those products.

USB Serial/JTAG is selected because GPIO43/44 are occupied by audio.
The board uses the default `NoDisplay` implementation.

## Build and flash

Use an ESP-IDF 6.1 terminal in the project root. The project requires at least
ESP-IDF 6.0.1; this board has been build-tested with 6.1.

Use the canonical build script so that the selected variant applies its
8 MB flash, `partitions/v2/8m.csv`, USB console and clock settings.

For a Flex Circular unit supplying 48 kHz clocks:

```powershell
python scripts\build.py seeed/xiao-esp32s3-respeaker-xvf3800 --name xiao-esp32s3-respeaker-flex-circular-48k --language zh-CN --wake-word nihaoxiaozhi
```

For an XMOS slave image requiring a 16 kHz host master:

```powershell
python scripts\build.py seeed/xiao-esp32s3-respeaker-xvf3800 --name xiao-esp32s3-respeaker-xvf3800 --language zh-CN --wake-word nihaoxiaozhi
```

The board selector containing `/` is a build-script identifier. On Linux/macOS,
use `scripts/build.py` for the script path instead. Language and wake word are
user options, not fixed board defaults.

After a successful build, either flash directly:

```powershell
idf.py -p COM5 flash monitor
```

Replace `COM5` with the XIAO port. Alternatively, use Espressif Flash Download
Tool: ESP32-S3, `build\merged-binary.bin` at **0x0**, DIO, 80 MHz, 8 MB.
Use the XIAO USB port, not the XMOS DFU port, to flash this image.
Back up needed device settings before erasing flash; erasing removes Wi-Fi
credentials and other NVS settings.

If configuring manually with `idf.py menuconfig`, selecting the board alone
does **not** apply `config.json` variant presets. Also set:

- Target: ESP32-S3.
- Flash size: 8 MB; custom partition table: `partitions/v2/8m.csv`.
- Console: USB Serial/JTAG.
- `RESPEAKER_I2S_EXTERNAL_CLOCK_48K`: enabled only for external 48 kHz clocks.
- Wake-word engine: AFE WakeNet, with a selected supported model.
- Assets: flash default assets so the selected model is included.
- `RESPEAKER_I2S_CLOCK_PROBE_ONLY`: disabled for normal operation.

Then run `idf.py build` and `idf.py flash monitor`. Re-run `idf.py merge-bin`
if using a merged image; an older image is not updated by menuconfig alone.

## Diagnostics and clock-loss behavior

Both diagnostic options default to disabled and only apply to this board.

- `RESPEAKER_AUDIO_DIAGNOSTICS`: reports raw slot peaks, selected-channel RMS,
  sample counts, full-scale counts, timeouts and four raw sample pairs every
  five seconds while capturing. Enable only when diagnosing audio.
- `RESPEAKER_I2S_CLOCK_PROBE_ONLY`: leaves BCLK/WS as GPIO inputs and reports
  sampled clock activity without starting I2S, Wi-Fi or chat. Its polling-based
  WS frequency is approximate; the BCLK edge count is not a frequency reading.
  No edges do not prove slave firmware: the XMOS may be inactive or unpowered.

Configure diagnostic options with menuconfig **after** selecting a build
variant, then use `idf.py build` / `idf.py merge-bin`. Re-running the build
script recreates sdkconfig from variant and user build options.
Fully power-cycle the XIAO and XMOS when testing clock profiles.

RX timeouts are reported with rate limiting. TX uses finite per-chunk waits
and checks an overall write budget between chunks. If clocks stop or a write
fails, it logs how much audio was accepted and drops the unwritten remainder
instead of blocking indefinitely. Current playback can be interrupted by an
XMOS reset; automatic XMOS reconfiguration/recovery is not provided.

## Validation status

| Item | Status |
| --- | --- |
| Flex Circular, preinstalled XMOS master near 48 kHz | Capture, GPIO0 button, wake word, speech recognition and multiple cloud dialogue turns observed |
| 16 kHz host-master profile | Build validation only; no corresponding hardware result recorded |
| Passive clock probe | External clock activity observed on the tested Flex unit |
| Physical speaker playback and volume | Not yet hardware-verified |
| Playback echo cancellation / voice interruption | Not yet hardware-verified; no new full-duplex/AEC support is enabled |
| Linear Flex, Lite, battery, external display or external LEDs | Not validated by this implementation |

Successful cloud replies or decoded TTS packets do not prove that a connected
speaker plays correctly. Verify playback and echo behavior on real hardware
before claiming those capabilities.
