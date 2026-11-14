// MediaPlayerPanel.cpp — 音视频播放器 + 音频波形
//
// 播放: Qt5 Multimedia (QMediaPlayer + QVideoWidget)
// 波形: QAudioDecoder 全文件解码, 按块取峰值后归一化绘制
// 时码: 按设定帧率把播放位置换算成 SMPTE HH:MM:SS:FF
#include "MediaPlayerPanel.h"
#include "AudioWaveformView.h"

#include <QMediaPlayer>
#include <QVideoWidget>
#include <QAudioDecoder>
#include <QAudioBuffer>
#include <QAudioFormat>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QUrl>
#include <QResizeEvent>
#include <QtMath>
#include <QFile>
#include <QDataStream>
#include <cstring>

#ifndef NO_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}
#endif

// ------------------------------------------------------------
// 内置 WAV 解析 — 直接读 PCM 数据算波形峰值
// (LTC 音频时码通常就是 WAV; 不依赖 Qt 的音频解码插件)
// ------------------------------------------------------------
static quint32 readU32(const char *p) {
    return quint32(uchar(p[0])) | (quint32(uchar(p[1])) << 8)
         | (quint32(uchar(p[2])) << 16) | (quint32(uchar(p[3])) << 24);
}
static quint16 readU16(const char *p) {
    return quint16(uchar(p[0])) | (quint16(uchar(p[1])) << 8);
}

static QVector<float> readWavPeaks(const QString &path, int maxPeaks, QString *error) {
    QVector<float> peaks;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QObject::tr("无法打开文件: %1").arg(f.errorString());
        return peaks;
    }
    const QByteArray all = f.readAll();
    f.close();
    if (all.size() < 44 || std::memcmp(all.constData(), "RIFF", 4) != 0
        || std::memcmp(all.constData() + 8, "WAVE", 4) != 0) {
        if (error) *error = QObject::tr("不是有效的 WAV 文件");
        return peaks;
    }

    const char *d = all.constData();
    int pos = 12;
    quint16 fmtTag = 0, channels = 0, bits = 0;
    const char *dataPtr = nullptr;
    quint32 dataLen = 0;

    while (pos + 8 <= all.size()) {
        const char *cid = d + pos;
        const quint32 csz = readU32(d + pos + 4);
        const int body = pos + 8;
        if (std::memcmp(cid, "fmt ", 4) == 0 && body + 16 <= all.size()) {
            fmtTag   = readU16(d + body);
            channels = readU16(d + body + 2);
            bits     = readU16(d + body + 14);
        } else if (std::memcmp(cid, "data", 4) == 0) {
            dataPtr = d + body;
            dataLen = qMin<quint32>(csz, quint32(all.size() - body));
        }
        pos = body + int(csz) + (int(csz) & 1);
    }

    if (!dataPtr || dataLen == 0 || channels == 0 || bits == 0) {
        if (error) *error = QObject::tr("WAV 缺少 fmt/data 块");
        return peaks;
    }
    if (fmtTag != 1 && fmtTag != 3) {   // 1=PCM, 3=IEEE float
        if (error) *error = QObject::tr("WAV 编码不受支持 (仅 PCM/浮点)");
        return peaks;
    }

    const int bytesPerSmp = bits / 8;
    const int frameBytes  = bytesPerSmp * channels;
    if (frameBytes <= 0) return peaks;
    const qint64 frames = dataLen / frameBytes;
    if (frames <= 0) return peaks;

    const int buckets = qBound(1, maxPeaks, int(frames));
    peaks.reserve(buckets);
    float globalMax = 0.0f;

    for (int b = 0; b < buckets; ++b) {
        const qint64 f0 = frames * b / buckets;
        const qint64 f1 = qMax(f0 + 1, frames * (b + 1) / buckets);
        float amp = 0.0f;
        for (qint64 fr = f0; fr < f1; ++fr) {
            const char *fp = dataPtr + fr * frameBytes;
            for (int c = 0; c < channels; ++c) {
                const char *sp = fp + c * bytesPerSmp;
                float v = 0.0f;
                if (fmtTag == 3 && bits == 32) {
                    float fv = 0.0f; std::memcpy(&fv, sp, 4); v = qAbs(fv);
                } else if (bits == 16) {
                    qint16 sv = 0; std::memcpy(&sv, sp, 2); v = qAbs(int(sv)) / 32768.0f;
                } else if (bits == 8) {
                    v = qAbs(int(uchar(sp[0])) - 128) / 128.0f;
                } else if (bits == 24) {
                    qint32 sv = (qint32(quint8(sp[2])) << 16)
                              | (qint32(quint8(sp[1])) << 8) | qint32(quint8(sp[0]));
                    if (sv & 0x800000) sv -= 0x1000000;
                    v = qAbs(sv) / 8388608.0f;
                } else if (bits == 32) {
                    qint32 sv = 0; std::memcpy(&sv, sp, 4);
                    v = float(qAbs(qint64(sv)) / 2147483648.0);
                }
                amp = qMax(amp, v);
            }
        }
        peaks.append(amp);
        if (amp > globalMax) globalMax = amp;
    }

    if (globalMax > 0.0001f) {
        const float scale = 1.0f / globalMax;
        for (float &v : peaks) v = qBound(0.0f, v * scale, 1.0f);
    }
    return peaks;
}

