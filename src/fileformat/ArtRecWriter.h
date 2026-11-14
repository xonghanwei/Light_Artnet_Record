// ArtRecWriter.h - 录制文件写入器
#ifndef ARTREC_WRITER_H
#define ARTREC_WRITER_H

#include "ArtRecTypes.h"
#include <QObject>
#include <QFile>
#include <QByteArray>
#include <QMap>
#include <QList>
#include <QMutex>

class ArtRecWriter {
public:
    ArtRecWriter();
    ~ArtRecWriter();
    
    /**
     * @brief 创建新的录制文件
     * @param filePath 输出文件路径
     * @param header 文件头信息
     * @param universes Universe 映射表
     * @return 成功返回 true
     */
    bool create(const QString &filePath, const ArtRecHeader &header,
                const QList<UniverseEntry> &universes);
    
    /**
     * @brief 写入一帧记录
     * @param frameIndex 帧序号
     * @param timestampUs 时间戳(微秒)
     * @param universeData 各Universe的DMX数据 (key=encoded universe)
     * @param dataChanged 本帧数据是否与上一帧不同
     * @return 成功返回 true
     */
    bool writeFrame(qint64 frameIndex, qint64 timestampUs,
                    const QMap<quint16, QByteArray> &universeData,
                    bool dataChanged);
    
    /**
     * @brief 写入带RDM数据的帧
     */
    bool writeFrameWithRDM(qint64 frameIndex, qint64 timestampUs,
                           const QMap<quint16, QByteArray> &universeData,
                           const QByteArray &rdmData);
    
    /**
     * @brief 完成写入并回写头部统计信息
     * @param totalFrames 总帧数
     * @param totalDurationUs 总时长(微秒)
     * @param mediaFilePath 关联的媒体文件路径 (可选)
     */
    bool finalize(qint64 totalFrames, qint64 totalDurationUs, 
                  const QString &mediaFilePath = QString());
    
    /**
     * @brief 关闭文件
     */
    void close();
    
    /**
     * @brief 检查是否已打开
     */
    bool isOpen() const { return m_initialized && m_file.isOpen(); }
    
    /**
     * @brief 获取当前写入位置
     */
    qint64 pos() const { return m_file.pos(); }
    
    /**
     * @brief 获取文件头引用
     */
    const ArtRecHeader& header() const { return m_header; }
    
    /**
     * @brief 获取Universe映射表
     */
    const QList<UniverseEntry>& universes() const { return m_universes; }
    
private:
    bool writeRaw(const void *data, qint64 length);
    bool writeRaw(const QByteArray &data);
    
    ArtRecHeader m_header;
    QList<UniverseEntry> m_universes;
    QFile m_file;
    bool m_initialized = false;
    QMutex m_mutex;
};

#endif // ARTREC_WRITER_H
