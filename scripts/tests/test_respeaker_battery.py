"""Exercise the actual reSpeaker battery methods with a host-side I2C double."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main" / "boards" / "seeed" / "xiao-esp32s3-respeaker-xvf3800"
CJSON = ROOT / "managed_components" / "espressif__cjson" / "cJSON"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class ReSpeakerBatteryTest(unittest.TestCase):
    def test_optional_monitor_protocol_and_failure_paths(self):
        board = (BOARD / "xiao_esp32s3_respeaker_xvf3800.cc").read_text(encoding="utf-8")
        constants = "\n".join(
            line for line in board.splitlines()
            if re.match(r"constexpr .+ k(?:Tag|Bq27220\w*|Battery\w*|I2c\w*)[=\s\[]", line)
        )
        driver = r"""
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "cJSON.h"
using esp_err_t = int;
using gpio_num_t = int;
using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;
constexpr int GPIO_NUM_5=5, GPIO_NUM_6=6;
constexpr int ESP_OK=0, ESP_ERR_TIMEOUT=1, ESP_ERR_NOT_FOUND=2;
constexpr int ESP_ERR_NOT_SUPPORTED=3, ESP_ERR_INVALID_STATE=4, ESP_FAIL=5;
constexpr int I2C_ADDR_BIT_LEN_7=0;
struct i2c_device_config_t {
    int dev_addr_length; unsigned device_address; int scl_speed_hz; int scl_wait_us;
    struct { int disable_ack_check; } flags;
};
std::string logs;
bool invalid_base_status=false;
void Log(const char*, const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logs += buffer;
    logs += '\n';
}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(expr) assert((expr) == ESP_OK)
// Match the actual 100 Hz FreeRTOS configuration, rather than treating ticks as ms.
#define pdMS_TO_TICKS(ms) ((ms) / 10)
uint16_t registers[256];
int probe_error, add_error, transmit_error, read_error_reg;
int probes, additions, removals, reads, transmissions, delay_ticks;
void Reset() {
    for (auto& value : registers) value=0;
    registers[0x08]=3700; registers[0x0c]=static_cast<uint16_t>(-50);
    registers[0x2c]=60; registers[0x3c]=1200; registers[0x40]=0x0220;
    probe_error=add_error=transmit_error=0; read_error_reg=-1;
    probes=additions=removals=reads=transmissions=delay_ticks=0;
    logs.clear();
}
int64_t esp_timer_get_time() { return 1000000; }
const char* esp_err_to_name(int err) {
    return err == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "error";
}
void vTaskDelay(int ticks) { delay_ticks += ticks; }
esp_err_t i2c_master_probe(void*, unsigned address, int timeout_ms) {
    ++probes; assert(address==0x55); assert(timeout_ms==100); return probe_error;
}
esp_err_t i2c_master_bus_add_device(void*, const i2c_device_config_t* cfg, void** device) {
    ++additions;
    assert(cfg->device_address==0x55 && cfg->scl_speed_hz==100000);
    assert(cfg->flags.disable_ack_check==0);
    if (!add_error) *device=&registers;
    return add_error;
}
esp_err_t i2c_master_bus_rm_device(void*) { ++removals; return 0; }
esp_err_t i2c_master_transmit(void*, const uint8_t* data, size_t len, int timeout) {
    ++transmissions;
    assert(len==3 && timeout==100);
    // DEVICE_NUMBER is command 0x0001, NOT the expected reply 0x0220.
    assert(data[0]==0x00 && data[1]==0x01 && data[2]==0x00);
    return transmit_error;
}
esp_err_t i2c_master_transmit_receive(void*, const uint8_t* reg, size_t tx_len,
                                     uint8_t* data, size_t rx_len, int timeout) {
    ++reads; assert(tx_len==1 && rx_len==2 && timeout==100);
    if (*reg==read_error_reg) return ESP_ERR_TIMEOUT;
    data[0]=registers[*reg]&255; data[1]=registers[*reg]>>8; return 0;
}
class Base {
public:
    virtual bool GetBatteryLevel(int&, bool&, bool&)=0;
    virtual ~Base()=default;
    virtual std::string GetDeviceStatusJson() {
        if (invalid_base_status) return "{invalid";
        int level=0; bool charging=false,discharging=false;
        cJSON* root=cJSON_CreateObject();
        cJSON_AddStringToObject(root,"network","unchanged");
        if (GetBatteryLevel(level,charging,discharging)) {
            cJSON* battery=cJSON_AddObjectToObject(root,"battery");
            cJSON_AddNumberToObject(battery,"level",level);
            cJSON_AddBoolToObject(battery,"charging",charging);
        }
        char* json=cJSON_PrintUnformatted(root);
        std::string result(json); cJSON_free(json); cJSON_Delete(root);
        return result;
    }
};
using WifiBoard=Base;
"""
        driver += constants
        driver += r"""