// ------------------------------------------------------------
// FFmpeg 解码音频 → 波形峰值 (支持 mp3/aac/flac/mp4 等容器)
// ------------------------------------------------------------
#ifndef NO_FFMPEG
static float frameSampleAbs(const AVFrame *frame, int i) {
    const AVSampleFormat fmt = AVSampleFormat(frame->format);
    const int bps = av_get_bytes_per_sample(fmt);
    if (bps <= 0) return 0.0f;
    const bool planar = av_sample_fmt_is_planar(fmt);
    const int ch = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 1;
    float amp = 0.0f;
    for (int c = 0; c < ch; ++c) {
        const uint8_t *p = planar ? frame->data[c] + qint64(i) * bps
                                  : frame->data[0] + (qint64(i) * ch + c) * bps;
        float v = 0.0f;
        switch (fmt) {
        case AV_SAMPLE_FMT_U8:  case AV_SAMPLE_FMT_U8P:  v = qAbs(int(*p) - 128) / 128.0f; break;
        case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: { qint16 s = 0; std::memcpy(&s, p, 2); v = qAbs(int(s)) / 32768.0f; } break;
        case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P: { qint32 s = 0; std::memcpy(&s, p, 4); v = float(qAbs(qint64(s)) / 2147483648.0); } break;
        case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: { float s = 0; std::memcpy(&s, p, 4); v = qAbs(s); } break;
        case AV_SAMPLE_FMT_DBL: case AV_SAMPLE_FMT_DBLP: { double s = 0; std::memcpy(&s, p, 8); v = float(qAbs(s)); } break;
        default: break;
        }
        amp = qMax(amp, v);
    }
    return amp;
}

