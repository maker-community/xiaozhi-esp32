# Seeed reSpeaker XVF3800 与 XIAO ESP32-S3

[English](README.md) | [简体中文](README_zh.md)

本板级实现为搭载 XIAO ESP32-S3 的圆形 reSpeaker XVF3800 USB 四麦阵列、
分体式 reSpeaker Flex Circular 提供两种 I²S 时钟配置。不依赖显示屏、
摄像头或 SD 卡。不适用于采用 XMOS XU316 的 ReSpeaker Lite / USB V3。

## 官方硬件与固件资料

- [reSpeaker Flex 与 XIAO ESP32-S3](https://wiki.seeedstudio.com/respeaker_flex_xiao_introduction/)：
  核心板、圆形/线性阵列、扩展接口及按键说明。
- [Flex 排针映射图](https://files.seeedstudio.com/wiki/reSpeaker_flex/header_pinout.jpg)：
  注意区分 XIAO 引脚与 XMOS GPIO 排针。
- [XVF3800 USB 阵列与 XIAO 入门说明](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/)：
  一体式 USB 阵列及其独立的 XMOS 固件。
- [USB 阵列 I²S 示例](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_i2s/)：
  16 kHz、双通道、32 位、XIAO 主机及下表所用的音频引脚。
- [Flex I²S 录放示例](https://wiki.seeedstudio.com/respeaker_flex_xiao_record_playback/)：
  文字描述主机模式，代码却设置 `is_master = false`，不能仅凭该示例确定时钟角色。
- [Flex 官方固件仓库](https://github.com/respeaker/reSpeaker_Flex/tree/main/xmos_firmwares)：
  固件命名、麦克风阵列布局、采样率及主从角色说明。仓库将可下载的 I²S
  固件描述为 XMOS 从机固件，不一定等同于某台设备预装的固件。

XMOS 与 ESP32-S3 分别运行独立固件。向 XIAO 烧录小智不会安装或修改 XMOS
固件。请按具体产品、麦克风阵列布局及 I²S 模式选择 XMOS 固件，
不能因为接口相似就混用 Lite、USB Array 和 Flex 的固件。

## 构建前选择时钟配置

| 固件变体（`--name`） | ESP32-S3 角色 | I²S 采样率 | 要求的 XMOS 模式 |
| --- | --- | --- | --- |
| `xiao-esp32s3-respeaker-xvf3800` | 主机 | 16 kHz | 从机、16 kHz |
| `xiao-esp32s3-respeaker-flex-circular-48k` | 从机 | 48 kHz | 主机、48 kHz |

第二种配置来自一台 Flex Circular 的实测，设备预装的 XMOS 固件版本未知。
当 XIAO 的时钟引脚设为输入时，观测到约 48 kHz 的 WS 信号；改用 XIAO
48 kHz 从机后，录音、唤醒及云端对话正常。这是该设备的实测结果，
不代表所有 Flex 出厂固件都使用相同的主从角色或采样率。

外部时钟模式下，小智现有音频服务将采集音频重采样至 16 kHz，
将播放音频重采样至 48 kHz。不要将此配置用于 XMOS 从机固件或 16 kHz 时钟源。

## 接线与操作

| 信号 | XIAO ESP32-S3 GPIO |
| --- | --- |
| I²S BCLK | 8 |
| I²S WS/LRCLK | 7 |
| ESP32-S3 向 XVF3800 输出音频 | 44 |
| ESP32-S3 接收 XVF3800 音频 | 43 |
| I²C SDA | 5 |
| I²C SCL | 6 |
| 地 | GND |

支持的预装 XIAO 产品已连接上述信号。自行接线时应共地并使用兼容 3.3V 的逻辑电平。
供电与喇叭连接请遵循官方说明；上表不是电源接线指南。

板级 codec 接收两个 32 位 I²S 槽位，选择左槽作为一路已处理的麦克风音频，
将有符号 32 位 PCM 转为 16 位，不附加增益。不把处理后的双通道输出当作
两路原始麦克风送入 ESP-SR BSS。小智单声道播放数据复制到左右槽，
由板载音频 codec/功放输出。

- **XIAO BOOT（GPIO0）**：单击开始/停止对话；启动阶段单击进入配网。
- **XIAO RST**：复位 ESP32-S3。
- **XIAO D3（GPIO4）**：接按键到 GND，单击控制对话；启动阶段单击进入配网，
  与板载 BOOT 的应用层操作相同。它不是硬件复位键，也不能代替 GPIO0 的 ROM BOOT 功能。
- **XIAO D0（GPIO1）**：可连接 12 颗 WS2812，显示下表中的助手灯效。
  请为灯带提供满足电流需求的 5 V 电源并与 XIAO 共地；不要从 GPIO 供电。
  建议在 GPIO1 与灯带 DIN 之间使用 5 V 兼容电平转换器。
- **XIAO D2（GPIO3）**：当前未使用。GPIO3 与启动绑带有关，外接按键前应确认电路不会
  在复位时改变其电平。
- **可选 BQ27220 电量计**：接到核心板的 `I2C_SDA`（D4/GPIO5）、`I2C_SCL`
  （D5/GPIO6）及共地。固件复用 XVF3800 的 I²C 总线，只读探测 `0x55`、器件型号、
  电压、电流、SOC 和设计容量。电池标称 4440 mWh / 3.7 V，对应约 1200 mAh；
  型号正确且读数有效时启用上报，设计容量不匹配只警告、不再关闭监测。
  未连接或读取失败时不提供电池数据，不影响启动。`self.get_device_status` 包含电量计的
  参考百分比、充电标记、电压（mV）、电流（mA，正为充电、负为放电）、电量计设计容量、
  电池标称容量及容量匹配标记；`soc_accuracy` 始终为 `unverified`，`soc_note` 提醒百分比
  未经校准。容量一致也不证明电芯曲线正确；不要据此推算准确剩余时间或实施自动关机。
- 启动时 `BQ27220 monitor v3` 标明探测所用的 SDA/SCL、地址及毫秒超时。
  `did not ACK` 表示地址未应答；`probe failed ... ESP_ERR_TIMEOUT` 表示探测超时，
  不能直接当作模块不存在，应检查供电、共地、接线及 3.3 V 上拉。
  识别成功后日志列出器件号、电压、SOC 和实际设计容量；容量不匹配会单独警告。
  设备号查询发送 `Control(0x0001)`，预期返回 `0x0220`。这只是信息查询，不写电芯参数。
  仅在启动时探测，断电接好模块后重启；不支持带电插拔或自动识别启动后才接入的模块。
- BQ27220 的 SOC 精度取决于电量计内已配置的电芯化学体系、容量及 CEDV 参数。本驱动
  **不会写入或修改电量计配置**；请确保模块已针对实际电芯完成配置，否则 SOC 可能不准确。
  I²C 逻辑电平必须兼容 3.3 V，勿让外接模块将 SDA/SCL 上拉到 5 V。BQ27220 不是充电器或
  保护板；电池采样、保护和供电需按电量计模块与电池规格连接。
- **Flex 核心板 RST、SafeMode/Boot**：作用于 XMOS，不是 XIAO 对话按键，
  本驱动没有将它们接入为用户按键。
- **状态灯**：使用低电平点亮的 XIAO GPIO21 LED，以及可选的 D0 WS2812 灯带；
  板载 LED 保持原有灯效，外接灯环使用渐变动效，不控制 XMOS 阵列上的灯。

### 外接助手灯环

| 状态 | 灯效 |
| --- | --- |
| 待机 | 极低亮度蓝色，约 5 秒一轮慢呼吸；不再完全熄灭 |
| 启动、连接 | 蓝青色柔和光晕缓慢绕环 |
| 聆听、录音测试 | 蓝青色呼吸，拾音能量使对称光晕增强；不表示声源方向 |
| 说话 | 蓝、青、紫渐变波纹，亮度随实际送入 I²S 的播放音频能量起伏 |
| 通知播放 | 暖琥珀色呼吸，同时响应播放音频能量 |
| 配网 | 约 3 秒一轮的琥珀色呼吸 |
| 激活 | 紫色慢呼吸 |
| 升级 | 薄荷绿光晕绕环，不代表实际升级百分比 |
| 致命错误 | 红色慢呼吸，不使用高频闪烁 |

灯环以约 40 帧/秒在独立低优先级任务中刷新；状态切换采用约 450 ms 的缓入缓出，
从当前颜色继续过渡，重复状态通知不会重置动画。亮度使用 2.2 伽马曲线，
单通道 PWM 上限为 80/255；这不是电流检测或硬件限流，电源仍需按灯带规格设计。
音频能量只计算本板 codec 已采集或 I²S 已接受的数据，包含当前播放音量影响；
短暂无音频时自动衰减。它不是逐采样同步、声源定位或云端情绪识别。
普通通知无法仅凭设备状态区分具体警告原因；未新增提示音，也不改变原有音频内容。
外接灯环、按钮和播放联动仍需实机验证。

动效设计参考 [Home Assistant Voice 灯环说明](https://www.home-assistant.io/voice-pe/),
以及 [Adafruit 伽马校正说明](https://learn.adafruit.com/led-tricks-gamma-correction/the-quick-fix)；
实际颜色、节奏与功耗上限是本板的设计选择，并非上述产品的精确复刻。

GPIO43/44 已用于音频，因此控制台使用 USB Serial/JTAG。
无显示屏时使用默认的 `NoDisplay`。

## 编译与烧录

在项目根目录使用 ESP-IDF 6.1 终端。项目最低要求 ESP-IDF 6.0.1；
本板型使用 6.1 进行构建验证。

推荐通过标准构建脚本选择固件变体，自动应用 8 MB Flash、
`partitions/v2/8m.csv` 分区、USB 控制台及时钟设置。

对于输出 48 kHz 时钟的 Flex Circular：

```powershell
python scripts\build.py seeed/xiao-esp32s3-respeaker-xvf3800 --name xiao-esp32s3-respeaker-flex-circular-48k --language zh-CN --wake-word nihaoxiaozhi
```

对于需要 16 kHz 主机时钟的 XMOS 从机固件：

```powershell
python scripts\build.py seeed/xiao-esp32s3-respeaker-xvf3800 --name xiao-esp32s3-respeaker-xvf3800 --language zh-CN --wake-word nihaoxiaozhi
```

包含 `/` 的板型参数是构建脚本标识。Linux/macOS 请把脚本路径写为
`scripts/build.py`。语言与唤醒词是用户选项，不是板型固定默认值。

构建成功后，可直接烧录：

```powershell
idf.py -p COM5 flash monitor
```

将 `COM5` 换成 XIAO 的实际端口。也可使用 Espressif Flash Download Tool：
芯片 ESP32-S3，`build\merged-binary.bin` 写入 **0x0**，DIO、80 MHz、8 MB。
小智固件应通过 XIAO USB 口烧录，不是 XMOS DFU 口。
擦除 Flash 前请备份需要保留的设备设置；擦除会清除 Wi-Fi 信息和其他 NVS 设置。

若手动使用 `idf.py menuconfig`，仅选择板型**不会**自动应用 `config.json`
中的固件变体配置，还需确认：

- 目标芯片：ESP32-S3。
- Flash：8 MB；自定义分区表：`partitions/v2/8m.csv`。
- 控制台：USB Serial/JTAG。
- `RESPEAKER_I2S_EXTERNAL_CLOCK_48K`：仅外部 48 kHz 时钟模式开启。
- 唤醒词引擎：AFE WakeNet，并选择受支持的模型。
- 资源：烧录默认资源，使所选模型包含在固件中。
- `RESPEAKER_I2S_CLOCK_PROBE_ONLY`：正常运行时关闭。

之后执行 `idf.py build` 和 `idf.py flash monitor`。若使用合并固件，还需执行
`idf.py merge-bin`；仅修改菜单配置不会更新旧的合并镜像。

## 诊断与时钟丢失处理

以下诊断选项均默认关闭，仅对本板型生效：

- `RESPEAKER_AUDIO_DIAGNOSTICS`：采集时每约 5 秒报告原始槽位峰值、
  所选通道 RMS、样本数、满量程计数、超时及四组原始样本。仅排查音频时开启。
- `RESPEAKER_I2S_CLOCK_PROBE_ONLY`：将 BCLK/WS 设为 GPIO 输入，
  不启动 I²S、Wi-Fi 或对话，只报告采样到的时钟跳变。WS 频率是轮询近似值，
  BCLK 跳变数不是频率值。没有跳变不代表一定是从机固件，
  也可能是 XMOS 尚未运行或未供电。

先选定构建变体，再通过 menuconfig 配置诊断选项，并使用 `idf.py build` /
`idf.py merge-bin` 构建。重新运行构建脚本会根据变体和用户选项重新生成 sdkconfig。
测试时钟配置时，请对 XIAO 和 XMOS 整体断电重启。

RX 超时采用限频日志报告。TX 分块写入使用有限等待，并在块之间检查整体写入
时间预算；时钟停止或写入失败时，报告已接受的数据量并丢弃剩余音频，
而不是无限阻塞。XMOS 复位可能中断正在播放的音频，
本实现不提供 XMOS 自动重新配置/恢复功能。

## 验证范围

| 项目 | 状态 |
| --- | --- |
| Flex Circular，预装 XMOS 主机约 48 kHz | 已观测到采集、GPIO0 按键、唤醒、语音识别及多轮云端对话正常 |
| 16 kHz 主机配置 | 仅构建验证，尚无对应硬件实测记录 |
| 被动时钟探测 | 已在测试设备上观测到外部时钟 |
| 实际喇叭播放与音量 | 尚未实机验证 |
| 播放回声消除/直接人声打断 | 尚未实机验证；本实现没有新增全双工/AEC 功能 |
| 线性 Flex、Lite、电池、外接屏幕或外接 LED | 不在本实现已验证范围内 |

收到云端回复或完成 TTS 解码，不代表接上的喇叭已经正常播放。
宣称支持播放及消回声前，还需真实硬件验证。
