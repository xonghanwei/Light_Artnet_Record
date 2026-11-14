// FFmpegDecoder.cpp
#include "FFmpegDecoder.h"
#include "../core/Logger.h"
#include <cmath>

FFmpegDecoder::FFmpegDecoder() {
#ifndef NO_FFMPEG
    // 初始化 FFmpeg (新版不需要 av_register_all)
    avformat_network_init();
#endif
}

FFmpegDecoder::~FFmpegDecoder() {
    close();
#ifndef NO_FFMPEG
    avformat_network_deinit();
#endif
}

bool FFmpegDecoder::open(const QString &filePath, bool openVideo, bool openAudio) {
    close();
    m_filePath = filePath;
    
#ifdef NO_FFMPEG
    LOG_ERROR("编译时未包含 FFmpeg 支持");
    return false;
#else
    const char *cPath = filePath.toUtf8().constData();
    
    if (avformat_open_input(&m_formatCtx, cPath, nullptr, nullptr) != 0) {
        LOG_ERROR(QString("无法打开媒体文件: %1").arg(filePath));
        return false;
    }
    
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        LOG_ERROR(QString("无法获取媒体流信息: %1").arg(filePath));
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
        return false;
    }
    
    // 查找视频流
    if (openVideo) {
        int videoIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, 
                                            nullptr, 0);
        if (videoIndex >= 0) {
            m_videoStreamIndex = videoIndex;
            
            AVStream *stream = m_formatCtx->streams[videoIndex];
            const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!codec) {
                LOG_WARN(QString("无法找到视频解码器: codec_id=%1")
                    .arg(stream->codecpar->codec_id));
                m_videoStreamIndex = -1;
            } else {
                m_videoCodecCtx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(m_videoCodecCtx, stream->codecpar);
                
                if (avcodec_open2(m_videoCodecCtx, codec, nullptr) < 0) {
                    LOG_WARN("无法打开视频解码器");
                    avcodec_free_context(&m_videoCodecCtx);
                    m_videoCodecCtx = nullptr;
                    m_videoStreamIndex = -1;
                } else {
                    m_videoFrame = av_frame_alloc();
                }
            }
        }
    }
    
    // 查找音频流
    if (openAudio) {
        int audioIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1,
                                            nullptr, 0);
        if (audioIndex >= 0) {
            m_audioStreamIndex = audioIndex;
            
            AVStream *stream = m_formatCtx->streams[audioIndex];
            const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!codec) {
                LOG_WARN(QString("无法找到音频解码器: codec_id=%1")
                    .arg(stream->codecpar->codec_id));
                m_audioStreamIndex = -1;
            } else {
                m_audioCodecCtx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(m_audioCodecCtx, stream->codecpar);
                
                if (avcodec_open2(m_audioCodecCtx, codec, nullptr) < 0) {
                    LOG_WARN("无法打开音频解码器");
                    avcodec_free_context(&m_audioCodecCtx);
                    m_audioCodecCtx = nullptr;
                    m_audioStreamIndex = -1;
                } else {
                    m_audioFrame = av_frame_alloc();
                }
            }
        }
    }
    
    m_opened = true;
    
    LOG_INFO(QString("媒体文件已打开: %1").arg(filePath));
    if (m_videoStreamIndex >= 0) {
        LOG_INFO(QString("  视频流: index=%1, fps=%2, duration=%3s")
            .arg(m_videoStreamIndex)
            .arg(videoFrameRateDouble(), 0, 'f', 3)
            .arg(totalDurationUs() / 1000000.0, 0, 'f', 3));
    }
    if (m_audioStreamIndex >= 0) {
        LOG_INFO(QString("  音频流: index=%1")
            .arg(m_audioStreamIndex));
    }
    
    return true;
#endif
}

void FFmpegDecoder::close() {
#ifdef NO_FFMPEG
    m_opened = false;
    return;
#else
    if (m_videoFrame) {
        av_frame_free(&m_videoFrame);
        m_videoFrame = nullptr;
    }
    if (m_audioFrame) {
        av_frame_free(&m_audioFrame);
        m_audioFrame = nullptr;
    }
    
    if (m_videoCodecCtx) {
        avcodec_free_context(&m_videoCodecCtx);
        m_videoCodecCtx = nullptr;
    }
    if (m_audioCodecCtx) {
        avcodec_free_context(&m_audioCodecCtx);
        m_audioCodecCtx = nullptr;
    }
    
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }
    
    m_videoStreamIndex = -1;
    m_audioStreamIndex = -1;
    m_lastVideoTimestampUs = 0;
    m_opened = false;
