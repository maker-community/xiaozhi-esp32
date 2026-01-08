# SignalR 与 Keycloak 认证集成

## 认证流程

SignalR 客户端自动使用 Keycloak OAuth 2.0 登录获得的 access_token 进行认证。

### 工作原理

```
┌────────────────────────────────────────────────────┐
│  启动流程                                           │
├────────────────────────────────────────────────────┤
│  1. 设备启动 → 网络连接                              │
│  2. ActivationTask 开始                            │
│  3. InitializeProtocol() → WebSocket 连接          │
│  4. InitializeSignalR()                            │
│     ├── 读取 signalr/hub_url 配置                   │
│     ├── 读取 keycloak 配置 (server_url, realm, client_id) │
│     ├── KeycloakAuth::LoadTokens() 加载本地 token   │
│     ├── KeycloakAuth::GetAccessToken()             │
│     └── 使用 access_token 连接 SignalR Hub         │
└────────────────────────────────────────────────────┘
```

## Keycloak 登录流程

### 1. 设备扫码登录 (Device Authorization Grant)

通过 MCP Server 实现的 Keycloak 登录流程：

```cpp
// mcp_server.cc 中的登录实现
KeycloakAuth keycloak_auth(server_url, realm, client_id);

// 1. 请求设备码
DeviceCodeResponse device_response;
keycloak_auth.RequestDeviceCode(device_response);

// 2. 显示二维码给用户扫描
display->ShowQRCode(verification_uri, user_code);

// 3. 轮询等待用户授权
while (polling) {
    TokenResponse token_response;
    esp_err_t err = keycloak_auth.PollToken(device_code, token_response);
    
    if (err == ESP_OK) {
        // 4. 获得 token，保存到本地
        keycloak_auth.SaveTokens(token_response);
        break;
    }
}
```

### 2. Token 存储

Keycloak 将 token 保存在 NVS 的 `keycloak` 命名空间：

```cpp
// keycloak_auth.cc
void KeycloakAuth::SaveTokens(const TokenResponse& token_response) {
    Settings settings("keycloak", true);
    settings.SetString("access_token", access_token_);
    settings.SetString("refresh_token", refresh_token_);
    settings.SetInt("access_expires", access_token_expires_at_);
    settings.SetInt("refresh_expires", refresh_token_expires_at_);
    settings.Commit();
}
```

### 3. SignalR 读取 Token

```cpp
// application.cc
void Application::InitializeSignalR() {
    // 读取 Keycloak 配置
    Settings keycloak_settings("keycloak", false);
    std::string server_url = keycloak_settings.GetString("server_url");
    std::string realm = keycloak_settings.GetString("realm");
    std::string client_id = keycloak_settings.GetString("client_id");
    
    // 创建 Keycloak 认证对象并加载 token
    KeycloakAuth keycloak_auth(server_url, realm, client_id);
    keycloak_auth.LoadTokens();
    
    // 检查认证状态并获取 token
    if (keycloak_auth.IsAuthenticated()) {
        std::string token = keycloak_auth.GetAccessToken();
        // 使用 token 初始化 SignalR
        signalr.Initialize(hub_url, token);
    }
}
```

## Token 刷新

Keycloak 支持自动刷新 access_token：

```cpp
// keycloak_auth.cc
bool KeycloakAuth::IsAuthenticated() {
    if (access_token_.empty()) {
        return false;
    }
    
    time_t now = time(nullptr);
    
    // 如果 access_token 即将过期（1分钟内），尝试刷新
    if (now >= access_token_expires_at_ - 60) {
        if (RefreshToken() != ESP_OK) {
            return false;
        }
    }
    
    return true;
}
```

**注意**：当前实现中，SignalR 连接建立后不会动态更新 token。如果需要支持 token 自动刷新，需要：

1. 监听 Keycloak token 刷新事件
2. 断开并重新连接 SignalR（使用新 token）
3. 或者实现 SignalR 的动态 token 更新机制

## 服务端验证

### ASP.NET Core SignalR Hub 配置