static QVector<float> readPeaksWithFFmpeg(const QString &path, int maxPeaks, QString *error) {
    QVector<float> peaks;
    AVFormatContext *fmt = nullptr;
    const QByteArray utf8 = path.toUtf8();

    if (avformat_open_input(&fmt, utf8.constData(), nullptr, nullptr) < 0) {
        if (error) *error = QObject::tr("FFmpeg 无法打开文件");
        return peaks;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        if (error) *error = QObject::tr("FFmpeg 无法读取流信息");
        return peaks;
    }
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (idx < 0) {
        avformat_close_input(&fmt);
        if (error) *error = QObject::tr("文件没有音频轨道");
        return peaks;
    }

    AVStream *st = fmt->streams[idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!codec || !ctx) {
        avformat_close_input(&fmt);
        if (error) *error = QObject::tr("找不到音频解码器");
        return peaks;
    }
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        if (error) *error = QObject::tr("无法打开音频解码器");
        return peaks;
    }

    qint64 durUs = 0;
    if (fmt->duration != AV_NOPTS_VALUE) {
        durUs = fmt->duration;
    } else if (st->duration != AV_NOPTS_VALUE) {
        durUs = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000000});
    }

    peaks.fill(0.0f, maxPeaks);
    const int sampleRate = qMax(1, ctx->sample_rate);
    const AVRational usBase{1, 1000000};

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    qint64 samplesSeen = 0;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }
        const bool sent = (avcodec_send_packet(ctx, pkt) >= 0);
        av_packet_unref(pkt);
        if (!sent) continue;

        while (avcodec_receive_frame(ctx, frame) == 0) {
            const qint64 startUs = (frame->pts != AV_NOPTS_VALUE)
                ? av_rescale_q(frame->pts, st->time_base, usBase)
                : samplesSeen * 1000000 / sampleRate;
            samplesSeen += frame->nb_samples;

            for (int i = 0; i < frame->nb_samples; ++i) {
                const qint64 tUs = startUs + qint64(i) * 1000000 / sampleRate;
                int b = (durUs > 0) ? int(tUs * maxPeaks / durUs)
                                    : int(samplesSeen * maxPeaks / qMax<qint64>(1, sampleRate));
                b = qBound(0, b, maxPeaks - 1);
                const float a = frameSampleAbs(frame, i);
                if (a > peaks[b]) peaks[b] = a;
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    float globalMax = 0.0f;
    for (float v : peaks) globalMax = qMax(globalMax, v);
    if (globalMax > 0.0001f) {
        const float scale = 1.0f / globalMax;
        for (float &v : peaks) v = qBound(0.0f, v * scale, 1.0f);
    } else {
        peaks.clear();
        if (error) *error = QObject::tr("音频轨道为空");
    }
    return peaks;
}
#endif // NO_FFMPEG

MediaPlayerPanel::MediaPlayerPanel(QWidget *parent) : QWidget(parent) {
    buildUi();
}

MediaPlayerPanel::~MediaPlayerPanel() {
    if (m_decoder) m_decoder->stop();
    if (m_player)  m_player->stop();
}

// ------------------------------------------------------------
// UI
// ------------------------------------------------------------
void MediaPlayerPanel::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    // --- 第一行: 打开 / 文件名 / 播放控制 ---
    auto *row1 = new QHBoxLayout;
    row1->setSpacing(8);

    m_btnOpen = new QPushButton(tr("打开媒体..."), this);
    m_btnOpen->setProperty("action", "icon");
    m_lblFile = new QLabel(tr("未选择文件"), this);
    m_lblFile->setStyleSheet("color:#8fb3d9; font-family:Consolas; font-size:11px;");
    m_lblFile->setMinimumWidth(80);

    m_btnPlay = new QPushButton(tr("▶ 播放"), this);
    m_btnPlay->setProperty("action", "start");
    m_btnStop = new QPushButton(tr("■ 停止"), this);
    m_btnStop->setProperty("action", "stop");

    row1->addWidget(m_btnOpen);
    row1->addWidget(m_lblFile, 1);
    row1->addWidget(m_btnPlay);
    row1->addWidget(m_btnStop);
    root->addLayout(row1);

    // --- 视频画面 (纯音频时隐藏; 画面窗口延迟到检测到视频轨时创建,
    //     避免无视频文件时创建原生子窗口) ---
    m_videoHolder = new QWidget(this);
    {
        auto *vl = new QVBoxLayout(m_videoHolder);
        vl->setContentsMargins(0, 0, 0, 0);
        vl->setAlignment(Qt::AlignHCenter);   // 16:9 画面水平居中
    }
    m_videoHolder->setVisible(false);
    m_videoHolder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    root->addWidget(m_videoHolder, 0);

    // --- 音频波形 (由页面放入独立的"音频波形"卡片, 见 waveformView()) ---
    m_wave = new AudioWaveformView(this);

    // --- 第三行: 时码 + 进度条 + 总长 ---
    auto *row3 = new QHBoxLayout;
    row3->setSpacing(10);

    m_lblPos = new QLabel(tr("00:00:00:00"), this);
    m_lblPos->setStyleSheet("color:#7df0ff; font-family:Consolas; font-size:13px; font-weight:bold;");
    m_lblDur = new QLabel(tr("总长 00:00:00:00"), this);
    m_lblDur->setStyleSheet("color:#7f96b0; font-family:Consolas; font-size:11px;");

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 1000);
    m_slider->setValue(0);

    row3->addWidget(m_lblPos);
    row3->addWidget(m_slider, 1);
    row3->addWidget(m_lblDur);
    root->addLayout(row3);

    // ------------------------------------------------------------
    // QMediaPlayer
    // ------------------------------------------------------------
    m_player = new QMediaPlayer(this);

    connect(m_btnOpen, &QPushButton::clicked, this, [this]{
        const QString path = QFileDialog::getOpenFileName(
            this, tr("选择音频/视频文件"), QString(),
            tr("媒体文件 (*.wav *.mp3 *.flac *.aac *.m4a *.ogg *.mp4 *.mov *.mkv *.avi *.webm);;所有文件 (*.*)"));
        if (!path.isEmpty()) openFile(path);
    });
    connect(m_btnPlay, &QPushButton::clicked, this, [this]{
        if (isPlaying()) pause(); else play();
    });
    connect(m_btnStop, &QPushButton::clicked, this, [this]{
        stop();
        seekTo(0);
    });

    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 pos){
        m_wave->setPositionMs(pos);
        if (!m_slider->isSliderDown() && durationMs() > 0)
            m_slider->setValue(int(pos * 1000 / durationMs()));
        updateTimeLabels();
        emit positionChanged(pos);
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 dur){
        m_wave->setDurationMs(dur);
        updateTimeLabels();
    });
    connect(m_player, &QMediaPlayer::stateChanged, this, [this](QMediaPlayer::State st){
        m_btnPlay->setText(st == QMediaPlayer::PlayingState ? tr("⏸ 暂停") : tr("▶ 播放"));
    });
    connect(m_player, &QMediaPlayer::videoAvailableChanged, this, [this](bool ok){
        m_hasVideo = ok;
        if (ok && !m_video) {
            // 检测到视频轨才创建画面窗口
            m_video = new QVideoWidget(m_videoHolder);
            m_video->setMinimumHeight(120);
            m_video->setAspectRatioMode(Qt::KeepAspectRatio);   // 保持画面原始比例, 不拉伸
            m_video->setStyleSheet("background:#050d15; border:1px solid #1e2d3d;");
            m_player->setVideoOutput(m_video);
            if (auto *vl = qobject_cast<QBoxLayout*>(m_videoHolder->layout()))
                vl->addWidget(m_video, 0, Qt::AlignHCenter);
        }
        if (m_videoHolder->layout()) m_videoHolder->layout()->invalidate();
        m_videoHolder->setVisible(ok && m_video != nullptr);
        if (ok) updateVideoGeometry();
    });
    connect(m_player, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), this,
            [this](QMediaPlayer::Error err){
                if (err != QMediaPlayer::NoError)
                    m_wave->setHintText(tr("无法播放该文件: %1").arg(m_player->errorString()));
            });

    connect(m_slider, &QSlider::sliderMoved, this, [this](int v){
        if (durationMs() > 0) seekTo(qint64(durationMs()) * v / 1000);
    });
    connect(m_wave, &AudioWaveformView::seekRequested, this, [this](qint64 ms){ seekTo(ms); });

    // ------------------------------------------------------------
    // QAudioDecoder — 全文件解码, 生成波形峰值
    // ------------------------------------------------------------
    m_decoder = new QAudioDecoder(this);
    connect(m_decoder, &QAudioDecoder::bufferReady, this, [this]{ onDecoderBuffer(); });
    connect(m_decoder, &QAudioDecoder::finished, this, [this]{ onDecoderFinished(); });
    connect(m_decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), this,
            [this](QAudioDecoder::Error){
                m_wave->clearPeaks();
                // 当前 Qt 只有 DirectShow 后端, 多数格式没有波形解码服务 → 提示改用 WAV
                m_wave->setHintText(tr("该格式暂不支持波形解析 (需要 Qt 音频解码插件), WAV 文件可直接显示波形"));
            });
}

