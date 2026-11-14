// ArtRecTypes.h - 录制文件格式相关数据结构
#ifndef ARTREC_TYPES_H
#define ARTREC_TYPES_H

#include "../core/Common.h"
#include <cstdint>
#include <cstring>

#pragma pack(push, 1)

// 文件魔数
const char ARTREC_MAGIC[8] = {'A', 'R', 'T', 'R', 'E', 'C', '\0', '\0'};

// 当前格式版本
const quint16 ARTREC_VERSION = 0x0100;

// ArtRec 文件头 (固定64字节)
struct ArtRecHeader {
    char     magic[8];              // "ARTREC\0\0"
    quint16  version;               // 格式版本
    quint16  frameRate;             // 帧率: 24,25,2997(29.97),30,60
    quint8   timeCodeType;          // 时间码类型 (TimeCodeType)
    quint8   timeCodeFormat;        // SMPTE格式 (TimeCodeFormat)
    quint16  universeCount;         // Universe数量
    quint32  totalFrames;           // 总帧数
    quint64  totalDurationUs;       // 总时长(微秒)
    quint64  createTimestamp;       // 文件创建时间(Unix毫秒)
    quint32  mediaFileOffset;       // 关联媒体文件名在尾部的偏移
    quint32  mediaFileLen;          // 关联媒体文件名长度
    quint8   reserved[20];          // 保留字段 (使总大小精确 64 字节)
    
    ArtRecHeader() {
        reset();
    }
    
    void reset() {
        std::memset(this, 0, sizeof(ArtRecHeader));
        std::memcpy(magic, ARTREC_MAGIC, 8);
        version = ARTREC_VERSION;
    }
    
    bool isValid() const {
        return std::memcmp(magic, ARTREC_MAGIC, 8) == 0;
    }
    
    static constexpr int size() { return 64; }
};

// Universe 映射表条目 (4字节)
struct UniverseEntry {
    quint8 net;              // Net (0-127)
    quint8 subNet;           // SubNet (0-15)
    quint8 universe;         // Universe (0-15)
    quint8 channelCount;     // 通道数 (0 表示 512)
    
    UniverseEntry() : net(0), subNet(0), universe(0), channelCount(0) {}
    UniverseEntry(quint16 encodedUniverse, quint16 channels = 512) {
        net = quint8((encodedUniverse >> 8) & 0x7F);
        subNet = quint8((encodedUniverse >> 4) & 0x0F);
        universe = quint8(encodedUniverse & 0x0F);
        channelCount = (channels == 512) ? 0 : quint8(channels & 0xFF);
    }
    
    quint16 encoded() const {
        return (quint16(net & 0x7F) << 8) | 
               (quint16(subNet & 0x0F) << 4) | 
               (quint16(universe & 0x0F));
    }
    
    quint16 realChannelCount() const {
        return channelCount == 0 ? 512 : channelCount;
    }
    
    static constexpr int size() { return 4; }
};

// 帧记录头 (固定15字节)
struct FrameRecordHeader {
    quint32 frameIndex;       // 帧序号 (从0开始)
    quint64 timestampUs;      // 时间戳(微秒)
    quint8  flags;            // 标志位
    quint16 blockCount;       // 数据块数量
    
    // flags 位定义
    static constexpr quint8 FLAG_DATA_CHANGED = 0x01;  // DMX数据有变化
    static constexpr quint8 FLAG_HAS_RDM      = 0x02;  // 包含RDM数据
    static constexpr quint8 FLAG_TC_LOST      = 0x04;  // 时间码丢失
    
    FrameRecordHeader() : frameIndex(0), timestampUs(0), flags(0), blockCount(0) {}
    
    static constexpr int size() { return 15; }
};

// Universe 数据块头 (固定4字节)
struct UniverseDataBlockHeader {
    quint8 universeIndex;    // 对应 UniverseEntry 索引
    quint8 dataType;         // 数据类型
    quint16 dataLength;      // 数据长度
    
    // dataType 定义
    static constexpr quint8 DATA_DMX       = 0;   // DMX 512通道数据
    static constexpr quint8 DATA_RDM       = 1;   // RDM 原始包
    static constexpr quint8 DATA_ARTNET    = 2;   // 完整 ArtNet 原始包
    static constexpr quint8 DATA_DMX_DELTA = 3;   // DMX 变化量 (只记录变化的通道)
    
    UniverseDataBlockHeader() : universeIndex(0), dataType(DATA_DMX), dataLength(0) {}
    
    static constexpr int size() { return 4; }
};

#pragma pack(pop)

#endif // ARTREC_TYPES_H
