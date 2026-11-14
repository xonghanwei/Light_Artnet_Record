// IStorageFormat.h — 存储格式抽象接口
//
// 所有文件存储格式的抽象基类:
//   - ArtRecStorage  → .artrec 二进制 (原有格式迁移)
//   - JsonStorage    → .json 通用格式
//   - XmlStorage     → .xml 通用格式
//   - 未来可扩展: CSV, Parquet, SQLite...
//
// 统一接口让上层 RecorderEngine 完全不关心底层文件格式.
//
#ifndef ISTORAGE_FORMAT_H
#define ISTORAGE_FORMAT_H

#include "StorageTypes.h"
#include <QString>
#include <QList>
#include <QObject>

class IStorageFormat : public QObject {
    Q_OBJECT
public:
    explicit IStorageFormat(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~IStorageFormat() = default;

    // 格式标识, 如 "artrec" / "json" / "xml"
    virtual QString formatName() const = 0;

    // 文件扩展名 (不含点), 如 "artrec" / "json" / "xml"
    virtual QString fileExtension() const = 0;

    // ============== 写入 ==============

    /** 创建新的录制文件 (写入文件头 + universe 表) */
    virtual bool create(const QString &filePath,
                        const StorageMeta &meta,
                        const QList<StorageUniverse> &universes) = 0;

    /** 写入一帧 */
    virtual bool writeFrame(const StorageFrame &frame) = 0;

    /** 最终化: 统计信息写回 + 关闭 */
    virtual bool finalize(quint32 totalFrames,
                          quint64 totalDurationUs,
                          const QString &mediaFilePath = QString()) = 0;

    /** 关闭文件 */
    virtual void close() = 0;

    bool isWritable() const { return m_openedForWrite; }

    // ============== 读取 ==============

    /** 打开已有文件 (只读) */
    virtual bool open(const QString &filePath) = 0;

    /** 读取下一帧, 返回 false 表示文件结束或错误 */
    virtual bool readNextFrame(StorageFrame &frame) = 0;

    /** 定位到指定帧 */
    virtual bool seekToFrame(qint64 frameIndex) = 0;

    bool isReadable() const { return m_openedForRead; }

    // ============== 共同访问器 ==============

    virtual bool isValid() const = 0;
    virtual const StorageMeta &meta() const = 0;
    virtual const QList<StorageUniverse> &universes() const = 0;

signals:
    void errorOccurred(const QString &message);

protected:
    bool m_openedForWrite = false;
    bool m_openedForRead  = false;
};

#endif // ISTORAGE_FORMAT_H
