// ArtRecStorage.h — .artrec 二进制格式 IStorageFormat 实现
//
// 内部组合 ArtRecWriter + ArtRecReader (保持原有稳定实现),
// 对外通过 StorageTypes DTO 暴露统一接口.
//
#ifndef ARTREC_STORAGE_H
#define ARTREC_STORAGE_H

#include "IStorageFormat.h"
#include "StorageTypes.h"

// 前置声明, 避免头文件互相 include
class ArtRecWriter;
class ArtRecReader;

class ArtRecStorage : public IStorageFormat {
    Q_OBJECT
public:
    explicit ArtRecStorage(QObject *parent = nullptr);
    ~ArtRecStorage() override;

    QString formatName() const override { return QStringLiteral("artrec"); }
    QString fileExtension() const override { return QStringLiteral("artrec"); }

    // ===== 写入 =====
    bool create(const QString &filePath,
                const StorageMeta &meta,
                const QList<StorageUniverse> &universes) override;
    bool writeFrame(const StorageFrame &frame) override;
    bool finalize(quint32 totalFrames,
                  quint64 totalDurationUs,
                  const QString &mediaFilePath = QString()) override;
    void close() override;

    // ===== 读取 =====
    bool open(const QString &filePath) override;
    bool readNextFrame(StorageFrame &frame) override;
    bool seekToFrame(qint64 frameIndex) override;

    // ===== 共同 =====
    bool isValid() const override;
    const StorageMeta &meta() const override { return m_meta; }
    const QList<StorageUniverse> &universes() const override { return m_universes; }

private:
    // DTO ↔ ArtRecTypes 转换
    struct ArtRecHeader  toArtRecHeader(const StorageMeta &meta) const;
    struct UniverseEntry toUniverseEntry(const StorageUniverse &u) const;

    ArtRecWriter *m_writer = nullptr;
    ArtRecReader *m_reader = nullptr;
    StorageMeta m_meta;
    QList<StorageUniverse> m_universes;
    QString m_filePath;
};

#endif // ARTREC_STORAGE_H