// ------------------------------------------------------------
// 文件 / 播放控制
// ------------------------------------------------------------
bool MediaPlayerPanel::openFile(const QString &path) {
    if (path.isEmpty()) return false;

    m_player->stop();
    m_filePath = path;
    m_player->setMedia(QUrl::fromLocalFile(path));

    const QFileInfo fi(path);
    m_lblFile->setText(fi.fileName());
    m_lblFile->setToolTip(path);

    m_wave->setDurationMs(0);
    m_wave->setPositionMs(0);
    m_slider->setValue(0);
    updateTimeLabels();

    startWaveformDecode(path);
    emit fileOpened(path);
    emit mediaInfoChanged(m_fps, m_hasVideo);
    return true;
}

void MediaPlayerPanel::play() {
    if (!m_player) return;
    if (m_player->mediaStatus() == QMediaPlayer::NoMedia && !m_filePath.isEmpty())
        m_player->setMedia(QUrl::fromLocalFile(m_filePath));
    m_player->play();
}

void MediaPlayerPanel::pause() {
    if (m_player) m_player->pause();
}

void MediaPlayerPanel::stop() {
    if (m_player) m_player->stop();
}

bool MediaPlayerPanel::isPlaying() const {
    return m_player && m_player->state() == QMediaPlayer::PlayingState;
}

