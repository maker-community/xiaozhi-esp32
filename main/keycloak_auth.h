#ifndef KEYCLOAK_AUTH_H
#define KEYCLOAK_AUTH_H

#include <string>
#include <memory>
#include <esp_err.h>

class Settings;
class Http;

/**
 * KeycloakAuth - OAuth 2.0 Device Authorization Grant (RFC 8628)
 * 实现Keycloak的设备流登录，适合IoT设备扫码认证
 */
class KeycloakAuth {
public:
    struct DeviceCodeResponse {
        std::string device_code;        // 设备码（用于轮询）
        std::string user_code;           // 用户码（显示给用户）
        std::string verification_uri;    // 验证URL
        std::string verification_uri_complete; // 完整验证URL（带user_code）
        int expires_in;                  // 过期时间（秒）
        int interval;                    // 轮询间隔（秒）
    };

    struct TokenResponse {
        std::string access_token;
        std::string refresh_token;
        std::string token_type;
        int expires_in;
        int refresh_expires_in;
    };

    /**
     * 构造函数
     * @param server_url Keycloak服务器地址，例如: https://keycloak.example.com
     * @param realm Realm名称，例如: myrealm
     * @param client_id 客户端ID
     */
    KeycloakAuth(const std::string& server_url, const std::string& realm, const std::string& client_id);
    ~KeycloakAuth();

    /**
     * 请求设备码
     * @return DeviceCodeResponse 包含设备码、用户码、验证URL等
     */
    esp_err_t RequestDeviceCode(DeviceCodeResponse& response);

    /**
     * 轮询token（在用户扫码授权后）
     * @param device_code 设备码
     * @param token_response 返回的token信息
     * @return ESP_OK 表示成功获取token，ESP_ERR_TIMEOUT 表示pending（继续轮询），其他表示错误
     */
    esp_err_t PollToken(const std::string& device_code, TokenResponse& token_response);

    /**
     * 刷新access token
     * @return ESP_OK 表示刷新成功
     */
    esp_err_t RefreshToken();

    /**
     * 检查是否已认证（有有效的access token）
     * @return true 已认证且未过期
     */
    bool IsAuthenticated();

    /**
     * 获取当前access token
     */
    std::string GetAccessToken();

    /**
     * 获取当前refresh token
     */
    std::string GetRefreshToken();

    /**
     * 保存token到本地存储
     */
    void SaveTokens(const TokenResponse& token_response);

    /**
     * 从本地存储加载token
     */
    void LoadTokens();

    /**
     * 清除本地token
     */
    void ClearTokens();

private:
    std::string server_url_;
    std::string realm_;
    std::string client_id_;
    
    std::string access_token_;
    std::string refresh_token_;
    int64_t access_token_expires_at_ = 0;  // Unix timestamp
    int64_t refresh_token_expires_at_ = 0; // Unix timestamp

    std::unique_ptr<Settings> settings_;

    std::string GetDeviceAuthUrl();
    std::string GetTokenUrl();
    esp_err_t ParseJsonResponse(const std::string& json, DeviceCodeResponse& response);
    esp_err_t ParseJsonResponse(const std::string& json, TokenResponse& response);
};

#endif // KEYCLOAK_AUTH_H
