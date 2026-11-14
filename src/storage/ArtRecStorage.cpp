// ArtRecStorage.cpp
#include "ArtRecStorage.h"
#include "fileformat/ArtRecWriter.h"
#include "fileformat/ArtRecReader.h"
#include "fileformat/ArtRecTypes.h"

// 命名空间别名 (ArtRecTypes.h 里用了, ArtRecWriter 没显式用但 Common.h 已引入)
using namespace ArtNet;

ArtRecStorage::ArtRecStorage(QObject *parent)
    : IStorageFormat(parent)
{
    m_writer = new ArtRecWriter();
    m_reader = new ArtRecReader();
}

ArtRecStorage::~ArtRecStorage() {
    close();
    delete m_writer;
    delete m_reader;
}

struct ArtRecHeader ArtRecStorage::toArtRecHeader(const StorageMeta &m) const {
    struct ArtRecHeader h;
    h.version = m.formatVersion;
    h.frameRate = m.frameRate;
    h.universeCount = quint16(m.universeCount);
    h.totalFrames = m.totalFrames;
    h.totalDurationUs = m.totalDurationUs;
    h.createTimestamp = m.createTimestamp;
    return h;
}

struct UniverseEntry ArtRecStorage::toUniverseEntry(const StorageUniverse &u) const {
    return UniverseEntry(u.encodedUniverse, u.channelCount);
}

// ============ 写入 ============

bool ArtRecStorage::create(const QString &filePath,
                          const StorageMeta &meta,
                          const QList<StorageUniverse> &universes) {
    close();

    // DTO → 二进制类型
    struct ArtRecHeader h = toArtRecHeader(meta);
    QList<struct UniverseEntry> ueList;
    for(const auto &u : universes)
        ueList.append(toUniverseEntry(u));

    if(!m_writer->create(filePath, h, ueList)) {
        emit errorOccurred(tr("ArtRecStorage::create failed: %1").arg(m_writer->isOpen() ? QString() : QStringLiteral("writer error")));
        return false;
    }

    m_filePath = filePath;
    m_meta = meta;
    m_universes = universes;
    m_openedForWrite = true;
    return true;
}

bool ArtRecStorage::writeFrame(const StorageFrame &frame) {
    if(!m_openedForWrite || !m_writer->isOpen()) return false;

    // StorageFrame.universeData: key = universe index (0-based)
    // ArtRecWriter.writeFrame:   key = encoded universe
    QMap<quint16, QByteArray> dataByEncoded;
    for(auto it = frame.universeData.constBegin(); it != frame.universeData.constEnd(); ++it) {
        int idx = it.key();
        if(idx >= 0 && idx < m_universes.size())
            dataByEncoded[m_universes[idx].encodedUniverse] = it.value();
    }

    return m_writer->writeFrame(frame.frameIndex, frame.timestampUs,
                                dataByEncoded, frame.dataChanged);
}

bool ArtRecStorage::finalize(quint32 totalFrames,
                            quint64 totalDurationUs,
                            const QString &mediaFilePath) {
    if(!m_openedForWrite) return false;
    bool ok = m_writer->finalize(totalFrames, totalDurationUs, mediaFilePath);
    if(ok) {
        m_meta.totalFrames = totalFrames;
        m_meta.totalDurationUs = totalDurationUs;
        m_meta.mediaFilePath = mediaFilePath;
    }
    m_openedForWrite = false;
    return ok;
}

void ArtRecStorage::close() {
    if(m_openedForWrite) {
        m_writer->close();
        m_openedForWrite = false;
    }
    if(m_openedForRead) {
        m_reader->close();
        m_openedForRead = false;
    }
}

// ============ 读取 ============

bool ArtRecStorage::open(const QString &filePath) {
    close();
    if(!m_reader->open(filePath)) {
        emit errorOccurred(tr("ArtRecStorage::open failed: %1").arg(filePath));
        return false;
    }

    // 二进制 → DTO
    const struct ArtRecHeader &rh = m_reader->header();
    m_meta.formatVersion = rh.version;
    m_meta.frameRate = rh.frameRate;
    m_meta.universeCount = rh.universeCount;
    m_meta.totalFrames = rh.totalFrames;
    m_meta.totalDurationUs = rh.totalDurationUs;
    m_meta.createTimestamp = rh.createTimestamp;
    m_meta.mediaFilePath = m_reader->mediaFilePath();

    m_universes.clear();
    for(const auto &ue : m_reader->universes())
        m_universes.append(StorageUniverse(ue.encoded(), ue.realChannelCount()));

    m_filePath = filePath;
    m_openedForRead = true;
    return true;
}

bool ArtRecStorage::readNextFrame(StorageFrame &frame) {
    if(!m_openedForRead || !m_reader->isValid()) return false;

    qint64 fi, tu;
    QMap<int, QByteArray> ud;
    if(!m_reader->readNextFrame(fi, tu, ud)) return false;

    frame.frameIndex = fi;
    frame.timestampUs = tu;
    frame.universeData = ud;  // key 已就是 universe index
    frame.dataChanged = true; // ArtRecReader 没暴露, 默认 true
    return true;
}

bool ArtRecStorage::seekToFrame(qint64 frameIndex) {
    if(!m_openedForRead) return false;
    return m_reader->seekToFrame(frameIndex);
}

bool ArtRecStorage::isValid() const {
    if(m_openedForRead)  return m_reader->isValid();
    if(m_openedForWrite) return m_writer->isOpen();
    return false;
}
