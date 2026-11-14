// ProtocolTypes.h - 协议无关的统一数据结构
//
// 设计原则 (来自跨芯片协议重构经验):
//   - LightFrame 是"谁负责解析帧头"的边界: Source 负责从各协议包中提取出
//     协议无关的 LightFrame, Sink 负责把 LightFrame 组装成各协议包
//   - 上层 (RecorderEngine/PlayerEngine) 只操作 LightFrame, 不感知底层协议
//   - 每个协议 Source/Sink 自己处理帧头/CRC/端口/序列等协议细节
#ifndef PROTOCOL_TYPES_H
#define PROTOCOL_TYPES_H

#include "../core/Common.h"
#include <QByteArray>
#include <QString>
#include <QList>
#include <QMetaType>
#include <cstdint>
#include <cstring>

// ============ 协议类型枚举 ============
enum class ProtocolType {
    ArtNet,         // Art-Net 标准 (UDP 6454)
    Npu,            // MA NPU / Ma-Net2 (UDP 9999)
    Dmx512,         // DMX512 串口 (RS-485 / USB-DMX)
    ArtRecFile,     // 离线录制文件重演 (.artrec)
    Sacn,           // sACN E1.31 (预留)
    OSC,            // OSC 协议 (预留)
    Unknown
};

inline QString protocolTypeToString(ProtocolType t) {
    switch(t) {
        case ProtocolType::ArtNet:     return "ArtNet";
        case ProtocolType::Npu:        return "NPU (Ma-Net2)";
        case ProtocolType::Dmx512:     return "DMX512";
        case ProtocolType::ArtRecFile: return "ArtRec File (离线录制)";
        case ProtocolType::Sacn:       return "sACN";
        case ProtocolType::OSC:        return "OSC";
        case ProtocolType::Unknown:    return "Unknown";
    }
    return "Unknown";
}

// ============ 统一帧结构 (协议无关) ============

/**
 * @brief 协议无关的 DMX 帧数据
 * 
 * 任何协议 (ArtNet/NPU/DMX512/sACN) 解析后都输出 LightFrame, 
 * 任何 Sink 接收 LightFrame 后组装成各自协议的包。
 */
struct LightFrame {
    ProtocolType sourceProtocol = ProtocolType::Unknown;
    
    quint16 universe = 0;       // Universe 编号 (ArtNet用编码值, NPU用原始编号)
    quint8  universeRaw = 0;    // 原始 Universe 编号 (NPU 0-255, ArtNet 低4bit)
    
    QByteArray dmxData;         // DMX 通道数据, 最多 512 字节
    
    qint64  timestampUs = 0;   // 接收/捕获时间戳 (微秒)
    quint32 sequence = 0;       // 序列号 (来自原协议)
    
    // RDM 数据 (可选)
    bool    hasRdm = false;
    QByteArray rdmPayload;
    
    // 辅助: 实际通道数 (512 或更少)
    int channelCount() const { return dmxData.size(); }
    
    // 获取完整 512 通道数据 (不足补 0)
    QByteArray paddedDmx() const {
        if (dmxData.size() >= ArtNet::DMX_CHANNEL_COUNT)
            return dmxData.left(ArtNet::DMX_CHANNEL_COUNT);
        QByteArray padded = dmxData;
        padded.append(QByteArray(ArtNet::DMX_CHANNEL_COUNT - dmxData.size(), 0));
        return padded;
    }
};

// ============ 连接参数 ============

/**
 * @brief 协议 Source/Sink 的通用连接参数
 */
struct ConnectionParams {
    ProtocolType protocol = ProtocolType::Unknown;
    
    // 网络类 (ArtNet/NPU/sACN)
    QString localBindAddress = "0.0.0.0";  // 接收时本地绑定
    quint16 localPort = 0;                 // 接收端口
    
    // Sink 目标 (网络类)
    QString remoteAddress;                 // 目标 IP (或 "255.255.255.255" 广播)
    quint16 remotePort = 0;                // 目标端口
    
    // Sink 多目标 fan-out
    // (保持简单, 高级 fan-out 由上层 OutputManager 管理)
    
    // 串口类 (DMX512)
    QString serialPortName;                // "COM3" / "/dev/ttyUSB0"
    qint32  serialBaudRate = 250000;
    
    // 文件类 (ArtRecFile)
    QString filePath;                      // 录制文件路径 (.artrec / .json / .xml)
    qreal   playbackSpeed = 1.0;           // 重演速度倍率 (1.0 = 原始速度)
    bool    loopPlayback = false;           // 是否循环重演
    
    // Universe 过滤 (空=全部)
    QList<quint16> filterUniverses;
};

// ============ MA NPU (Ma-Net2) 协议 ============
/**
 * NPU 是 MA lighting 的网络协议, 端口 9999/UDP
 * 常用 OpCode:
 *   0x01 = DMX 数据包 (1 + 512 字节)
 *   0x02 = Universe 数据请求 / 应答
 *   0x03 = 时间码
 *   0x10 = MA 专用命令
 * 
 * 简单 DMX 包格式:
 *   [2 bytes length LE] [1 byte cmd=0x01] [1 byte universe] [512 bytes DMX]
 */
namespace NpuProtocol {
    constexpr quint16 DEFAULT_PORT = 9999;
    constexpr quint8 OP_DMX = 0x01;
    constexpr quint8 OP_UNIVERSE_INFO = 0x02;
    constexpr quint8 OP_TIMECODE = 0x03;
    constexpr quint8 OP_MA_COMMAND = 0x10;
    
#pragma pack(push, 1)
    struct NpuDmxPacket {
        quint16 length;     // 小端, 固定 = 514 (cmd + universe + 512)
        quint8  cmd;        // 0x01
        quint8  universe;   // 0-255
        quint8  dmxData[512];
    };
#pragma pack(pop)
    
    constexpr int DMX_PACKET_SIZE = 2 + 1 + 1 + 512; // 516 bytes
}

// ============ Qt Meta-Type 注册 ============
// LightFrame 通过信号槽传递, 必须注册后才能用 QSignalSpy / queued connection
Q_DECLARE_METATYPE(LightFrame)

#endif // PROTOCOL_TYPES_H
