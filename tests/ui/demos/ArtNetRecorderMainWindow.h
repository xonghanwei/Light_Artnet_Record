// ArtNetRecorderMainWindow.h — ArtNet 录制器全功能 SCADA 面板
// 左侧导航 + 右侧卡片栅格, 集成 RecorderEngine / ISyncClock / ArtNetReceiver /
// OutputManager / ArtRecReader / ProtocolFactory (Source/Sink) 等全部生产模块
#ifndef ARTNET_RECORDER_MAIN_WINDOW_H
#define ARTNET_RECORDER_MAIN_WINDOW_H

#include "../base/TestWindowBase.h"
#include "DmxChannelBars.h"

class MediaPlayerPanel;

// 生产类
#include "recorder/RecorderEngine.h"
#include "sync/ISyncClock.h"
#include "sync/SyncClockFactory.h"
#include "sync/SyncTypes.h"
#include "network/ArtNetReceiver.h"
#include "output/OutputManager.h"
#include "fileformat/ArtRecReader.h"
#include "fileformat/ArtRecWriter.h"
#include "protocol/ProtocolFactory.h"
#include "protocol/IProtocolSource.h"
#include "protocol/IProtocolSink.h"
#include "protocol/ProtocolTypes.h"

// UI 控件
#include <QListWidget>
#include <QStackedWidget>
#include <QFrame>
#include <QLabel>
#include <QSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QTableWidget>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QDateTime>
#include <QTimer>
#include <QGroupBox>
#include <QTabWidget>
#include <QMap>
#include <QHash>

class QSerialPort;
class QUdpSocket;

// 协议输入/输出页面共享的 tab 数据结构
struct ProtocolInputTab {
    ProtocolType type;
    QString key;                       // "artnet" / "npu" / "dmx512" / "sacn" / "rdm" / "artrecfile"
    IProtocolSource *source = nullptr;

    // UI 控件 (参数配置)
    QLineEdit *editBindAddr = nullptr;
    QSpinBox  *spinPort     = nullptr;
    QLineEdit *editSerialPort = nullptr;
    QLineEdit *editFilePath = nullptr;
    QPushButton *btnBrowseFile = nullptr;
    QCheckBox *chkLoop      = nullptr;
    QDoubleSpinBox *spinSpeed = nullptr;
    QSpinBox  *spinUniverse = nullptr;   // sACN: Universe (1-63999)
    QSpinBox  *spinPriority = nullptr;   // sACN: 优先级过滤 (0=全部)

    // 实时显示控件
    QLabel *lblStatus    = nullptr;
    QLabel *lblFrameCount= nullptr;
    QLabel *lblUniverse  = nullptr;
    QLabel *lblTs        = nullptr;
    QLabel *lblDataHex   = nullptr;
    QLabel *lblExtra     = nullptr;      // 附加统计 (RDM: DMX 包数 / sACN: 来源 CID)

    // 通道电平柱状图 (实时数据可视化)
    DmxChannelBars *bars = nullptr;
    QLabel *lblChanInfo  = nullptr;   // 域/子网/Universe/帧率/总帧/时间码
    double fps = 0.0;
    qint64 firstTsUs = 0;
    qint64 lastTsUs  = 0;

    // Universe 通道峰值表 (协议无关)
    QTableWidget *tblUniverses = nullptr;

    // 操作按钮
    QPushButton *btnStart = nullptr;
    QPushButton *btnStop  = nullptr;

    quint64 frameCount = 0;
    quint64 dmxCount = 0;                // RDM: 同时收到的 DMX 包
    quint64 otherCount = 0;              // RDM: 原始包总数
    QHash<quint16, QByteArray> universePeaks; // universe → 每通道 max
};

struct ProtocolOutputTab {
    ProtocolType type;
    QString key;
    IProtocolSink *sink = nullptr;

    // UI 控件 (目标配置)
    QLineEdit *editRemoteAddr = nullptr;
    QSpinBox  *spinRemotePort = nullptr;
    QLineEdit *editSerialPort = nullptr;
    QSpinBox  *spinUniverse   = nullptr;  // sACN: 输出 Universe
    QSpinBox  *spinPriority   = nullptr;  // sACN: 优先级 (0-200)

