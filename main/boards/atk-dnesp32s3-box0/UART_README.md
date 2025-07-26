# ATK-DNESP32S3-BOX0 串口功能说明

## 概述

本文档说明了为 ATK-DNESP32S3-BOX0 开发板新增的串口通信功能，用于通过 UART 控制外部设备。

## 硬件配置

### 串口引脚配置
- **TX (发送)**: GPIO45
- **RX (接收)**: GPIO46
- **波特率**: 115200
- **数据位**: 8
- **停止位**: 1
- **校验位**: 无
- **流控**: 无

### 连接方式
```
ATK-DNESP32S3-BOX0    外部设备
     GPIO45    ------>    RX
     GPIO46    <------    TX
       GND     --------    GND
```

## 功能特性

### 1. 基础串口通信
- 支持命令发送和响应接收
- 超时处理机制
- 自动添加换行符（\r\n）
- 响应数据清理和格式化

### 2. MCP 协议集成
通过 MCP (Model Context Protocol) 提供以下控制工具：

#### 基础控制
- `self.device.send_command` - 发送自定义命令
- `self.device.init` - 初始化机器人
- `self.device.home` - 回到初始位置
- `self.device.stop` - 停止设备
- `self.device.get_status` - 获取设备状态

#### 运动控制
- `self.device.walk` - 机器人行走（通用）
  - 参数: steps (1-10), speed (500-3000), direction (-1到1), amount (0-50)
- `self.device.move_forward` - 前进控制
  - 参数: steps (1-10), speed (500-3000)
- `self.device.move_backward` - 后退控制
  - 参数: steps (1-10), speed (500-3000)
- `self.device.turn` - 转向控制（通用）
  - 参数: steps (1-5), speed (1000-3000), direction (-1到1), amount (0-50)
- `self.device.turn_left` - 左转控制
  - 参数: steps (1-5), speed (1000-3000)
- `self.device.turn_right` - 右转控制
  - 参数: steps (1-5), speed (1000-3000)

#### 动作控制
- `self.device.jump` - 机器人跳跃
  - 参数: steps (1-3), speed (1000-3000)
- `self.device.swing` - 机器人摇摆
  - 参数: steps (1-5), speed (500-2000), height (10-50)

#### 手部动作
- `self.device.hands_up` - 举手动作
  - 参数: speed (500-2000), direction (-1到1)
- `self.device.hands_down` - 放手动作
  - 参数: speed (500-2000), direction (-1到1)
- `self.device.hand_wave` - 挥手动作
  - 参数: speed (500-2000), direction (-1到1)

#### 设备控制
- `self.device.move_servo` - 控制舵机
  - 参数: servo (1-8), position (0-180)

## 使用示例

### 1. 直接命令发送
```cpp
// 发送自定义命令
std::string response = SendUartCommand("HELLO", 3000);
```

### 2. MCP 工具调用示例

#### 前进命令
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.device.move_forward",
    "arguments": {
      "steps": 5,
      "speed": 1500
    }
  },
  "id": 1
}
```

#### 机器人行走（通用命令）
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.device.walk",
    "arguments": {
      "steps": 3,
      "speed": 1200,
      "direction": 1,
      "amount": 30
    }
  },
  "id": 2
}
```

#### 举手动作
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.device.hands_up",
    "arguments": {
      "speed": 1000,
      "direction": 0
    }
  },
  "id": 3
}
```

## 外部设备协议规范

为了确保正确通信，外部设备应遵循以下协议规范：

### 命令格式
- 发送: `COMMAND [参数]\r\n`
- 响应: `OK: [结果]` 或 `ERROR: [错误信息]`

### 支持的标准命令
- `INIT` - 设备初始化
- `HOME [hands_down]` - 回到初始位置
- `WALK [steps] [speed] [direction] [amount]` - 行走
- `TURN [steps] [speed] [direction] [amount]` - 转向
- `JUMP [steps] [speed]` - 跳跃
- `SWING [steps] [speed] [height]` - 摇摆
- `HANDS_UP [speed] [direction]` - 举手
- `HANDS_DOWN [speed] [direction]` - 放手
- `HAND_WAVE [speed] [direction]` - 挥手
- `STOP` - 停止
- `GET_STATUS` - 获取状态
- `SERVO_MOVE [servo] [position]` - 控制舵机

### 响应示例
```
发送: WALK 3 1200 1 30\r\n
响应: OK: 前进3步完成

发送: HANDS_UP 1000 0\r\n
响应: OK: 双手举起完成

发送: TURN 2 2000 1 0\r\n
响应: OK: 左转2步完成

发送: INVALID_COMMAND\r\n
响应: ERROR: 未知命令
```

## 调试和日志

### 日志级别
- `ESP_LOGI` - 正常操作信息
- `ESP_LOGW` - 警告信息（如超时）
- `ESP_LOGE` - 错误信息
- `ESP_LOGD` - 调试详细信息

### 常见日志消息
```
I (12345) atk_dnesp32s3_box0: 开始初始化UART串口通讯...
I (12346) atk_dnesp32s3_box0: UART引脚配置完成 - TXD: GPIO45, RXD: GPIO46
I (12347) atk_dnesp32s3_box0: ✅ UART初始化完成，TXD:45, RXD:46
I (12348) atk_dnesp32s3_box0: 发送UART命令: [INIT] (长度: 6)
I (12349) atk_dnesp32s3_box0: 接收到响应: [OK: 初始化完成] (总长度: 12)
```

## 故障排除

### 常见问题

1. **无响应或超时**
   - 检查外部设备是否正确连接
   - 确认波特率设置是否匹配
   - 检查电源和地线连接

2. **响应异常**
   - 确认外部设备协议是否符合规范
   - 检查命令格式是否正确
   - 查看详细日志信息

3. **初始化失败**
   - 检查GPIO引脚是否有冲突
   - 确认UART端口是否已被占用
   - 检查硬件连接

### 调试建议
1. 使用串口调试工具验证外部设备通信
2. 查看详细日志输出
3. 逐步测试基础命令
4. 确认硬件连接和电源供应

## 扩展开发

### 添加新的MCP工具
```cpp
mcp_server.AddTool("self.device.custom_action", "自定义动作",
    PropertyList({Property("param1", kPropertyTypeInteger, 1, 0, 100)}),
    [this](const PropertyList& properties) -> ReturnValue {
        int param1 = properties["param1"].value<int>();
        std::string command = "CUSTOM " + std::to_string(param1);
        std::string response = SendUartCommand(command.c_str(), 3000);
        return "自定义动作完成 | 响应: " + response;
    });
```

### 修改超时时间
```cpp
// 在 SendUartCommand 调用中指定超时时间（毫秒）
std::string response = SendUartCommand("COMMAND", 5000);  // 5秒超时
```

## 参考资料

- [ESP32-S3 UART 编程指南](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/uart.html)
- [MCP 协议文档](../../docs/mcp-protocol.md)
- [Lichuang Dev 串口实现参考](../lichuang-dev/lichuang_dev_board.cc)
- [ESP SparkBot 串口实现参考](../esp-sparkbot/esp_sparkbot_board.cc)
