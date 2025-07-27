#ifndef USB_ESP32_CAMERA_H
#define USB_ESP32_CAMERA_H

#include <esp_camera.h>
#include <lvgl.h>
#include <thread>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "usb_stream.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "camera.h"

#define DEMO_KEY_RESOLUTION         "resolution"
#define DEMO_UVC_XFER_BUFFER_SIZE   ( 200 * 1024) /* 优化：增加缓冲区大小以支持高分辨率 */

typedef struct {
    uint16_t width;
    uint16_t height;
} camera_frame_size_t;

struct JpegData {
    uint8_t* fb_buf;
    size_t fb_buf_size;
};

typedef struct {
    camera_frame_size_t camera_frame_size;
    uvc_frame_size_t *camera_frame_list;
    size_t camera_frame_list_num;
    size_t camera_currect_frame_index;
} camera_resolution_info_t;

class USB_Esp32Camera : public Camera {
private:
    lv_img_dsc_t preview_image_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;

public:
    USB_Esp32Camera();
    ~USB_Esp32Camera();
    
    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);
};

#endif // ESP32_CAMERA_H