    // 实时显示控件
    QLabel *lblStatus     = nullptr;
    QLabel *lblSentCount  = nullptr;
    QLabel *lblTarget     = nullptr;

    // 最近一次发送帧的通道电平
    DmxChannelBars *bars = nullptr;
    QLabel *lblChanInfo  = nullptr;   // 域/子网/Universe/帧率/总帧/时间码
    qint64 firstTsUs = 0;
    qint64 lastTsUs  = 0;

    // 操作按钮
    QPushButton *btnStart = nullptr;
    QPushButton *btnStop  = nullptr;

    quint64 sentCount = 0;
};

// ===== 网络协议收发测试页 (Art-Net / NPU) 共享数据结构 =====
struct NetTestPage {
    ProtocolType type = ProtocolType::ArtNet;

    // 目标与参数
    QLineEdit   *editTargetIp  = nullptr;
    QSpinBox    *spinTargetPort= nullptr;
    QSpinBox    *spinListenPort= nullptr;   // 接收测试绑定的本地端口 (回环时可与目标端口不同)
    QSpinBox    *spinUniverse  = nullptr;
    QSpinBox    *spinPriority  = nullptr;   // sACN 优先级
    QSpinBox    *spinFrameRate = nullptr;
    QComboBox   *cmbMode       = nullptr;   // 恒值 / 单通道扫描 / 逐通道走灯
    QSpinBox    *spinChannel   = nullptr;
    QSpinBox    *spinLevel     = nullptr;
    QSpinBox    *spinStepMs    = nullptr;

    // 数据送入实时监视页
    QCheckBox   *chkFeedOut    = nullptr;
    QCheckBox   *chkFeedIn     = nullptr;

    // 按钮与状态
    QPushButton *btnTx         = nullptr;
    QPushButton *btnRx         = nullptr;
    QPushButton *btnLoop       = nullptr;   // 一键回环: 目标指向本机接收端口, 同时收+发
    QLabel      *lblState      = nullptr;

    // 结果卡
    QLabel      *lblTxFrames   = nullptr;
    QLabel      *lblRxFrames   = nullptr;
    QLabel      *lblMismatch   = nullptr;
    QLabel      *lblInterval   = nullptr;
    QLabel      *lblRate       = nullptr;
    QLabel      *lblVerdict    = nullptr;   // 回环自动判定结果
    qint64       loopStartMs   = 0;         // 收发同时运行的起始时刻
    int          verdictState  = 0;         // 0 未运行 / 1 采集 / 2 通过 / 3 失败

    // 运行时
    QUdpSocket  *txSocket      = nullptr;
    QUdpSocket  *rxSocket      = nullptr;
    QTimer      *txTimer       = nullptr;
    QTimer      *statTimer     = nullptr;
    QByteArray   txBuf;                    // 当前发送的 512 通道
    QByteArray   rxAccum;                  // 接收累积缓冲
    QByteArray   cid;                      // sACN 源 CID
    quint64      txCount       = 0;
    quint64      rxCount       = 0;
    quint64      mismatch      = 0;
    qint64       lastRxUs      = 0;
    qint64       intervalUs    = 0;
    qint64       lastStepMs    = 0;
    int          scanCh        = 0;
    int          ramp          = 0;
    bool         rampUp        = true;
    quint16      lastUniverse  = 0;
    bool         rxTestMode    = false;
};

class ArtNetRecorderMainWindow : public TestWindowBase {
    Q_OBJECT
public:
    explicit ArtNetRecorderMainWindow(QWidget *parent = nullptr);
    ~ArtNetRecorderMainWindow() override;

protected:
    void setupContent() override;

private slots:
    void onNavChanged(int row);
    void onClockTick(qint64 frameIdx, qint64 tsUs);
    void onEngineStateChanged(RecordState state);
    void onEngineFrameRecorded(qint64 frameIdx, qint64 tsUs);
    void onEngineFinished(qint64 frames, qint64 durationUs);
    void onEngineError(const QString &err);
    void onArtNetDmxReceived(const ArtNetDmxFrame &frame);
    void onClockLooped(int cnt);

    void onStartClicked() override;
    void onStopClicked() override;
    void onResetClicked() override;
    void onBtnOpenFileClicked();
    void onBtnPlayFileClicked();