qint64 MediaPlayerPanel::positionMs() const {
    return m_player ? m_player->position() : 0;
}

qint64 MediaPlayerPanel::durationMs() const {
    return m_player ? m_player->duration() : 0;
}

void MediaPlayerPanel::seekTo(qint64 ms) {
    if (!m_player) return;
    m_player->setPosition(qMax<qint64>(0, ms));
    m_wave->setPositionMs(ms);
    updateTimeLabels();
}

void MediaPlayerPanel::setTimecodeFrameRate(int fps) {
    m_fps = qBound(1, fps, 120);
    updateTimeLabels();
    emit mediaInfoChanged(m_fps, m_hasVideo);
}

// ------------------------------------------------------------
// 波形解码
// ------------------------------------------------------------
void MediaPlayerPanel::startWaveformDecode(const QString &path) {
    m_peaks.clear();
    m_peakMax = 0.0f;
    m_wave->clearPeaks();

    // WAV 直接内置解析 (LTC 音频时码的标准容器, 不依赖解码插件)
    if (path.endsWith(".wav", Qt::CaseInsensitive)) {
        QString err;
        const QVector<float> peaks = readWavPeaks(path, 2000, &err);
        if (!peaks.isEmpty()) {
            m_peaks = peaks;
            m_peakMax = 1.0f;
            m_wave->setPeaks(m_peaks);
            return;
        }
        m_wave->setHintText(tr("无法解析音频波形: %1").arg(err));
        return;
    }

    // 其他格式: 优先用 FFmpeg 解码 (mp3/aac/flac/mp4 等)
#ifndef NO_FFMPEG
    {
        QString err;
        const QVector<float> peaks = readPeaksWithFFmpeg(path, 2000, &err);
        if (!peaks.isEmpty()) {
            m_peaks = peaks;
            m_peakMax = 1.0f;
            m_wave->setPeaks(m_peaks);
            return;
        }
        if (!err.isEmpty()) {
            m_wave->setHintText(tr("无法解析音频波形: %1").arg(err));
            return;
        }
    }
#endif

    // 兜底: Qt 音频解码器
    m_wave->setHintText(tr("正在解析音频波形..."));
    m_decoder->stop();
    m_decoder->setSourceFilename(path);
    m_decoder->start();
}

