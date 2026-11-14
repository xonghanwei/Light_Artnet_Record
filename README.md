# Light_Artnet_Record

ArtNet / DMX512 / MA NPU 协议录制器 — 基于时间码同步的专业灯光录制工具

> **当前版本: v1.2** — 完整 Protocol 抽象层 + NPU 转发 + 端到端集成测试 + Bug 回归守护

---

## 🆚 版本变更

### v1.2 (当前)
- ✅ 完整 **Protocol 抽象层** (`IProtocolSource` / `IProtocolSink`)
  - ArtNet / NPU (Ma-Net2) / DMX512 三种协议统一接口
  - `ProtocolFactory` 工厂模式创建 Source / Sink
- ✅ **MA NPU (Ma-Net2) 转发** 支持 (`--npu-forward`)
- ✅ **端到端集成测试** — 7 个测试 binary, 97 个 Incident 全绿
- ✅ **Bug 回归测试** — 8 个 Phase 5 修复的 Bug 守护, 防止复发
- ✅ **IProtocolSink 继承 QObject** — 统一信号槽 (frameSent / statsUpdated)
- ✅ ArtRecHeader 精确 64 字节 (pragma pack + reserved[20])

### v1.1
- ArtNet / DMX512 边录边转发 (OutputManager + fan-out)
- Universe 过滤 (录制 / 转发独立配置)

### v1.0
- ArtNet ArtDMX 监听 + .artrec 文件录制
- Internal Clock / Video (FFmpeg) 同步源
- QT Creator / CMake 构建系统

---

## 功能特性

- 🔌 **多协议监听**: ArtNet / NPU (Ma-Net2) / DMX512 统一抽象
- ⏱ **多源时间码同步**:
  - 内部系统时钟 (默认)
  - 视频文件时间戳 (FFmpeg 解码)
  - 音频文件 LTC (预留接口)
- 🎛 **多 Universe 录制**: 自动过滤指定 Universe, 格式兼容 NetSubUni 编码
- 📁 自定义录制格式 (.artrec): 固定头 + 变长帧结构, 高效存储
- 🔄 **边录边转发**: ArtNet + NPU + DMX512 三路同时 fan-out
- 🎬 **命令行工具**: 无 UI, 可集成到自动化工作流
- 🧪 **完整测试覆盖**: 协议层 / 文件格式 / 录制引擎端到端
- 🖥️ **跨平台**: Windows / Linux / macOS 统一代码库 (Qt 抽象 + CMake)

## 技术栈

| 组件 | 版本 | 说明 |
|------|------|------|
| C++ | 17 | 语言标准 |
| Qt | 5.15.2 | 网络 + 串口 + Test (x86_64 MinGW/GCC 8.1.0) |
| FFmpeg | 7.x / 8.x | 媒体解码 (avformat/avcodec/avutil/swresample) |
| CMake | 3.16+ | 构建系统 |

## 架构分层

```
┌─────────────────────────────────────────────────────────────┐
│ Layer 5: main.cpp (CLI入口) + RecorderEngine (编排层)       │
│   状态机: Idle → Recording → Paused → Stopped               │
│   预览定时器驱动逐帧写入                                     │
├─────────────────────────────────────────────────────────────┤
│ Layer 4: Protocol 抽象层 (v1.2 新增)                         │
│   IProtocolSource  IProtocolSink (均继承 QObject)            │
│   ProtocolFactory → 创建 Source / Sink                      │
│   LightFrame: { universe, dmxData, timestampUs }            │
├─────────────────────────────────────────────────────────────┤
│ Layer 3: 协议实现                                            │
│   Source: ArtNetSource / NpuSource / Dmx512Source           │
│   Sink:   ArtNetSink   / NpuSink   / Dmx512Sink             │
│   底层: ArtNetReceiver/ArtNetSender (QUdpSocket)            │
│         Dmx512Sender (QSerialPort)                          │
├─────────────────────────────────────────────────────────────┤
│ Layer 2: 文件格式 + 输出管理                                 │
│   ArtRecWriter / ArtRecReader (.artrec 容器)                │
│   OutputManager (统一 fan-out, Universe 过滤)                │
├─────────────────────────────────────────────────────────────┤
│ Layer 1: 基础模块                                            │
│   Common.h (常量/类型/RecordState)                          │
│   Logger (分级日志)                                          │
│   FFmpegDecoder (视频时间码)                                 │
└─────────────────────────────────────────────────────────────┘
```