    void tickUpdate();
    void updateNavIndicators();

    // ===== 协议输入页面槽 =====
    void onInputTabFrameReceived(ProtocolInputTab *tab, const LightFrame &frame);
    void onInputTabStatusChanged(ProtocolInputTab *tab, const QString &s);
    void onInputTabError(ProtocolInputTab *tab, const QString &e);

    // ===== 协议输出页面槽 =====
    void onOutputTabFrameSent(ProtocolOutputTab *tab, quint16 universe, quint64 count);
    void onOutputTabStarted(ProtocolOutputTab *tab, const QString &info);
    void onOutputTabStopped(ProtocolOutputTab *tab);
    void onOutputTabError(ProtocolOutputTab *tab, const QString &e);

private:
    // ===== 生产对象 =====
    RecorderEngine *m_engine  = nullptr;
    ISyncClock     *m_clock   = nullptr;
    ArtNetReceiver *m_receiver = nullptr;
    OutputManager  *m_outputMgr = nullptr;

    // ===== 左侧导航 =====
    QListWidget   *m_navList   = nullptr;
    QStringList    m_navBaseTexts;      // 导航原始文本 (用于附加运行标记)
    QStackedWidget *m_stack   = nullptr;

    // ===== 页面 1: 录制仪表盘 =====
    QWidget *buildPageDashboard();
    QLabel *m_lblEngineState   = nullptr;
    QLabel *m_lblRecordFrames  = nullptr;
    QLabel *m_lblRecordSecs    = nullptr;
    QLabel *m_lblClockType     = nullptr;
    QLabel *m_lblClockFps      = nullptr;
    QLabel *m_lblClockTs       = nullptr;
    QLabel *m_lblClockFrameIdx = nullptr;
    QLabel *m_lblArtNetStatus  = nullptr;
    QLabel *m_lblArtNetPort    = nullptr;
    QLabel *m_lblArtNetPkts    = nullptr;
    QLabel *m_lblLastUniverse  = nullptr;
    QLabel *m_lblLastUniverseData = nullptr;
    QLabel *m_lblOutputStatus  = nullptr;

    // ===== 页面 2: 时间码源 =====
    QWidget *buildPageSyncClock();
    QComboBox *m_cmbClockType  = nullptr;
    QSpinBox  *m_spinFps       = nullptr;
    QSpinBox  *m_spinArtNetPort = nullptr;
    QLineEdit *m_editBindAddr  = nullptr;
    QLineEdit *m_editMediaFile = nullptr;
    QPushButton *m_btnBrowseMedia = nullptr;
    QCheckBox *m_chkLoopOnEnd = nullptr;

    // 时间码源只能单选: 按当前源显示对应的配置组
    QWidget   *m_grpClockTimer  = nullptr;   // Internal Timer: 帧率
    QWidget   *m_grpClockArtNet = nullptr;   // ArtNet TimeCode: 绑定地址/端口
    QWidget   *m_grpClockMidi   = nullptr;   // MIDI MTC: 设备/通道
    QWidget   *m_grpClockFile   = nullptr;   // Timecode/Video/Audio: 文件/循环
    QComboBox *m_cmbMidiDevice  = nullptr;   // 本地 MIDI 输入设备列表
    QPushButton *m_btnRefreshMidi = nullptr;
    QSpinBox  *m_spinMidiChannel = nullptr;
    QCheckBox *m_chkFileRateHeader = nullptr;
    void updateClockConfigRows();
    void refreshMidiDevices();

    // ---- 配置文件 (ui_config.json, 与 exe 同目录) ----
    QString uiConfigPath() const;
    void loadUiConfig();
    void saveUiConfig();

    // 时间码源页右侧"实时时间码显示"大数字
    QLabel *m_lblClockBigTs    = nullptr;
    QLabel *m_lblClockBigFrame = nullptr;
    QLabel *m_lblClockBigTc    = nullptr;
    QLabel *m_lblClockBigFps   = nullptr;
    QLabel *m_lblClockBigFrameCount = nullptr;   // 当前帧数
    QLabel *m_lblClockBigTotalFrames = nullptr;  // 总帧数
    void updateClockDisplay(qint64 tsUs, qint64 frameIdx, int fps);

