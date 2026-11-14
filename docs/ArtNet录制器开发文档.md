# ArtNet/DMX512 录制器开发文档

> **版本**: v1.2  
> **更新**: 2026-11-12  
> **变更**: v1.1 新增边录边转发 (ArtNet/DMX512 输出)  
>          v1.2 新增协议抽象层 (IProtocolSource/IProtocolSink), 支持 NPU/DMX512 输入,  
>               统一 LightFrame 数据结构, ProtocolFactory 统一工厂

---

## 1. 项目概述

### 1.1 项目目标
开发一个命令行灯光录制工具:
- 基于**时间码同步**录制 DMX 数据帧
- 支持**多种输入协议**: ArtNet / MA NPU (Ma-Net2) / DMX512 串口
- 支持**边录边转发**: 录制同时把 DMX 数据输出到 ArtNet / DMX512 / NPU 设备
- 录制格式自定义 (.artrec), 类 MP4 容器结构

### 1.2 应用场景
- 灯光秀录制与回放
- 现场演出备份 (录制同时继续驱动灯光)
- 多协议桥接 (ArtNet ↔ NPU ↔ DMX512)
- 灯光节目预编

### 1.3 技术栈

| 组件 | 版本 | 说明 |
|------|------|------|
| C++ | 17 | 语言标准 |
| Qt | 5.15+ | Core + Network + SerialPort |
| FFmpeg | 7.x/8.x | 视频解码提取时间戳 |
| CMake | 3.16+ | 构建系统 |

### 1.4 目录结构

```
Light_Artnet_Record/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── ArtNet录制器开发文档.md
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── core/                    # 通用
│   │   ├── Common.h             # ArtNet/DMX常量, 帧结构, 时间码
│   │   └── Logger.h/.cpp        # 日志系统
│   ├── protocol/                # ★ 协议抽象层 (v1.2)
│   │   ├── ProtocolTypes.h      # LightFrame, ProtocolType, ConnectionParams, NPU协议定义
│   │   ├── IProtocolSource.h    # ★ 接收抽象接口 (纯虚)
│   │   ├── IProtocolSink.h      # ★ 输出抽象接口 (纯虚)
│   │   ├── ProtocolFactory.h/.cpp # ★ 统一工厂
│   │   ├── sources/
│   │   │   ├── ArtNetSource.h/.cpp  # 包装 ArtNetReceiver
│   │   │   ├── NpuSource.h/.cpp     # MA NPU (Ma-Net2) 协议接收
│   │   │   └── Dmx512Source.h/.cpp  # DMX512 串口被动接收
│   │   └── sinks/
│   │       ├── ArtNetSink.h/.cpp    # 包装 ArtNetSender
│   │       ├── NpuSink.h/.cpp       # MA NPU 协议输出
│   │       └── Dmx512Sink.h/.cpp    # 包装 Dmx512Sender
│   ├── network/                  # 底层 ArtNet 组件
│   │   └── ArtNetReceiver.h/.cpp
│   ├── output/                   # 底层输出组件 (v1.1)
│   │   ├── ArtNetSender.h/.cpp
│   │   ├── Dmx512Sender.h/.cpp
│   │   └── OutputManager.h/.cpp  # 便捷的 fan-out 包装
│   ├── media/
│   │   └── FFmpegDecoder.h/.cpp
│   ├── recorder/
│   │   └── RecorderEngine.h/.cpp
│   └── fileformat/
│       ├── ArtRecTypes.h
│       ├── ArtRecWriter.h/.cpp
│       └── ArtRecReader.h/.cpp
└── third_party/
    ├── ffmpeg/                   # 用户提供
    └── cmake/FindFFmpeg.cmake
```

---

## 2. 协议抽象层设计 (v1.2 核心)

### 2.1 设计动机

不同灯光协议 (ArtNet / NPU / DMX512 / sACN) 最终携带的都是"512 字节 DMX 通道数据 + Universe 编号 + 时间戳"。
以前每个协议写一套独立的接收/发送代码 → **重复、难扩展**。

**核心决策**: 把协议处理分成两层:

```
┌────────────────────────────────────────────────┐
│           应用层 (RecorderEngine, Player)       │
│   只操作 LightFrame, 不知道底层是什么协议        │
└─────────────────┬──────────────────────────────┘
                  │ LightFrame (协议无关)
┌─────────────────▼──────────────────────────────┐
│       协议抽象层 (IProtocolSource / Sink)       │
│   统一接口, 屏蔽各协议的帧头/端口/CRC 差异      │
└──────┬──────────┬──────────┬──────────┬───────┘
       │          │          │          │
   ArtNetSource NpuSource  Dmx512Source   ...
   ArtNetSink   NpuSink   Dmx512Sink      ...
```

### 2.2 LightFrame — 协议无关数据帧

