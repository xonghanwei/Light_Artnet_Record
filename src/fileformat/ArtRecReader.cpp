// ArtRecReader.cpp
#include "ArtRecReader.h"
#include "../core/Logger.h"
#include <cstring>

ArtRecReader::ArtRecReader() {
}

ArtRecReader::~ArtRecReader() {
    close();
}

bool ArtRecReader::open(const QString &filePath) {
    close();
    
    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        LOG_ERROR(QString("无法打开录制文件: %1 - %2")
            .arg(filePath, m_file.errorString()));
        return false;
    }
    
    // 读取文件头
    if (m_file.read(reinterpret_cast<char*>(&m_header), ArtRecHeader::size()) != ArtRecHeader::size()) {
        LOG_ERROR("读取文件头失败");
        return false;
    }
    
    if (!m_header.isValid()) {
        LOG_ERROR("文件魔数无效");
        return false;
    }
    
    // 读取 Universe 映射表
    m_universes.clear();
    for (quint16 i = 0; i < m_header.universeCount; ++i) {
        UniverseEntry entry;
        if (m_file.read(reinterpret_cast<char*>(&entry), UniverseEntry::size()) != UniverseEntry::size()) {
            LOG_ERROR("读取 Universe 映射失败");
            return false;
        }
        m_universes.append(entry);
    }
    
    m_initialized = true;
    m_currentFrame = -1;
    
    LOG_INFO(QString("已打开录制文件: %1 (Universe: %2, 帧率: %3, 总帧: %4)")
        .arg(filePath)
        .arg(m_header.universeCount)
        .arg(m_header.frameRate)
        .arg(m_header.totalFrames));
    
    return true;
}

void ArtRecReader::close() {
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_initialized = false;
    m_currentFrame = -1;
}

bool ArtRecReader::readNextFrame(qint64 &frameIndex, qint64 &timestampUs,
                                 QMap<int, QByteArray> &universeData) {
    if (!m_initialized || !m_file.isOpen()) {
        return false;
    }
    
    FrameRecordHeader frameHeader;
    if (!readFrameRecord(frameHeader, universeData)) {
        return false;
    }
    
    frameIndex = frameHeader.frameIndex;
    timestampUs = frameHeader.timestampUs;
    m_currentFrame = frameIndex;
    
    return true;
}

bool ArtRecReader::readFrameRecord(FrameRecordHeader &frameHeader,
                                   QMap<int, QByteArray> &universeData) {
    // 读取帧头
    if (m_file.read(reinterpret_cast<char*>(&frameHeader), 
                    FrameRecordHeader::size()) != FrameRecordHeader::size()) {
        return false; // EOF
    }
    
    // 读取每个数据块
    universeData.clear();
    for (quint16 i = 0; i < frameHeader.blockCount; ++i) {
        UniverseDataBlockHeader blockHeader;
        if (m_file.read(reinterpret_cast<char*>(&blockHeader), 
                        UniverseDataBlockHeader::size()) != UniverseDataBlockHeader::size()) {
            return false;
        }
        
        QByteArray data = m_file.read(blockHeader.dataLength);
        if (data.size() != blockHeader.dataLength) {
            return false;
        }
        
        // 只保存 DMX 类型的数据块
        if (blockHeader.dataType == UniverseDataBlockHeader::DATA_DMX) {
            universeData[blockHeader.universeIndex] = data;
        }
    }
    
    return true;
}

bool ArtRecReader::seekToFrame(qint64 frameIndex) {
    if (!m_initialized || frameIndex < 0) return false;
    if (frameIndex >= qint64(m_header.totalFrames)) return false;

    // 从头跳到帧区起点, 逐帧跳过 frame 0..frameIndex-1
    // 这样循环结束时文件光标正好停在 frameIndex 开头
    m_file.seek(ArtRecHeader::size() + m_header.universeCount * UniverseEntry::size());

    FrameRecordHeader frameHeader;
    QMap<int, QByteArray> universeData;

    qint64 current = -1;
    while (current + 1 < frameIndex) {
        if (!readFrameRecord(frameHeader, universeData)) {
            return false;
        }
        current = frameHeader.frameIndex;
    }

    m_currentFrame = frameIndex - 1;  // 我们跳过了 frame 0..frameIndex-1
    return true;                      // 文件光标现在正好在 frameIndex 开头
}

QString ArtRecReader::mediaFilePath() const {
    if (!m_initialized || m_header.mediaFileLen == 0) {
        return QString();
    }
    
    // 临时保存当前位置
    qint64 savedPos = m_file.pos();
    
    // 定位到媒体文件名位置
    if (m_file.seek(m_header.mediaFileOffset)) {
        QByteArray data = m_file.read(m_header.mediaFileLen);
        m_file.seek(savedPos);
        return QString::fromUtf8(data);
    }
    
    m_file.seek(savedPos);
    return QString();
}
