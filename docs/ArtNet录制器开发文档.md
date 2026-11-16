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

MA Lighting 的主从架构协议 (grandMA2/3 控台推送 DMX)。传输为单播/广播推送，接收端不加组播组。

**DMX 数据包格式** (整包 516 字节, `NpuProtocol::DMX_PACKET_SIZE = 2 + 1 + 1 + 512`):
```
偏移  长度  字段
0     2     length (小端, = 514, 即 cmd+universe+512B 数据的负载字节数, 不含本 2 字节头)
2     1     cmd (0x01 = DMX 数据)
3     1     universe (0-255, 原始编号)
4     512   DMX 通道数据
```
> 校验口径: 接收端 `NpuSource` 仅校验整包长度 ≥ 516，**不校验** length 字段值；发送端 `NpuSink` 在 length 字段写 514。

**OpCode 表** (`src/protocol/ProtocolTypes.h` — `NpuProtocol` 命名空间):

| cmd | 常量 | 含义 |
|-----|------|------|
| 0x01 | `OP_DMX` | DMX 数据包 |
| 0x02 | `OP_UNIVERSE_INFO` | Universe 信息 / 请求 |
| 0x03 | `OP_TIMECODE` | 时间码 |
| 0x10 | `OP_MA_COMMAND` | MA 专用命令 |

非 0x01 的包接收端静默忽略。

**关键参数**:

| 项 | 值 |
|---|---|
| 默认端口 | 9999 (`NpuProtocol::DEFAULT_PORT`) |
| Universe 范围 | 0–255 |
| 原始编号 | 存于 `LightFrame::universeRaw`，协议转换路由 (如 NPU→DMX 桥接) 必须用它 |
| ArtNet 兼容编码 | `LightFrame::universe = universeRaw & 0x0F` (Net=0, SubNet=0)，仅供统一帧结构使用，不适合直接路由 |

### 3.3 DMX512 / RS-485 (串口)

**协议帧**: BREAK(≥92μs) + MAB(≥8μs) + Start Code(0x00) + 512 channels (整帧 513 字节)  
**串口参数**: 250000 baud, 8 data bits, 2 stop bits, No parity (8N2)  
**电气**: RS-485 差分半双工; 本项目只做发送器 (TX) 与只读接收器 (RX), 无 DE/RE 方向控制, 不支持 RDM 反向应答

#### 3.3.1 发送实现 (`src/output/Dmx512Sender.cpp`)

**串口参数校验** (`open()`): 必须先 `open(QIODevice::WriteOnly)` 再下发 250000 8N2 —— 端口未打开时 `setBaudRate` 只缓存参数且恒返回 true, 驱动是否支持要到真正下发时才暴露; 五项设置逐一检查返回值并回读 `baudRate()` 比对, 任一失败即关闭端口、发 `portError` 信号并返回 false。250000 为非标准波特率, 部分 CH340/PL2303 驱动不支持, 若不校验会静默按错误波特率发帧 (现象为灯具乱闪/无响应)。

```cpp
// Dmx512Sender::open() — 先 open 再下发参数, 回读校验 (L79-L96)
if (!m_serial->open(QIODevice::WriteOnly)) { /* 报错 + portError, 返回 false */ }

const bool paramsOk = m_serial->setBaudRate(baudRate)
    && m_serial->setDataBits(QSerialPort::Data8)
    && m_serial->setParity(QSerialPort::NoParity)
    && m_serial->setStopBits(QSerialPort::TwoStop)
    && m_serial->setFlowControl(QSerialPort::NoFlowControl);
if (!paramsOk || m_serial->baudRate() != baudRate) {
    closeLocked();                       // 驱动不支持 250000/8N2 → 拒绝启动
    emit portError(...);                 // 锁释放后再 emit, 防槽重入死锁
    return false;
}
```

