#include "device_info.h"
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <stdio.h>

#define TAG "DeviceInfo"

std::string DeviceInfo::GetMacAddress() {
    uint8_t mac[6];
    esp_err_t ret = esp_efuse_mac_get_default(mac);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get MAC address");
        return "00:00:00:00:00:00";
    }

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return std::string(mac_str);
}

std::string DeviceInfo::GetChipModel() {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    switch (chip_info.model) {
        case CHIP_ESP32:
            return "ESP32";
#ifdef CHIP_ESP32S2
        case CHIP_ESP32S2:
            return "ESP32-S2";
#endif
#ifdef CHIP_ESP32S3
        case CHIP_ESP32S3:
            return "ESP32-S3";
#endif
#ifdef CHIP_ESP32C3
        case CHIP_ESP32C3:
            return "ESP32-C3";
#endif
#ifdef CHIP_ESP32C2
        case CHIP_ESP32C2:
            return "ESP32-C2";
#endif
#ifdef CHIP_ESP32C6
        case CHIP_ESP32C6:
            return "ESP32-C6";
#endif
#ifdef CHIP_ESP32H2
        case CHIP_ESP32H2:
            return "ESP32-H2";
#endif
        default:
            return "Unknown";
    }
}

std::string DeviceInfo::GetFirmwareVersion() {
    // Use IDF version as firmware version
    return std::string(IDF_VER);
}

size_t DeviceInfo::GetFreeHeap() {
    return esp_get_free_heap_size();
}

size_t DeviceInfo::GetMinFreeHeap() {
    return esp_get_minimum_free_heap_size();
}

std::string DeviceInfo::BuildMetadataJson() {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // Create JSON object
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return "{}";
    }

    // Add chip information
    cJSON_AddStringToObject(root, "chipModel", GetChipModel().c_str());
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "revision", chip_info.revision);

    // Add firmware version
    cJSON_AddStringToObject(root, "firmwareVersion", GetFirmwareVersion().c_str());
    cJSON_AddStringToObject(root, "sdkVersion", IDF_VER);

    // Add memory information
    cJSON_AddNumberToObject(root, "freeHeap", (double)GetFreeHeap());
    cJSON_AddNumberToObject(root, "minFreeHeap", (double)GetMinFreeHeap());

    // Add features
    cJSON* features = cJSON_CreateArray();
    if (chip_info.features & CHIP_FEATURE_WIFI_BGN) {
        cJSON_AddItemToArray(features, cJSON_CreateString("WiFi"));
    }
    if (chip_info.features & CHIP_FEATURE_BT) {
        cJSON_AddItemToArray(features, cJSON_CreateString("Bluetooth"));
    }
    if (chip_info.features & CHIP_FEATURE_BLE) {
        cJSON_AddItemToArray(features, cJSON_CreateString("BLE"));
    }
#ifdef CHIP_FEATURE_IEEE802154
    if (chip_info.features & CHIP_FEATURE_IEEE802154) {
        cJSON_AddItemToArray(features, cJSON_CreateString("IEEE802154"));
    }
#endif
    cJSON_AddItemToObject(root, "features", features);

    // Add flash information
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);  // NULL = default flash chip
    cJSON_AddNumberToObject(root, "flashSize", (double)flash_size);

    // Convert to string
    char* json_str = cJSON_PrintUnformatted(root);
    std::string result;
    if (json_str) {
        result = std::string(json_str);
        cJSON_free(json_str);
    } else {
        result = "{}";
    }

    cJSON_Delete(root);
    return result;
}
