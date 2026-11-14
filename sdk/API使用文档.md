# ArtNetSdk.dll API 使用文档

**版本**：1.0.0  
**文件**：`artnet_sdk.dll`（Windows x64，C ABI，32 个导出函数）  
**依赖**：Qt 5.15.2（Core / Network / SerialPort / Xml）、MinGW 运行库、FFmpeg 运行库  
**头文件**：`sdk/include/ArtNetSdk.h`

---

## 目录

1. [概述](#1-概述)
2. [快速开始](#2-快速开始)
3. [集成方式](#3-集成方式)
4. [生命周期与线程模型](#4-生命周期与线程模型)
5. [数据结构与常量](#5-数据结构与常量)
6. [API 参考](#6-api-参考)
   - 6.1 [版本与生命周期](#61-版本与生命周期)
   - 6.2 [输出目标](#62-输出目标)
   - 6.3 [直接发送 DMX](#63-直接发送-dmx)
   - 6.4 [录制 .artrec](#64-录制-artrec)
   - 6.5 [打开与回放](#65-打开与回放)
   - 6.6 [外部时间码（宿主推送）](#66-外部时间码宿主推送)
   - 6.7 [外部时间码（DLL 监听）](#67-外部时间码dll-监听)
   - 6.8 [自检](#68-自检)
7. [时间码换算规则](#7-时间码换算规则)
8. [典型工作流](#8-典型工作流)
9. [性能与实时性](#9-性能与实时性)
10. [.artrec 文件格式](#10-artrec-文件格式)
11. [错误排查](#11-错误排查)
12. [版本与兼容性](#12-版本与兼容性)

---

## 1. 概述

`artnet_sdk.dll` 把 Light Artnet Record 的协议能力封装为无界面 C 接口，供第三方程序集成：

| 能力 | 说明 |
|---|---|
| 协议输出 | Art-Net 单播（UDP 6454，512 通道/域）、DMX512 串口（250000 bps，8N2） |
| 录制 | 宿主逐帧推送 DMX 数据与时间戳 → 生成 `.artrec` 录制文件 |
| 回放 | 打开 `.artrec`，按外部时间码定位并输出对应帧，实现同步重演 |
| 时间码 | 宿主推送（微秒 / SMPTE / 帧号）或 DLL 内建监听（Art-Net TimeCode、MIDI MTC） |
| 自检 | 内置录制→回放→UDP 校验闭环，用于部署验证 |

设计要点：

- **纯 C ABI**：函数名 `ans_*`，不要求调用方使用 C++ 或 Qt 头文件。
- **单实例句柄**：所有状态保存在 `ArtNetSdk*` 中，可创建多个实例（各自独立输出/回放）。
- **无界面**：不依赖 Qt Widgets。
- **不接管主循环**：除监听模式外，不要求宿主事件循环。

---

## 2. 快速开始

### 2.1 最小可运行代码

```c
#include "ArtNetSdk.h"
#include <stdio.h>

int main(void) {
    ArtNetSdk *sdk = ans_create();

    ans_add_artnet_target(sdk, "192.168.1.100", 6454);
    if (ans_start_outputs(sdk) != 0) {
        printf("输出启动失败: %s\n", ans_last_error(sdk));
    }

    if (ans_open_recording(sdk, "show.artrec") == 0) {
        ans_set_timecode_offset(sdk, 3600LL * 1000000LL);  // 01:00:00:00 = 第 0 帧

        // 每收到一个外部时间码调用一次
        ans_set_timecode_string(sdk, "01:00:05:12", 25);
    }

    ans_destroy(sdk);
    return 0;
}
```

### 2.2 构建 SDK 与运行自检

在本项目根目录：

```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --target artnet_sdk artnet_sdk_test -j 8
build\bin\artnet_sdk_test.exe
```

预期最后两行：

```
[PASS] 录制 + 时间码回放自检 — ...
结果: 10 通过, 0 失败
```

---

## 3. 集成方式

### 3.1 需要分发的文件

| 文件 | 时机 |
|---|---|
| `ArtNetSdk.h` | 编译期 |
| `libartnet_sdk.dll.a`（MinGW） | 链接期 |
| `artnet_sdk.dll` | 运行期 |
| `Qt5Core.dll` `Qt5Network.dll` `Qt5SerialPort.dll` `Qt5Xml.dll` | 运行期 |
| `libgcc_s_seh-1.dll` `libstdc++-6.dll` `libwinpthread-1.dll` | 运行期（MinGW） |
| `avcodec-63.dll` `avformat-63.dll` `avutil-61.dll` `swresample-7.dll` `swscale-10.dll` | 运行期（FFmpeg） |

运行库必须与宿主 exe 同目录，或位于 `PATH`。可用 `windeployqt artnet_sdk.dll` 自动收集 Qt/MinGW 运行库。

### 3.2 MinGW（推荐）

```cmake
add_executable(MyApp main.c)
target_include_directories(MyApp PRIVATE ${SDK_ROOT}/include)
target_link_libraries(MyApp PRIVATE ${SDK_ROOT}/lib/libartnet_sdk.dll.a)
```

### 3.3 MSVC

`libartnet_sdk.dll.a` 不能直接被 MSVC 使用，二选一：

1. 用 MSVC 重新构建 SDK（`cmake -G "Visual Studio 17 2022"`），得到 `artnet_sdk.lib`；
2. 运行时动态加载，不需要导入库：

```c
HMODULE h = LoadLibraryA("artnet_sdk.dll");
typedef ArtNetSdk* (*FnCreate)(void);
FnCreate create = (FnCreate)GetProcAddress(h, "ans_create");
```

完整动态加载示例见 `sdk/test/main.cpp`。

### 3.4 运行期定位 DLL

Windows 先搜索 exe 所在目录；把 `artnet_sdk.dll` 与运行库放同一目录即可，无需注册表或环境变量。

---

## 4. 生命周期与线程模型

### 4.1 生命周期

```
ans_create()
   ├─ 可选: ans_start_recording() → ans_record_frame()×N → ans_stop_recording()
   ├─ ans_add_artnet_target() / ans_add_dmx512_target()
   ├─ ans_start_outputs()
   ├─ ans_open_recording()
   ├─ 时间码: ans_set_timecode*()  或  ans_start_timecode_listener()
   └─ ans_destroy()
```

`ans_destroy()` 内部顺序：停止时间码监听 → 若仍在录制则收尾 → 停止输出 → 释放资源。即使遗漏 `ans_stop_*` 也不会泄漏或损坏文件。

### 4.2 线程规则

| 场景 | 规则 |
|---|---|
| 常规调用 | 所有函数必须与 `ans_create()` 处于**同一线程**（SDK 内部使用 Qt 对象，具有线程亲和性） |
| 宿主推送时间码 | 同线程调用；建议由宿主的帧定时器/MTC 回调线程切换后调用 |
| `ans_start_timecode_listener()` | 调用线程**必须存在 Qt 事件循环**（`QApplication/QCoreApplication::exec()`，或周期性 `processEvents()`）；否则套接字与定时器不会派发 |
| 多实例 | 每个实例独立，但仍应各自在创建线程内使用 |

### 4.3 错误处理约定

- 返回 `int` 的函数：`0` 成功，`-1` 失败（部分接口另有 `1` 表示"已忽略"）。
- 失败原因通过 `ans_last_error(sdk)` 读取（UTF-8）。
- `ans_last_error` 与 `ans_selftest_result` 返回**静态缓冲区**，内容在下一次调用时被覆盖；需要保留请立即拷贝。
- 成功调用通常不清空错误文本；`ans_open_recording`、`ans_start_recording`、`ans_start_timecode_listener` 启动成功时会清空。

---

## 5. 数据结构与常量

### 5.1 `AnsPlaybackInfo`

```c
typedef struct AnsPlaybackInfo {
    int       frameRateX100;    /* 2400 / 2500 / 2997 / 3000 / 5994 / 6000 */
    int       totalFrames;      /* 总帧数 */
    long long totalDurationUs;  /* 总时长（微秒） */
    int       universeCount;    /* Universe 数量 */
} AnsPlaybackInfo;
```

`frameRateX100` 是帧率 ×100：`2500` = 25 fps，`2997` = 29.97 fps，`5994` = 59.94 fps。

### 5.2 时间码源常量

```c
#define ANS_TC_SOURCE_ARTNET 1   /* Art-Net TimeCode, OpCode 0x0D00 (UDP) */
#define ANS_TC_SOURCE_MTC    2   /* MIDI Time Code (quarter-frame / full-frame) */
```

### 5.3 Universe 编码

Universe 使用 Art-Net 15 位编码：`(net << 8) | (subnet << 4) | universe`，取值 `0–32767`。

| 参数 | 含义 |
|---|---|
| `0` | Net 0 / SubNet 0 / Universe 0 |
| `1` | Net 0 / SubNet 0 / Universe 1 |
| `256` | Net 1 / SubNet 0 / Universe 0 |

---

## 6. API 参考

> 约定：下文中"帧"指 `.artrec` 文件内的帧序号（从 0 开始）。

### 6.1 版本与生命周期

#### `ans_version`

```c
const char* ans_version(void);
```

返回 SDK 版本字符串（如 `"1.0.0"`），静态常量，无需释放。

#### `ans_qt_version`

```c
const char* ans_qt_version(void);
```

返回编译 SDK 时使用的 Qt 版本（如 `"5.15.2"`）。宿主若也使用 Qt，应保持主版本一致，避免同一进程加载两套 Qt 运行库。

#### `ans_create`

```c
ArtNetSdk* ans_create(void);
```

创建实例。内部创建 `OutputManager`（Art-Net / DMX512 输出），初始无目标、输出未启动、无录制/回放。

返回实例指针；`NULL` 表示内存不足。

```c
ArtNetSdk *sdk = ans_create();
```

#### `ans_destroy`

```c
void ans_destroy(ArtNetSdk* sdk);
```

销毁实例，内部依次：停止时间码监听 → 若在录制中则收尾写入文件头 → 停止并释放输出 → 释放内存。传入 `NULL` 安全。

销毁后句柄不可再用。

#### `ans_last_error`

```c
const char* ans_last_error(ArtNetSdk* sdk);
```

返回最近一次设置的错误文本（UTF-8）；`sdk` 为 `NULL` 时返回 `"invalid sdk handle"`。

返回值为静态缓冲区，**下一次调用即失效**。

### 6.2 输出目标

输出目标在 `ans_start_outputs()` 时统一启动。目标带有 Universe 过滤能力（SDK 封装层默认不过滤，全部转发）。

#### `ans_add_artnet_target`

```c
int ans_add_artnet_target(ArtNetSdk* sdk, const char* ip, int port);
```

| 参数 | 说明 |
|---|---|
| `ip` | 目标 IPv4 地址，如 `"192.168.1.100"`；不可为空 |
| `port` | UDP 端口，`<=0` 时使用 6454 |

返回目标 ID（从 1 开始，>0），失败返回 `-1`。

说明：Art-Net 为单播；同一 Universe 会分别发往每个匹配目标。

```c
int id = ans_add_artnet_target(sdk, "192.168.1.100", 6454);
```

#### `ans_add_dmx512_target`

```c
int ans_add_dmx512_target(ArtNetSdk* sdk, const char* portName, int baudRate);
```

| 参数 | 说明 |
|---|---|
| `portName` | 串口名，如 `"COM3"`；不可为空 |
| `baudRate` | `<=0` 时使用 250000；DMX512 固定 8 数据位、2 停止位、无校验 |

返回目标 ID，失败返回 `-1`。

注意：

- 当前版本同一实例只使用**第一个** DMX512 目标；多串口同时输出暂不支持。
- 同一个串口只应添加一次；重复添加会在启动阶段失败或争用端口。
- 一个串口同一时刻只能发送一个 Universe 的一帧（约 22.6 ms / 帧）。
- 物理层需要能产生 Break 的发送器；普通 RS485 芯片需由驱动/硬件产生 Break，SDK 按标准 250000/8N2 输出。

#### `ans_clear_targets`

```c
void ans_clear_targets(ArtNetSdk* sdk);
```

停止输出（若在运行）并删除全部目标。

#### `ans_start_outputs`

```c
int ans_start_outputs(ArtNetSdk* sdk);
```

按顺序启动所有目标：

- Art-Net：创建 UDP 套接字（本地端口自动分配）；
- DMX512：打开串口并配置 250000/8N2。

返回 `0` 成功；`-1` 失败：Art-Net 套接字创建失败，或已配置的 DMX512 串口无法打开（端口不存在/被占用），错误文本用 `ans_last_error` 读取。已启动时再次调用为幂等操作。

目标应在调用本函数前添加完毕；运行中新增目标需 `ans_stop_outputs` → `ans_start_outputs` 后才会注册。

#### `ans_stop_outputs`

```c
void ans_stop_outputs(ArtNetSdk* sdk);
```

停止全部输出。目标配置保留，可再次 `ans_start_outputs` 恢复。

### 6.3 直接发送 DMX

#### `ans_send_dmx`

```c
int ans_send_dmx(ArtNetSdk* sdk, int universe, const unsigned char* data, int length);
```

| 参数 | 说明 |
|---|---|
| `universe` | 0–32767，按 15 位 Art-Net 编码；越界位会被掩码 |
| `data` | 通道数据指针，不可为 `NULL` |
| `length` | 数据字节数，钳制到 `0–512` |

返回 `0` 成功，`-1` 参数非法。

行为说明：

- 输出未启动时调用为**静默丢弃**（不报错），因此务必先 `ans_start_outputs`。
- Art-Net 包固定携带 512 通道，`length < 512` 时高位补 0；`length > 512` 截断。
- DMX512 目标同样按 512 通道帧发送。

```c
unsigned char dmx[512] = {0};
dmx[0] = 255;                       // CH1 = 100%
ans_send_dmx(sdk, 0, dmx, 512);     // Universe 0
```

#### `ans_packets_sent`

```c
unsigned long long ans_packets_sent(ArtNetSdk* sdk);
```

返回累计发送计数，**按 "Universe 帧" 计**：

- 每调用一次 `ans_send_dmx` / 回放一帧的每个 Universe，计数 +1（仅 Art-Net 路径）；
- 同一 Universe 发往多个 Art-Net 目标时仍只 +1；
- DMX512 串口发送**不计入**。

适合用于判断"是否在出数据"，不等同于 UDP 数据报总数。

### 6.4 录制 .artrec

#### `ans_start_recording`

```c
int ans_start_recording(ArtNetSdk* sdk, const char* path, int fps,
                        const int* universes, int universeCount);
```

| 参数 | 说明 |
|---|---|
| `path` | 输出文件路径（UTF-8）。若已存在会先删除 |
| `fps` | 文件帧率，`<=0` 时使用 30；支持 24/25/29.97/30/59.94/60 等 |
| `universes` | Universe 编码数组，如 `{0, 1}` |
| `universeCount` | 数组长度，必须 >0 |

返回 `0` 成功；`-1` 失败（已在录制、路径为空、参数非法、文件无法创建）。

说明：

- 每个 Universe 固定按 512 通道记录。
- 录制头中的总帧数/总时长在 `ans_stop_recording` 时回写。
- 录制期间仍可正常回放与输出（互不干扰），但通常不建议同时使用同一实例。

#### `ans_record_frame`

```c
int ans_record_frame(ArtNetSdk* sdk, long long timestampUs,
                     const int* universes,
                     const unsigned char* const* dmxData,
                     const int* dmxLen, int count);
```

| 参数 | 说明 |
|---|---|
| `timestampUs` | 本帧时间戳（微秒），相对录制起点；用于计算总时长 |
| `universes` | Universe 编码数组，长度为 `count` |
| `dmxData` | 各 Universe 的数据指针数组，元素可为 `NULL`（跳过） |
| `dmxLen` | 各 Universe 的数据长度数组；传 `NULL` 时按 512；单元素钳制到 0–512 |
| `count` | 本次写入的 Universe 数量 |

返回 `0` 成功；`-1` 失败（未开始录制、无有效数据、写盘失败）。

```c
const int unis[2] = {0, 1};
unsigned char a[512] = {0}, b[512] = {0};
const unsigned char *data[2] = {a, b};
const int lens[2] = {512, 512};

ans_start_recording(sdk, "demo.artrec", 25, unis, 2);
for (int i = 0; i < 75; ++i) {
    a[0] = (unsigned char)i;
    ans_record_frame(sdk, (long long)i * 1000000LL / 25, unis, data, lens, 2);
}
ans_stop_recording(sdk);
```

#### `ans_stop_recording`

```c
int ans_stop_recording(ArtNetSdk* sdk);
```

收尾：回写总帧数与总时长（`lastTs - firstTs`；时间戳无效时按 `帧数 / fps` 估算），关闭文件。

返回 `0` 成功，`-1` 失败或当前未在录制。

#### `ans_recorded_frames`

```c
long long ans_recorded_frames(ArtNetSdk* sdk);
```

返回当前录制已写入的帧数（停止后仍可读取）。

### 6.5 打开与回放

#### `ans_open_recording`

```c
int ans_open_recording(ArtNetSdk* sdk, const char* path);
```

打开 `.artrec` 用于回放。副作用：

- 停止正在运行的时间码监听；
- 当前帧复位为 `-1`，已发送帧计数清零；
- 时间码偏移（`ans_set_timecode_offset`）**保留**。

返回 `0` 成功；`-1` 失败（文件不存在、格式不符、路径为空）。

#### `ans_reset_playback`

```c
void ans_reset_playback(ArtNetSdk* sdk);
```

仅把当前帧位置复位为 `-1`（下一次相同时间码会重新发送该帧）。不改变文件、偏移与统计。

#### `ans_current_frame`

```c
long long ans_current_frame(ArtNetSdk* sdk);
```

返回最近一次成功输出的文件帧序号；尚未输出过为 `-1`。

#### `ans_total_frames`

```c
int ans_total_frames(ArtNetSdk* sdk);
```

返回文件总帧数；未打开时返回 `0`。

#### `ans_frames_sent`

```c
int ans_frames_sent(ArtNetSdk* sdk);
```

返回本次打开以来成功输出的帧数（重复帧不计）。用于判断回放是否推进。

#### `ans_playback_state`

```c
int ans_playback_state(ArtNetSdk* sdk);
```

| 返回值 | 含义 |
|---|---|
| `0` | 未打开文件 |
| `1` | 已打开，尚未收到时间码 |
| `2` | 已收到时间码（或监听已启动） |

#### `ans_playback_info`

```c
int ans_playback_info(ArtNetSdk* sdk, AnsPlaybackInfo* info);
```

读取文件元信息。返回 `0` 成功；`-1` 未打开或 `info` 为空。

```c
AnsPlaybackInfo info;
if (ans_playback_info(sdk, &info) == 0)
    printf("%d.%02d fps, %d 帧, %d 个域\n",
           info.frameRateX100 / 100, info.frameRateX100 % 100,
           info.totalFrames, info.universeCount);
```

### 6.6 外部时间码（宿主推送）

所有时间码入口共享同一套定位逻辑：

1. `rel = 时间码 - offset`；
2. `frame = round(rel × fps / 1e6)`，钳制到 `[0, totalFrames-1]`；
3. 与当前帧相同 → 返回 `1`（忽略）；
4. 否则定位并输出该帧 → 返回 `0`。

#### `ans_set_timecode`

```c
int ans_set_timecode(ArtNetSdk* sdk, long long timestampUs, int fps);
```

| 参数 | 说明 |
|---|---|
| `timestampUs` | 绝对时间码（微秒），不小于 0 |
| `fps` | `0` = 使用文件帧率；`2997` = 29.97；`5994` = 59.94；其它整数为该帧率 |

返回：`0` 已输出，`1` 忽略（早于偏移起点或重复帧），`-1` 失败。

```c
ans_set_timecode(sdk, 3600LL*1000000LL + 5*40000, 25);  // 01:00:05:00 @25fps
```

#### `ans_set_timecode_smpte`

```c
int ans_set_timecode_smpte(ArtNetSdk* sdk, int hh, int mm, int ss, int ff, int fps);
```

按 **非丢帧** SMPTE 计数的便捷入口：`hh:mm:ss:ff`。

- `fps <= 0` 使用文件帧率；`2997` 表示 29.97；
- `ff` 必须小于名义帧率（29.97 → 30）；
- 内部按名义帧数换算为真实时间后定位，29.97/59.94 下结果与时间轴一致；
- 丢帧计数请使用 `ans_set_timecode_string` 的 `;` 写法。

返回值同 `ans_set_timecode`。

#### `ans_set_timecode_string`

```c
int ans_set_timecode_string(ArtNetSdk* sdk, const char* smpte, int fps);
```

解析 SMPTE 字符串并定位。

| 写法 | 含义 |
|---|---|
| `"01:00:05:12"` | 非丢帧（冒号分隔） |
| `"01:00:05;12"` | 丢帧计数（分号），仅 29.97 / 59.94 生效 |

规则：

- 格式必须为 4 段数字；`hh` 0–23，`mm/ss` 0–59，`ff` < 名义帧率；
- 丢帧换算：每分钟丢 2 帧（29.97）/ 4 帧（59.94），整十分钟不丢；
- 非法格式或数值越界返回 `-1`；
- 其余返回值同 `ans_set_timecode`。

```c
ans_set_timecode_string(sdk, "01:00:00;00", 2997);  // 丢帧 00:00:00 后第 1 分钟
```

#### `ans_set_timecode_frame`

```c
int ans_set_timecode_frame(ArtNetSdk* sdk, long long frameIndex, int fps);
```

时间码源已经给出帧计数（MTC full-frame、LTC 解码器）时使用。内部换算：`ts = frameIndex / fps + offset`。

返回 `0`/`1`/`-1`，语义同上。

#### `ans_set_timecode_offset`

```c
void ans_set_timecode_offset(ArtNetSdk* sdk, long long offsetUs);
```

设置时间码基准偏移，**影响之后所有时间码入口**。

典型场景：演出时间码从 `01:00:00:00` 起算，而文件第 0 帧应对应该时刻：

```c
ans_set_timecode_offset(sdk, 3600LL * 1000000LL);
```

文件重新打开时偏移保留；需要复位传 `0`。

### 6.7 外部时间码（DLL 监听）

让 DLL 自行接收外部时间码并驱动回放，宿主只需负责输出目标与文件。

#### `ans_start_timecode_listener`

```c
int ans_start_timecode_listener(ArtNetSdk* sdk, int source, int param, int fps);
```

前置条件：

- 已 `ans_open_recording`；
- 已 `ans_start_outputs`（否则返回 `-1`，这正是常见错误来源）；
- 调用线程存在 Qt 事件循环。

| `source` | `param` 含义 |
|---|---|
| `ANS_TC_SOURCE_ARTNET` | UDP 监听端口，`0` → 6454。监听 Art-Net TimeCode（OpCode `0x0D00`） |
| `ANS_TC_SOURCE_MTC` | MIDI 输入设备索引，`-1` → 第一个可用设备 |

| `fps` | 含义 |
|---|---|
| `0` | 使用文件帧率 |
| 其它 | 时钟期望帧率（用于内部帧序号计算） |

返回 `0` 成功，`-1` 失败。重复调用会先停止旧监听。

收到时间码后按 `6.6` 的规则定位并输出，无需宿主干预。

```c
ans_set_timecode_offset(sdk, 3600LL * 1000000LL);
ans_start_timecode_listener(sdk, ANS_TC_SOURCE_ARTNET, 6454, 0);
```

注意：

- Art-Net TimeCode 的 6454 端口若已被其它 Art-Net 程序占用会启动失败；
- 监听 2 秒收不到新时间码会把超时信息写入 `ans_last_error`，收到新时间码后自动恢复；
- MTC 需要系统存在 MIDI 输入设备。

#### `ans_stop_timecode_listener`

```c
void ans_stop_timecode_listener(ArtNetSdk* sdk);
```

停止并释放监听。未启动时调用安全。`ans_destroy` 会自动调用。

### 6.8 自检

#### `ans_selftest`

```c
int ans_selftest(void);
```

无参数、全局执行一次完整闭环（不依赖 Qt 事件循环）：

1. 在系统临时目录录制 1801 帧素材（30 fps，1 个 Universe，CH1 = 帧号 × 8）；
2. 打开该文件，添加 `127.0.0.1:<随机端口>` 的 Art-Net 目标并启动输出；
3. 推送 30 个时间码，校验收到 30 个 ArtDmx 包；
4. 校验 SMPTE 字符串、分钟位、丢帧、偏移、帧号、文件信息、SMPTE 便捷入口；
5. 删除临时文件。

返回 `0` 表示全部通过；非 0 为失败步骤：

| 返回 | 失败环节 |
|---|---|
| 1 | 录制启动 |
| 2 | 写入帧 |
| 3 | UDP 绑定 |
| 4 | 打开录制 |
| 5 | 输出启动 |
| 6 | 推送时间码收发校验 |
| 7 | SMPTE 字符串定位 |
| 8 | 分钟位 / 丢帧换算 |
| 9 | 时间码偏移 |
| 10 | 帧号驱动 |
| 11 | 文件信息 |
| 12 | SMPTE 便捷入口 |

#### `ans_selftest_result`

```c
const char* ans_selftest_result(void);
```

返回最近一次自检的详情文本（UTF-8），静态缓冲区，下次调用失效。

---

## 7. 时间码换算规则

### 7.1 帧率取值

| 传入 `fps` | 实际帧率 | 名义帧数/秒 |
|---|---|---|
| `0` | 文件帧率（读文件头） | 同文件 |
| `24` | 24 | 24 |
| `25` | 25 | 25 |
| `2997` | 29.97 | 30 |
| `30` | 30 | 30 |
| `5994` | 59.94 | 60 |
| `60` | 60 | 60 |

### 7.2 帧号计算

```
frame = round((时间码 - 偏移) × 帧率 / 1,000,000)
```

例：偏移 `3600s`，时间码 `01:00:05:00 @25fps`，则
`rel = 5s`，`frame = 5 × 25 = 125`。

### 7.3 丢帧（Drop-Frame）

仅当字符串使用 `;` 且帧率为 29.97/59.94 时生效：

```
frameNumber = nominalFps × (3600×hh + 60×mm + ss) + ff
              - dropPerMin × (totalMinutes - totalMinutes / 10)
```

`nominalFps` 为 30（29.97）或 60（59.94），`dropPerMin` 为 2 或 4。
时间戳按 `frameNumber × 1001 / (nominalFps × 1000)` 秒换算。

### 7.4 边界与返回值

| 情况 | 返回 |
|---|---|
| 早于偏移起点（`rel < 0`） | `1`，不输出 |
| 与当前帧相同 | `1`，不输出 |
| 时间码超出文件末尾 | 钳制到最后一帧后输出，返回 `0`（若已是当前帧则 `1`） |
| 参数非法 / 未打开文件 / 定位失败 | `-1`，读取 `ans_last_error` |

---

## 8. 典型工作流

### 8.1 宿主推送时间码（跨语言、最常见）

```
ans_create
  → ans_add_artnet_target / ans_add_dmx512_target
  → ans_start_outputs
  → ans_open_recording
  → ans_set_timecode_offset
  → 循环: 收到外部时间码 → ans_set_timecode_string / ans_set_timecode_frame
  → ans_destroy
```

宿主可以是任何语言（C/C++/C#/Python 等），只要按 C ABI 调用；SDK 不要求事件循环。

### 8.2 DLL 自行监听（纯 Qt 宿主）

```
ans_create → 配置输出 → ans_start_outputs → ans_open_recording
  → ans_set_timecode_offset
  → ans_start_timecode_listener(ANS_TC_SOURCE_ARTNET, 6454, 0)
  → 宿主保持 Qt 事件循环
  → ans_destroy
```

### 8.3 录制素材

```
ans_create
  → ans_start_recording(path, fps, universes, n)
  → 循环: ans_record_frame(tsUs, universes, data, lens, n)
  → ans_stop_recording
  → ans_destroy
```

### 8.4 边录边转发

同一实例可同时录制与输出：录制接口只写文件，输出接口只走协议，互不阻塞。
若需要"输入 → 录制 + 输出"，宿主收到数据后分别调用 `ans_record_frame` 与 `ans_send_dmx`。

---

## 9. 性能与实时性

| 项目 | 参考值 / 说明 |
|---|---|
| Art-Net 发送 | 每 Universe 一个 UDP 数据报（530 字节），单播；单帧调用为微秒级 |
| 输出刷新率 | 常用 25/30/40 fps；回放按时间码驱动，无内部定时器 |
| DMX512 串口 | 250000 bps / 513 字节 ≈ 22.6 ms/帧；同一串口多 Universe 需串行排队 |
| 回放定位 | 每次时间码跳变执行一次文件 seek + 读取（毫秒级以内） |
| 时间码监听 | Art-Net TC / MTC 事件驱动；2 秒无包判定超时（仅记录错误文本） |

建议：

- 时间码按外部源的实际速率送入即可（LTC/MTC 通常每帧或每两帧一次），重复帧由 SDK 自动忽略；
- 多 Universe 输出到同一 DMX512 串口时，控制总帧率 ≤ 40 fps（每帧 1 Universe）以免排队；
- 高频调用时避免在同一线程做耗时操作，保证 UDP 发送及时。

---

## 10. .artrec 文件格式

SDK 录制/回放使用项目自有的二进制格式，结构如下（小端序，`#pragma pack(1)`）：

### 10.1 文件头（64 字节）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | char[8] | magic | `"ARTREC\0\0"` |
| 8 | uint16 | version | 格式版本，当前 `0x0100` |
| 10 | uint16 | frameRate | 24 / 25 / 2997 / 30 / 60 |
| 12 | uint8 | timeCodeType | 时间码类型 |
| 13 | uint8 | timeCodeFormat | SMPTE 格式 |
| 14 | uint16 | universeCount | Universe 数量 |
| 16 | uint32 | totalFrames | 总帧数（收尾时回写） |
| 20 | uint64 | totalDurationUs | 总时长（微秒） |
| 28 | uint64 | createTimestamp | 创建时间（Unix 毫秒） |
| 36 | uint32 | mediaFileOffset | 关联媒体文件名偏移 |
| 40 | uint32 | mediaFileLen | 关联媒体文件名长度 |
| 44 | uint8[20] | reserved | 保留 |

### 10.2 Universe 映射表

每个条目 4 字节：`net`(u8) / `subNet`(u8) / `universe`(u8) / `channelCount`(u8)。
`channelCount = 0` 表示 512 通道。

### 10.3 帧记录

每帧：

```
FrameRecordHeader (15B)
  uint32 frameIndex
  uint64 timestampUs
  uint8  flags        (0x01 数据变化 / 0x02 含 RDM / 0x04 时间码丢失)
  uint16 blockCount
UniverseDataBlockHeader (4B) × blockCount
  uint8  universeIndex   (映射表下标)
  uint8  dataType        (0=DMX, 1=RDM, 2=ArtNet 原始包, 3=DMX 增量)
  uint16 dataLength
  uint8[dataLength] data
```

SDK 录制的帧均为 `dataType = 0`、每个 Universe 512 字节。

---

## 11. 错误排查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| `LoadLibrary` 失败 | 缺少依赖 DLL | 检查 `artnet_sdk.dll` 同目录下 Qt/FFmpeg/MinGW 运行库 |
| `ans_create` 返回 NULL | 内存不足 | 检查系统资源 |
| `ans_add_artnet_target` 返回 -1 | IP 为空 | 传有效 IPv4 字符串 |
| `ans_start_outputs` 返回 -1 | 串口被占用/不存在，或 Art-Net 套接字创建失败 | 关闭占用程序，确认端口名；读取 `ans_last_error` |
| 运行中新增目标无输出 | 目标只在 start 时注册 | 先 `ans_stop_outputs`，再 `ans_start_outputs` |
| 调用 `ans_send_dmx` 无输出 | 未启动输出 | 先 `ans_start_outputs` |
| `ans_open_recording` 返回 -1 | 文件不存在/格式不符 | 确认是 `.artrec`，用 `ans_playback_info` 验证 |
| 时间码无反应（返回 1） | 早于偏移起点或重复帧 | 检查 `ans_set_timecode_offset`，或时间码未变化 |
| 时间码无反应（返回 -1） | 未打开文件 | 先 `ans_open_recording` |
| 帧号对不上 | 偏移缺省、fps 不符 | 设置偏移；确认使用 2997/5994 而非 29/59 |
| 监听启动 -1 | 无 Qt 事件循环 / 端口占用 / 输出未启动 | 检查三项前置条件 |
| 监听收不到 | TC 未发到对应端口 | 抓包确认 `udp.port == 监听端口` |
| 采集到异常包 | 网络中存在其它 Art-Net 源 | 用抓包过滤发送方 IP |
| 打包后别的机器打不开 | 缺 Qt/MinGW/FFmpeg 运行库 | 用 `windeployqt artnet_sdk.dll` 补齐 |

调试建议：

1. 先运行 `ans_selftest()`，`0` 表示 SDK 本身与网络栈正常；
2. 用 Wireshark 过滤 `udp.port == 6454` 验证 ArtDmx 包（长度 530，OpCode `0x5000`）；
3. 用 `ans_packets_sent` / `ans_frames_sent` / `ans_current_frame` 判断链路卡在哪一环。

---

## 12. 版本与兼容性

| 项目 | 说明 |
|---|---|
| SDK 版本 | 1.0.0 |
| 导出符号 | 32 个 `ans_*` 函数，C 调用约定 |
| 头文件 | `sdk/include/ArtNetSdk.h`（`extern "C"`，C/C++ 通用） |
| Qt 依赖 | 5.15.2；宿主若自带 Qt，需主版本一致 |
| 平台 | Windows x64 |
| 二进制兼容 | 在 1.x 内保持函数签名与语义不变；新增函数只追加 |

自检与示例：

- 自检：`build\bin\artnet_sdk_test.exe`（动态加载 + `ans_selftest`）
- 时间码监听：`build\bin\artnet_sdk_tc_test.exe`
- 示例工程：`examples\sdk_demo`（源码与分发包）
