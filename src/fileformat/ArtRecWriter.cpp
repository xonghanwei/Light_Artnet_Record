// ArtRecWriter.cpp
#include "ArtRecWriter.h"
#include "../core/Logger.h"
#include <QDateTime>
#include <cstring>

ArtRecWriter::ArtRecWriter() {
}

ArtRecWriter::~ArtRecWriter() {
    close();
}

bool ArtRecWriter::create(const QString &filePath, const ArtRecHeader &header,
                          const QList<UniverseEntry> &universes) {
    QMutexLocker locker(&m_mutex);
    
    // 之前文件可能已打开, 先关掉 (内联, 避免在已持锁时再调 close() 嵌套死锁)
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
    m_initialized = false;
    
    m_header = header;
    m_universes = universes;
    
    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::WriteOnly)) {
        LOG_ERROR(QString("无法创建录制文件: %1 - %2")
            .arg(filePath, m_file.errorString()));
        return false;
    }
    
    // 写入占位的文件头 (之后会回写)
    ArtRecHeader headerToWrite = m_header;
    headerToWrite.universeCount = quint16(universes.size());
    if (headerToWrite.createTimestamp == 0) {
        headerToWrite.createTimestamp = QDateTime::currentMSecsSinceEpoch();
    }
    
    if (!writeRaw(&headerToWrite, ArtRecHeader::size())) {
        LOG_ERROR("写入文件头失败");
        return false;
    }
    
    // 写入 Universe 映射表
    for (const auto &entry : m_universes) {
        if (!writeRaw(&entry, UniverseEntry::size())) {
            LOG_ERROR("写入 Universe 映射失败");
            return false;
        }
    }
    
    m_header = headerToWrite;
    m_initialized = true;
    
    LOG_INFO(QString("录制文件已创建: %1 (Universe数: %2)")
        .arg(filePath)
        .arg(m_universes.size()));
    
    return true;
}

bool ArtRecWriter::writeFrame(qint64 frameIndex, qint64 timestampUs,
                              const QMap<quint16, QByteArray> &universeData,
                              bool dataChanged) {
    QMutexLocker locker(&m_mutex);
    
    if (!m_initialized || !m_file.isOpen()) {
        LOG_ERROR("写入帧失败: 文件未初始化");
        return false;
    }
    
    // 构造帧记录头
    FrameRecordHeader frameHeader;
    frameHeader.frameIndex = quint32(frameIndex);
    frameHeader.timestampUs = quint64(timestampUs);
    frameHeader.flags = dataChanged ? FrameRecordHeader::FLAG_DATA_CHANGED : 0;
    frameHeader.blockCount = quint16(m_universes.size());
    
    if (!writeRaw(&frameHeader, FrameRecordHeader::size())) {
        return false;
    }
    
    // 写入每个 Universe 的数据块
    for (int i = 0; i < m_universes.size(); ++i) {
        quint16 universeKey = m_universes[i].encoded();
        const QByteArray &data = universeData.value(universeKey);
        
        // Universe 数据块头
        UniverseDataBlockHeader blockHeader;
        blockHeader.universeIndex = quint8(i);
        blockHeader.dataType = UniverseDataBlockHeader::DATA_DMX;
        
        // 确保数据长度正确 (最多512字节)
        QByteArray dmxData = data.left(ArtNet::DMX_CHANNEL_COUNT);
        if (dmxData.size() < ArtNet::DMX_CHANNEL_COUNT) {
            dmxData.append(QByteArray(ArtNet::DMX_CHANNEL_COUNT - dmxData.size(), 0));
        }
        
        blockHeader.dataLength = quint16(dmxData.size());
        
        if (!writeRaw(&blockHeader, UniverseDataBlockHeader::size())) {
            return false;
        }
        if (!writeRaw(dmxData)) {
            return false;
        }
    }
    
    return true;
}

