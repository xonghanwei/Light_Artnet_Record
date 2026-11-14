// JsonStorage.h — .json 通用格式 IStorageFormat 实现
//
// JSON 格式:
// {
//   "version": 1,
//   "meta": { "formatVersion": 256, "frameRate": 30, ... },
//   "universes": [ { "encodedUniverse": 1, "channelCount": 512 }, ... ],
//   "frames": [
//     { "frameIndex": 0, "timestampUs": 0, "dataChanged": true,
//       "universeData": { "0": [255,0,0,...], "1": [...] } }
//   ]
// }
//
#ifndef JSON_STORAGE_H
#define JSON_STORAGE_H

#include "IStorageFormat.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

class JsonStorage : public IStorageFormat {
    Q_OBJECT
public:
    explicit JsonStorage(QObject *parent = nullptr);
    ~JsonStorage() override;

    QString formatName() const override { return QStringLiteral("json"); }
    QString fileExtension() const override { return QStringLiteral("json"); }

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
    bool isValid() const override { return m_valid; }
    const StorageMeta &meta() const override { return m_meta; }
    const QList<StorageUniverse> &universes() const override { return m_universes; }

private:
    StorageMeta m_meta;
    QList<StorageUniverse> m_universes;
    QJsonDocument m_doc;            // 整个 JSON 树 (读写都持有)
    QJsonObject   m_root;
    QJsonArray    m_framesArray;    // 引用 root["frames"]
    QString       m_filePath;
    bool          m_valid = false;
    int           m_readCursor = -1; // 读位置
};

#endif // JSON_STORAGE_H
