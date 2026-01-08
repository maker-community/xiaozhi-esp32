# SignalR 客户端集成文档

## 概述

SignalR 客户端已成功集成到 Xiaozhi-ESP32 项目中，提供独立的实时双向通信通道，与现有的 WebSocket 音频流通道并行工作。

## 架构设计

```
┌─────────────────────────────────────────────────────┐
│              Application (主应用)                    │
├──────────────────────────┬──────────────────────────┤
│  WebSocket Protocol      │  SignalR Client          │
│  ├── 音频流 (双向)        │  ├── 独立连接             │
│  ├── JSON 消息           │  ├── Hub 消息推送         │
│  │   ├── tts/stt        │  ├── 远程命令             │
│  │   ├── llm            │  └── 实时通知             │
│  │   └── mcp            │                          │
│  └── 随对话打开/关闭      │  长连接保持               │
└──────────────────────────┴──────────────────────────┘
         ↓                           ↓
    WebSocket Server          SignalR Hub
```

## 功能特性

### ✅ 已实现功能

1. **SignalRClient 封装类**
   - 单例模式管理连接
   - 线程安全的消息处理
   - 自动重连支持（库内置）

2. **消息类型支持**
   - `notification` - 通知消息（带标题、内容、表情）
   - `command` - 远程命令（reboot、wake 等）
   - `display` - 显示自定义内容
   - 自定义 JSON 消息

3. **集成方式**
   - 通过 Kconfig 编译时可配置
   - 运行时通过 NVS 配置连接参数
   - 与现有 Protocol 系统并行运行
   - 使用 Application::Schedule() 保证线程安全

4. **状态管理**
   - 连接状态监控
   - 错误处理和日志记录
   - 断线重连支持

## 配置方法

### 1. 编译时配置 (menuconfig)

```bash
idf.py menuconfig
```

导航到：`Xiaozhi Assistant` → 启用以下选项：

```
[*] Enable SignalR Client
    SignalR Hub URL: wss://your-server.com/hub
[*] Enable SignalR Auto Reconnect
```

**注意**：认证 Token 会自动从 Keycloak 登录信息中获取，无需手动配置。

### 2. 运行时配置 (NVS Settings)

通过 Settings API 配置：

```cpp
Settings settings("signalr", true);
settings.SetString("hub_url", "wss://your-server.com/hub");
settings.Commit();
```

**Token 认证**：
SignalR 客户端会自动从 Keycloak 登录的 access_token 中获取认证信息。
确保设备已通过 Keycloak 登录后，SignalR 连接将自动带上 Bearer Token。

或通过 MCP 工具远程配置：
```json
{
  "namespace": "signalr",
  "hub_url": "wss://your-server.com/hub",
  "token": "Bearer xxx..."
}
```

## 使用方法

### 服务端 Hub 实现 (ASP.NET Core)

```csharp
public class DeviceHub : Hub
{
    // 向设备发送自定义消息
    public async Task SendCustomMessage(string deviceId, object message)
    {
        await Clients.Client(deviceId).SendAsync("CustomMessage", 
            JsonSerializer.Serialize(message));
    }
    
    // 接收设备的消息
    public async Task<string> ReceiveFromDevice(string message)
    {
        // 处理来自设备的消息
        return "Message received";
    }
}
```

### 消息格式

#### 1. 通知消息
```json
{
  "action": "notification",
  "title": "系统提示",
  "content": "您有新消息",
  "emotion": "bell"
}
```

#### 2. 命令消息
```json
{
  "action": "command",
  "command": "reboot"
}
```

支持的命令：
- `reboot` - 重启设备
- `wake` - 触发唤醒词检测

#### 3. 显示消息
```json
{
  "action": "display",
  "content": "要显示的文本内容"
}
```

#### 4. 自定义消息
任何不包含 `action` 字段的 JSON 都会作为系统消息显示。

### 设备端主动调用 Hub 方法