bool ArtRecWriter::writeFrameWithRDM(qint64 frameIndex, qint64 timestampUs,
                                     const QMap<quint16, QByteArray> &universeData,
                                     const QByteArray &rdmData) {
    QMutexLocker locker(&m_mutex);
    
    if (!m_initialized || !m_file.isOpen()) {
        return false;
    }
    
    FrameRecordHeader frameHeader;
    frameHeader.frameIndex = quint32(frameIndex);
    frameHeader.timestampUs = quint64(timestampUs);
    frameHeader.flags = FrameRecordHeader::FLAG_HAS_RDM;
    if (!rdmData.isEmpty()) frameHeader.flags |= FrameRecordHeader::FLAG_DATA_CHANGED;
    frameHeader.blockCount = quint16(m_universes.size() + (rdmData.isEmpty() ? 0 : 1));
    
    if (!writeRaw(&frameHeader, FrameRecordHeader::size())) {
        return false;
    }
    
    // DMX 数据块
    for (int i = 0; i < m_universes.size(); ++i) {
        quint16 universeKey = m_universes[i].encoded();
        const QByteArray &data = universeData.value(universeKey);
        
        UniverseDataBlockHeader blockHeader;
        blockHeader.universeIndex = quint8(i);
        blockHeader.dataType = UniverseDataBlockHeader::DATA_DMX;
        
        QByteArray dmxData = data.left(ArtNet::DMX_CHANNEL_COUNT);
        if (dmxData.size() < ArtNet::DMX_CHANNEL_COUNT) {
            dmxData.append(QByteArray(ArtNet::DMX_CHANNEL_COUNT - dmxData.size(), 0));
        }
        blockHeader.dataLength = quint16(dmxData.size());
        
        if (!writeRaw(&blockHeader, UniverseDataBlockHeader::size())) return false;
        if (!writeRaw(dmxData)) return false;
    }
    
    // RDM 数据块
    if (!rdmData.isEmpty()) {
        UniverseDataBlockHeader rdmBlock;
        rdmBlock.universeIndex = 0xFF; // RDM 用特殊索引
        rdmBlock.dataType = UniverseDataBlockHeader::DATA_RDM;
        rdmBlock.dataLength = quint16(rdmData.size());
        
        if (!writeRaw(&rdmBlock, UniverseDataBlockHeader::size())) return false;
        if (!writeRaw(rdmData)) return false;
    }
    
    return true;
}

bool ArtRecWriter::finalize(qint64 totalFrames, qint64 totalDurationUs, 
                            const QString &mediaFilePath) {
    QMutexLocker locker(&m_mutex);
    
    if (!m_initialized || !m_file.isOpen()) {
        return false;
    }
    
    // 记录文件尾位置
    qint64 tailPos = m_file.pos();
    
    // 写入关联的媒体文件路径
    if (!mediaFilePath.isEmpty()) {
        QByteArray mediaPathBytes = mediaFilePath.toUtf8();
        
        // 先写入媒体文件名
        if (!writeRaw(mediaPathBytes)) {
            return false;
        }
        
        // 回写文件头中的 mediaFileOffset 和 mediaFileLen
        m_header.mediaFileOffset = quint32(tailPos);
        m_header.mediaFileLen = quint32(mediaPathBytes.size());
    }
    
    // 回写更新后的文件头 (在文件开头)
    m_header.totalFrames = quint32(totalFrames);
    m_header.totalDurationUs = quint64(totalDurationUs);
    
    if (!m_file.seek(0)) {
        LOG_ERROR("定位到文件开头失败");
        return false;
    }
    
    if (!writeRaw(&m_header, ArtRecHeader::size())) {
        return false;
    }
    
    // 回到文件尾
    if (!m_file.seek(tailPos)) {
        // 继续保持当前位置
    }
    
    m_file.flush();
    
    LOG_INFO(QString("录制文件已完成: %1 (帧数: %2, 时长: %3ms)")
        .arg(m_file.fileName())
        .arg(totalFrames)
        .arg(totalDurationUs / 1000));
    
    return true;
}

void ArtRecWriter::close() {
    QMutexLocker locker(&m_mutex);
    
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
    m_initialized = false;
}

bool ArtRecWriter::writeRaw(const void *data, qint64 length) {
    if (!m_file.isOpen()) return false;
    return m_file.write(reinterpret_cast<const char*>(data), length) == length;
}

bool ArtRecWriter::writeRaw(const QByteArray &data) {
    if (!m_file.isOpen()) return false;
    return m_file.write(data.constData(), data.size()) == data.size();
}