**微秒级延时** (`preciseDelayUs()`): Windows `Sleep`/`usleep` 受系统定时器精度 (~15.6ms) 限制, 无法满足 BREAK 100μs 时序, 必须用高分辨率单调时钟忙等 —— Windows `QueryPerformanceCounter`, POSIX `clock_gettime(CLOCK_MONOTONIC)`。

```cpp
// Dmx512Sender.cpp L18-L38
static inline void preciseDelayUs(int us)
{
#ifdef Q_OS_WIN
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    const LONGLONG target = static_cast<LONGLONG>(freq.QuadPart * (static_cast<double>(us) / 1000000.0));
    do { QueryPerformanceCounter(&now); } while (now.QuadPart - start.QuadPart < target);
#else
    struct timespec start{}, now{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long ns = static_cast<long>(us) * 1000L;
    do { clock_gettime(CLOCK_MONOTONIC, &now); }
    while ((now.tv_sec - start.tv_sec) * 1000000000L + (now.tv_nsec - start.tv_nsec) < ns);
#endif
}
```

**平台延时计算原理**:

本质是同一公式: `目标计数 = 时钟频率 × 延迟时长`, 循环直到 `当前计数 - 起始计数 ≥ 目标计数`。两平台差异在时钟源:

| 平台 | 时钟源 | 频率 | 换算 | 注意事项 |
|---|---|---|---|---|
| Windows | `QueryPerformanceCounter` | **不固定** (现代机典型 10MHz=100ns/tick, 少数为 CPU 主频) | `target = freq × us / 1e6`, 必须先 `QueryPerformanceFrequency` 动态获取, 不能假设 1 tick=100ns | 频率 static 缓存一次 |
| Linux/macOS | `clock_gettime` | 固定纳秒分辨率 (1e9 Hz) | `ns = us × 1000`, 无需查频率 | 必须用 `CLOCK_MONOTONIC`: `CLOCK_REALTIME` 遇 NTP 校时/手动回拨时差值变负, 循环挂死或提前退出; 32 位 `long` 下 `(秒差)×1e9` 要求秒差 ≤2.1s, 对 μs 级延时足够 |

为什么必须忙等而不是 `Sleep`/`usleep`:

| 机制 | 最小粒度 | 请求 100μs 实际耗时 |
|---|---|---|
| `Sleep(1)` / `usleep(100)` | 系统定时器 ~15.6ms (未调 `timeBeginPeriod` 时) | 1~16ms, BREAK 波形失真 10~160 倍 |
| 高精度时钟忙等 | tick 级 (~0.1μs / ns) | ≈100μs |

sleep 语义是"至少 X 后唤醒", 唤醒还要过调度器, 抖动毫秒级; DMX BREAK 规范下限 92μs, 只有忙等能保证。代价: 每帧自旋 115μs (BREAK 100 + MAB 15), 44Hz 满帧 CPU 占用 ≈0.5%, 可接受。

**三种 BREAK 发送方式** (按优先级自动选择):

| # | 方式 | 平台 | 实现 | 时序 |
|---|------|------|------|------|
| 1 | 硬件 BREAK | Windows | `EscapeCommFunction(handle, SETBREAK)` → 拉低 TX | SETBREAK → 100μs → CLRBREAK → 15μs MAB |
| 2 | 硬件 BREAK | Linux/macOS | `ioctl(fd, TIOCSBRK)` / `TIOCCBRK` | 同上 |
| 3 | 软件模拟 (回退) | 全平台 | 写 2 字节 `0x00` + `flush()` + 100μs | 非连续低电平, 见局限 |