    // ---- 音视频时间码: 媒体播放器 + 音频波形 ----
    QWidget *m_grpMediaPlayer = nullptr;
    MediaPlayerPanel *m_mediaPlayer = nullptr;
    QSpinBox *m_spinFileFps = nullptr;       // 音视频文件的时码帧率
    QTimer   *m_playerClockTimer = nullptr;  // FFmpeg 不可用时用播放位置驱动时码
    bool      m_usingPlayerClock = false;
    void onPlayerClockTick();

    // ---- 内建 MTC 模拟发送 (测试 MIDI 输入链路, 不依赖外部工具) ----
    QComboBox   *m_cmbMidiOut   = nullptr;
    QPushButton *m_btnMtcSend   = nullptr;
    QSpinBox    *m_spinMtcH     = nullptr;
    QSpinBox    *m_spinMtcM     = nullptr;
    QSpinBox    *m_spinMtcS     = nullptr;
    QSpinBox    *m_spinMtcF     = nullptr;
    QTimer      *m_mtcTimer     = nullptr;
    void *m_midiOutHandle       = nullptr;   // WinMM HMIDIOUT
    quint8 m_mtcQfIndex         = 0;
    int  m_mtcHour = 0, m_mtcMin = 0, m_mtcSec = 0, m_mtcFrame = 0;
    void refreshMidiOutDevices();
    void toggleMtcSimulation();
    void onMtcSimTick();

    // ===== 页面: sACN / RDM 测试 =====
    QWidget *buildPageRdmTest();
    void startRdmTestRx();
    void stopRdmTestRx();
    void sendRdmTestRequest(bool discovery);
    void onRdmTestRx();
    void updateRdmTestStats();

    NetTestPage *m_sacnTest = nullptr;

    QLineEdit   *m_rdmTestIp        = nullptr;
    QSpinBox    *m_rdmTestPort      = nullptr;
    QPushButton *m_btnRdmListen     = nullptr;
    QPushButton *m_btnRdmDisc       = nullptr;
    QPushButton *m_btnRdmGetInfo    = nullptr;
    QLabel      *m_lblRdmState      = nullptr;
    QLabel      *m_lblRdmTxCount    = nullptr;
    QLabel      *m_lblRdmRxCount    = nullptr;
    QLabel      *m_lblRdmInterval   = nullptr;
    QLabel      *m_lblRdmSrcUid     = nullptr;
    QLabel      *m_lblRdmHex        = nullptr;
    QUdpSocket  *m_rdmTestTxSock    = nullptr;
    QUdpSocket  *m_rdmTestRxSock    = nullptr;
    QTimer      *m_rdmTestStatTimer = nullptr;
    quint64      m_rdmTxCount       = 0;
    quint64      m_rdmRxCount       = 0;
    quint64      m_rdmOtherCount    = 0;
    qint64       m_rdmLastReqUs     = 0;
    qint64       m_rdmRespIntervalUs= 0;
    bool         m_rdmListening     = false;
    QByteArray   m_rdmLastPayload;

    // ===== 页面: Art-Net / NPU 收发测试 =====
    QWidget *buildPageNetTest(ProtocolType type);
    void startNetTestTx(NetTestPage *p);
    void stopNetTestTx(NetTestPage *p);
    void startNetTestRx(NetTestPage *p);
    void stopNetTestRx(NetTestPage *p);
    void writeNetTestFrame(NetTestPage *p);
    void onNetTestRx(NetTestPage *p);
    void updateNetTestStats(NetTestPage *p);

    NetTestPage *m_artnetTest = nullptr;
    NetTestPage *m_npuTest    = nullptr;

    // ===== 页面: DMX512 专项测试 (环回 / 时序 / 走灯) =====
    QWidget *buildPageDmxTest();
    void refreshDmxTestPorts();
    void startDmxTestTx();
    void stopDmxTestTx();
    void startDmxRxTest();
    void stopDmxRxTest();
    void writeDmxTestFrame();
    void onDmxTestRxData();
    void updateDmxTestStats();