## 目录结构

```
Light_Artnet_Record/
├── CMakeLists.txt
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp                    # CLI 入口
│   │
│   ├── core/                       # Layer 1 — 基础模块
│   │   ├── Common.h                # 常量/RecordState/encodeUniverse
│   │   ├── Logger.h / .cpp         # 分级日志 (Debug/Info/Warn/Error)
│   │
│   ├── fileformat/                 # Layer 2 — .artrec 容器
│   │   ├── ArtRecTypes.h           # ArtRecHeader (64B) + UniverseEntry + FrameRecordHeader
│   │   ├── ArtRecWriter.h / .cpp   # 文件写入器 (finalize + 回写头)
│   │   └── ArtRecReader.h / .cpp   # 文件读取器 (seek + 逐帧遍历)
│   │
│   ├── protocol/                   # Layer 4 — Protocol 抽象层 (v1.2)
│   │   ├── IProtocolSource.h/.cpp  # 协议输入源接口 (QObject, signals frameReceived)
│   │   ├── IProtocolSink.h/.cpp    # 协议输出接口 (QObject, signals frameSent/statsUpdated)
│   │   ├── ProtocolFactory.h/.cpp  # 工厂: createSource() / createSink()
│   │   ├── ProtocolTypes.h         # LightFrame struct + Q_DECLARE_METATYPE
│   │   ├── sources/                # ArtNetSource / NpuSource / Dmx512Source
│   │   └── sinks/                  # ArtNetSink / NpuSink / Dmx512Sink
│   │
│   ├── network/                    # Layer 3 — ArtNet 底层
│   │   ├── ArtNetReceiver.h/.cpp   # QUdpSocket 接收 + ArtDMX 解析
│   │   └── ArtNetSender.h/.cpp     # ArtDMX 包构造 + QUdpSocket 发送
│   │
│   ├── output/                     # Layer 2 — 输出管理
│   │   ├── OutputManager.h/.cpp    # 统一输出路由, fan-out, Universe 过滤
│   │   └── Dmx512Sender.h/.cpp     # QSerialPort (USB-RS485 适配器)
│   │
│   ├── media/                      # Layer 1 — 媒体解码
│   │   └── FFmpegDecoder.h/.cpp    # avformat/avcodec 封装, 时间戳提取
│   │
│   └── recorder/                   # Layer 5 — 核心引擎
│       └── RecorderEngine.h/.cpp   # 状态机 + 预览定时器 + 录制编排
│
├── tests/
│   ├── CMakeLists.txt
│   └── unit/
│       ├── TestCommon.cpp              # Common.h / Logger
│       ├── TestProtocolFactory.cpp     # ProtocolFactory 工厂创建
│       ├── TestProtocolSources.cpp     # ArtNetSource / NpuSource / Dmx512Source
│       ├── TestProtocolSinks.cpp       # ArtNetSink / NpuSink / Dmx512Sink
│       ├── TestArtRecFormat.cpp        # .artrec 文件格式读写
│       ├── TestRecorderEngine.cpp      # 端到端: UDP → 录制 → .artrec → 验证
│       └── TestBugRegressions.cpp      # Phase 5 Bug 回归守护 (8 cases)
│
├── third_party/
│   ├── ffmpeg/
│   └── cmake/FindFFmpeg.cmake
│
└── docs/
    └── ArtNet录制器开发文档.md
```

## 快速开始 (Windows + MinGW/GCC)

### 1. 准备 Qt 5.15.2

从 Qt Online Installer 安装 **Qt 5.15.2 → MinGW 8.1.0 64-bit**。
确保路径:
```
C:\Qt\5.15.2\mingw81_64\bin\          # Qt DLLs
C:\Qt\Tools\mingw810_64\bin\          # GCC 工具链
```

### 2. 准备 FFmpeg

下载 BtbN FFmpeg Builds (推荐 `ffmpeg-n7.1-latest-win64-gpl-shared-7.1.zip`),
解压到 `third_party/ffmpeg/`:
```
third_party/ffmpeg/
├── include/        # libavformat / libavcodec / libavutil / libswresample
├── lib/            # .lib 导入库
└── bin/            # DLL (avformat-61.dll, avcodec-61.dll, avutil-61.dll, swresample-6.dll)
```

### 3. 构建