- 方式 1/2 检查 API 返回值; 失败 (部分驱动不实现 break 控制, 如旧版 ch341) 则置 `m_hwBreakFailed` 记住状态并**永久降级**到方式 3, 避免每帧重复探测; `open()` 成功时重置该状态 (换设备后重新探测)。
- 方式 2 不用 `tcsendbreak(fd, 0)`: POSIX 规定 duration=0 时 BREAK 固定约 0.25s, 超规范 ~2700 倍, 每帧卡死 250ms。
- 方式 3 的局限: 8N2 下每个 0x00 字节含停止位间隙, 线路是"低-高-低"而非连续 ≥92μs 低电平, 且接收端会把这两个 0x00 解析成 Start Code + 通道 1 数据而非 BREAK。仅部分接收器可容忍, 只在拿不到原生句柄/驱动不支持时兜底。
- 探测顺序: 每帧先试硬件方式, `hwBreakSent == false` 才走软件回退, 随后统一发送 `buildDmxFrame()` 产物 (Start Code 0x00 + 512 通道, 不足补 0)。

**方式 1 — Windows 硬件 BREAK** (`sendFrame()` L178-L192):

```cpp
HANDLE handle = m_serial->handle();            // QSerialPort 原生句柄
if (handle != INVALID_HANDLE_VALUE) {
    const BOOL setOk  = EscapeCommFunction(handle, SETBREAK);  // TX 强制拉低
    preciseDelayUs(100);                                       // BREAK ≥92μs
    const BOOL clrOk = EscapeCommFunction(handle, CLRBREAK);   // 释放回空闲高电平
    preciseDelayUs(15);                                        // MAB ≥8μs
    hwBreakSent = setOk && clrOk;
    if (!hwBreakSent && !m_hwBreakFailed) {    // 无论 SETBREAK 结果都尝试了 CLRBREAK,
        m_hwBreakFailed = true;                // 防线路卡死在 break 态; 记住失败, 后续帧不再探测
    }
}
```

**方式 2 — Linux/macOS 硬件 BREAK** (`sendFrame()` L194-L208):

```cpp
const int fd = static_cast<int>(m_serial->handle());
const int setOk = ioctl(fd, TIOCSBRK);         // TX 强制拉低 (驱动实现, 非 tcsendbreak!)
preciseDelayUs(100);
const int clrOk = ioctl(fd, TIOCCBRK);         // 释放
preciseDelayUs(15);
hwBreakSent = (setOk == 0 && clrOk == 0);
if (!hwBreakSent && !m_hwBreakFailed) { m_hwBreakFailed = true; }
```

> 弃用 `tcsendbreak(fd, 0)` 的原因: POSIX 规定 duration=0 时 BREAK 固定约 0.25s (超规范 ~2700 倍), 每帧卡 250ms。

**方式 3 — 软件模拟回退** (`sendFrame()` L211-L221, 仅当硬件方式不可用):

```cpp
if (!hwBreakSent) {
    // 2 字节 0x00 @250k ≈ 88μs 低电平; 8N2 下含停止位间隙, 非连续低, 部分接收器可容忍
    const qint64 brk = m_serial->write("\x00\x00", 2);
    const bool brkFlushed = m_serial->flush();
    preciseDelayUs(100);                       // 近似 BREAK
    preciseDelayUs(15);                        // 近似 MAB
    if (brk != 2 || !brkFlushed) { /* portError, 返回 false */ }
}
```

**帧体发送** (三种方式共用, `sendFrame()` L232-L241):

```cpp
const QByteArray frame = buildDmxFrame(data);  // 0x00 + 512 通道 (不足补 0)
const qint64 written = m_serial->write(frame);
if (written != frame.size() || !m_serial->flush()) {
    // 适配器运行中被拔出等: 不静默计入 m_framesSent
    emit portError(m_serial->errorString());   // 锁释放后 emit
    return false;
}
m_framesSent++;
```

**发送流程** (`sendFrame()`): 全程持 `m_mutex` (open/close 同锁, 防并发 delete); `write` 校验整帧写入量, `flush` 校验返回值 (适配器被拔出时不静默计入 `m_framesSent`)。`open/close` 的信号统一在**释放锁后** emit, 防槽函数回调造成非递归锁死锁。

