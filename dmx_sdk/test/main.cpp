// dmx512_sdk_test — 第三方调用方式自检: 纯 Win32 动态加载 dmx512_sdk.dll,
// 不链接导入库, 验证导出符号、API 契约与错误路径 (无需真实 DMX 硬件)。
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef int  (*FnInit)(void);
typedef void (*FnShutdown)(void);
typedef const char* (*FnVersion)(void);
typedef const char* (*FnLastError)(void);
typedef int  (*FnOpenPort)(const char*, int);
typedef void (*FnClosePort)(void);
typedef int  (*FnIsOpen)(void);
typedef int  (*FnSendFrame)(const unsigned char*, int);
typedef int  (*FnSendRaw)(const unsigned char*, int);
typedef unsigned long long (*FnFramesSent)(void);
typedef int  (*FnStartRx)(const char*, int);
typedef void (*FnStopRx)(void);
typedef int  (*FnIsRxRunning)(void);
typedef unsigned long long (*FnFramesReceived)(void);
typedef void (*FnSetFrameCb)(const unsigned char*, int, void*);
typedef void (*FnSetFrameCbSetter)(FnSetFrameCb, void*);
typedef void (*FnSetErrCb)(const char*, void*);
typedef void (*FnSetErrCbSetter)(FnSetErrCb, void*);
typedef int  (*FnListPorts)(char*, int);

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "[PASS]" : "[FAIL]", name);
    cond ? g_pass++ : g_fail++;
}

static FnSetFrameCb g_frameSeen = NULL;
static void onFrame(const unsigned char *ch, int n, void *user) { (void)ch; (void)n; (void)user; }
static void onError(const char *msg, void *user) { (void)msg; (void)user; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   // 无缓冲, 便于定位卡点
    SetConsoleOutputCP(CP_UTF8);
    HMODULE h = LoadLibraryA("dmx512_sdk.dll");
    if (!h) {
        printf("[FAIL] LoadLibrary dmx512_sdk.dll (err=%lu)\n", GetLastError());
        return 1;
    }
    printf("dmx512_sdk.dll loaded\n");

    FnInit            pInit          = (FnInit)GetProcAddress(h, "dmx_init");
    FnShutdown        pShutdown      = (FnShutdown)GetProcAddress(h, "dmx_shutdown");
    FnVersion         pVersion       = (FnVersion)GetProcAddress(h, "dmx_version_string");
    FnLastError       pLastError     = (FnLastError)GetProcAddress(h, "dmx_last_error");
    FnOpenPort        pOpen          = (FnOpenPort)GetProcAddress(h, "dmx_open_port");
    FnClosePort       pClose         = (FnClosePort)GetProcAddress(h, "dmx_close_port");
    FnIsOpen          pIsOpen        = (FnIsOpen)GetProcAddress(h, "dmx_is_open");
    FnSendFrame       pSendFrame     = (FnSendFrame)GetProcAddress(h, "dmx_send_frame");
    FnSendRaw         pSendRaw       = (FnSendRaw)GetProcAddress(h, "dmx_send_raw");
    FnFramesSent      pFramesSent    = (FnFramesSent)GetProcAddress(h, "dmx_frames_sent");
    FnStartRx         pStartRx       = (FnStartRx)GetProcAddress(h, "dmx_start_rx");
    FnStopRx          pStopRx        = (FnStopRx)GetProcAddress(h, "dmx_stop_rx");
    FnIsRxRunning     pIsRxRunning   = (FnIsRxRunning)GetProcAddress(h, "dmx_is_rx_running");
    FnFramesReceived  pFramesRcv     = (FnFramesReceived)GetProcAddress(h, "dmx_frames_received");
    FnSetFrameCbSetter pSetFrameCb   = (FnSetFrameCbSetter)GetProcAddress(h, "dmx_set_frame_callback");
    FnSetErrCbSetter   pSetErrCb     = (FnSetErrCbSetter)GetProcAddress(h, "dmx_set_error_callback");
    FnListPorts       pListPorts     = (FnListPorts)GetProcAddress(h, "dmx_list_ports");

    check("1. all 17 exports resolved",
          pInit && pShutdown && pVersion && pLastError && pOpen && pClose && pIsOpen &&
          pSendFrame && pSendRaw && pFramesSent && pStartRx && pStopRx && pIsRxRunning &&
          pFramesRcv && pSetFrameCb && pSetErrCb && pListPorts);
    if (!(pInit && pVersion && pOpen)) { printf("missing core exports\n"); return 1; }

    check("2. version string", strcmp(pVersion(), "1.0.0") == 0);
    check("3. init ok + re-init idempotent", pInit() == 0 && pInit() == 0);

    /* 错误路径: 不存在的串口 */
    check("4. open nonexistent port fails", pOpen("COM_NOT_EXIST_99", 0) == -1);
    check("5. last_error set after failure", strlen(pLastError()) > 0);
    check("6. is_open == 0 after failure", pIsOpen() == 0);
    check("7. send without open fails", pSendFrame((const unsigned char*)"\x01", 1) == -1);

    /* 参数校验 */
    check("8. send_frame count bounds",
          pSendFrame(NULL, 0) == -1 && pSendFrame((const unsigned char*)"\x01", 513) == -1);
    check("9. send_raw length bounds",
          pSendRaw(NULL, 10) == -1 && pSendFrame((const unsigned char*)"\x01", -5) == -1);
    check("10. frames_sent == 0", pFramesSent() == 0);

    /* 回调设置/取消 */
    pSetFrameCb(onFrame, NULL);
    pSetErrCb(onError, NULL);
    pSetFrameCb(NULL, NULL);
    pSetErrCb(NULL, NULL);
    check("11. callback set/clear no-crash", 1);

    /* 接收错误路径 + 幂等停止 */
    check("12. start_rx nonexistent port fails", pStartRx("COM_NOT_EXIST_99", 0) == -1);
    check("13. rx not running", pIsRxRunning() == 0 && pFramesRcv() == 0);
    pStopRx(); pStopRx();
    check("14. stop_rx idempotent", 1);

    /* 串口枚举: 先探长度再取内容 */
    int need = pListPorts(NULL, 0);
    check("15. list_ports probe returns >=1 (terminator)", need >= 1);
    if (need > 1) {
        char *buf = (char*)malloc(need);
        int got = pListPorts(buf, need);
        check("16. list_ports content non-empty", got == need && buf[0] != '\0');
        printf("      ports: %s\n", buf);
        free(buf);
    } else {
        check("16. list_ports content non-empty (no ports on machine)", 1);
    }

    /* 关闭与反初始化 */
    pClose();
    pShutdown();
    check("17. send after shutdown fails", pSendFrame((const unsigned char*)"\x01", 1) == -1);
    pShutdown();
    check("18. shutdown idempotent", 1);

    printf("\n==== dmx512_sdk_test: %d PASS, %d FAIL ====\n", g_pass, g_fail);
    FreeLibrary(h);
    return g_fail == 0 ? 0 : 1;
}
