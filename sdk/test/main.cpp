// sdk/test/main.cpp — 第三方接入演示 + 自动化测试
// 纯 Win32 动态加载 artnet_sdk.dll, 不依赖 Qt / 不链接 SDK 导入库.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

typedef const char* (*FnVersion)(void);
typedef void*       (*FnCreate)(void);
typedef void        (*FnDestroy)(void*);
typedef int         (*FnSelftest)(void);
typedef const char* (*FnSelfResult)(void);
typedef const char* (*FnLastError)(void*);
typedef unsigned long long (*FnPackets)(void*);

static int g_pass = 0, g_fail = 0;

static void check(const char *name, bool ok, const std::string &detail = std::string()) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
                detail.empty() ? "" : " — ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

int main() {
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.find_last_of("\\/"));
    const std::string dllPath = dir + "\\artnet_sdk.dll";

    std::printf("SDK: %s\n", dllPath.c_str());
    HMODULE h = LoadLibraryA(dllPath.c_str());
    if (!h) {
        std::printf("[FAIL] LoadLibrary failed, error=%lu\n", GetLastError());
        return 1;
    }
    check("加载 artnet_sdk.dll", true);

    auto ver      = (FnVersion)GetProcAddress(h, "ans_version");
    auto qtVer    = (FnVersion)GetProcAddress(h, "ans_qt_version");
    auto create   = (FnCreate)GetProcAddress(h, "ans_create");
    auto destroy  = (FnDestroy)GetProcAddress(h, "ans_destroy");
    auto selftest = (FnSelftest)GetProcAddress(h, "ans_selftest");
    auto selfRes  = (FnSelfResult)GetProcAddress(h, "ans_selftest_result");
    auto lastErr  = (FnLastError)GetProcAddress(h, "ans_last_error");
    auto packets  = (FnPackets)GetProcAddress(h, "ans_packets_sent");

    check("解析核心导出函数", ver && qtVer && create && destroy && selftest && selfRes);
    if (g_fail) { FreeLibrary(h); return 1; }

    const char *tcExports[] = {
        "ans_set_timecode_offset", "ans_set_timecode_string", "ans_set_timecode_frame",
        "ans_playback_state", "ans_playback_info",
        "ans_start_timecode_listener", "ans_stop_timecode_listener"
    };
    int missing = 0;
    for (const char *name : tcExports)
        if (!GetProcAddress(h, name)) ++missing;
    check("解析外部时间码接口", missing == 0,
          missing ? std::to_string(missing) + " 个缺失" : "7/7");

    check("版本号", ver && ver()[0] != 0, ver ? ver() : "");
    check("Qt 版本", qtVer && qtVer()[0] != 0, qtVer ? qtVer() : "");

    void *sdk = create();
    check("ans_create", sdk != nullptr);
    check("ans_last_error 可调用", lastErr && lastErr(sdk)[0] == 0,
          lastErr ? lastErr(sdk) : "");
    check("ans_packets_sent 可调用", packets && packets(sdk) == 0);

    const int rc = selftest();
    check("录制 + 时间码回放自检", rc == 0,
          std::string(selfRes ? selfRes() : "") + (rc ? " (rc=" + std::to_string(rc) + ")" : ""));

    destroy(sdk);
    check("ans_destroy", true);

    FreeLibrary(h);
    std::printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
