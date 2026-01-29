#include "device_state_machine.h"

#include <algorithm>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

static const char* TAG = "StateMachine";

// State name strings for logging
static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "wifi_configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

DeviceStateMachine::DeviceStateMachine() {
}

const char* DeviceStateMachine::GetStateName(DeviceState state) {
    if (state >= 0 && state <= kDeviceStateFatalError) {
        return STATE_STRINGS[state];
    }
    return STATE_STRINGS[kDeviceStateFatalError + 1];
}

bool DeviceStateMachine::IsValidTransition(DeviceState from, DeviceState to) const {
    // Allow transition to the same state (no-op)
    if (from == to) {
        return true;
    }

    // Define valid state transitions based on the state diagram
    switch (from) {
        case kDeviceStateUnknown:
            // Can only go to starting
            return to == kDeviceStateStarting;

        case kDeviceStateStarting:
            // Can go to wifi configuring or activating
            return to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateActivating;

        case kDeviceStateWifiConfiguring:
            // Can go to activating (after wifi connected) or audio testing
            return to == kDeviceStateActivating ||
                   to == kDeviceStateAudioTesting;

        case kDeviceStateAudioTesting:
            // Can go back to wifi configuring
            return to == kDeviceStateWifiConfiguring;

        case kDeviceStateActivating:
            // Can go to upgrading, idle, or back to wifi configuring (on error)
            return to == kDeviceStateUpgrading ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateWifiConfiguring;

        case kDeviceStateUpgrading:
            // Can go to idle (upgrade failed) or activating
            return to == kDeviceStateIdle ||
                   to == kDeviceStateActivating;

        case kDeviceStateIdle:
            // Can go to connecting, listening (manual mode), speaking, activating, upgrading, or wifi configuring
            return to == kDeviceStateConnecting ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateActivating ||
                   to == kDeviceStateUpgrading ||
                   to == kDeviceStateWifiConfiguring;

        case kDeviceStateConnecting:
            // Can go to idle (failed) or listening (success)
            return to == kDeviceStateIdle ||
                   to == kDeviceStateListening;

        case kDeviceStateListening:
            // Can go to speaking or idle
            return to == kDeviceStateSpeaking ||
                   to == kDeviceStateIdle;

        case kDeviceStateSpeaking:
            // Can go to listening or idle
            return to == kDeviceStateListening ||
                   to == kDeviceStateIdle;

        case kDeviceStateFatalError:
            // Cannot transition out of fatal error
            return false;

        default:
            return false;
    }
}

bool DeviceStateMachine::CanTransitionTo(DeviceState target) const {
    return IsValidTransition(current_state_.load(), target);
}

bool DeviceStateMachine::TransitionTo(DeviceState new_state) {
    DeviceState old_state = current_state_.load();
    
    // No-op if already in the target state
    if (old_state == new_state) {
        return true;
    }

    // Validate transition
    if (!IsValidTransition(old_state, new_state)) {
        ESP_LOGW(TAG, "Invalid state transition: %s -> %s",
                 GetStateName(old_state), GetStateName(new_state));
        return false;
    }

    // Perform transition
    current_state_.store(new_state);
    ESP_LOGI(TAG, "State: %s -> %s",
             GetStateName(old_state), GetStateName(new_state));

    /* Diagnostic: when entering listening, print heap and task list for runtime investigation */
    if (new_state == kDeviceStateListening) {
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t free_spiram = 0;
        size_t min_spiram = 0;
    #if CONFIG_SPIRAM_SUPPORT
        free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        min_spiram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    #endif

        ESP_LOGI(TAG, "Heap total free: %u, min total free: %u", (unsigned int)free_heap, (unsigned int)min_free_heap);
        ESP_LOGI(TAG, "Heap internal free: %u, min internal free: %u", (unsigned int)free_internal, (unsigned int)min_internal);
    #if CONFIG_SPIRAM_SUPPORT
        ESP_LOGI(TAG, "Heap PSRAM free: %u, min PSRAM free: %u", (unsigned int)free_spiram, (unsigned int)min_spiram);
    #endif

        UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
        ESP_LOGI(TAG, "Task count: %u", (unsigned int)num_tasks);

    /* Detailed per-task stack high-water logging when trace facility is available */
    #if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
        TaskStatus_t* status_array = (TaskStatus_t*)heap_caps_malloc(num_tasks * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
        if (status_array != nullptr) {
            UBaseType_t returned = uxTaskGetSystemState(status_array, num_tasks, NULL);
            for (UBaseType_t i = 0; i < returned; ++i) {
                ESP_LOGI(TAG, "Task %s state=%u stackHighWater=%u", status_array[i].pcTaskName,
                         (unsigned int)status_array[i].eCurrentState,
                         (unsigned int)status_array[i].usStackHighWaterMark);
            }
            heap_caps_free(status_array);
        } else {
            ESP_LOGW(TAG, "Failed to allocate PSRAM TaskStatus_t array for diagnostics (PSRAM not available?)");
        }
    #else
        ESP_LOGI(TAG, "Task stats not available; CONFIG_FREERTOS_USE_TRACE_FACILITY disabled");
    #endif
    }

    // Notify callback
    NotifyStateChange(old_state, new_state);
    return true;
}

int DeviceStateMachine::AddStateChangeListener(StateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    int id = next_listener_id_++;
    listeners_.emplace_back(id, std::move(callback));
    return id;
}

void DeviceStateMachine::RemoveStateChangeListener(int listener_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
            [listener_id](const auto& p) { return p.first == listener_id; }),
        listeners_.end());
}

void DeviceStateMachine::NotifyStateChange(DeviceState old_state, DeviceState new_state) {
    std::vector<StateCallback> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_copy.reserve(listeners_.size());
        for (const auto& [id, cb] : listeners_) {
            callbacks_copy.push_back(cb);
        }
    }
    
    for (const auto& cb : callbacks_copy) {
        cb(old_state, new_state);
    }
}