class Monitor : public Base {
public:
    void* i2c_bus_=&registers;
    void* battery_i2c_device_=nullptr;
    std::mutex battery_mutex_;
    int64_t last_battery_error_us_=-kBatteryLogIntervalUs;
    uint16_t battery_gauge_design_capacity_mah_=0;
    uint16_t battery_voltage_mv_=0;
    int battery_current_ma_=0;
"""
        for signature in (
            "esp_err_t ReadBatteryWord(",
            "esp_err_t VerifyBq27220Device(",
            "void InitializeBatteryMonitor(",
            "bool GetBatteryLevel(",
            "std::string GetDeviceStatusJson(",
        ):
            driver += function(board, signature) + "\n"
        driver += r"""
};
int main() {
    Reset(); Monitor absent; probe_error=ESP_ERR_NOT_FOUND;
    absent.InitializeBatteryMonitor();
    assert(!absent.battery_i2c_device_ && probes==1 && !additions && !reads && !transmissions);
    assert(logs.find("did not ACK")!=std::string::npos);
    int level=77; bool charging=true, discharging=true;
    assert(!absent.GetBatteryLevel(level,charging,discharging));
    assert(level==77 && charging && discharging);
    auto absent_json=absent.GetDeviceStatusJson();
    assert(absent_json.find("battery")==std::string::npos);
    assert(absent_json.find("unchanged")!=std::string::npos);

    Reset(); Monitor timeout; probe_error=ESP_ERR_TIMEOUT;
    timeout.InitializeBatteryMonitor();
    assert(!timeout.battery_i2c_device_ && !additions);
    assert(logs.find("ESP_ERR_TIMEOUT")!=std::string::npos);
    assert(logs.find("did not ACK")==std::string::npos);

    Reset(); Monitor no_bus; no_bus.i2c_bus_=nullptr;
    no_bus.InitializeBatteryMonitor(); assert(!probes && !additions);

    Reset(); Monitor allocation; add_error=ESP_FAIL;
    allocation.InitializeBatteryMonitor(); assert(!allocation.battery_i2c_device_ && !reads);

    Reset(); Monitor valid;
    valid.InitializeBatteryMonitor();
    assert(valid.battery_i2c_device_ && transmissions==1 && reads==4 && !removals);
    assert(delay_ticks>0 && logs.find("device=0x0220")!=std::string::npos);
    assert(valid.GetBatteryLevel(level,charging,discharging));
    assert(level==60 && !charging && discharging);
    registers[0x0c]=75; registers[0x2c]=100;
    assert(valid.GetBatteryLevel(level,charging,discharging));
    assert(level==100 && charging && !discharging);
    registers[0x0c]=0; registers[0x2c]=0;
    assert(valid.GetBatteryLevel(level,charging,discharging));
    assert(level==0 && !charging && !discharging);

    for (int reg : {0x08,0x0c,0x2c}) {
        read_error_reg=reg; level=77; charging=discharging=true;
        assert(!valid.GetBatteryLevel(level,charging,discharging));
        assert(level==77 && charging && discharging);
    }
    read_error_reg=-1; registers[0x2c]=65535;
    assert(!valid.GetBatteryLevel(level,charging,discharging));
    registers[0x2c]=50; registers[0x08]=0;
    assert(!valid.GetBatteryLevel(level,charging,discharging));
    registers[0x08]=4200;
    assert(valid.GetBatteryLevel(level,charging,discharging));
    cJSON* valid_json=cJSON_Parse(valid.GetDeviceStatusJson().c_str());
    auto* valid_battery=cJSON_GetObjectItemCaseSensitive(valid_json,"battery");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(valid_battery,"capacity_matches")));
    assert(std::string(cJSON_GetObjectItemCaseSensitive(valid_battery,"soc_accuracy")->valuestring)
           =="unverified");
    cJSON_Delete(valid_json);

    Reset(); Monitor mismatch; registers[0x3c]=3000;
    mismatch.InitializeBatteryMonitor();
    assert(mismatch.battery_i2c_device_ && removals==0 && transmissions==1);
    assert(logs.find("design=3000 mAh, expected=1200 mAh")!=std::string::npos);
    assert(logs.find("reference SOC")!=std::string::npos);
    assert(mismatch.GetBatteryLevel(level,charging,discharging));
    assert(level==60 && !charging && discharging);
    cJSON* mismatch_json=cJSON_Parse(mismatch.GetDeviceStatusJson().c_str());
    auto* battery=cJSON_GetObjectItemCaseSensitive(mismatch_json,"battery");
    assert(cJSON_GetObjectItemCaseSensitive(battery,"level")->valueint==60);
    assert(cJSON_GetObjectItemCaseSensitive(battery,"voltage_mv")->valueint==3700);
    assert(cJSON_GetObjectItemCaseSensitive(battery,"current_ma")->valueint==-50);
    assert(cJSON_GetObjectItemCaseSensitive(battery,"gauge_design_capacity_mah")->valueint==3000);
    assert(cJSON_GetObjectItemCaseSensitive(battery,"expected_capacity_mah")->valueint==1200);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(battery,"capacity_matches")));
    assert(std::string(cJSON_GetObjectItemCaseSensitive(battery,"soc_accuracy")->valuestring)
           =="unverified");
    assert(std::string(cJSON_GetObjectItemCaseSensitive(battery,"soc_note")->valuestring)
           .find("not a calibrated percentage")!=std::string::npos);
    assert(std::string(cJSON_GetObjectItemCaseSensitive(mismatch_json,"network")->valuestring)
           =="unchanged");
    cJSON_Delete(mismatch_json);
    read_error_reg=0x08;
    assert(mismatch.GetDeviceStatusJson().find("battery")==std::string::npos);
    invalid_base_status=true;
    auto error_json=mismatch.GetDeviceStatusJson();
    assert(error_json.find("\"error\"")!=std::string::npos);
    assert(logs.find("Could not parse device status")!=std::string::npos);
    invalid_base_status=false;

    Reset(); Monitor wrong_chip; registers[0x40]=0x1234;
    wrong_chip.InitializeBatteryMonitor();
    assert(!wrong_chip.battery_i2c_device_ && removals==1);
    assert(logs.find("received 0x1234")!=std::string::npos);

    for (int reg : {0x08,0x2c,0x3c,0x40}) {
        Reset(); Monitor failed; read_error_reg=reg;
        failed.InitializeBatteryMonitor();
        assert(!failed.battery_i2c_device_ && removals==1);
    }
    Reset(); Monitor write_failed; transmit_error=ESP_FAIL;
    write_failed.InitializeBatteryMonitor();
    assert(!write_failed.battery_i2c_device_ && removals==1);
    std::cout << "Battery protocol, optional absence, timeout diagnostics, identity/capacity "
                 "checks, signed current, read failures and recovery passed\n";
}
"""
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            source = build_dir / "battery_test.cc"
            source.write_text(driver, encoding="utf-8")
            executable = build_dir / ("battery_test.exe" if os.name == "nt" else "battery_test")
            compiler = shlex.split(os.environ.get("CXX", "c++"))
            cjson_object = build_dir / ("cjson.obj" if os.name == "nt" else "cjson.o")
            if Path(compiler[0]).stem.lower() == "cl":
                cjson_flags = ["/nologo", "/c", str(CJSON / "cJSON.c"), f"/Fo:{cjson_object}"]
                flags = ["/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
                         f"/I{CJSON}", str(source), str(cjson_object), f"/Fe:{executable}"]
            else:
                cjson_flags = ["-x", "c", "-c", str(CJSON / "cJSON.c"),
                               "-o", str(cjson_object)]
                flags = ["-std=c++20", "-fno-exceptions", "-Wall", "-Wextra", "-Werror",
                         f"-I{CJSON}", str(source), str(cjson_object), "-o", str(executable)]
            subprocess.run(compiler + cjson_flags, check=True, cwd=build_dir)
            subprocess.run(compiler + flags, check=True, cwd=build_dir)
            subprocess.run([executable], check=True, cwd=build_dir)


if __name__ == "__main__":
    unittest.main()