```powershell
# 设置 PATH (重要!)
$env:PATH = "C:\Qt\5.15.2\mingw81_64\bin;C:\Qt\Tools\mingw810_64\bin;" + $env:PATH

# 配置 + 构建
mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="C:\Qt\5.15.2\mingw81_64" -DBUILD_TESTING=ON
mingw32-make -j4
```

### 4. 运行时 DLL

```powershell
cd build\bin
Copy-Item "C:\Qt\5.15.2\mingw81_64\bin\*.dll" .
Copy-Item "..\..\third_party\ffmpeg\bin\*.dll" .
Copy-Item "C:\Qt\Tools\mingw810_64\bin\libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll" .
```

### 5. 运行测试

```powershell
cd build\tests\bin
$env:PATH = "C:\Qt\5.15.2\mingw81_64\bin;C:\Qt\Tools\mingw810_64\bin;" + $env:PATH

# 全量 (7 个 binary, 97 incidents, 全绿)
.\TestCommon.exe -o tc.xml,xml
.\TestProtocolFactory.exe -o tpf.xml,xml
.\TestArtRecFormat.exe -o tar.xml,xml
.\TestProtocolSources.exe -o tps.xml,xml
.\TestProtocolSinks.exe -o tpk.xml,xml
.\TestRecorderEngine.exe -o tre.xml,xml
.\TestBugRegressions.exe -o tbr.xml,xml
```

### MSVC 方案 (备选)

```powershell
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:\Qt\5.15.2\msvc2019_64" -DBUILD_TESTING=ON
cmake --build . --config Release
```

## 使用方法

### 内部时钟录制 (最简单)
```powershell
# 录制 Universe 0-3, 30fps, 内部时钟
artnet_recorder.exe -o show.artrec -s internal -u "0,1,2,3" -f 30

# 停止时按 Ctrl+C
```

### 视频文件同步
```powershell
# 从 video.mp4 提取时间戳, 同步录制 ArtNet 数据
artnet_recorder.exe -o show.artrec -s video --media video.mp4 -u "0,1,2" -f 25
```

### 限制时长
```powershell
# 录制 60 秒后自动停止
artnet_recorder.exe -o show.artrec -s internal -d 60
```

### 边录边转发

录制同时把 DMX 数据实时输出到其他设备。

#### ArtNet 转发 (网络)
```powershell
# 录制同时转发到另一台 ArtNet 解码器 (192.168.1.100:6454)
artnet_recorder.exe -o show.artrec -s internal -u "0,1,2,3" --artnet-forward 192.168.1.100

# 多目标
artnet_recorder.exe -o show.artrec -u "0,1,2,3" `
    --artnet-forward 192.168.1.100 `
    --artnet-forward 192.168.1.101:6454

# 录制所有 Universe, 只转发 0 和 1
artnet_recorder.exe -o show.artrec -u "0,1,2,3" `
    --artnet-forward 192.168.1.100 `
    --artnet-forward-univ "0,1"
```

#### MA NPU (Ma-Net2) 转发 (v1.2 新增)
```powershell
# 录制同时转发到 MA2 onPC 的 NPU (默认端口 9999)
artnet_recorder.exe -o show.artrec -s internal -u "0,1,2" --npu-forward 192.168.1.50

# 多目标 + Universe 过滤
artnet_recorder.exe -o show.artrec -u "0,1,2,3" `
    --npu-forward 192.168.1.50 `
    --npu-forward 192.168.1.51:9999 `
    --npu-forward-univ "1,2"
```

#### DMX512 串口输出
```powershell
# 录制同时驱动 DMX King ultraDMX Micro
artnet_recorder.exe -o show.artrec -s internal -u "0,1,2" --dmx-port COM3

# 指定波特率 (默认 250000)
artnet_recorder.exe -o show.artrec -u "0,1" --dmx-port COM3 --dmx-baud 250000
```

#### 组合: ArtNet + NPU + DMX512 同时
```powershell
# 录制 → 同时三路输出
artnet_recorder.exe -o show.artrec -s video --media show.mp4 -u "0,1,2,3" `
    --artnet-forward 192.168.1.200 `
    --npu-forward 192.168.1.50 `
    --dmx-port COM3
```

### 验证录制文件
```powershell
# 查看录制文件元信息
artnet_recorder.exe -m verify -o show.artrec
```

### Universe 编码说明

ArtNet 使用 **NetSubUni** 编码: `Net(7bit) | SubNet(4bit) | Universe(4bit)`

