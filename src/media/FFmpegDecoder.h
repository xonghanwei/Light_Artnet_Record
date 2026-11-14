// FFmpegDecoder.h - FFmpeg 媒体解码封装
#ifndef FFMPEG_DECODER_H
#define FFMPEG_DECODER_H

#include "../core/Common.h"
#include <QString>
#include <QObject>
#include <functional>

#ifdef NO_FFMPEG
// 无 FFmpeg 支持时，提供空实现
#else
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>
}
#endif

class FFmpegDecoder {
public:
    FFmpegDecoder();
    ~FFmpegDecoder();
    
    /**
     * @brief 打开媒体文件
     * @param filePath 文件路径
     * @param openVideo 是否打开视频流
     * @param openAudio 是否打开音频流
     * @return 成功返回true
     */
    bool open(const QString &filePath, bool openVideo = true, bool openAudio = false);
    
    /**
     * @brief 关闭文件并释放资源
     */
    void close();
    
    /**
     * @brief 是否已打开
     */
    bool isOpen() const { return m_opened; }
    
    /**
     * @brief 获取媒体总时长 (微秒)
     */
    qint64 totalDurationUs() const;
    
    /**
     * @brief 获取视频帧率 (整数)
     */
    int videoFrameRate() const;
    
    /**
     * @brief 获取精确帧率 (double)
     */
    double videoFrameRateDouble() const;
    
    /**
     * @brief 是否为视频文件
     */
#ifndef NO_FFMPEG
    bool hasVideo() const { return m_videoStreamIndex >= 0; }
    
    /**
     * @brief 是否有音频
     */
    bool hasAudio() const { return m_audioStreamIndex >= 0; }
#else
    bool hasVideo() const { return false; }
    bool hasAudio() const { return false; }
#endif
    
    /**
     * @brief 获取媒体文件名
     */
    QString filePath() const { return m_filePath; }
    
    /**
     * @brief 获取当前位置 (微秒)
     */
    qint64 currentPositionUs() const;
    
    /**
     * @brief 解码到下一帧，返回帧的时间戳 (微秒)
     * @param streamType 0=视频, 1=音频
     * @return 时间戳(微秒), 失败或EOF返回-1
     */
    qint64 decodeNextFrame(int streamType = 0);
    
    /**
     * @brief 获取当前视频帧时间戳 (微秒)
     */
    qint64 getVideoFrameTimestampUs();
    
    /**
     * @brief 定位到指定时间位置
     * @param timeUs 目标时间 (微秒)
     */
    bool seek(qint64 timeUs);

private:
    bool m_opened = false;
    QString m_filePath;
    
#ifndef NO_FFMPEG
    AVFormatContext *m_formatCtx = nullptr;
    
    AVCodecContext  *m_videoCodecCtx = nullptr;
    AVCodecContext  *m_audioCodecCtx = nullptr;
    AVFrame         *m_videoFrame = nullptr;
    AVFrame         *m_audioFrame = nullptr;
    
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;
    
    qint64 m_lastVideoTimestampUs = 0;
#endif
    
    // 将 FFmpeg 时间戳转换为微秒
#ifndef NO_FFMPEG
    qint64 convertTimestampToUs(int64_t pts, const AVRational &timeBase) const;
#endif
};

#endif // FFMPEG_DECODER_H
