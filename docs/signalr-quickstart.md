# SignalR 快速开始指南

## 🚀 快速集成步骤

### 1️⃣ 启用 SignalR 功能

```bash
idf.py menuconfig
```

导航到：**Xiaozhi Assistant** → 启用以下选项：
- `[*] Enable SignalR Client`
- （可选）配置默认 Hub URL 和 Token

### 2️⃣ 编译项目

```bash
idf.py build
```

### 3️⃣ 配置连接参数（运行时）

通过串口或 MCP 工具配置：

```cpp
// 方式 1：通过代码配置
Settings settings("signalr", true);
settings.SetString("hub_url", "wss://your-server.com/hub");
settings.Commit();

// 方式 2：通过 MCP 工具远程配置（推荐）
// 使用 MCP 的 storage/set 工具
```

**认证 Token**：
SignalR 会自动使用 Keycloak 登录的 access_token，无需手动配置。
确保设备已完成 Keycloak 认证流程。

### 4️⃣ 重启设备

配置后重启，SignalR 客户端将在网络激活后自动连接。

---

## 📡 服务端示例 (ASP.NET Core)

### 安装 SignalR

```bash
dotnet add package Microsoft.AspNetCore.SignalR
```

### 创建 Hub

```csharp
using Microsoft.AspNetCore.SignalR;

public class DeviceHub : Hub
{
    // 向特定设备发送消息
    public async Task SendToDevice(string connectionId, string message)
    {
        await Clients.Client(connectionId).SendAsync("CustomMessage", message);
    }
    
    // 向所有设备广播
    public async Task BroadcastToAll(string message)
    {
        await Clients.All.SendAsync("CustomMessage", message);
    }
    
    // 接收设备上报
    public async Task<string> DeviceReport(string status)
    {
        Console.WriteLine($"Device {Context.ConnectionId}: {status}");
        return "OK";
    }
    
    // 连接事件
    public override async Task OnConnectedAsync()
    {
        Console.WriteLine($"Device connected: {Context.ConnectionId}");
        await base.OnConnectedAsync();
    }
}
```

### 注册 SignalR 服务

```csharp
// Program.cs
var builder = WebApplication.CreateBuilder(args);

builder.Services.AddSignalR();

var app = builder.Build();

app.MapHub<DeviceHub>("/hub");

app.Run();
```

---

## 💬 消息发送示例

### 从服务端发送通知

```csharp
// 注入 IHubContext
public class NotificationService
{
    private readonly IHubContext<DeviceHub> _hubContext;
    
    public NotificationService(IHubContext<DeviceHub> hubContext)
    {
        _hubContext = hubContext;
    }
    
    public async Task SendNotification(string deviceId, string title, string content)
    {
        var message = new
        {
            action = "notification",
            title = title,
            content = content,
            emotion = "bell"
        };
        
        await _hubContext.Clients
            .Client(deviceId)
            .SendAsync("CustomMessage", JsonSerializer.Serialize(message));
    }
    
    public async Task SendCommand(string deviceId, string command)
    {
        var message = new
        {
            action = "command",
            command = command
        };
        
        await _hubContext.Clients
            .Client(deviceId)
            .SendAsync("CustomMessage", JsonSerializer.Serialize(message));
    }
}
```

---

## 🧪 测试连接

### 1. 使用 SignalR 客户端工具测试

```bash
# 安装 Microsoft SignalR CLI
npm install -g @microsoft/signalr

# 测试连接
signalr-cli -u wss://your-server.com/hub
```

### 2. 使用浏览器控制台

```javascript
// 在浏览器控制台测试
const connection = new signalR.HubConnectionBuilder()
    .withUrl("https://your-server.com/hub")
    .build();

connection.start().then(() => {
    console.log("Connected!");
    
    // 发送消息到设备
    connection.invoke("SendToDevice", "device-connection-id", 
        JSON.stringify({
            action: "notification",
            title: "测试",
            content: "这是一条测试消息"
        }));
});
```

### 3. 查看设备日志

```bash
idf.py monitor
```

查找类似日志：
```
I (12345) SignalRClient: SignalR client initialized with URL: wss://...
I (12456) SignalRClient: Connected to SignalR hub, connection ID: xxx
I (12567) SignalRClient: Received CustomMessage: {"action":"notification",...}
```

---

## 📊 支持的消息类型

### 1. 通知消息
```json
{
  "action": "notification",
  "title": "系统提示",
  "content": "您有新消息",
  "emotion": "bell"
}
```
**效果**：设备显示通知，播放提示音

### 2. 远程命令
```json
{
  "action": "command",
  "command": "reboot"
}
```
**支持的命令**：
- `reboot` - 重启设备
- `wake` - 触发唤醒词

### 3. 显示内容
```json
{
  "action": "display",
  "content": "要显示的内容"
}
```
**效果**：在设备屏幕显示指定文本

### 4. 自定义消息
任何不包含 `action` 的 JSON：
```json
{
  "custom_field": "value",
  "data": [1, 2, 3]
}
```
**效果**：作为系统消息显示在屏幕上

---

## 🔧 常见问题

### Q: SignalR 和 WebSocket Protocol 有什么区别？

**A**: 
- **WebSocket Protocol**: 用于语音对话的主路径（音频流 + 实时消息）
- **SignalR**: 独立的控制/通知通道，用于后台推送和远程命令

两者并行工作，互不干扰。

### Q: 如何查看连接状态？

**A**: 通过串口监控查看日志，或者实现 MCP 工具查询状态：
```cpp
auto& signalr = SignalRClient::GetInstance();
ESP_LOGI("Status", "SignalR: %s", signalr.GetConnectionState().c_str());
```

### Q: 可以从设备主动发送消息吗？

**A**: 可以！使用 `InvokeHubMethod` 或 `SendHubMessage`：
```cpp
SignalRClient::GetInstance().SendHubMessage(
    "DeviceReport", 
    "[\"status\", \"online\"]"
);
```

### Q: 内存占用多少？

**A**: 约 24KB RAM（SignalR 库 22KB + 封装类 2KB）

### Q: 如果网络断开会怎样？

**A**: SignalR 库会自动尝试重连（如果启用了 `CONFIG_SIGNALR_AUTO_RECONNECT`）

---

## ✅ 检查清单

在使用前确认：

- [ ] 已启用 `CONFIG_ENABLE_SIGNALR_CLIENT`
- [ ] 服务端 Hub 已部署并运行
- [ ] Hub URL 配置正确（wss:// 或 https://）
- [ ] 网络已连接
- [ ] 设备已完成激活流程
- [ ] **Keycloak 已登录**（SignalR 会自动使用 access_token）
- [ ] 堆内存充足（至少 30KB 可用）

---

## 📚 更多信息

详细文档请参考：[SignalR 集成完整文档](./signalr-integration.md)

---

**祝您使用愉快！** 🎉