- **十进制**: `0, 1, 2, 3` 表示 Universe 0-3 (Net=0, SubNet=0)
- **十六进制**: 更精确, 格式 `0xNNSSUU`
  - `0x0000` = Net0 Sub0 Uni0
  - `0x0102` = Net0 Sub1 Uni2 (十进制 258)
  - `0x7F0F` = Net127 Sub15 Uni15 (最大 Universe 32767)

CLI 默认 **十六进制优先**, 解析失败回退十进制。

### 完整命令行参数

| 参数 | 缩写 | 说明 | 默认值 |
|------|------|------|--------|
| `--output` | `-o` | 输出 .artrec 文件路径 | **必需** |
| `--mode` | `-m` | `record` / `verify` | `record` |
| `--sync` | `-s` | `internal` / `video` / `audio` | `internal` |
| `--media` | | 媒体文件路径 (video/audio 需要) | — |
| `--universe` | `-u` | Universe 列表 (逗号分隔, 十进制/十六进制) | `0,1,2,3` |
| `--fps` | `-f` | 帧率: 24 / 25 / 29.97 / 30 / 60 | `30` |
| `--duration` | `-d` | 最大时长 (秒, 0=不限) | `0` |
| `--port` | `-p` | ArtNet UDP 监听端口 | `6454` |
| `--verbose` | `-v` | 详细日志 (Debug 级别) | off |
| `--no-preview` | | 禁用实时预览输出 | off |
| `--artnet-forward` | | ArtNet 转发目标 `IP[:PORT]` (可多次) | — |
| `--artnet-forward-univ` | | ArtNet 转发 Universe 过滤 | 全部 |
| `--npu-forward` | | NPU 转发目标 `IP[:PORT]` (可多次) | — |
| `--npu-forward-univ` | | NPU 转发 Universe 过滤 | 全部 |
| `--dmx-port` | | DMX512 串口 (如 `COM3`, `/dev/ttyUSB0`) | — |
| `--dmx-baud` | | DMX512 串口波特率 | `250000` |

## 录制文件格式 (.artrec)

### 结构概览

```
┌─────────────────────────────────────┐
│ ArtRecHeader (固定 64 字节)          │
│   magic "ARTREC\0\0" (8)            │
│   version (2) + frameRate (2)       │
│   timeCodeType/Format (2)           │
│   universeCount (2)                 │
│   totalFrames (4) + totalDuration (8)│
│   createTimestamp (8)               │
│   mediaFileOffset + mediaFileLen (8) │
│   reserved[20] = 0 (padding 精确64B) │
├─────────────────────────────────────┤
│ Universe 映射表 (N × 4 字节)         │
│   UniverseEntry: net + subNet +     │
│                  universe + channel  │
├─────────────────────────────────────┤
│ Frame Data 区 (变长)                 │
│  ┌─────────────────────────────┐   │
│  │ FrameRecordHeader (15 字节)  │   │
│  │   frameIndex (4)            │   │
│  │   timestampUs (8)           │   │
│  │   flags (1) + blockCount (2)│   │
│  ├─────────────────────────────┤   │
│  │ UniverseDataBlock × N       │   │
│  │   BlockHeader (4B) + Data   │   │
│  │     (DMX 512B / Delta /     │   │
│  │      ArtNet Raw / RDM)      │   │
│  └─────────────────────────────┘   │
├─────────────────────────────────────┤
│ 文件尾 (可选: 媒体文件名)            │
└─────────────────────────────────────┘
```

### C++ 读取示例

```cpp
#include "fileformat/ArtRecReader.h"

ArtRecReader reader;
if (reader.open("show.artrec")) {
    const ArtRecHeader &h = reader.header();
    qDebug() << "Frames:" << h.totalFrames << "Duration:" << h.totalDurationUs << "us";
    
    qint64 frameIndex, timestampUs;
    QMap<int, QByteArray> universeData;  // key=universeIndex (从0开始)
    
    while (reader.readNextFrame(frameIndex, timestampUs, universeData)) {
        // universeData[0] → 第一个 Universe 的 512 字节 DMX
        // universeData[1] → 第二个 Universe 的 512 字节 DMX
        for (auto it = universeData.constBegin(); it != universeData.constEnd(); ++it) {
            // it.key() = Universe 在映射表中的 index
            // it.value() = DMX 512 字节数据
        }
    }
    
    reader.close();
}
```