任何协议 Source 解析后输出 LightFrame, Sink 接收 LightFrame 后组装成协议包。

```cpp
struct LightFrame {
    ProtocolType sourceProtocol;  // 来自哪个协议
    quint16 universe;             // 编码后的 Universe (ArtNet 兼容)
    quint8  universeRaw;          // 协议原始 Universe 号 (NPU 0-255)
    QByteArray dmxData;           // DMX 通道数据, 最多 512 字节
    qint64  timestampUs;          // 接收时间戳 (微秒)
    quint32 sequence;             // 协议原始序列号
    bool    hasRdm;               // 是否含 RDM 数据
    QByteArray rdmPayload;
};
```

### 2.3 IProtocolSource — 接收接口

```cpp
class IProtocolSource : public QObject {
    Q_OBJECT
public:
    virtual ProtocolType protocolType() const = 0;
    virtual bool start(const ConnectionParams &params) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual void setFilterUniverses(const QList<quint16> &u) = 0;
    virtual quint64 framesReceived() const = 0;
    
signals:
    void frameReceived(const LightFrame &frame);  // ★ 唯一数据出口
    void statusChanged(const QString &status);
    void errorOccurred(const QString &error);
};
```

**职责边界**: Source 负责解析协议帧头/长度/校验, 输出填充好的 LightFrame。
**上层 (RecorderEngine) 不接触任何协议细节**。

### 2.4 IProtocolSink — 输出接口

```cpp
class IProtocolSink {  // 纯接口, 不继承 QObject
public:
    virtual ProtocolType protocolType() const = 0;
    virtual bool start(const ConnectionParams &params) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual bool sendFrame(const LightFrame &frame) = 0;
    virtual quint64 framesSent() const = 0;
};
```

**Sink 内部**负责组装协议帧头、填充 DMX 数据、发包。上层统一调 `sendFrame(LightFrame)`。

### 2.5 ProtocolFactory — 统一工厂

```cpp
// 按枚举创建
auto *source = ProtocolFactory::createSource(ProtocolType::ArtNet);
auto *sink   = ProtocolFactory::createSink(ProtocolType::Npu);

// 按名称创建 (支持 "artnet", "npu", "dmx512")
auto *source = ProtocolFactory::createSource("npu");
```

### 2.6 Source / Sink 实现清单

| 协议 | Source 类 | Sink 类 | 底层组件 | 传输 |
|------|-----------|---------|---------|------|
| ArtNet | ArtNetSource | ArtNetSink | ArtNetReceiver / ArtNetSender | UDP 6454 |
| NPU (Ma-Net2) | NpuSource | NpuSink | QUdpSocket (自实现) | UDP 9999 |
| DMX512 | Dmx512Source | Dmx512Sink | QSerialPort / Dmx512Sender | RS-485 / COM |

### 2.7 ConnectionParams — 统一连接参数

```cpp
struct ConnectionParams {
    ProtocolType protocol;
    
    // 网络协议 (ArtNet/NPU/sACN)
    QString localBindAddress = "0.0.0.0";
    quint16 localPort = 0;       // Source: 接收端口; Sink: 本地绑定
    QString remoteAddress;       // Sink: 目标 IP
    quint16 remotePort = 0;      // Sink: 目标端口
    
    // 串口协议 (DMX512)
    QString serialPortName;      // "COM3" / "/dev/ttyUSB0"
    qint32  serialBaudRate = 250000;
    
    // Universe 过滤 (空=全部)
    QList<quint16> filterUniverses;
};
```

---

## 3. 各协议规范

### 3.1 ArtNet (UDP 6454)

见原开发文档 §2.1。保持不变。

### 3.2 MA NPU / Ma-Net2 (UDP 9999)

MA Lighting 的主从架构协议。

**DMX 数据包格式**:
```
偏移  长度  字段
0     2     length (小端, 固定 514)
2     1     cmd (0x01 = DMX 数据)
3     1     universe (0-255)
4     512   DMX 通道数据
```

**常用 OpCode**:

| cmd | 含义 |
|-----|------|
| 0x01 | DMX 数据包 |
| 0x02 | Universe 信息 / 请求 |
| 0x03 | 时间码 |
| 0x10 | MA 专用命令 |

### 3.3 DMX512 (串口)

**协议帧**: BREAK(90μs) + MAB(12μs) + Start Code(0x00) + 512 channels  
**串口参数**: 250000 baud, 8 data bits, 2 stop bits, No parity  
**接收说明**: DMX512 通常是单向的(控制台→灯具), 接收需要硬件支持 (Enttec Pro Mk3 等)  
**发送说明**: 用 USB-DMX 设备 (DMXKing ultraDMX Micro, Enttec Open DMX) 发送

---

## 4. 文件格式 (.artrec)

