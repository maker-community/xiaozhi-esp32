# AtomEchoS3R
## 简介

AtomEchoS3R 是 M5Stack 推出的基于 ESP32-S3-PICO-1-N8R8 的物联网可编程控制器，采用了 ES8311 单声道音频解码器、MEMS 麦克风和 NS4150B 功率放大器的集成方案。

> **Verdure Buddy Rover 改造版**：在 AtomEchoS3R 基础上新增
> - 1.47 英寸 ST7789 172×320 SPI 彩屏（接底部 GPIO G5/G6/G7/G8/G38/G39）
> - Grove 口自定义 UART 命令控制（G1=GPIO1 TX, G2=GPIO2 RX, 115200）
>
> 音频引脚未改动，与原始 AtomEchoS3R 完全一致。

## 引脚分配

| 功能 | GPIO | 备注 |
| ---- | ---- | ---- |
| LCD CS | GPIO5 (G5) | 屏幕片选 |
| LCD MOSI | GPIO6 (G6) | SPI 数据 |
| LCD SCLK | GPIO7 (G7) | SPI 时钟 |
| LCD DC | GPIO8 (G8) | 数据/命令 |
| LCD RST | GPIO38 (G38) | 复位 |
| LCD BL | GPIO39 (G39) | 背光 (PWM) |
| 串口 TX | GPIO1 (G1) | UART 命令控制 |
| 串口 RX | GPIO2 (G2) | UART 命令控制 |

## 串口命令

波特率 115200，8N1，以换行符 `\n` 结尾（可带 `\r\n`），命令不区分大小写：

| 命令 | 说明 |
| ---- | ---- |
| `help` / `?` | 显示帮助 |
| `status` | 显示设备状态与音量 |
| `toggle` | 切换对话/聆听状态 |
| `wake [词]` | 触发唤醒词（默认“你好小智”） |
| `listen` | 开始聆听 |
| `stop` | 停止聆听 |
| `volume [0-100]` | 查询/设置输出音量 |
| `screen <文本>` | 在屏幕上显示通知 |
| `backlight [0-100]` | 查询/设置背光亮度 |
| `reboot` | 重启设备 |

## 配置、编译命令

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig 并配置**

```bash
idf.py menuconfig
```

分别配置如下选项：

- `Xiaozhi Assistant` → `Board Type` → 选择 `AtomEchoS3R`
- `Partition Table` → `Custom partition CSV file` → 删除原有内容，输入 `partitions/v2/8m.csv`
- `Serial flasher config` → `Flash size` → 选择 `8 MB`
- `Component config` → `ESP PSRAM` → `Support for external, SPI-connected RAM` → `SPI RAM config` → 选择 `Octal Mode PSRAM`

按 `S` 保存，按 `Q` 退出。

**编译**

```bash
idf.py build
```

**烧录**

将 AtomEchoS3R 连接到电脑，按住侧面 RESET 按键，直到 RESET 按键下方绿灯闪烁。

```bash
idf.py flash
```

烧录完毕后，按一下 RESET 按钮重启设备。
