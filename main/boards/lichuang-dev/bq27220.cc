#include "bq27220.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "BQ27220"

Bq27220::Bq27220(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
    ESP_LOGI(TAG, "BQ27220 driver created at address 0x%02X", addr);
}

uint16_t Bq27220::ReadReg16(uint8_t reg) {
    uint8_t buffer[2];
    ReadRegs(reg, buffer, 2);
    // BQ27220 uses little endian
    return buffer[0] | (buffer[1] << 8);
}

uint16_t Bq27220::ControlCommand(uint16_t sub_cmd) {
    // Write control sub-command
    uint8_t cmd_buf[3];
    cmd_buf[0] = CMD_CONTROL;
    cmd_buf[1] = sub_cmd & 0xFF;
    cmd_buf[2] = (sub_cmd >> 8) & 0xFF;
    i2c_master_transmit(i2c_device_, cmd_buf, 3, 100);
    
    // Wait for command to complete
    vTaskDelay(pdMS_TO_TICKS(15));
    
    // Read response from MAC_DATA
    return ReadReg16(CMD_MAC_DATA);
}

bool Bq27220::Init() {
    ESP_LOGI(TAG, "Initializing BQ27220...");
    
    // Verify device ID
    uint16_t device_id = ControlCommand(CTRL_DEVICE_NUMBER);
    if (device_id != DEVICE_ID) {
        ESP_LOGE(TAG, "Invalid Device ID: 0x%04X (expected 0x%04X)", device_id, DEVICE_ID);
        return false;
    }
    ESP_LOGI(TAG, "Device ID verified: 0x%04X", device_id);
    
    // Read firmware version
    uint16_t fw_version = GetFirmwareVersion();
    ESP_LOGI(TAG, "Firmware Version: 0x%04X", fw_version);
    
    // Read hardware version
    uint16_t hw_version = GetHardwareVersion();
    ESP_LOGI(TAG, "Hardware Version: 0x%04X", hw_version);
    
    // Read initial battery info
    ESP_LOGI(TAG, "Battery SOC: %d%%, Voltage: %dmV, Current: %dmA, Temp: %d°C",
             GetBatteryLevel(), GetVoltage(), GetCurrent(), GetTemperature());
    
    return true;
}

int Bq27220::GetBatteryLevel() {
    uint16_t soc = ReadReg16(CMD_STATE_OF_CHARGE);
    // State of charge is in percentage (0-100)
    if (soc > 100) {
        soc = 100;
    }
    return soc;
}

int Bq27220::GetVoltage() {
    // Voltage in mV
    return ReadReg16(CMD_VOLTAGE);
}

int Bq27220::GetCurrent() {
    // Current in mA (signed)
    int16_t current = (int16_t)ReadReg16(CMD_CURRENT);
    return current;
}

int Bq27220::GetTemperature() {
    // Temperature in 0.1°K, convert to Celsius
    uint16_t temp_k = ReadReg16(CMD_TEMPERATURE);
    // Convert from 0.1°K to °C: (temp_k / 10) - 273.15
    int temp_c = (temp_k / 10) - 273;
    return temp_c;
}

int Bq27220::GetRemainingCapacity() {
    // Remaining capacity in mAh
    return ReadReg16(CMD_REMAINING_CAPACITY);
}

int Bq27220::GetFullCapacity() {
    // Full charge capacity in mAh
    return ReadReg16(CMD_FULL_CHARGE_CAPACITY);
}

int Bq27220::GetDesignCapacity() {
    // Design capacity in mAh
    return ReadReg16(CMD_DESIGN_CAPACITY);
}

int Bq27220::GetStateOfHealth() {
    // State of health in percentage
    uint16_t soh = ReadReg16(CMD_STATE_OF_HEALTH);
    if (soh > 100) {
        soh = 100;
    }
    return soh;
}

bool Bq27220::GetBatteryStatus(BatteryStatus* status) {
    if (!status) {
        return false;
    }
    
    uint16_t status_reg = ReadReg16(CMD_BATTERY_STATUS);
    // Copy the register value to the status structure
    *((uint16_t*)status) = status_reg;
    
    return true;
}

uint16_t Bq27220::GetFirmwareVersion() {
    return ControlCommand(CTRL_FW_VERSION);
}

uint16_t Bq27220::GetHardwareVersion() {
    return ControlCommand(CTRL_HW_VERSION);
}

int Bq27220::GetAveragePower() {
    // Average power in mW (signed)
    return (int16_t)ReadReg16(CMD_AVERAGE_POWER);
}

int Bq27220::GetTimeToEmpty() {
    // Time to empty in minutes
    return ReadReg16(CMD_TIME_TO_EMPTY);
}

int Bq27220::GetTimeToFull() {
    // Time to full in minutes
    return ReadReg16(CMD_TIME_TO_FULL);
}

int Bq27220::GetCycleCount() {
    // Number of charge/discharge cycles
    return ReadReg16(CMD_CYCLE_COUNT);
}

bool Bq27220::IsCharging() {
    int16_t current = GetCurrent();
    // Positive current means charging (with threshold to avoid noise)
    return current > 50;  // 50mA threshold
}

bool Bq27220::IsDischarging() {
    BatteryStatus status;
    if (GetBatteryStatus(&status)) {
        return status.dsg;
    }
    return false;
}

bool Bq27220::IsFullyCharged() {
    BatteryStatus status;
    if (GetBatteryStatus(&status)) {
        return status.fc;
    }
    return false;
}
