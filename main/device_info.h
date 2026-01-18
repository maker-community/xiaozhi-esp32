#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <string>
#include <cJSON.h>

/**
 * Utility class for collecting ESP32 device information
 * Used for SignalR device registration metadata
 */
class DeviceInfo {
public:
    /**
     * Get device MAC address as string (format: AA:BB:CC:DD:EE:FF)
     */
    static std::string GetMacAddress();

    /**
     * Get chip model name (e.g., "ESP32", "ESP32-S3", "ESP32-C3")
     */
    static std::string GetChipModel();

    /**
     * Get firmware version (from IDF version)
     */
    static std::string GetFirmwareVersion();

    /**
     * Get free heap size in bytes
     */
    static size_t GetFreeHeap();

    /**
     * Get minimum free heap size ever recorded
     */
    static size_t GetMinFreeHeap();

    /**
     * Build JSON metadata string for device registration
     * Includes: chipModel, firmwareVersion, freeHeap, minFreeHeap, sdkVersion
     * @return JSON string ready for SignalR registration
     */
    static std::string BuildMetadataJson();
};

#endif // DEVICE_INFO_H