    QComboBox   *m_dmxTxPort     = nullptr;
    QComboBox   *m_dmxRxPort     = nullptr;
    QSpinBox    *m_dmxFrameRate  = nullptr;
    QComboBox   *m_dmxMode       = nullptr;   // 恒值 / 单通道扫描 / 逐通道走灯
    QComboBox   *m_dmxRxFormat   = nullptr;   // 接收帧格式 (适配不同 USB-RS485)
    QComboBox   *m_dmxBreakMode  = nullptr;   // Break 方式: 串口Break / 波特率模拟 / 无
    QSpinBox    *m_dmxChannel    = nullptr;   // 1-512
    QSpinBox    *m_dmxLevel      = nullptr;   // 0-255
    QSpinBox    *m_dmxStepMs     = nullptr;   // 走灯步进间隔
    QCheckBox   *m_dmxFeedOut    = nullptr;   // 发送数据送入实时输出页
    QCheckBox   *m_dmxFeedIn     = nullptr;   // 接收数据送入实时输入页
    QCheckBox   *m_dmxLoopbackSim = nullptr;  // 软件环回模拟 (无硬件环回时)
    QPushButton *m_btnDmxTx      = nullptr;
    QPushButton *m_btnDmxRxTest  = nullptr;   // 独立接收测试 (只收不发)
    bool         m_dmxRxTestMode = false;
    QLabel      *m_lblDmxState   = nullptr;
    QLabel      *m_lblDmxTxFrames = nullptr;
    QLabel      *m_lblDmxRxFrames = nullptr;
    QLabel      *m_lblDmxMismatch = nullptr;
    QLabel      *m_lblDmxInterval = nullptr;
    QLabel      *m_lblDmxRate     = nullptr;
    QLabel      *m_lblDmxHex      = nullptr;   // 最近接收原始字节 (HEX 诊断)
    QLabel      *m_lblDmxTxBytes  = nullptr;   // 实际写入串口的字节数
    QSerialPort *m_dmxTxSerial   = nullptr;
    QSerialPort *m_dmxRxSerial   = nullptr;
    QTimer      *m_dmxTxTimer    = nullptr;
    QTimer      *m_dmxStatTimer  = nullptr;
    QByteArray   m_dmxTxBuf;               // 当前发送的 512 通道
    QByteArray   m_dmxRxAccum;             // 接收累积缓冲
    QByteArray   m_dmxRxRaw;               // 最近 64 字节原始接收 (诊断用)
    quint64      m_dmxTxCount    = 0;
    quint64      m_dmxTxBytes    = 0;
    quint64      m_dmxRxCount    = 0;
    quint64      m_dmxMismatch   = 0;
    qint64       m_dmxLastRxUs   = 0;
    qint64       m_dmxIntervalUs = 0;
    qint64       m_dmxLastStepMs = 0;
    int          m_dmxScanCh     = 0;
    int          m_dmxRamp       = 0;
    bool         m_dmxRampUp     = true;

    // ===== 页面: Art-Net → DMX512 转换测试 =====
    QWidget *buildPageArtToDmx();
    void refreshArtToDmxPorts();
    void startArtToDmx();
    void stopArtToDmx();
    void onArtToDmxDatagram();
    void writeArtToDmxFrame(const QByteArray &dmx);

    QSpinBox    *m_brListenPort  = nullptr;
    QSpinBox    *m_brUniverse    = nullptr;
    QComboBox   *m_brSerialPort  = nullptr;
    QComboBox   *m_brBreakMode   = nullptr;
    QCheckBox   *m_brFilterUni   = nullptr;   // 只转发匹配域的包
    QCheckBox   *m_brFeedOut     = nullptr;   // 转换结果写入实时输出页
    QPushButton *m_btnBrToggle   = nullptr;
    QLabel      *m_lblBrState    = nullptr;
    QLabel      *m_lblBrRxFrames = nullptr;
    QLabel      *m_lblBrTxFrames = nullptr;
    QLabel      *m_lblBrDropped  = nullptr;
    QLabel      *m_lblBrLastUni  = nullptr;
    QLabel      *m_lblBrLatency  = nullptr;
    QLabel      *m_lblBrCh1      = nullptr;
    QUdpSocket  *m_brSocket      = nullptr;
    QSerialPort *m_brSerial      = nullptr;
    QTimer      *m_brStatTimer   = nullptr;
    quint64      m_brRxCount     = 0;
    quint64      m_brTxCount     = 0;
    quint64      m_brDropCount   = 0;
    qint64       m_brLastLatencyUs = 0;
    quint16      m_brLastUni     = 0;
    QByteArray   m_brLastDmx;