保持 v1.0 设计不变。见原开发文档 §3。

**快速参考**:
```
ArtRecHeader (固定 64 字节)
  Magic: "ARTREC\0\0", Version: 0x0100
  FrameRate, TimeCodeType, TimeCodeFormat
  UniverseCount, TotalFrames, TotalDurationUs
  CreateTimestamp, MediaFileOffset/Len, Reserved

Universe 映射表 (N × 4 字节)
  Net | SubNet | Universe | ChannelCount

Frame Data 区
  ┌─────────────────────────────┐
  │ FrameRecordHeader (15 字节)  │ frameIndex + timestampUs + flags + blockCount
  │ UniverseDataBlock × N        │
  │   ┌───────────────────────┐ │
  │   │ BlockHeader (4 字节)   │ │ universeIndex + dataType + dataLength
  │   │ DMX Data (512 字节)    │ │
  │   └───────────────────────┘ │
  └─────────────────────────────┘

文件尾 (可选)
  关联媒体文件名
```

---

## 5. 整体架构 (v1.2)

```
┌─────────────────────────────────────────────────────────┐
│                    main.cpp (CLI)                        │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│                  RecorderEngine                          │
│                                                          │
│  Config: outputFile, syncSource, fps, universes          │
│          + outputTargets (多协议转发配置)                 │
│                                                          │
│  帧对齐 / 时间码同步 / DMX 变化检测 / 写文件              │
│                                                          │
│  内部只操作 LightFrame, 不知道用的什么协议                │
└──┬─────────────┬─────────────┬──────────────────────────┘
   │             │             │
   ▼             ▼             ▼
┌──────┐    ┌─────────┐   ┌──────────────┐
│Source│    │  Writer │   │  Sink 列表    │
│工厂  │    │         │   │ (fan-out)    │
└──┬───┘    └─────────┘   └──┬─────┬─────┘
   │                         │     │
   ▼                         ▼     ▼
 ArtNetSource            ArtNetSink NpuSink Dmx512Sink
 NpuSource                 │           │       │
 Dmx512Source               ▼           ▼       ▼
                         UDP 6454   UDP 9999   COM3
                      (解码器)  (MA NPU)  (灯具)

┌─────────────────────────────────────────────────────────┐
│  时间码同步子系统                                         │
│  ┌────────────────┐  ┌──────────────┐                   │
│  │ FFmpegDecoder  │  │ 内部时钟     │                   │
│  │ (视频帧时间戳)  │  │ (QElapsed)   │                   │
│  └────────────────┘  └──────────────┘                   │
└─────────────────────────────────────────────────────────┘
```

---

## 6. 边录边转发 (v1.1)

### 6.1 架构

RecorderEngine 每一帧写完文件后, **同步**发送到所有配置的 Sink:

```
帧周期触发
  │
  ▼
RecorderEngine.onPreviewTick()
  │
  ├─▶ ArtRecWriter.writeFrame()          ← 写文件
  │
  └─▶ OutputManager.sendMultipleUniverses()   ← 转发 (可选)
        │
        ├─▶ ArtNetSender.sendMultipleUniverses()  ──▶ UDP 6454 → 解码器
        ├─▶ NpuSink.sendFrame()                    ──▶ UDP 9999 → MA NPU
        └─▶ Dmx512Sender.sendFrame()               ──▶ COM3 → DMX King → 灯具
```

### 6.2 转发目标配置

每个目标独立配置:

| 字段 | 说明 |
|------|------|
| type | ArtNet / DMX512 / NPU |
| artNetIp:port | ArtNet 目标 (可多目标 fan-out) |
| dmxPortName | DMX512 串口名 (COM3, /dev/ttyUSB0) |
| filterUniverses | 空=全部, 否则只转发指定 Universe |
| enabled | 可动态启停 |

### 6.3 命令行示例

```bash
# 录同时转发给另一台 ArtNet 解码器
artnet_recorder -o show.artrec -u "0,1,2,3" --artnet-forward 192.168.1.100

# 多目标 + Universe 过滤
artnet_recorder -o show.artrec -u "0,1,2,3" \
    --artnet-forward 192.168.1.100 \
    --artnet-forward 192.168.1.101:6455 \
    --artnet-forward-univ "0,1"

# DMX512 串口驱动
artnet_recorder -o show.artrec -u "0,1,2" --dmx-port COM3

# 全部一起
artnet_recorder -o show.artrec -s video -m show.mp4 -u "0,1,2,3" \
    --artnet-forward 192.168.1.200 \
    --dmx-port COM3
```

---

## 7. 时间码同步

### 7.1 三种同步模式

| 模式 | 说明 | 来源 |
|------|------|------|
| InternalClock | 内部系统时钟 | QElapsedTimer |
| VideoFile | 视频文件帧时间戳 | FFmpeg `frame->pts * time_base` |
| AudioFile | 音频文件 LTC | 预留 |