### 文件头结构体 (pragma pack 1)

```cpp
struct ArtRecHeader {      // 精确 64 字节
    char     magic[8];     // "ARTREC\0\0"
    quint16  version;      // 格式版本 (0x0100)
    quint16  frameRate;    // 24/25/2997/30/60
    quint8   timeCodeType;
    quint8   timeCodeFormat;
    quint16  universeCount;
    quint32  totalFrames;
    quint64  totalDurationUs;
    quint64  createTimestamp;
    quint32  mediaFileOffset;
    quint32  mediaFileLen;
    quint8   reserved[20]; // padding → 总大小 64
};
```

## 跨平台特性

### 支持矩阵

| 平台 | 状态 | 编译器 | 构建方式 | DMX512 后端 |
|------|------|--------|---------|-------------|
| **Windows 10/11 x64** | ✅ 已验证 | GCC 8.1 (MinGW) / MSVC 2022 | CMake + MinGW / VS | `EscapeCommFunction(SETBREAK)` |
| **Linux x64** | ✅ 可编译 | GCC 9+/Clang 11+ | CMake + Make / Ninja | `tcsendbreak(fd, 0)` |
| **Linux ARM64** (树莓派) | ✅ 可编译 | GCC 11+ | CMake + Make | `tcsendbreak(fd, 0)` |
| **macOS Intel/Apple Silicon** | ✅ 可编译 | Xcode 13+ / Homebrew GCC | CMake + Xcode / Ninja | `tcsendbreak(fd, 0)` |

### 平台抽象层设计

```
跨平台问题            解决方案                     代码位置
──────────────────────────────────────────────────────────────────
UDP/IP 网络收发  →  Qt5::QUdpSocket          ArtNetReceiver / ArtNetSender
串口通信         →  Qt5::QSerialPort          Dmx512Sender (统一 API)
文件 I/O         →  QFile + QDataStream       ArtRecWriter / ArtRecReader
线程/定时        →  QThread + QTimer          RecorderEngine 预览定时器
时间戳           →  QDateTime / QElapsedTimer  RecorderEngine
日志             →  自实现 Logger              core/Logger (无平台依赖)
进程信号         →  std::signal + std::atomic  main.cpp (POSIX + Windows)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
DMX512 BREAK  →  Windows: EscapeCommFunction   Dmx512Sender::sendFrame()
               Linux/macOS: tcsendbreak
                  (条件编译: #ifdef Q_OS_WIN)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
FFmpeg 链接    →  Windows: *.lib              CMakeLists.txt
               Linux/macOS: *.so / *.a
                  (pkg_check_modules 回退)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Windows 特有    →  ws2_32 (winsock2)          src/CMakeLists.txt
链接库              Qt5::Network 内部已处理大部分
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
进程中断处理    →  Windows: signal(SIGINT)     main.cpp
               Linux/macOS: signal(SIGINT, SIGTERM)
                  (POSIX 头 unistd.h 跨平台)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

### 源码中唯一的平台分支

整个项目 **32 个源文件** 中, 只有 **1 处** 条件编译 —— DMX512 BREAK 信号发送:

```cpp
// 文件: src/output/Dmx512Sender.cpp 第 105-132 行
#ifdef Q_OS_WIN
    // Windows: Win32 API EscapeCommFunction
    HANDLE handle = m_serial->handle();
    EscapeCommFunction(handle, SETBREAK);
    EscapeCommFunction(handle, CLRBREAK);
#else
    // Linux/macOS: POSIX tcsendbreak
    int fd = m_serial->handle();
    tcsendbreak(fd, 0);
#endif
```

除此之外 —— **ArtNet / NPU (Ma-Net2) / 文件格式 / 录制引擎 / 协议抽象层 / Logger** — 全部平台无关。

### Linux 构建

```bash
# Ubuntu 22.04 / Debian 12
sudo apt install qt5-default libqt5serialport5-dev cmake build-essential \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
make -j$(nproc)

# 运行测试
ctest --output-on-failure
```

### macOS 构建

```bash
# Homebrew
brew install qt@5 ffmpeg cmake

mkdir build && cd build
export CMAKE_PREFIX_PATH="$(brew --prefix qt@5)"
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
make -j$(sysctl -n hw.ncpu)

