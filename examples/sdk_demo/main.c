// ArtNetSdkDemo — Light Artnet Record 第三方 SDK 示例 (纯 C, 链接 artnet_sdk.dll)
//
// 演示完整流程:
//   1. 录制测试素材: 2 个 Universe, 25fps, 3 秒 → .artrec
//   2. 配置 Art-Net 输出目标并启动
//   3. 打开回放, 读取文件信息
//   4. 外部时间码 (01:00:00:00 起) 驱动同步重演
//   5. 微秒 / 帧号两种时间码入口
//
// 构建 (任选其一):
//   cmake -S examples/sdk_demo -B build/example_demo -G "MinGW Makefiles"
//   cmake --build build/example_demo
//   或直接把 main.c + sdk/include/ArtNetSdk.h + libartnet_sdk.dll.a 加入自己的工程.
//
// 运行: ArtNetSdkDemo.exe [输出文件路径]
#include "ArtNetSdk.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void check(int ok, const char *msg) {
    printf("[%s] %s\n", ok ? " OK " : "FAIL", msg);
    if (!ok) ++g_fail;
}

// 生成一段演示素材: 75 帧 @25fps = 3 秒
static void make_demo_recording(ArtNetSdk *sdk, const char *path) {
    const int universes[2] = {0, 1};
    const int fps = 25;
    const int frames = 75;

    if (ans_start_recording(sdk, path, fps, universes, 2) != 0) {
        printf("  ans_start_recording 失败: %s\n", ans_last_error(sdk));
        ++g_fail;
        return;
    }

    for (int i = 0; i < frames; ++i) {
        unsigned char dmx0[512], dmx1[512];
        memset(dmx0, 0, sizeof(dmx0));
        memset(dmx1, 0, sizeof(dmx1));
        dmx0[0] = (unsigned char)(i * 3);        // Universe 0 CH1 递增
        dmx0[1] = (unsigned char)(255 - i * 3);  // CH2 递减
        dmx1[0] = (unsigned char)(i * 2);        // Universe 1 CH1 递增
        dmx1[511] = 255;                         // 末通道常亮

        const unsigned char *data[2] = {dmx0, dmx1};
        const int lens[2] = {512, 512};
        const long long tsUs = (long long)i * 1000000LL / fps;

        if (ans_record_frame(sdk, tsUs, universes, data, lens, 2) != 0) {
            printf("  ans_record_frame(%d) 失败: %s\n", i, ans_last_error(sdk));
            ++g_fail;
            break;
        }
    }

    check(ans_stop_recording(sdk) == 0, "录制并收尾");
    printf("  录制帧数: %lld\n", ans_recorded_frames(sdk));
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "sdk_demo.artrec";

    printf("== Light Artnet Record 第三方 SDK 示例 ==\n");
    printf("SDK %s / Qt %s\n\n", ans_version(), ans_qt_version());

    ArtNetSdk *sdk = ans_create();
    if (!sdk) {
        printf("ans_create 失败\n");
        return 1;
    }

    printf("1) 生成测试素材: %s\n", path);
    make_demo_recording(sdk, path);

    printf("\n2) 配置输出目标\n");
    const int targetId = ans_add_artnet_target(sdk, "127.0.0.1", 6454);
    check(targetId > 0, "添加 Art-Net 目标 127.0.0.1:6454");
    printf("  目标 ID = %d (串口输出用 ans_add_dmx512_target(sdk, \"COM3\", 250000))\n", targetId);
    check(ans_start_outputs(sdk) == 0, "启动输出");

    printf("\n3) 打开回放并读取文件信息\n");
    check(ans_open_recording(sdk, path) == 0, "打开 .artrec");
    AnsPlaybackInfo info;
    memset(&info, 0, sizeof(info));
    check(ans_playback_info(sdk, &info) == 0, "读取文件信息");
    printf("  帧率 %d.%02d / 总帧 %d / Universe %d / 时长 %.2f s\n",
           info.frameRateX100 / 100, info.frameRateX100 % 100,
           info.totalFrames, info.universeCount, info.totalDurationUs / 1000000.0);

    // 外部时间码从 01:00:00:00 开始 → 文件第 0 帧
    ans_set_timecode_offset(sdk, 3600LL * 1000000LL);

    printf("\n4) 外部时间码驱动 (25fps, 01:00:00:00 起)\n");
    for (int i = 0; i < info.totalFrames + 6; ++i) {
        char smpte[16];
        const int sec = i / 25;
        snprintf(smpte, sizeof(smpte), "01:%02d:%02d:%02d", sec / 60, sec % 60, i % 25);
        const int rc = ans_set_timecode_string(sdk, smpte, 25);
        if (rc == 0 && (i == 0 || i % 25 == 24))
            printf("  TC %s → 帧 %lld\n", smpte, ans_current_frame(sdk));
    }
    printf("  发送帧 %d / 状态 %d / 累计包 %llu\n",
           ans_frames_sent(sdk), ans_playback_state(sdk),
           (unsigned long long)ans_packets_sent(sdk));

    printf("\n5) 其它时间码入口\n");
    check(ans_set_timecode(sdk, 3600LL * 1000000LL + 1000000LL, 25) == 0,
          "ans_set_timecode(01:00:01:00)");
    check(ans_set_timecode_frame(sdk, 10, 25) == 0, "ans_set_timecode_frame(10)");

    // 若希望 DLL 自己监听外部时间码 (需要宿主有 Qt 事件循环):
    //   ans_set_timecode_offset(sdk, 3600LL * 1000000LL);
    //   ans_start_timecode_listener(sdk, ANS_TC_SOURCE_ARTNET, 6454, 0);

    ans_destroy(sdk);

    printf("\n%s\n", g_fail ? "示例运行存在失败" : "示例运行完成");
    return g_fail ? 1 : 0;
}
