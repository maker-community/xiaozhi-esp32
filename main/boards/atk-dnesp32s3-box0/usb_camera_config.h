#ifndef USB_CAMERA_CONFIG_H
#define USB_CAMERA_CONFIG_H

// 参考ESP-IoT-Solution的USB摄像头配置优化
// 针对ATK-DNESP32S3-BOX0的性能优化配置

/* USB摄像头分辨率配置 */
#define USB_CAMERA_MAX_WIDTH        800
#define USB_CAMERA_MAX_HEIGHT       600
#define USB_CAMERA_DEFAULT_WIDTH    640
#define USB_CAMERA_DEFAULT_HEIGHT   480

/* 内存优化配置 */
#define USB_CAMERA_BUFFER_SIZE      (200 * 1024)   // 增加缓冲区大小以支持高分辨率
#define USB_CAMERA_DECODE_BUFFER_SIZE (USB_CAMERA_MAX_WIDTH * USB_CAMERA_MAX_HEIGHT * 2)

/* 显示优化配置 */
#define DISPLAY_MAX_FPS             30
#define DISPLAY_SCALE_ALGORITHM     1    // 0: nearest neighbor, 1: bilinear

/* JPEG解码优化 */
#define JPEG_DECODE_QUALITY         85   // 解码质量（0-100）
#define JPEG_DECODE_USE_DMA         1    // 启用DMA加速

/* 性能优化选项 */
#define USB_CAMERA_ENABLE_DOUBLE_BUFFER     1    // 启用双缓冲
#define USB_CAMERA_ENABLE_FRAME_SKIP        1    // 启用跳帧以提升性能
#define USB_CAMERA_FRAME_SKIP_THRESHOLD     3    // 跳帧阈值

/* 内存分配策略 */
#define USB_CAMERA_USE_SPIRAM              1    // 优先使用SPIRAM
#define USB_CAMERA_MEMORY_ALIGNMENT        16   // 内存对齐字节数

#endif // USB_CAMERA_CONFIG_H