**时序约束**: 513 字节 @250000 (11 bit/字节) 线上时间 ≈ 22.6ms, 帧率上限 ~44Hz (符合 DMX 标准); 上层调用间隔小于 22.6ms 时, 下一帧的 SETBREAK 会截断上一帧尾部 —— DMX 接收器以 BREAK 复位可容忍。

#### 3.3.2 接收实现 (`src/protocol/sources/Dmx512Source.cpp`)

- 以 `QIODevice::ReadOnly` 打开, 参数校验与发送侧同一规则 (先 open 后下发 + 回读比对)。
- 异步接收: `readyRead` 信号 → `readAll()` 追加进 `m_readBuffer`, 串口层可能把一帧拆成多次读, 需缓冲区对齐。
- 帧提取: 循环取 ≥513 字节 —— 在前 10 字节内搜索 Start Code `0x00` 作对齐标记 (找不到丢弃缓冲); 命中后 `mid(startIdx)` 去头, `left(513)` 取一帧, 跳过 Start Code 取 512 通道数据。

```cpp
// Dmx512Source::processSerialData() L97-L137 — 缓冲区对齐 + Start Code 搜索
QByteArray incoming = m_serial->readAll();
m_readBuffer.append(incoming);

while (m_readBuffer.size() >= 513) {
    int startIdx = -1;                          // 前 10 字节内找 Start Code 0x00
    for (int i = 0; i < m_readBuffer.size() && i < 10; ++i) {
        if (quint8(m_readBuffer[i]) == 0x00) { startIdx = i; break; }
    }
    if (startIdx < 0) { m_readBuffer.clear(); break; }   // 无帧头, 丢弃重同步
    if (startIdx > 0) m_readBuffer = m_readBuffer.mid(startIdx);
    if (m_readBuffer.size() < 513) break;                // 半帧, 等下一次 readyRead

    QByteArray frame = m_readBuffer.left(513);           // Start Code + 512 通道
    m_readBuffer = m_readBuffer.mid(513);

    LightFrame lf;
    lf.sourceProtocol = ProtocolType::Dmx512;
    lf.universe = 0;  lf.universeRaw = 0;                // DMX512 无 Universe 概念
    lf.dmxData = frame.mid(1, 512);                      // 跳过 Start Code
    lf.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
    lf.sequence = quint32(m_framesReceived);
    m_framesReceived++;
    emit frameReceived(lf);
}
```
- 输出 `LightFrame`: `universe = universeRaw = 0` (DMX512 无 Universe 概念), 打时间戳与递增序号, emit `frameReceived`。
- 注意: 本类依赖 BREAK 做帧边界不可靠 (串口层已抹掉 BREAK 电平信息), 实际以 Start Code 搜索对齐; 连续两帧间若有噪声字节, 靠搜索窗口重新同步。

**接收硬件要求**: DMX512 线路通常单向 (控台→灯具), 接收需硬件支持 RX 方向 (Enttec Pro Mk3 等带光耦双向适配器)。

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

### 9.2 DMX512 BREAK 发送 (v1.1, v1.3 修订)
- **Windows**: `EscapeCommFunction(h, SETBREAK/CLRBREAK)` 硬件 BREAK
- **Linux/macOS**: `ioctl(fd, TIOCSBRK/TIOCCBRK)` 硬件 BREAK —— 弃用早期方案 `tcsendbreak(fd, 0)`: POSIX 规定 duration=0 时 BREAK 固定约 0.25s, 超 DMX 规范 ~2700 倍
- **回退**: 写 2 字节 0x00 + 延时模拟 (非连续低电平, 仅兜底); 硬件 API 失败自动降级并记住状态, 换串口后重新探测
- **延时**: 一律 QPC/CLOCK_MONOTONIC 忙等 (`preciseDelayUs`), 不用 Sleep/usleep (定时器精度 ~15.6ms 会破坏 100μs 时序)
- 详见 §3.3.1; 不同 USB-DMX 设备对 BREAK 支持程度不同, 真机建议示波器验证波形

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