# 运行测试
ctest --output-on-failure
```

### macOS 备注

DMX512 设备识别: macOS 上 USB-DMX 适配器通常以 `/dev/tty.usb*` 或 `/dev/cu.usb*` 命名,
用 `--dmx-port /dev/cu.usbmodem*` 指定 (推荐 `cu.` 前缀)。

### Linux 备注

1. **串口权限**: 普通用户可能无权访问 `/dev/ttyUSB*`, 需:
   ```bash
   sudo usermod -aG dialout $USER
   # 或临时
   sudo chmod 666 /dev/ttyUSB0
   ```
2. **Realtime 调度** (可选): UDP 高频接收时可获得更稳定延迟:
   ```bash
   sudo setcap cap_sys_nice,cap_ipc_lock+ep artnet_recorder
   ```
3. **网络参数优化** (ArtNet 60fps 512 channels):
   ```bash
   sudo sysctl -w net.core.rmem_max=262144
   sudo sysctl -w net.core.wmem_max=262144
   ```

### 已在 Windows 验证

| 模块 | 测试方式 | 状态 |
|------|---------|------|
| ArtNet UDP 收发 | QUdpSocket 自环 127.0.0.1 | ✅ 通过 |
| NPU (Ma-Net2) UDP 收发 | QUdpSocket 自环 | ✅ 通过 |
| DMX512 串口 | FT232 / CH340 适配器 | ✅ 硬件验证 |
| .artrec 文件读写 | ArtRecWriter → ArtRecReader | ✅ 64 字节头精确对齐 |
| RecorderEngine 端到端 | UDP → 录制 → 读回验证 | ✅ 6 帧 512B DMX 完整 |
| FFmpeg 视频同步 | BtbN build avformat/avcodec | ✅ 已链接 |
| 全量测试 7 binary | ctest + XML 输出 | ✅ 97 incidents, 0 fails |

## 测试覆盖

| 测试 Binary | Slots | 覆盖范围 |
|------------|-------|---------|
| TestCommon | ~20 | Common.h / RecordState / Logger / Universe 编解码 |
| TestProtocolFactory | 10 | 工厂创建 Source/Sink / 类型检查 |
| TestProtocolSources | 14 | ArtNetSource / NpuSource / Dmx512Source 包解析 + 信号发射 |
| TestProtocolSinks | 18 | ArtNetSink / NpuSink / Dmx512Sink 包构造 + UDP/DMX 发送 |
| TestArtRecFormat | 10 | .artrec 写入 + 读取 + 覆盖 + 空文件 + 损坏检测 |
| **TestRecorderEngine** | **5** | **端到端: start/stop/状态机/端口冲突/UDP自回路录制/Universe过滤** |
| **TestBugRegressions** | **8** | **Phase 5 Bug 回归: 死锁/state被覆盖/universeCount/端口独占/ArtDMX长度字节数/Header sizeof/单Universe key** |

### 端到端测试流程 (TestRecorderEngine::test_udpRoundTrip)

```
QUdpSocket → 构造 ArtDMX 包 (530 字节)
          → 发到 127.0.0.1:26674
          → ArtNetReceiver 解析
          → emit frameReceived(LightFrame)
          → RecorderEngine cache → preview tick
          → ArtRecWriter.writeFrame()
          → engine.stop() → finalize() → close()
          → ArtRecReader.open() → readNextFrame()
          → 验证 DMX 数据完整性
```

## 协议参考

| 协议 | 标准 | 说明 |
|------|------|------|
| Art-Net | v1.4 | UDP/IP 灯光控制网络协议 (端口 6454, ArtDMX OpCode 0x5000) |
| Ma-Net2 | — | MA Lighting NPU 网络协议 (端口 9999) |
| DMX512 | ANSI E1.11 | 512 通道串行数字传输 (250000 baud, 8N2) |
| RDM | ANSI E1.20 | 远程设备管理 (预留) |

## 后续扩展计划

- [ ] MIDI 时间码 (MTC) 输入支持
- [ ] LTC 音频时间码解析
- [ ] 录制预览和播放引擎 (Renderer / Player)
- [ ] RDM 设备数据录制
- [ ] Qt Widgets GUI 界面
- [ ] sACN (E1.31) 协议 Source/Sink
- [ ] Art-Net 4 发现/广播 (ArtPoll / ArtProbe)
- [ ] DMX Delta 压缩 (只记录变化通道)
- [ ] OSC (Open Sound Control) 时间码 / 触发

## 许可证

本项目使用 MIT 许可证。
