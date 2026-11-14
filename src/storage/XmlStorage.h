// XmlStorage.h — .xml 通用格式 IStorageFormat 实现
//
// XML 格式:
// <recording version="1">
//   <meta formatVersion="256" frameRate="30" totalFrames="100" ... />
//   <universes>
//     <universe encodedUniverse="1" channelCount="512" />
//   </universes>
//   <frames>
//     <frame index="0" timestampUs="0" dataChanged="true">
//       <universeData index="0">AAN//w==</universeData>  (base64)
//     </frame>
//   </frames>
// </recording>
//
#ifndef XML_STORAGE_H
#define XML_STORAGE_H

#include "IStorageFormat.h"
#include <QDomDocument>
#include <QDomElement>

class XmlStorage : public IStorageFormat {
    Q_OBJECT
public:
    explicit XmlStorage(QObject *parent = nullptr);
    ~XmlStorage() override;

    QString formatName() const override { return QStringLiteral("xml"); }
    QString fileExtension() const override { return QStringLiteral("xml"); }

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
    QDomDocument m_doc;
    QDomElement  m_root;      // <recording>
    QDomElement  m_framesEl;  // <frames>
    QString      m_filePath;
    bool         m_valid = false;
    QList<QDomElement> m_frameNodes;  // 预展开便于随机读取
    int          m_readCursor = -1;
};

#endif // XML_STORAGE_H