```csharp
// Startup.cs 或 Program.cs
builder.Services.AddAuthentication(JwtBearerDefaults.AuthenticationScheme)
    .AddJwtBearer(options =>
    {
        options.Authority = "https://keycloak.example.com/realms/myrealm";
        options.Audience = "your-client-id";
        options.RequireHttpsMetadata = true;
        
        // 允许从 WebSocket 查询字符串获取 token（可选）
        options.Events = new JwtBearerEvents
        {
            OnMessageReceived = context =>
            {
                var accessToken = context.Request.Query["access_token"];
                var path = context.HttpContext.Request.Path;
                
                if (!string.IsNullOrEmpty(accessToken) && path.StartsWithSegments("/hub"))
                {
                    context.Token = accessToken;
                }
                
                return Task.CompletedTask;
            }
        };
    });

builder.Services.AddAuthorization();

var app = builder.Build();

app.UseAuthentication();
app.UseAuthorization();

app.MapHub<DeviceHub>("/hub").RequireAuthorization();
```

### Hub 中访问用户信息

```csharp
public class DeviceHub : Hub
{
    public override async Task OnConnectedAsync()
    {
        var userId = Context.User?.FindFirst("sub")?.Value;
        var username = Context.User?.FindFirst("preferred_username")?.Value;
        
        Console.WriteLine($"User {username} (ID: {userId}) connected from device {Context.ConnectionId}");
        
        await base.OnConnectedAsync();
    }
    
    [Authorize]
    public async Task SendMessage(string message)
    {
        // 只有认证用户才能调用
        var userId = Context.User.FindFirst("sub")?.Value;
        await Clients.All.SendAsync("ReceiveMessage", userId, message);
    }
}
```

## 配置示例

### 设备端配置（通过 MCP 工具）

```json
{
  "keycloak": {
    "server_url": "https://keycloak.example.com",
    "realm": "myrealm",
    "client_id": "iot-device"
  },
  "signalr": {
    "hub_url": "wss://your-server.com/hub"
  }
}
```

### 工作流程

1. **首次使用**：
   - 设备通过 MCP 的 Keycloak 登录工具扫码登录
   - 获得 access_token 和 refresh_token 保存到 NVS
   
2. **后续启动**：
   - 设备启动时自动加载本地 token
   - SignalR 使用 access_token 连接 Hub
   - 如果 token 过期，自动使用 refresh_token 刷新

3. **Token 过期处理**：
   - access_token 有效期通常 5-15 分钟
   - refresh_token 有效期通常 7-30 天
   - 自动刷新逻辑确保无感知续期

## 安全建议

1. **使用 HTTPS/WSS**
   - 始终使用加密连接传输 token
   - Keycloak 服务器和 SignalR Hub 都应启用 TLS

2. **Token 存储安全**
   - Token 存储在 ESP32 加密 NVS 分区
   - 考虑启用 Flash 加密功能

3. **最小权限原则**
   - Keycloak 客户端配置最小必要权限
   - SignalR Hub 方法使用细粒度授权

4. **Token 泄露处理**
   - 实现 token 撤销机制
   - 定期轮换客户端凭证

## 故障排除

### SignalR 连接失败：Unauthorized

**原因**：Token 无效或过期

**解决**：
1. 检查 Keycloak 是否已登录：`keycloak_auth.IsAuthenticated()`
2. 检查 token 有效期
3. 重新登录 Keycloak
4. 检查服务端 JWT 验证配置

### Token 刷新失败

**原因**：refresh_token 过期

**解决**：
1. 用户需要重新扫码登录
2. 考虑延长 refresh_token 有效期（Keycloak 配置）
3. 实现登录状态监控和提醒

### 服务端收不到请求

**原因**：CORS 或 WebSocket 握手失败

**解决**：
1. 检查 CORS 配置
2. 验证 Bearer Token 格式
3. 查看服务端日志

## 参考资料

- [Keycloak Device Authorization Grant](https://www.keycloak.org/docs/latest/securing_apps/#_device_authorization_grant)
- [ASP.NET Core JWT Bearer Authentication](https://docs.microsoft.com/aspnet/core/security/authentication/jwt-authn)
- [SignalR Authentication and Authorization](https://docs.microsoft.com/aspnet/core/signalr/authn-and-authz)
- [OAuth 2.0 Device Authorization Grant (RFC 8628)](https://datatracker.ietf.org/doc/html/rfc8628)

---

**集成完成日期**: 2026-01-08
