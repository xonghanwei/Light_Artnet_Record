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
#include <QVector>
#include <functional>

class QSerialPort;
class QUdpSocket;
class Dmx512Sender;
class NpuSource;

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
    QSpinBox    *spinUniverseCount = nullptr;  // 连续发送域数量 1..8 (自起始域)
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
    int          txTaskId      = -1;       // 调度器流任务 id (周期发送帧)
    int          statTaskId    = -1;       // 调度器统计任务 id (300ms 刷新)
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
    quint64      m_rdmTxCount       = 0;
    quint64      m_rdmRxCount       = 0;
    quint64      m_rdmOtherCount    = 0;
    qint64       m_rdmLastReqUs     = 0;
    qint64       m_rdmRespIntervalUs= 0;
    bool         m_rdmListening     = false;
    QByteArray   m_rdmLastPayload;

    // ---- RDM 设备管理扩展: 发现 / 改地址 / 故障查询 (ArtRDM 0x6000) ----
    QTableWidget *m_tblRdmDevices   = nullptr;   // 发现的灯具列表
    QLabel      *m_lblRdmDevCount   = nullptr;
    QComboBox   *m_cmbRdmUid        = nullptr;   // 选中灯具 UID
    QSpinBox    *m_spinRdmAddr      = nullptr;   // 新 DMX 地址
    QPushButton *m_btnRdmScan       = nullptr;
    QPushButton *m_btnRdmSetAddr    = nullptr;
    QPushButton *m_btnRdmStatus     = nullptr;
    QPushButton *m_btnRdmStatusAll  = nullptr;
    QLabel      *m_lblRdmOpResult   = nullptr;
    QTimer      *m_rdmDiscTimer     = nullptr;   // 发现二分搜索步进
    QTimer      *m_rdmQueryTimer    = nullptr;   // 逐设备查询步进

    struct RdmUidRange { quint64 lower; quint64 upper; };
    QVector<RdmUidRange> m_rdmDiscStack;         // 待搜索 UID 区间栈
    QVector<quint64>     m_rdmDiscHits;          // 本轮 DISC 响应命中 UID
    QVector<quint64>     m_rdmFoundUids;         // 已确认设备 UID
    quint64 m_rdmDiscLower = 0;                  // 当前查询区间下界
    quint64 m_rdmDiscUpper = 0;                  // 当前查询区间上界
    int     m_rdmDiscSends = 0;                  // 本轮已发送 DISC 请求数
    bool    m_rdmDiscRunning = false;

    struct RdmQueryJob { quint64 uid; quint16 pid; };
    QVector<RdmQueryJob> m_rdmQueryQueue;        // 待执行的 GET 请求队列
    quint8  m_rdmTransaction = 0;                // 事务号 (0..255 循环)

    bool   ensureRdmTxSock();                    // 懒创建发送 socket
    void   rdmSendRequest(quint64 destUid, quint8 cc, quint16 pid,
                          const QByteArray &params = QByteArray());
    void   rdmStartDiscovery();                  // 开始二分发现
    void   rdmDiscoveryStep();                   // 一步: 发送/判定/分裂
    void   rdmFinishDiscovery();                 // 发现结束: 填表+排队查询
    void   rdmEnqueueQueries(quint64 uid, const QVector<quint16> &pids);
    void   rdmQueryStep();                       // 执行队列中下一个 GET
    void   rdmHandleResponse(const uchar *r, int payloadSize); // 解析应答并更新表
    void   rdmRefreshUidCombo();
    int    rdmDeviceRow(quint64 uid) const;      // UID → 表行号
    static QString rdmUidStr(quint64 uid);       // 48bit UID → "xx:xx:.."
    static quint64 rdmParseUid(const uchar *p);  // 6 字节 → 48bit UID

    // ---- RDM 串口传输 (DMX512 线 E1.20 格式, 半双工) ----
    void   onRdmSerialRx();                      // 串口收包组帧 → 归一化后统一处理
    void   rdmProcessResponse(const uchar *r, int size); // ArtRDM 布局应答统一处理
    bool   rdmSerialOpen();                       // 按当前选择打开 RDM 串口 (读写)
    void   refreshRdmSerialPorts();               // 刷新串口下拉

    // ---- RDM 接收测试 (模拟灯具应答器: 被动收请求并自动应答) ----
    void   startRdmResponder();
    void   stopRdmResponder();
    void   onRdmReqRx();                          // UDP: 接收 ArtRDM 请求
    bool   rdmHandleRequest(const uchar *r, int size, bool viaSerial,
                           const QHostAddress &peer);  // 解析请求+自动应答, 返回是否已处理
    QByteArray rdmFixtureResponse(quint8 cc, quint16 pid, const QByteArray &params,
                                  quint64 destUid, quint8 txn, quint8 sub); // ArtRDM 布局应答
    static QByteArray rdmWireFrame(quint8 startCode, quint64 destUid, quint64 srcUid,
                                   quint8 cc, quint16 pid, const QByteArray &params); // E1.20 线上帧
    quint64 rdmFixtureUid() const;                // 解析模拟灯具 UID 输入
    static void rdmMaskedUid(quint64 uid, uchar *out12); // UID → 12 字节奇偶掩码

    QComboBox    *m_cmbRdmTransport = nullptr;   // 0=ArtRDM UDP / 1=DMX512 串口
    QComboBox    *m_cmbRdmSerial    = nullptr;   // RDM 串口选择
    QPushButton  *m_btnRdmSerialRefresh = nullptr;
    Dmx512Sender *m_rdmSerialSender = nullptr;   // 串口传输实例 (读写模式)
    QByteArray    m_rdmRxAccum;                  // 串口接收组帧缓冲

    QUdpSocket   *m_rdmReqSock = nullptr;        // 应答器监听 socket
    bool          m_rdmRespActive = false;       // 应答器运行中
    quint64       m_rdmReqCount = 0;             // 收到请求数
    quint64       m_rdmRespSent = 0;             // 自动应答数
    QSpinBox     *m_spinRdmRespPort = nullptr;   // 应答器监听端口
    QLineEdit    *m_editRdmFixUid = nullptr;     // 模拟灯具 UID (hex)
    QSpinBox     *m_spinRdmFixAddr = nullptr;    // 模拟灯具 DMX 地址
    QPushButton  *m_btnRdmResp = nullptr;
    QLabel       *m_lblRdmRespState = nullptr;
    QLabel       *m_lblRdmReqCount = nullptr;
    QLabel       *m_lblRdmRespSent = nullptr;
    QLabel       *m_lblRdmReqInfo = nullptr;
    QLabel       *m_lblRdmReqHex = nullptr;

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
    int          m_dmxTxTaskId   = -1;      // 调度器流任务 id (周期发送 DMX 帧)
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

    // ===== 页面: Art-Net → DMX512 转换测试 (最多 8 路串口输出) =====
    QWidget *buildPageArtToDmx();
    void refreshArtToDmxPorts();
    void startArtToDmx();
    void stopArtToDmx();
    void onArtToDmxDatagram();
    void tickArtToDmxBatch();   // 统一节拍: 批量非阻塞写全部脏端口

    enum { kArtToDmxOuts = 8 };
    // 每路输出: 启用 + 串口 + 起始 Universe + 覆盖域数, 运行期各自持有 Dmx512Sender
    struct ArtToDmxOutput {
        QCheckBox    *chkEnabled = nullptr;
        QComboBox    *cmbPort    = nullptr;
        QSpinBox     *spinStart  = nullptr;
        QSpinBox     *spinSpan   = nullptr;
        QLabel       *lblSent    = nullptr;
        Dmx512Sender *sender     = nullptr;   // 未启动时为 nullptr
        QByteArray    frame;                  // 最新待发送 512 通道 (收到包只更新缓存)
        bool          dirty = false;          // 有新数据待节拍发送
        qint64        arrivedUs = 0;          // 帧到达时刻 (ms*1000, 延迟统计)
    };

    QLineEdit   *m_brListenIp   = nullptr;   // 绑定接收 Art-Net 包的本地 IP
    QSpinBox    *m_brListenPort  = nullptr;
    QCheckBox   *m_brFeedOut     = nullptr;   // 转换结果写入实时输出页
    QPushButton *m_btnBrToggle   = nullptr;
    QLabel      *m_lblBrState    = nullptr;
    QLabel      *m_lblBrRxFrames = nullptr;
    QLabel      *m_lblBrTxFrames = nullptr;   // 全部输出行发送帧合计
    QLabel      *m_lblBrDropped  = nullptr;
    QLabel      *m_lblBrLastUni  = nullptr;
    QLabel      *m_lblBrLatency  = nullptr;
    QLabel      *m_lblBrCh1      = nullptr;
    QUdpSocket  *m_brSocket      = nullptr;
    QTimer      *m_brTickTimer   = nullptr;   // 23ms 统一发送节拍 (~43Hz)
    ArtToDmxOutput m_brOuts[kArtToDmxOuts];
    quint64      m_brRxCount     = 0;
    quint64      m_brDropCount   = 0;
    qint64       m_brLastLatencyUs = 0;
    quint16      m_brLastUni     = 0;
    QByteArray   m_brLastDmx;

    // ===== 页面: DMX512 → Art-Net 转换测试 (最多 8 路串口输入) =====
    QWidget *buildPageDmxToArt();
    void refreshD2aPorts();
    void startDmxToArt();
    void stopDmxToArt();
    void onD2aSerialData(int row);
    void convertDmxToArt(int row, const QByteArray &dmx);

    enum { kDmxToArtIns = 8 };
    // 每路输入: 启用 + 串口 + 目标 Universe, 独立接收累积缓冲 / 帧计数 / ArtDmx 序号
    struct DmxToArtInput {
        QCheckBox   *chkEnabled = nullptr;
        QComboBox   *cmbPort    = nullptr;
        QSpinBox    *spinUni    = nullptr;
        QLabel      *lblRx      = nullptr;
        QSerialPort *serial     = nullptr;   // 未启动时为 nullptr
        QByteArray   accum;
        quint64      rxCount    = 0;
        quint8       seq        = 0;
    };

    QComboBox   *m_d2aFormat     = nullptr;   // 512 / 513 / 514
    QLineEdit   *m_d2aTargetIp   = nullptr;
    QSpinBox    *m_d2aTargetPort = nullptr;
    QCheckBox   *m_d2aFeedIn     = nullptr;
    QCheckBox   *m_d2aFeedOut    = nullptr;
    QPushButton *m_btnD2aToggle  = nullptr;
    QLabel      *m_lblD2aState   = nullptr;
    QLabel      *m_lblD2aRx      = nullptr;   // 全部输入行收到帧合计
    QLabel      *m_lblD2aTx      = nullptr;
    QLabel      *m_lblD2aDrop    = nullptr;
    QLabel      *m_lblD2aUni     = nullptr;
    QLabel      *m_lblD2aLatency = nullptr;
    QLabel      *m_lblD2aCh1     = nullptr;
    QUdpSocket  *m_d2aSocket     = nullptr;
    DmxToArtInput m_d2aIns[kDmxToArtIns];
    QByteArray   m_d2aLastDmx;
    quint64      m_d2aTxCount    = 0;
    quint64      m_d2aDropCount  = 0;
    qint64       m_d2aLatencyUs  = 0;
    quint16      m_d2aLastUni    = 0;

    // ===== 页面: NPU → DMX512 转换测试 (最多 8 路串口输出) =====
    QWidget *buildPageNpuToDmx();
    void refreshNp2dPorts();
    void startNpuToDmx();
    void stopNpuToDmx();
    void onNp2dFrame(const LightFrame &frame);
    void tickNp2dBatch();       // 统一节拍: 批量非阻塞写全部脏端口

    enum { kNp2dOuts = 8 };
    // 每路输出: 启用 + 串口 + 起始 Universe(0..255) + 覆盖域数, 运行期各自持有 Dmx512Sender
    struct Np2dOutput {
        QCheckBox    *chkEnabled = nullptr;
        QComboBox    *cmbPort    = nullptr;
        QSpinBox     *spinStart  = nullptr;
        QSpinBox     *spinSpan   = nullptr;
        QLabel       *lblSent    = nullptr;
        Dmx512Sender *sender     = nullptr;   // 未启动时为 nullptr
        QByteArray    frame;                  // 最新待发送 512 通道 (收到帧只更新缓存)
        bool          dirty = false;          // 有新数据待节拍发送
    };

    QLineEdit   *m_np2dListenIp   = nullptr;
    QSpinBox    *m_np2dListenPort = nullptr;
    QCheckBox   *m_np2dFeedOut    = nullptr;   // 转换结果写入实时输出页
    QPushButton *m_btnNp2dToggle  = nullptr;
    QLabel      *m_lblNp2dState   = nullptr;
    QLabel      *m_lblNp2dRxFrames= nullptr;
    QLabel      *m_lblNp2dTxFrames= nullptr;   // 全部输出行发送帧合计
    QLabel      *m_lblNp2dDropped = nullptr;
    QLabel      *m_lblNp2dLastUni = nullptr;
    QLabel      *m_lblNp2dCh1     = nullptr;
    NpuSource   *m_np2dSource     = nullptr;   // 复用生产 NPU 接收路径
    QTimer      *m_np2dTickTimer  = nullptr;   // 23ms 统一发送节拍 (~43Hz)
    Np2dOutput   m_np2dOuts[kNp2dOuts];
    quint64      m_np2dRxCount    = 0;
    quint64      m_np2dDropCount  = 0;
    quint16      m_np2dLastUni    = 0;
    QByteArray   m_np2dLastDmx;

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

    // ===== 分组时间片任务调度器 =====
    // 各测试页的周期发送/统计刷新不再各挂一个 QTimer, 统一由两个 master timer 驱动:
    //   流组 5ms 精确时间片 (周期发送), 统计组 300ms (仅当前可见页)
    // pageId 归属 QStackedWidget 页; 切页时离开页的流任务置 Paused, 回页恢复
    enum { kPageAlways = -1, kPagePending = -2 };   // -1 总是运行 / -2 待解析页索引
    struct StreamTask {
        enum State { Idle, Running, Paused, Finished };
        State  state      = Idle;
        int    periodMs   = 0;        // 任务自身周期 (如 25ms = 40fps)
        qint64 deadlineMs = -1;       // -1 无限运行; >0 累计运行时长到达后自动结束
        qint64 accMs      = 0;        // 距上次触发的累计毫秒
        qint64 runMs      = 0;        // 累计运行毫秒 (deadlineMs 原子结束判定用)
        int    pageId     = kPageAlways;
        QWidget *page     = nullptr;  // 注册时页面尚未入栈, 首次 tick 延迟解析 pageId
        std::function<void()> onTick;
        std::function<void()> onFinished;
    };
    struct StatTask {
        int pageId = kPageAlways;
        QWidget *page = nullptr;
        std::function<void()> onTick;
    };

    // 注册接口: page 传页面顶层 widget (构建期入栈前), nullptr = 总是运行
    int  addStreamTask(QWidget *page, std::function<void()> onTick,
                       qint64 deadlineMs = -1, std::function<void()> onFinished = nullptr);
    int  addStatTask(QWidget *page, std::function<void()> onTick);
    void startStreamTask(int id, int periodMs);   // 置 Running, 按当前帧率重设周期并清零累计
    void stopStreamTask(int id);                  // 置 Idle
    void removeTasksOfPage(QWidget *page);        // 页销毁时移除其全部任务 (会使其余 id 失效, 慎用)
    void onSchedStreamTick();                     // 流组 master tick
    void onSchedStatTick();                       // 统计组 master tick
    void onSchedPageChanged(int cur);             // 切页暂停/恢复流任务

    QVector<StreamTask> m_streamTasks;
    QVector<StatTask>   m_statTasks;
    QTimer *m_schedStreamTimer = nullptr;         // 流组: 5ms Qt::PreciseTimer
    QTimer *m_schedStatTimer   = nullptr;         // 统计组: 300ms
    int     m_schedCurPage     = 0;               // 当前页索引 (切页暂停用)

    QTimer *m_uiTimer = nullptr;
    QLabel *m_lblTopClock = nullptr;
};

#endif // ARTNET_RECORDER_MAIN_WINDOW_H