#endif
}

qint64 FFmpegDecoder::totalDurationUs() const {
#ifdef NO_FFMPEG
    return 0;
#else
    if (!m_formatCtx) return 0;
    
    // 优先使用视频流时长
    if (m_videoStreamIndex >= 0) {
        AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
        if (stream->duration != AV_NOPTS_VALUE) {
            return convertTimestampToUs(stream->duration, stream->time_base);
        }
    }
    
    // 使用容器时长
    if (m_formatCtx->duration != AV_NOPTS_VALUE) {
        return m_formatCtx->duration / 1000; // AV_TIME_BASE = 1000000
    }
    
    return 0;
#endif
}

int FFmpegDecoder::videoFrameRate() const {
    return int(std::round(videoFrameRateDouble()));
}

double FFmpegDecoder::videoFrameRateDouble() const {
#ifdef NO_FFMPEG
    return 30.0;
#else
    if (!m_formatCtx || m_videoStreamIndex < 0) return 30.0;
    
    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    // 使用 r_frame_rate (实际帧率)
    if (stream->r_frame_rate.den > 0) {
        return double(stream->r_frame_rate.num) / double(stream->r_frame_rate.den);
    }
    // 回退: 使用 avg_frame_rate
    if (stream->avg_frame_rate.den > 0) {
        return double(stream->avg_frame_rate.num) / double(stream->avg_frame_rate.den);
    }
    
    return 30.0;
#endif
}

qint64 FFmpegDecoder::currentPositionUs() const {
#ifdef NO_FFMPEG
    return 0;
#else
    return m_lastVideoTimestampUs;
#endif
}

qint64 FFmpegDecoder::decodeNextFrame(int streamType) {
#ifdef NO_FFMPEG
    return -1;
#else
    if (!m_opened || !m_formatCtx) return -1;
    
    AVPacket *packet = av_packet_alloc();
    qint64 result = -1;
    
    int targetStreamIndex = (streamType == 0) ? m_videoStreamIndex : m_audioStreamIndex;
    
    AVCodecContext *codecCtx = (streamType == 0) ? m_videoCodecCtx : m_audioCodecCtx;
    AVFrame *frame = (streamType == 0) ? m_videoFrame : m_audioFrame;

    if (!codecCtx || !frame) {
        av_packet_free(&packet);
        return -1;
    }

    while (av_read_frame(m_formatCtx, packet) >= 0) {
        if (packet->stream_index != targetStreamIndex) {
            av_packet_unref(packet);
            continue;
        }

        const int sent = avcodec_send_packet(codecCtx, packet);
        av_packet_unref(packet);
        if (sent < 0) continue;

        // 解码器可能暂时没有输出 (B 帧/需要更多包), 继续读下一包而不是当作 EOF
        if (avcodec_receive_frame(codecCtx, frame) == 0) {
            AVStream *stream = m_formatCtx->streams[targetStreamIndex];
            const qint64 pts = frame->pts;
            if (pts != AV_NOPTS_VALUE) {
                result = convertTimestampToUs(pts, stream->time_base);
                if (streamType == 0) m_lastVideoTimestampUs = result;
            }
            break;
        }
    }
    
    av_packet_free(&packet);
    return result;
#endif
}

qint64 FFmpegDecoder::getVideoFrameTimestampUs() {
    return decodeNextFrame(0);
}

bool FFmpegDecoder::seek(qint64 timeUs) {
#ifdef NO_FFMPEG
    return false;
#else
    if (!m_opened || !m_formatCtx) return false;
    
    int64_t timestamp = timeUs; // 微秒
    AVRational timeBase = {1, 1000000};
    
    int result = avformat_seek_file(m_formatCtx, m_videoStreamIndex, 
                                    INT64_MIN, timestamp, INT64_MAX, AVSEEK_FLAG_ANY);
    
    if (result >= 0) {
        if (m_videoCodecCtx) avcodec_flush_buffers(m_videoCodecCtx);
        if (m_audioCodecCtx) avcodec_flush_buffers(m_audioCodecCtx);
        m_lastVideoTimestampUs = timeUs;
        return true;
    }
    
    return false;
#endif
}

#ifndef NO_FFMPEG
qint64 FFmpegDecoder::convertTimestampToUs(int64_t pts, const AVRational &timeBase) const {
    if (pts == AV_NOPTS_VALUE) return 0;
    // pts * timeBase.num / timeBase.den * 1000000 (微秒)
    return qint64(double(pts) * double(timeBase.num) / double(timeBase.den) * 1000000.0 + 0.5);
}
#endif