    // ===== 页面: DMX512 → Art-Net 转换测试 =====
    QWidget *buildPageDmxToArt();
    void refreshD2aPorts();
    void startDmxToArt();
    void stopDmxToArt();
    void onD2aSerialData();
    void convertDmxToArt(const QByteArray &dmx);

    QComboBox   *m_d2aSerialPort = nullptr;
    QComboBox   *m_d2aFormat     = nullptr;   // 512 / 513 / 514
    QLineEdit   *m_d2aTargetIp   = nullptr;
    QSpinBox    *m_d2aTargetPort = nullptr;
    QSpinBox    *m_d2aUniverse   = nullptr;
    QCheckBox   *m_d2aFeedIn     = nullptr;
    QCheckBox   *m_d2aFeedOut    = nullptr;
    QPushButton *m_btnD2aToggle  = nullptr;
    QLabel      *m_lblD2aState   = nullptr;
    QLabel      *m_lblD2aRx      = nullptr;
    QLabel      *m_lblD2aTx      = nullptr;
    QLabel      *m_lblD2aDrop    = nullptr;
    QLabel      *m_lblD2aUni     = nullptr;
    QLabel      *m_lblD2aLatency = nullptr;
    QLabel      *m_lblD2aCh1     = nullptr;
    QSerialPort *m_d2aSerial     = nullptr;
    QUdpSocket  *m_d2aSocket     = nullptr;
    QTimer      *m_d2aStatTimer  = nullptr;
    QByteArray   m_d2aAccum;
    QByteArray   m_d2aLastDmx;
    quint64      m_d2aRxCount    = 0;
    quint64      m_d2aTxCount    = 0;
    quint64      m_d2aDropCount  = 0;
    qint64       m_d2aLatencyUs  = 0;
    quint16      m_d2aLastUni    = 0;
    quint8       m_d2aSeq        = 0;

    // ===== 页面 3: 输入数据 (QTabWidget: ArtNet / DMX512 / NPU / RDM / ArtRecFile) =====
    QWidget *buildPageInputData();
    QTabWidget *m_inputTabs = nullptr;
    QMap<ProtocolType, ProtocolInputTab*> m_inputTabsData;

    QWidget *buildInputTabArtNet(ProtocolInputTab *data);
    QWidget *buildInputTabDmx512(ProtocolInputTab *data);
    QWidget *buildInputTabNpu(ProtocolInputTab *data);
    QWidget *buildInputTabSacn(ProtocolInputTab *data);
    QWidget *buildInputTabRdm(ProtocolInputTab *data);
    QWidget *buildInputTabArtRecFile(ProtocolInputTab *data);

    void startInputTab(ProtocolInputTab *tab);
    void stopInputTab(ProtocolInputTab *tab);

    // ===== 页面 4: 输出数据 (QTabWidget: ArtNet / DMX512 / NPU / sACN / RDM) =====
    QWidget *buildPageOutputData();
    QTabWidget *m_outputTabs = nullptr;
    QMap<ProtocolType, ProtocolOutputTab*> m_outputTabsData;

    QWidget *buildOutputTabArtNet(ProtocolOutputTab *data);
    QWidget *buildOutputTabDmx512(ProtocolOutputTab *data);
    QWidget *buildOutputTabNpu(ProtocolOutputTab *data);
    QWidget *buildOutputTabSacn(ProtocolOutputTab *data);
    QWidget *buildOutputTabRdm(ProtocolOutputTab *data);

    void startOutputTab(ProtocolOutputTab *tab);
    void stopOutputTab(ProtocolOutputTab *tab);

    // RDM 输入用生产 ArtNetReceiver (ArtRDM 0x6000)
    ArtNetReceiver *m_rdmReceiver = nullptr;

