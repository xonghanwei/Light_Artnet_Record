// ArtNetUiPlugin.h — 功能测试 UI 插件 DLL 的 C 接口
// 宿主程序 (Qt 5.15 / MinGW 8.1) 先创建 QApplication, 再把指针传入即可创建窗口.
#ifndef ARTNET_UI_PLUGIN_H
#define ARTNET_UI_PLUGIN_H

#if defined(_WIN32)
#  if defined(ARTNET_UI_PLUGIN_BUILD)
#    define ARTNET_UI_PLUGIN_API __declspec(dllexport)
#  else
#    define ARTNET_UI_PLUGIN_API __declspec(dllimport)
#  endif
#else
#  define ARTNET_UI_PLUGIN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 插件版本号, 例如 "1.0.0" */
ARTNET_UI_PLUGIN_API const char* artnet_ui_plugin_version(void);

/* 插件编译时使用的 Qt 版本, 例如 "5.15.2" */
ARTNET_UI_PLUGIN_API const char* artnet_ui_plugin_qt_version(void);

/*
 * 创建测试工具主窗口.
 *   qapp   : 宿主已创建的 QApplication* (必填, 不能为空)
 *   parent : 父窗口 QWidget*, 可为空 (为空时创建独立顶层窗口)
 * 返回 QWidget* (以 void* 传递), 失败返回空.
 * 窗口关闭时自动 delete (WA_DeleteOnClose).
 */
ARTNET_UI_PLUGIN_API void* artnet_ui_plugin_create_window(void* qapp, void* parent);

/* 主动销毁由 create_window 创建的窗口 (可选, 一般由用户关闭窗口) */
ARTNET_UI_PLUGIN_API void artnet_ui_plugin_destroy_window(void* window);

/*
 * 插件内置自检: 在本机 UDP 回环发送 count 个 ArtDMX 包并校验.
 * 返回 0 = 通过; 1 = socket 失败; 2 = 丢包/内容不一致.
 * 详细结果通过 artnet_ui_plugin_last_result() 读取.
 */
ARTNET_UI_PLUGIN_API int artnet_ui_plugin_selftest(int count);

/* 最近一次自检的文字结果 (UTF-8), 无需释放 */
ARTNET_UI_PLUGIN_API const char* artnet_ui_plugin_last_result(void);

/* ============================================================
 * 外部时间码驱动的录制文件重演 (.artrec)
 *
 * 使用流程:
 *   void *p = artnet_ui_playback_create();
 *   artnet_ui_playback_set_artnet_target(p, "192.168.1.100", 6454);
 *   artnet_ui_playback_open(p, "D:/shows/demo.artrec");
 *   // 外部时间码每来一次调用一次 (单位: 微秒):
 *   artnet_ui_playback_set_timecode(p, timecodeUs, 30);
 *   ...
 *   artnet_ui_playback_destroy(p);
 *
 * 线程要求: 与创建它的 Qt 事件循环同一线程调用 (通常是 GUI 线程).
 * ============================================================ */

/* 创建/销毁回放上下文, 失败返回 NULL */
ARTNET_UI_PLUGIN_API void* artnet_ui_playback_create(void);
ARTNET_UI_PLUGIN_API void  artnet_ui_playback_destroy(void* ctx);

/* 打开 .artrec 录制文件, 返回 0 成功, -1 失败 (用 last_error 取原因) */
ARTNET_UI_PLUGIN_API int artnet_ui_playback_open(void* ctx, const char* artrecPath);

/* 配置输出目标: Art-Net 单播 (ip + port), 返回 0 成功 */
ARTNET_UI_PLUGIN_API int artnet_ui_playback_set_artnet_target(void* ctx, const char* ip, int port);

/* 配置输出目标: DMX512 串口 (250000 8N2), 返回 0 成功 */
ARTNET_UI_PLUGIN_API int artnet_ui_playback_set_dmx_port(void* ctx, const char* portName);

/*
 * 送入外部时间码: timestampUs = 当前时间码对应的微秒数, fps = 时间码帧率 (0 表示用文件帧率).
 * 内部按 frame = timestampUs * fps / 1e6 定位并发送该帧到所有已配置目标.
 * 返回 0 成功; -1 失败; 1 表示该帧已发送过 (重复时间码, 已忽略).
 */
ARTNET_UI_PLUGIN_API int artnet_ui_playback_set_timecode(void* ctx, long long timestampUs, int fps);

/* SMPTE 时间码便捷入口: hh:mm:ss:ff, fps 为帧率 (如 30) */
ARTNET_UI_PLUGIN_API int artnet_ui_playback_set_timecode_smpte(void* ctx, int hh, int mm,
                                                               int ss, int ff, int fps);

/* 复位到文件开头 (下一次时间码从头定位) */
ARTNET_UI_PLUGIN_API void artnet_ui_playback_reset(void* ctx);

/* 状态查询 */
ARTNET_UI_PLUGIN_API long long artnet_ui_playback_current_frame(void* ctx);
ARTNET_UI_PLUGIN_API int       artnet_ui_playback_total_frames(void* ctx);
ARTNET_UI_PLUGIN_API int       artnet_ui_playback_frames_sent(void* ctx);
ARTNET_UI_PLUGIN_API const char* artnet_ui_playback_last_error(void* ctx);

/*
 * 回放接口自检: 自动生成临时 .artrec (30 帧), 通过本机 UDP 回环验证
 * 时间码定位与帧发送是否正确. 返回 0 = PASS; 结果见 last_error.
 */
ARTNET_UI_PLUGIN_API int artnet_ui_playback_selftest(void);

/* 回放接口自检的文字结果 (UTF-8) */
ARTNET_UI_PLUGIN_API const char* artnet_ui_playback_selftest_result(void);

#ifdef __cplusplus
}
#endif

#endif // ARTNET_UI_PLUGIN_H
