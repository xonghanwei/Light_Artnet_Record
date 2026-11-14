// ArtRecFileSource.h - 离线录制文件重演 Source
//
// 把 .artrec 录制文件当作一个"虚拟协议输入端", 按原始帧率定时吐出 LightFrame。
// 用途: 把之前录制的灯光数据重新送入输出口, 做重演/测试。
//
// 设计:
//   - 内部持有 ArtRecReader, 不关心 StorageFactory 那一层
//     (Storage 层适合转换/序列化, Source 层要的是"帧数据 + 时间戳 + universe 映射")
//   - QTimer 按文件原始帧率 * playbackSpeed 定时 tick
//   - 每个 tick 读一帧, 把该帧里所有 universe 数据分别 emit 一个 LightFrame
//   - 支持 loopPlayback: 到末尾 seek(0) 重来
#ifndef ARTREC_FILE_SOURCE_H
#define ARTREC_FILE_SOURCE_H

#include "../IProtocolSource.h"
#include "fileformat/ArtRecReader.h"
#include <QTimer>
#include <memory>

/**
 * @brief 离线录制文件重演 Source
 * 
 * 用法:
 *   ConnectionParams p;
 *   p.protocol = ProtocolType::ArtRecFile;
 *   p.filePath = "recording.artrec";
 *   p.playbackSpeed = 1.0;
 *   p.loopPlayback = true;
 *   auto *src = ProtocolFactory::createSource("artrecfile");
 *   src->start(p);   // 开始按原始帧率吐帧
 */
class ArtRecFileSource : public IProtocolSource {
    Q_OBJECT
public:
    explicit ArtRecFileSource(QObject *parent = nullptr);
    ~ArtRecFileSource() override;
    
    ProtocolType protocolType() const override { return ProtocolType::ArtRecFile; }
    QString protocolName() const override { return "ArtRec File"; }
    
    bool start(const ConnectionParams &params) override;
    void stop() override;
    bool isRunning() const override { return m_timer && m_timer->isActive(); }
    
    void setFilterUniverses(const QList<quint16> &universes) override {
        m_filterUniverses = universes;
    }
    
    quint64 framesReceived() const override { return m_totalFramesEmitted; }
    
    // ---- 文件元信息 (start 后有效) ----
    quint16 fileFrameRate() const { return m_reader.header().frameRate; }
    quint32 totalFrames() const { return m_reader.header().totalFrames; }
    const QList<UniverseEntry> &fileUniverses() const { return m_reader.universes(); }
    
signals:
    /** 文件重演到末尾 (loopPlayback=false 时) */
    void playbackFinished();
    
private slots:
    void onTick();
    
private:
    ArtRecReader m_reader;
    QTimer *m_timer = nullptr;
    
    ConnectionParams m_params;
    QList<quint16> m_filterUniverses;
    
    qint64 m_totalFramesEmitted = 0;   // 累计 emit 的 LightFrame 数
    quint64 m_ticksProcessed = 0;      // 累计 tick 数 (== 读的帧数, 用于进度)
};

#endif // ARTREC_FILE_SOURCE_H
