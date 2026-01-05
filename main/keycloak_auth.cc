#include "keycloak_auth.h"
#include "settings.h"
#include "board.h"
#include "http.h"

#include <esp_log.h>
#include <cJSON.h>
#include <sys/time.h>

#define TAG "KeycloakAuth"

KeycloakAuth::KeycloakAuth(const std::string& server_url, const std::string& realm, const std::string& client_id)
    : server_url_(server_url), realm_(realm), client_id_(client_id) {
    settings_ = std::make_unique<Settings>("keycloak", true);
    LoadTokens();
}

KeycloakAuth::~KeycloakAuth() {
}

std::string KeycloakAuth::GetDeviceAuthUrl() {
    return server_url_ + "/realms/" + realm_ + "/protocol/openid-connect/auth/device";
}

std::string KeycloakAuth::GetTokenUrl() {
    return server_url_ + "/realms/" + realm_ + "/protocol/openid-connect/token";
}

esp_err_t KeycloakAuth::RequestDeviceCode(DeviceCodeResponse& response) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(30);
    
    std::string url = GetDeviceAuthUrl();
    std::string post_data = "client_id=" + client_id_;
    
    http->SetHeader("Content-Type", "application/x-www-form-urlencoded");
    
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open device auth URL");
        return ESP_FAIL;
    }
    
    http->Write(post_data.c_str(), post_data.size());
    http->Write("", 0); // Finish writing
    
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Device auth request failed with status: %d", status_code);
        http->Close();
        return ESP_FAIL;
    }
    
    std::string json_response = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "Device code response: %s", json_response.c_str());
    
    return ParseJsonResponse(json_response, response);
}

esp_err_t KeycloakAuth::ParseJsonResponse(const std::string& json, DeviceCodeResponse& response) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse device code response JSON");
        return ESP_FAIL;
    }
    
    cJSON* device_code = cJSON_GetObjectItem(root, "device_code");
    cJSON* user_code = cJSON_GetObjectItem(root, "user_code");
    cJSON* verification_uri = cJSON_GetObjectItem(root, "verification_uri");
    cJSON* verification_uri_complete = cJSON_GetObjectItem(root, "verification_uri_complete");
    cJSON* expires_in = cJSON_GetObjectItem(root, "expires_in");
    cJSON* interval = cJSON_GetObjectItem(root, "interval");
    
    if (!device_code || !user_code || !verification_uri || !expires_in) {
        ESP_LOGE(TAG, "Missing required fields in device code response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    response.device_code = device_code->valuestring;
    response.user_code = user_code->valuestring;
    response.verification_uri = verification_uri->valuestring;
    response.expires_in = expires_in->valueint;
    response.interval = interval ? interval->valueint : 5;
    
    if (verification_uri_complete && cJSON_IsString(verification_uri_complete)) {
        response.verification_uri_complete = verification_uri_complete->valuestring;
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t KeycloakAuth::PollToken(const std::string& device_code, TokenResponse& token_response) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(30);
    
    std::string url = GetTokenUrl();
    std::string post_data = "grant_type=urn:ietf:params:oauth:grant-type:device_code&"
                           "client_id=" + client_id_ + "&"
                           "device_code=" + device_code;
    
    http->SetHeader("Content-Type", "application/x-www-form-urlencoded");
    
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open token URL");
        return ESP_FAIL;
    }
    
    http->Write(post_data.c_str(), post_data.size());
    http->Write("", 0);
    
    int status_code = http->GetStatusCode();
    std::string json_response = http->ReadAll();
    http->Close();
    
    if (status_code == 400) {
        // 检查是否是 authorization_pending
        cJSON* root = cJSON_Parse(json_response.c_str());
        if (root) {
            cJSON* error = cJSON_GetObjectItem(root, "error");
            if (error && cJSON_IsString(error)) {
                std::string error_str = error->valuestring;
                cJSON_Delete(root);
                
                if (error_str == "authorization_pending") {
                    ESP_LOGD(TAG, "Authorization pending, continue polling");
                    return ESP_ERR_TIMEOUT;
                } else if (error_str == "slow_down") {
                    ESP_LOGW(TAG, "Polling too fast, slow down");
                    return ESP_ERR_TIMEOUT;
                }
            }
            cJSON_Delete(root);
        }
        ESP_LOGE(TAG, "Token request failed: %s", json_response.c_str());
        return ESP_FAIL;
    }
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Token request failed with status: %d", status_code);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Token response: %s", json_response.c_str());
    
    return ParseJsonResponse(json_response, token_response);
}