### 7.2 SMPTE 时间码格式

24fps / 25fps / 29.97fps (drop-frame) / 30fps

---

## 8. 录制主循环 (简化)

```
onPreviewTick()  (每帧周期触发)
  │
  ├─ calculateTargetFrameIndex()
  │     ├─ Video 模式: m_decoder->decodeNextFrame() → pts → targetFrame
  │     └─ Internal 模式: (now - start) / interval → targetFrame
  │
  ├─ while (lastFrame < targetFrame):
  │     ├─ 从 m_universeCache 取最新 DMX 数据
  │     ├─ 检测变化 (与 m_prevUniverseSnapshot 比较)
  │     ├─ m_writer->writeFrame(...)
  │     └─ 如有转发 → m_sink.sendFrame(...)
  │
  └─ 周期状态输出
```

---

## 9. 关键设计决策

### 9.1 协议抽象层 (v1.2)
- **IProtocolSource 继承 QObject**, 因为需要 Qt 信号槽 (`frameReceived`)
- **IProtocolSink 不继承 QObject**, 纯虚接口, 上层直接调 `sendFrame()`
- **谁解析帧头**: Source 内部负责, LightFrame 已经是协议无关的数据
- **为什么包装现有 ArtNetReceiver**: 不重写底层, 只是加一层适配器

### 9.2 DMX512 BREAK 发送 (v1.1)
- **Windows**: `EscapeCommFunction(h, SETBREAK/CLRBREAK)` 硬件 BREAK
- **Linux**: `tcsendbreak(fd, 0)` POSIX BREAK  
- **回退**: 写 0x00 + 延时模拟
- 不同 USB-DMX 设备对 BREAK 支持程度不同

### 9.3 Universe 编码
- ArtNet: NetSubUni (7+4+4 bit)
- NPU: 0-255 原始编号
- LightFrame: **两种都存** (`universe` ArtNet编码 + `universeRaw` 原始号)

---

## 10. 命令行参数总览

### 基础录制
| 参数 | 说明 | 默认 |
|------|------|------|
| `-o --output` | 输出 .artrec 文件 | 必需 |
| `-m --mode` | record / verify | record |
| `-s --sync` | internal / video / audio | internal |
| `--media` | 媒体文件路径 | - |
| `-u --universe` | Universe 列表, 逗号分隔 | 0,1,2,3 |
| `-f --fps` | 帧率 (24/25/29.97/30/60) | 30 |
| `-d --duration` | 最大秒数, 0=不限 | 0 |
| `-p --port` | ArtNet 监听端口 | 6454 |

### 协议 (v1.2)
| 参数 | 说明 | 默认 |
|------|------|------|
| `--input-protocol` | 输入协议: artnet / npu / dmx512 | artnet |
| `--input-port` | 输入端口 (NPU 用 9999) | artnet端口 |
| `--input-serial` | DMX512 输入串口 | - |

### 转发 (v1.1)
| 参数 | 说明 | 默认 |
|------|------|------|
| `--artnet-forward IP[:PORT]` | ArtNet 转发目标 (可多次) | - |
| `--artnet-forward-univ` | 转发过滤 Universe | 全部 |
| `--dmx-port` | DMX512 输出串口 | - |
| `--dmx-baud` | DMX512 波特率 | 250000 |
| `--dmx-universe` | DMX512 输出 Universe | 第一个 |

---

## 11. 后续扩展

| 方向 | 状态 | 说明 |
|------|------|------|
| sACN (E1.31) | 预留 | Source/Sink 接口已就绪, 实现直接继承 |
| RDM 完整支持 | 部分 | ArtNetReceiver 已嗅探 RDM, 录制器只标记 hasRdm |
| MIDI 时间码 (MTC) | 待实现 | FFmpeg + 或独立 midi 库 |
| LTC 音频时间码 | 待实现 | 从音频文件解析 LTC 时间码 |
| 多 Sink 并发优化 | 可扩展 | 目前同步 fan-out, 高频下可改异步队列 |
| 多 Universe DMX512 | 可扩展 | 多串口或单串口多路 (需要硬件) |
| QML/Qt Widgets GUI | 待实现 | 基于 CLI 功能之上层 GUI |

---

## 12. 参考标准

| 标准 | 说明 |
|------|------|
| Art-Net v1.4/4 | UDP/IP 灯光控制网络协议 |
| DMX512 ANSI E1.11 | 512 通道串行数字传输 |
| RDM ANSI E1.20 | 远程设备管理 |
| MA NPU / Ma-Net2 | MA Lighting 专有网络协议 (UDP 9999) |
| SMPTE ST 12M | 时间码标准 |
