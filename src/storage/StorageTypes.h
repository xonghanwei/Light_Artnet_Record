// StorageTypes.h — 存储层统一 DTO (与具体文件格式无关)
//
// 所有存储格式 (.artrec / .json / .xml / 未来扩展) 共享这些数据结构.
// 二进制格式 ArtRecTypes.h 保留给文件格式层内部使用, 不上浮到业务层.
//
#ifndef STORAGE_TYPES_H
#define STORAGE_TYPES_H

#include <QString>
#include <QList>
#include <QMap>
#include <QByteArray>
#include <QtGlobal>

// Universe 条目 (与 UniverseEntry 等价但命名更直观)
struct StorageUniverse {
    quint16 encodedUniverse = 0;  // NetSubUni 编码
    quint16 channelCount = 512;

    StorageUniverse() = default;
    StorageUniverse(quint16 enc, quint16 ch = 512)
        : encodedUniverse(enc), channelCount(ch) {}
};

// 文件头元数据 (所有格式通用)
struct StorageMeta {
    quint16 formatVersion = 0x0100;
    quint16 frameRate = 30;
    quint16 universeCount = 0;
    quint32 totalFrames = 0;
    quint64 totalDurationUs = 0;
    quint64 createTimestamp = 0;
    QString mediaFilePath;

    StorageMeta() = default;
};

// 一帧的数据
struct StorageFrame {
    qint64 frameIndex = -1;
    qint64 timestampUs = 0;
    bool   dataChanged = true;
    bool   hasRDM = false;
    // key = universe index (0-based, 对应 universes[] 下标)
    QMap<int, QByteArray> universeData;
    QByteArray rdmData;

    StorageFrame() = default;
};

#endif // STORAGE_TYPES_H