esp_err_t KeycloakAuth::ParseJsonResponse(const std::string& json, TokenResponse& response) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse token response JSON");
        return ESP_FAIL;
    }
    
    cJSON* access_token = cJSON_GetObjectItem(root, "access_token");
    cJSON* refresh_token = cJSON_GetObjectItem(root, "refresh_token");
    cJSON* token_type = cJSON_GetObjectItem(root, "token_type");
    cJSON* expires_in = cJSON_GetObjectItem(root, "expires_in");
    cJSON* refresh_expires_in = cJSON_GetObjectItem(root, "refresh_expires_in");
    
    if (!access_token || !token_type || !expires_in) {
        ESP_LOGE(TAG, "Missing required fields in token response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    response.access_token = access_token->valuestring;
    response.token_type = token_type->valuestring;
    response.expires_in = expires_in->valueint;
    
    if (refresh_token && cJSON_IsString(refresh_token)) {
        response.refresh_token = refresh_token->valuestring;
    }
    
    if (refresh_expires_in && cJSON_IsNumber(refresh_expires_in)) {
        response.refresh_expires_in = refresh_expires_in->valueint;
    } else {
        response.refresh_expires_in = 0;
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t KeycloakAuth::RefreshToken() {
    if (refresh_token_.empty()) {
        ESP_LOGE(TAG, "No refresh token available");
        return ESP_FAIL;
    }
    
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(30);
    
    std::string url = GetTokenUrl();
    std::string post_data = "grant_type=refresh_token&"
                           "client_id=" + client_id_ + "&"
                           "refresh_token=" + refresh_token_;
    
    http->SetHeader("Content-Type", "application/x-www-form-urlencoded");
    
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open token URL for refresh");
        return ESP_FAIL;
    }
    
    http->Write(post_data.c_str(), post_data.size());
    http->Write("", 0);
    
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Refresh token request failed with status: %d", status_code);
        http->Close();
        return ESP_FAIL;
    }
    
    std::string json_response = http->ReadAll();
    http->Close();
    
    TokenResponse token_response;
    esp_err_t ret = ParseJsonResponse(json_response, token_response);
    if (ret == ESP_OK) {
        SaveTokens(token_response);
    }
    
    return ret;
}

bool KeycloakAuth::IsAuthenticated() {
    if (access_token_.empty()) {
        return false;
    }
    
    // 获取当前时间戳
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now = tv.tv_sec;
    
    // 检查access token是否过期（提前60秒刷新）
    if (now >= access_token_expires_at_ - 60) {
        ESP_LOGI(TAG, "Access token expired or expiring soon");
        
        // 尝试刷新
        if (!refresh_token_.empty() && now < refresh_token_expires_at_) {
            ESP_LOGI(TAG, "Attempting to refresh token");
            if (RefreshToken() == ESP_OK) {
                return true;
            }
        }
        
        return false;
    }
    
    return true;
}

std::string KeycloakAuth::GetAccessToken() {
    return access_token_;
}

std::string KeycloakAuth::GetRefreshToken() {
    return refresh_token_;
}

void KeycloakAuth::SaveTokens(const TokenResponse& token_response) {
    access_token_ = token_response.access_token;
    refresh_token_ = token_response.refresh_token;
    
    // 计算过期时间戳
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now = tv.tv_sec;
    
    access_token_expires_at_ = now + token_response.expires_in;
    refresh_token_expires_at_ = now + token_response.refresh_expires_in;
    
    // 保存到NVS
    settings_->SetString("access_token", access_token_);
    settings_->SetString("refresh_token", refresh_token_);
    settings_->SetInt("access_expires", access_token_expires_at_);
    settings_->SetInt("refresh_expires", refresh_token_expires_at_);
    
    ESP_LOGI(TAG, "Tokens saved successfully");
}

void KeycloakAuth::LoadTokens() {
    access_token_ = settings_->GetString("access_token", "");
    refresh_token_ = settings_->GetString("refresh_token", "");
    access_token_expires_at_ = settings_->GetInt("access_expires", 0);
    refresh_token_expires_at_ = settings_->GetInt("refresh_expires", 0);
    
    if (!access_token_.empty()) {
        ESP_LOGI(TAG, "Tokens loaded from storage");
    }
}

void KeycloakAuth::ClearTokens() {
    access_token_.clear();
    refresh_token_.clear();
    access_token_expires_at_ = 0;
    refresh_token_expires_at_ = 0;
    
    settings_->EraseKey("access_token");
    settings_->EraseKey("refresh_token");
    settings_->EraseKey("access_expires");
    settings_->EraseKey("refresh_expires");
    
    ESP_LOGI(TAG, "Tokens cleared");
}
