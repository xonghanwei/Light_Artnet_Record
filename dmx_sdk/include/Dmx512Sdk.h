// Dmx512Sdk.h - DMX512 串口读写第三方 SDK (纯 C ABI, dmx512_sdk.dll)
//
// 功能: 封装 USB-DMX / RS-485 总线的 DMX512 发送与被动接收:
//   - 发送: 250000 8N2, 硬件 BREAK(100μs)+MAB(15μs) 时序, 驱动不支持时自动软件降级
//   - 接收: SDK 内部工作线程解析 513 字节帧 (Start Code 0x00 + 512 通道),
//           宿主无需 Qt 事件循环
//   - 回调: 帧到达 / 错误 通过 C 函数指针推送 (在 SDK 内部线程调用)
//
// 依赖: Qt5Core + Qt5SerialPort 运行库需与 dmx512_sdk.dll 同目录或 PATH 可达。
//
// 典型用法:
//   dmx_init();
//   dmx_open_port("COM3", 0);                       // 0 = 250000 标准波特率
//   dmx_set_frame_callback(onFrame, ctx);
//   dmx_start_rx("COM4", 0);                        // 独立接收口 (可与发送口相同, 需硬件支持)
//   unsigned char ch[512] = {0}; ch[0] = 255;
//   dmx_send_frame(ch, 512);
//   dmx_stop_rx(); dmx_close_port(); dmx_shutdown();
//
#ifndef DMX512_SDK_H
#define DMX512_SDK_H

#ifdef _WIN32
#  ifdef DMX512_SDK_BUILD
#    define DMX512_SDK_API __declspec(dllexport)
#  else
#    define DMX512_SDK_API __declspec(dllimport)
#  endif
#else
#  define DMX512_SDK_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DMX512_CHANNELS 512   /* 每帧通道数 (DMX512-A 标准) */

/** 帧数据回调: channels 为 512 通道数据, 在 SDK 接收线程调用, 勿做阻塞操作 */
typedef void (*Dmx512FrameCallback)(const unsigned char *channels, int count, void *user);

/** 错误回调: message 为 UTF-8 文本, 仅在回调期间有效, 需要保留请自行拷贝 */
typedef void (*Dmx512ErrorCallback)(const char *message, void *user);

/* ==================== 生命周期 ==================== */

/**
 * @brief 初始化 SDK (创建内部对象与工作线程)
 * @return 0 成功, -1 失败 (见 dmx_last_error)
 * 可重复调用: 已初始化时直接返回 0。
 */
DMX512_SDK_API int dmx_init(void);

/** 关闭收发并释放资源, 之后可再次 dmx_init */
DMX512_SDK_API void dmx_shutdown(void);

/** 版本字符串, 如 "1.0.0" (静态存储, 勿释放) */
DMX512_SDK_API const char *dmx_version_string(void);

/** 最近一次失败的描述 (UTF-8, 线程内全局, 无错误时为空字符串) */
DMX512_SDK_API const char *dmx_last_error(void);

/* ==================== 发送 ==================== */

/**
 * @brief 打开 DMX 发送串口
 * @param port  串口名, 如 "COM3" 或 "/dev/ttyUSB0"
 * @param baud  波特率, 0 = 250000 (DMX512 标准)
 * @return 0 成功, -1 失败
 * 驱动不支持 250000/8N2 时返回 -1 并置错误信息, 不会静默降速。
 */
DMX512_SDK_API int dmx_open_port(const char *port, int baud);

/** 关闭发送串口 (幂等) */
DMX512_SDK_API void dmx_close_port(void);

/** 发送串口是否已打开: 1 是, 0 否 */
DMX512_SDK_API int dmx_is_open(void);

/**
 * @brief 发送一帧 (自动加 BREAK/MAB/Start Code)
 * @param channels 通道数据, count 1..512, 不足 512 自动补 0, 超出截断
 * @return 0 成功, -1 失败 (未打开 / 写入错误)
 * 注意: 同步阻塞调用, 一帧线上时间约 22.6ms, 建议独立线程按 ~44Hz 调用。
 */
DMX512_SDK_API int dmx_send_frame(const unsigned char *channels, int count);

/**
 * @brief 发送原始字节 (含自定义 Start Code, 如 RDM 的 0xCC), 前置 BREAK/MAB 由 SDK 发送
 * @param data 字节流, len 1..513
 */
DMX512_SDK_API int dmx_send_raw(const unsigned char *data, int len);

/** 累计成功发送帧数 */
DMX512_SDK_API unsigned long long dmx_frames_sent(void);

/* ==================== 接收 ==================== */

/**
 * @brief 在指定串口启动 DMX 被动接收 (SDK 内部线程, 无需宿主事件循环)
 * @param port 串口名; baud 0 = 250000
 * @return 0 成功, -1 失败
 * 接收与发送可各用一个串口; 同一串口先 dmx_open_port 再 dmx_start_rx 时,
 * 接收使用独立句柄, 需硬件/驱动允许多次打开。
 */
DMX512_SDK_API int dmx_start_rx(const char *port, int baud);

/** 停止接收 (幂等) */
DMX512_SDK_API void dmx_stop_rx(void);

/** 接收是否在运行: 1 是, 0 否 */
DMX512_SDK_API int dmx_is_rx_running(void);

/** 累计接收帧数 */
DMX512_SDK_API unsigned long long dmx_frames_received(void);

/* ==================== 回调 ==================== */

/** 设置帧回调 (cb 传 NULL 取消); 可先于 dmx_start_rx 设置 */
DMX512_SDK_API void dmx_set_frame_callback(Dmx512FrameCallback cb, void *user);

/** 设置错误回调 (cb 传 NULL 取消); 接收线程内部错误经此推送 */
DMX512_SDK_API void dmx_set_error_callback(Dmx512ErrorCallback cb, void *user);

/* ==================== 工具 ==================== */

/**
 * @brief 枚举系统串口名, 分号分隔写入 buf, 如 "COM3;COM4;"
 * @return 实际需要的字节数 (含结尾 0); buf_size 不足时内容截断, 调用方可按返回值重试
 */
DMX512_SDK_API int dmx_list_ports(char *buf, int buf_size);

#ifdef __cplusplus
}
#endif

#endif /* DMX512_SDK_H */