```cpp
auto& signalr = SignalRClient::GetInstance();

// 发送消息（fire and forget）
signalr.SendHubMessage("SendStatus", "[\"online\", 100]");

// 调用方法并获取返回值
signalr.InvokeHubMethod("GetConfiguration", "[]", 
    [](bool success, const std::string& result) {
        if (success) {
            ESP_LOGI("SignalR", "Result: %s", result.c_str());
        }
    });
```

## API 参考

### SignalRClient 类

#### 初始化
```cpp
bool Initialize(const std::string& hub_url, const std::string& token = "");
```

#### 连接管理
```cpp
bool Connect();                    // 连接到 Hub
void Disconnect();                 // 断开连接
bool IsConnected() const;          // 检查连接状态
std::string GetConnectionState();  // 获取状态字符串
```

#### 消息处理
```cpp
void OnCustomMessage(std::function<void(const cJSON*)> callback);
void OnConnectionStateChanged(std::function<void(bool, const std::string&)> callback);
```

#### Hub 方法调用
```cpp
void InvokeHubMethod(
    const std::string& method_name,
    const std::string& args_json = "[]",
    std::function<void(bool, const std::string&)> callback = nullptr
);

void SendHubMessage(
    const std::string& method_name,
    const std::string& args_json = "[]"
);
```

## 内存使用

- SignalR 客户端库：~22KB RAM
- SignalRClient 封装类：~2KB
- **总计：约 24KB RAM**

## 调试

启用详细日志：

```cpp
// 在 signalr_client.cc 修改 TAG 日志级别
esp_log_level_set("SignalRClient", ESP_LOG_DEBUG);
```

查看连接状态：
```cpp
auto& signalr = SignalRClient::GetInstance();
ESP_LOGI("DEBUG", "State: %s", signalr.GetConnectionState().c_str());
```

## 故障排除

### 1. 编译错误：找不到 hub_connection.h

**原因**：未启用 SignalR 客户端
**解决**：运行 `idf.py menuconfig` 并启用 `CONFIG_ENABLE_SIGNALR_CLIENT`

### 2. 连接失败

**检查**：
- Hub URL 格式正确（wss:// 或 https://）
- 网络已连接
- 服务器 Hub 正常运行
- Token 有效（如果需要）

### 3. 消息未接收

**检查**：
- Hub 方法名为 "CustomMessage"
- 消息格式为 JSON 字符串
- 已注册 `OnCustomMessage` 回调

### 4. 内存不足

SignalR 需要约 24KB RAM，如果内存紧张：
- 增加堆大小配置
- 禁用其他不必要的功能
- 考虑使用外部 PSRAM

## 示例场景

### 场景 1：远程通知推送
服务器推送天气预报通知到设备显示屏。

### 场景 2：远程控制命令
通过 Web/App 远程重启设备或触发唤醒词。

### 场景 3：实时状态同步
设备状态变化实时推送到服务器，服务器配置变更实时下发到设备。

### 场景 4：多设备协同
通过 SignalR 群组功能实现多设备消息广播。

## 与现有系统的区别

| 特性 | WebSocket Protocol | SignalR Client |
|------|-------------------|----------------|
| **用途** | 语音对话主路径 | 控制/通知通道 |
| **连接时机** | 按需连接（对话时） | 激活后长连接 |
| **消息类型** | 固定格式(tts/stt/mcp) | 灵活的 Hub 方法 |
| **重连策略** | 手动管理 | 自动重连 |
| **双向通信** | 有限支持 | 完全双向 RPC |

## 后续扩展

可能的增强功能：
1. 支持 MessagePack 协议（更小的消息体）
2. 添加消息队列缓存
3. 支持文件传输
4. 实现设备间 P2P 通信
5. 添加消息加密

## 参考资料

- [esp-signalr GitHub](https://github.com/maker-community/esp-signalr)
- [ASP.NET Core SignalR 文档](https://docs.microsoft.com/aspnet/core/signalr)
- [SignalR Protocol Specification](https://github.com/dotnet/aspnetcore/blob/main/src/SignalR/docs/specs/HubProtocol.md)

---

**集成完成日期**: 2026-01-08
**版本**: 1.0.0
