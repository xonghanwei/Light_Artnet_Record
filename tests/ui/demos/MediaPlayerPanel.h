// MediaPlayerPanel.h — 音视频播放器 (视频画面 + 音频波形 + SMPTE 时码)
#ifndef MEDIA_PLAYER_PANEL_H
#define MEDIA_PLAYER_PANEL_H

#include <QWidget>
#include <QVector>
#include <QString>

class QMediaPlayer;
class QVideoWidget;
class QAudioDecoder;
class QPushButton;
class QLabel;
class QSlider;
class AudioWaveformView;

class MediaPlayerPanel : public QWidget {
    Q_OBJECT
public:
    explicit MediaPlayerPanel(QWidget *parent = nullptr);
    ~MediaPlayerPanel() override;

    /** 打开音视频文件 (同时解析音频波形) */
    bool openFile(const QString &path);
    QString filePath() const { return m_filePath; }

    void play();
    void pause();
    void stop();
    bool isPlaying() const;

    qint64 positionMs() const;
    qint64 durationMs() const;
    void seekTo(qint64 ms);

    /** 时码显示帧率 (0 = 用当前实际帧率取整) */
    void setTimecodeFrameRate(int fps);

    /** 波形视图 (由页面放到独立卡片中显示) */
    AudioWaveformView *waveformView() const { return m_wave; }

    int frameRate() const { return m_fps; }
    bool hasVideo() const { return m_hasVideo; }

signals:
    /** 播放器内新打开的媒体文件 */
    void fileOpened(const QString &path);
    /** 播放位置变化 (毫秒) */
    void positionChanged(qint64 ms);
    /** 打开媒体后: 帧率 / 是否含视频轨 */
    void mediaInfoChanged(int fps, bool hasVideo);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUi();
    /** 视频画面严格按 16:9 计算尺寸并水平居中 */
    void updateVideoGeometry();
    void startWaveformDecode(const QString &path);
    void onDecoderBuffer();
    void onDecoderFinished();
    void updateTimeLabels();
    static QString formatTimecode(qint64 ms, int fps);

    QMediaPlayer *m_player = nullptr;
    QVideoWidget *m_video = nullptr;
    QWidget *m_videoHolder = nullptr;
    QAudioDecoder *m_decoder = nullptr;
    AudioWaveformView *m_wave = nullptr;

    QPushButton *m_btnOpen = nullptr;
    QPushButton *m_btnPlay = nullptr;
    QPushButton *m_btnStop = nullptr;
    QLabel *m_lblFile = nullptr;
    QLabel *m_lblPos = nullptr;
    QLabel *m_lblDur = nullptr;
    QSlider *m_slider = nullptr;

    QString m_filePath;
    QVector<float> m_peaks;
    float m_peakMax = 0.0f;
    int m_fps = 30;
    bool m_hasVideo = false;
};

#endif // MEDIA_PLAYER_PANEL_H