void MediaPlayerPanel::onDecoderBuffer() {
    if (!m_decoder) return;
    while (m_decoder->bufferAvailable()) {
        const QAudioBuffer buf = m_decoder->read();
        if (!buf.isValid()) break;

        const QAudioFormat fmt = buf.format();
        const int channels     = qMax(1, fmt.channelCount());
        const int frames       = buf.frameCount();
        const int bytesPerSmp  = fmt.sampleSize() / 8;
        if (frames <= 0 || bytesPerSmp <= 0) continue;

        const uchar *base = buf.constData<uchar>();
        const int step = bytesPerSmp * channels;
        const int chunk = qMax(1, frames / 96);   // 每个 buffer 约 96 个峰值

        for (int f0 = 0; f0 < frames; f0 += chunk) {
            const int f1 = qMin(frames, f0 + chunk);
            float amp = 0.0f;
            for (int f = f0; f < f1; ++f) {
                const uchar *fp = base + qint64(f) * step;
                for (int c = 0; c < channels; ++c) {
                    const uchar *sp = fp + c * bytesPerSmp;
                    float v = 0.0f;
                    if (fmt.sampleType() == QAudioFormat::Float && bytesPerSmp == 4) {
                        float fv = 0.0f;
                        std::memcpy(&fv, sp, 4);
                        v = qAbs(fv);
                    } else if (fmt.sampleType() == QAudioFormat::SignedInt && bytesPerSmp == 2) {
                        qint16 sv = 0;
                        std::memcpy(&sv, sp, 2);
                        v = qAbs(int(sv)) / 32768.0f;
                    } else if (fmt.sampleType() == QAudioFormat::SignedInt && bytesPerSmp == 4) {
                        qint32 sv = 0;
                        std::memcpy(&sv, sp, 4);
                        v = float(qAbs(qint64(sv)) / 2147483648.0);
                    } else if (fmt.sampleType() == QAudioFormat::UnSignedInt && bytesPerSmp == 1) {
                        v = qAbs(int(sp[0]) - 128) / 128.0f;
                    } else if (fmt.sampleType() == QAudioFormat::UnSignedInt && bytesPerSmp == 2) {
                        quint16 sv = 0;
                        std::memcpy(&sv, sp, 2);
                        v = qAbs(int(sv) - 32768) / 32768.0f;
                    }
                    amp = qMax(amp, v);
                }
            }
            m_peaks.append(amp);
            if (amp > m_peakMax) m_peakMax = amp;
        }
    }
}

void MediaPlayerPanel::onDecoderFinished() {
    if (m_peaks.isEmpty()) {
        m_wave->setHintText(tr("该文件没有可解析的音频轨道"));
        return;
    }
    // 归一化, 让波形占满显示高度
    if (m_peakMax > 0.001f) {
        const float scale = 1.0f / m_peakMax;
        for (float &v : m_peaks) v = qBound(0.0f, v * scale, 1.0f);
    }
    m_wave->setPeaks(m_peaks);
}

// ------------------------------------------------------------
// 视频区按 16:9 定高 (宽度变化时跟随), 避免画面被拉扁
// ------------------------------------------------------------
void MediaPlayerPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateVideoGeometry();
}

// 视频画面严格 16:9: 按可用宽度算高度, 再按高度反算宽度并水平居中
void MediaPlayerPanel::updateVideoGeometry() {
    if (!m_video || !m_videoHolder || !m_videoHolder->isVisible()) return;

    const int availW = qMax(240, width());
    const int maxByHeight = qMax(140, height() - 90);      // 预留按钮行/进度行
    int h = qBound(140, availW * 9 / 16, qMin(380, maxByHeight));
    int w = h * 16 / 9;
    if (w > availW) { w = availW; h = w * 9 / 16; }

    m_video->setFixedSize(w, h);        // 容器与画面都保持 16:9
    m_videoHolder->setFixedHeight(h);
}

// ------------------------------------------------------------
// 时码显示
// ------------------------------------------------------------
QString MediaPlayerPanel::formatTimecode(qint64 ms, int fps) {
    if (ms < 0) ms = 0;
    const int rate = qBound(1, fps, 120);
    const qint64 totalFrames = ms * rate / 1000;
    const int ff = int(totalFrames % rate);
    const qint64 totalSec = totalFrames / rate;
    const int ss = int(totalSec % 60);
    const int mm = int((totalSec / 60) % 60);
    const int hh = int(totalSec / 3600);
    return QString("%1:%2:%3:%4")
        .arg(hh, 2, 10, QChar('0'))
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}

void MediaPlayerPanel::updateTimeLabels() {
    if (!m_lblPos || !m_lblDur) return;
    m_lblPos->setText(formatTimecode(positionMs(), m_fps));
    m_lblDur->setText(tr("总长 %1").arg(formatTimecode(durationMs(), m_fps)));
}
