// ArtRecReader.h - 录制文件读取器 (用于回放和验证)
#ifndef ARTREC_READER_H
#define ARTREC_READER_H

#include "ArtRecTypes.h"
#include <QObject>
#include <QFile>
#include <QByteArray>
#include <QMap>
#include <QList>

class ArtRecReader {
public:
    ArtRecReader();
    ~ArtRecReader();
    
    bool open(const QString &filePath);
    void close();
    
    bool isValid() const { return m_initialized && m_header.isValid(); }
    
    const ArtRecHeader& header() const { return m_header; }
    const QList<UniverseEntry>& universes() const { return m_universes; }
    
    /**
     * @brief 读取下一帧
     * @param frameIndex 输出帧序号
     * @param timestampUs 输出时间戳
     * @param universeData 输出Universe数据 (key=universe index)
     * @return 成功返回true, 文件结束返回false
     */
    bool readNextFrame(qint64 &frameIndex, qint64 &timestampUs,
                       QMap<int, QByteArray> &universeData);
    
    /**
     * @brief 定位到指定帧
     */
    bool seekToFrame(qint64 frameIndex);
    
    /**
     * @brief 获取关联的媒体文件路径
     */
    QString mediaFilePath() const;
    
    qint64 currentFrame() const { return m_currentFrame; }
    
private:
    ArtRecHeader m_header;
    QList<UniverseEntry> m_universes;
    mutable QFile m_file;
    bool m_initialized = false;
    qint64 m_currentFrame = -1;
    
    bool readFrameRecord(FrameRecordHeader &frameHeader,
                         QMap<int, QByteArray> &universeData);
};

#endif // ARTREC_READER_H
