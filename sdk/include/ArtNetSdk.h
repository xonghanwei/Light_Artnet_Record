// ArtNetSdk.h — Light Artnet Record 第三方 SDK (C ABI)
//
// 面向第三方程序的协议无关接口:
//   * 输出: Art-Net 单播 / DMX512 串口
//   * 回放: 打开 .artrec, 由外部时间码驱动同步重演
//   * 录制: 宿主推送 DMX 帧与时间戳, 生成 .artrec
//
// 线程要求: 所有调用需与 ans_create() 所在 Qt 事件循环同一线程 (通常是 GUI 线程).
#ifndef ARTNET_SDK_H
#define ARTNET_SDK_H

#if defined(_WIN32)
#  if defined(ARTNET_SDK_BUILD)
#    define ANS_API __declspec(dllexport)
#  else
#    define ANS_API __declspec(dllimport)
#  endif
#else
#  define ANS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArtNetSdk ArtNetSdk;

/* ===== 版本与生命周期 ===== */
ANS_API const char* ans_version(void);
ANS_API const char* ans_qt_version(void);

ANS_API ArtNetSdk* ans_create(void);
ANS_API void       ans_destroy(ArtNetSdk* sdk);

/* 最近一次错误 (UTF-8), 成功时为空字符串 */
ANS_API const char* ans_last_error(ArtNetSdk* sdk);

/* ===== 输出目标 ===== */
/* 添加 Art-Net 单播目标, 返回目标 ID (>0), 失败返回 -1 */
ANS_API int ans_add_artnet_target(ArtNetSdk* sdk, const char* ip, int port);
/* 添加 DMX512 串口目标 (8N2), baudRate 传 0 使用 250000 */
ANS_API int ans_add_dmx512_target(ArtNetSdk* sdk, const char* portName, int baudRate);
/* 移除全部目标 (会先停止输出) */
ANS_API void ans_clear_targets(ArtNetSdk* sdk);

ANS_API int  ans_start_outputs(ArtNetSdk* sdk);
ANS_API void ans_stop_outputs(ArtNetSdk* sdk);

/* 直接发送一个 Universe 的 512 通道数据 (宿主人肉推送场景) */
ANS_API int ans_send_dmx(ArtNetSdk* sdk, int universe, const unsigned char* data, int length);

ANS_API unsigned long long ans_packets_sent(ArtNetSdk* sdk);

/* ===== 录制文件回放 (外部时间码同步) ===== */
/* 打开 .artrec, 0 成功 */
ANS_API int ans_open_recording(ArtNetSdk* sdk, const char* path);

/*
 * 送入外部时间码 (微秒), fps 为时间码帧率 (0=使用文件帧率, 2997 表示 29.97).
 * 返回: 0 已发送; 1 重复帧已忽略; -1 失败.
 */
ANS_API int ans_set_timecode(ArtNetSdk* sdk, long long timestampUs, int fps);
/* SMPTE 便捷入口: hh:mm:ss:ff */
ANS_API int ans_set_timecode_smpte(ArtNetSdk* sdk, int hh, int mm, int ss, int ff, int fps);

ANS_API void      ans_reset_playback(ArtNetSdk* sdk);
ANS_API long long ans_current_frame(ArtNetSdk* sdk);
ANS_API int       ans_total_frames(ArtNetSdk* sdk);
ANS_API int       ans_frames_sent(ArtNetSdk* sdk);

/* ===== 外部时间码接口 (同步重演) ===== */

/* 回放文件信息 */
typedef struct AnsPlaybackInfo {
    int       frameRateX100;    /* 2400 / 2500 / 2997 / 3000 / 6000 */
    int       totalFrames;      /* 总帧数 */
    long long totalDurationUs;  /* 总时长 (微秒) */
    int       universeCount;    /* Universe 数量 */
} AnsPlaybackInfo;

/* 内建时间码监听源 */
#define ANS_TC_SOURCE_ARTNET 1  /* Art-Net TimeCode (OpCode 0x0D00) */
#define ANS_TC_SOURCE_MTC    2  /* MIDI Time Code */

/*
 * 时间码基准偏移: 外部时间码 01:00:00:00 对应文件第 0 帧时传 3600000000.
 * 所有送入的时间码都会先减去该偏移再定位帧.
 */
ANS_API void ans_set_timecode_offset(ArtNetSdk* sdk, long long offsetUs);

/*
 * 送入 SMPTE 字符串 "HH:MM:SS:FF".
 * 分隔符用 ';' (如 "01:00:00;00") 表示 29.97/59.94 丢帧计数.
 * fps: 0=文件帧率, 2997=29.97, 5994=59.94, 其它为该帧率.
 */
ANS_API int ans_set_timecode_string(ArtNetSdk* sdk, const char* smpte, int fps);

/* 直接按帧号驱动 (MTC/LTC 解码器已给出帧计数时使用) */
ANS_API int ans_set_timecode_frame(ArtNetSdk* sdk, long long frameIndex, int fps);

/* 回放状态: 0=未打开 1=就绪 2=已收到时间码 */
ANS_API int ans_playback_state(ArtNetSdk* sdk);

/* 读取回放文件信息, 0 成功 */
ANS_API int ans_playback_info(ArtNetSdk* sdk, AnsPlaybackInfo* info);

/*
 * 内建时间码监听: DLL 自己接收外部时间码并驱动回放 (无需宿主转发).
 * 需要调用线程存在 Qt 事件循环 (宿主为 Qt 程序, 或自行调用 processEvents).
 * source: ANS_TC_SOURCE_ARTNET → param = UDP 监听端口 (0=6454)
 *         ANS_TC_SOURCE_MTC    → param = MIDI 输入设备索引 (-1=自动)
 * fps: 0=使用文件帧率. 返回 0 成功.
 */
ANS_API int  ans_start_timecode_listener(ArtNetSdk* sdk, int source, int param, int fps);
ANS_API void ans_stop_timecode_listener(ArtNetSdk* sdk);

/* ===== 录制 (宿主推送帧) ===== */
/*
 * universes: encoded universe 数组 (net/subnet/universe, 0-32767).
 * fps: 文件帧率, 0 使用 30.
 */
ANS_API int ans_start_recording(ArtNetSdk* sdk, const char* path, int fps,
                                const int* universes, int universeCount);

/*
 * 写入一帧: tsUs 时间戳, count 为 universe 数量,
 * dmxData[i]/dmxLen[i] 对应 universes[i] 的 DMX 数据 (取前 512 字节).
 */
ANS_API int ans_record_frame(ArtNetSdk* sdk, long long timestampUs,
                             const int* universes,
                             const unsigned char* const* dmxData,
                             const int* dmxLen, int count);

ANS_API int       ans_stop_recording(ArtNetSdk* sdk);
ANS_API long long ans_recorded_frames(ArtNetSdk* sdk);

/* ===== 自检 ===== */
/* 生成临时 .artrec → 录制接口写入 → 回放接口按时间码发送 → 本机 UDP 校验 */
ANS_API int         ans_selftest(void);
ANS_API const char* ans_selftest_result(void);

#ifdef __cplusplus
}
#endif

#endif // ARTNET_SDK_H