    // ===== 页面 5: 转发输出 / 老页面保留 =====
    QWidget *buildPageOutput();
    QTableWidget *m_tblOutputTargets = nullptr;
    QPushButton  *m_btnAddArtNet = nullptr;
    QPushButton  *m_btnAddDmx    = nullptr;
    QPushButton  *m_btnRemove    = nullptr;

    // ===== 页面 6: 文件管理 =====
    QWidget *buildPageFileMgr();
    QLabel *m_lblFileHeaderVersion = nullptr;
    QLabel *m_lblFileFps           = nullptr;
    QLabel *m_lblFileTotalFrames   = nullptr;
    QLabel *m_lblFileDuration      = nullptr;
    QLabel *m_lblFileUniverseCount = nullptr;
    QLabel *m_lblFilePath          = nullptr;
    QTableWidget *m_tblFileFrames  = nullptr;

    // ===== 页面 7: 系统状态 =====
    QWidget *buildPageSystem();
    QLabel *m_lblRuntime     = nullptr;
    QLabel *m_lblQtVersion   = nullptr;
    QLabel *m_lblBuildType   = nullptr;
    QLabel *m_lblPlatform   = nullptr;
    QLabel *m_lblAppVersion  = nullptr;
    QLabel *m_lblClockCount  = nullptr;

    // ===== 通用辅助 =====
    QFrame *createCard(const QString &title, QWidget *content);
    QLabel *createValNumber(const QString &val, int size = 20);
    QLabel *createValLabel(const QString &text);

    // 通道电平面板 (参数行 + 柱状图), 输入/输出 Tab 共用
    QWidget *createChannelPanel(QLabel **lblInfo, DmxChannelBars **bars, QWidget *parent);
    static void updateChannelPanel(QLabel *lblInfo, quint16 universe, double fps,
                                   quint64 totalFrames, qint64 relTsUs);
    /** 实时页单行信息: net/subnet/Universe + 帧率/总帧/时间码/时间长度 */
    static QString formatLiveInfoLine(quint16 universe, double fps,
                                      quint64 totalFrames, qint64 relTsUs);

    // ===== 实时输入 / 实时输出页 =====
    QWidget *buildPageLiveInput();
    QWidget *buildPageLiveOutput();
    QWidget *buildLivePage(bool isInput);

    // ---- 实时输入/输出: 按域(Universe)缓存, 支持切换显示 (解码器最多 16 个域) ----
    struct LiveUniverseData {
        QByteArray dmx;
        quint64    frames = 0;
        qint64     firstTsUs = 0;
        qint64     lastTsUs = 0;
        double     fps = 0.0;
    };
    QHash<quint16, LiveUniverseData> m_liveInMap;
    QHash<quint16, LiveUniverseData> m_liveOutMap;
    qint64 m_liveInActiveUs  = 0;      // 最近一次写入实时输入的时间
    qint64 m_liveOutActiveUs = 0;      // 最近一次写入实时输出的时间
    QComboBox *m_cmbLiveInUniverse = nullptr;
    QComboBox *m_cmbLiveOutUniverse = nullptr;
    QLabel *m_lblLiveInUniCount = nullptr;    // 已接收域数量 (Art-Net 4 可达 32768)
    QLabel *m_lblLiveOutUniCount = nullptr;
    quint16 m_liveInUniverse = 0xFFFF;
    quint16 m_liveOutUniverse = 0xFFFF;
    void updateLiveUniverseCombo(bool isInput, quint16 universe);
    void feedLiveUniverse(bool isInput, quint16 universe, const QByteArray &dmx, qint64 tsUs);
    void applyLiveUniverse(bool isInput);

    QLabel *m_liveInInfo  = nullptr;
    QLabel *m_liveInPage  = nullptr;
    DmxChannelBars *m_liveInBars = nullptr;
    int m_liveInPageIdx = 0;

    QLabel *m_liveOutInfo = nullptr;
    QLabel *m_liveOutPage = nullptr;
    DmxChannelBars *m_liveOutBars = nullptr;
    int m_liveOutPageIdx = 0;

    int   m_tickCount       = 0;
    int   m_dmxPacketCount  = 0;
    qint64 m_startWallUs    = 0;

    QTimer *m_uiTimer = nullptr;
    QLabel *m_lblTopClock = nullptr;
};

#endif // ARTNET_RECORDER_MAIN_WINDOW_H
