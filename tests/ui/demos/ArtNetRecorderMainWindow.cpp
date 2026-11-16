// ArtNetRecorderMainWindow.cpp — SCADA 全功能面板实现
#include "ArtNetRecorderMainWindow.h"
#include "MediaPlayerPanel.h"
#include "AudioWaveformView.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QFileDialog>
#include <QGroupBox>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QStyle>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include "output/ArtNetSender.h"
#include "output/Dmx512Sender.h"
#include "protocol/sources/NpuSource.h"
#include <cstring>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <mmsystem.h>
#endif

#if defined(ARTNET_WITH_FFMPEG)
extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

// ============================================================
// 枚举本机 MIDI 输入设备 (WinMM midiIn*)
// ============================================================
static QStringList enumerateMidiInputDevices() {
    QStringList devices;
#ifdef Q_OS_WIN
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPS caps;
        std::memset(&caps, 0, sizeof(caps));
        if (midiInGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
            continue;
        const QString name = QString::fromWCharArray(caps.szPname).trimmed();
        devices << (name.isEmpty() ? QStringLiteral("MIDI In %1").arg(i) : name);
    }
#endif
    return devices;
}

// 枚举本机 MIDI 输出设备 (WinMM midiOut*)
static QStringList enumerateMidiOutputDevices() {
    QStringList devices;
#ifdef Q_OS_WIN
    const UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPS caps;
        std::memset(&caps, 0, sizeof(caps));
        if (midiOutGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
            continue;
        const QString name = QString::fromWCharArray(caps.szPname).trimmed();
        devices << (name.isEmpty() ? QStringLiteral("MIDI Out %1").arg(i) : name);
    }
#endif
    return devices;
}

// ============================================================
// sACN (ANSI E1.31) 收发实现 — 生产层尚未提供 SacnSource/SacnSink,
// 这里在 UI 测试层内嵌一个符合 E1.31 的收发实现, 用于功能验证.
// 包结构 (638 字节):
//   [0..3]    Preamble 0x0010 / Postamble 0x0000
//   [4..15]   ACN Packet Identifier "ASC-E1.17\0\0\0"
//   [16..17]  Root Flags+Length       [18..21] Root Vector = 0x04
//   [22..37]  CID (16B source id)
//   [38..39]  Framing Flags+Length    [40..43] Framing Vector = 0x02
//   [44..107] Source Name (64B)       [108] Priority
//   [109..110] Reserved               [111..112] Sync Address
//   [113] Sequence                    [114] Options
//   [115..116] Universe (1-63999)     [117..118] DMP Flags+Length
//   [119] DMP Vector = 0x02           [120] Start Code
//   [121..632] DMX 512
// ============================================================
static constexpr int kE131PacketSize = 638;

static QHostAddress sacnMulticastAddress(quint16 universe) {
    // 239.255.(universe>>8).(universe&0xFF)
    return QHostAddress(quint32(0xEFFF0000u) | quint32(universe));
}

static QByteArray buildE131Packet(quint16 universe, const QByteArray &dmx, const QByteArray &cid,
                                  quint8 priority, quint8 seq, quint8 options) {
    QByteArray pkt(kE131PacketSize, 0);
    auto *d = reinterpret_cast<uchar*>(pkt.data());

    d[0] = 0x00; d[1] = 0x10;              // Preamble Size = 0x0010
    d[2] = 0x00; d[3] = 0x00;              // Post-amble Size = 0
    std::memcpy(d + 4, "ASC-E1.17", 9);    // ACN Packet Identifier

    const quint16 rootLen = quint16(kE131PacketSize - 16);
    d[16] = quint8(0x70 | (rootLen >> 8)); d[17] = quint8(rootLen & 0xFF);
    d[18] = 0x00; d[19] = 0x00; d[20] = 0x00; d[21] = 0x04;   // Root Vector

    QByteArray c = cid;
    if (c.size() != 16) c = QByteArray(16, 0);
    std::memcpy(d + 22, c.constData(), 16);                    // CID

    const quint16 frameLen = quint16(kE131PacketSize - 38);
    d[38] = quint8(0x70 | (frameLen >> 8)); d[39] = quint8(frameLen & 0xFF);
    d[40] = 0x00; d[41] = 0x00; d[42] = 0x00; d[43] = 0x02;   // Framing Vector

    std::memcpy(d + 44, "Light Artnet Record", 19);            // Source Name
    d[108] = priority;                                         // Priority
    d[111] = 0; d[112] = 0;                                    // Sync Address
    d[113] = seq;                                              // Sequence
    d[114] = options;                                          // Options
    d[115] = quint8(universe >> 8); d[116] = quint8(universe & 0xFF); // Universe

    const quint16 dmpLen = quint16(kE131PacketSize - 117);
    d[117] = quint8(0x70 | (dmpLen >> 8)); d[118] = quint8(dmpLen & 0xFF);
    d[119] = 0x02;                                             // DMP Vector
    d[120] = 0x00;                                             // Start Code
    int n = qMin<int>(512, dmx.size());
    std::memcpy(d + 121, dmx.constData(), n);
    return pkt;
}

static QByteArray makeTestCid() {
    QByteArray cid(16, 0);
    for (int i = 0; i < 16; ++i) cid[i] = char(QRandomGenerator::global()->bounded(256));
    return cid;
}

// sACN 输入 Source (E1.31 单播/组播接收)
class SacnSourceImpl : public IProtocolSource {
public:
    ~SacnSourceImpl() override { stop(); }
    ProtocolType protocolType() const override { return ProtocolType::Sacn; }
    QString protocolName() const override { return "sACN (E1.31)"; }

    bool start(const ConnectionParams &p) override {
        if (m_running) stop();
        m_socket = new QUdpSocket(this);
        if (!m_socket->bind(QHostAddress(p.localBindAddress.isEmpty() ? "0.0.0.0" : p.localBindAddress),
                            p.localPort ? p.localPort : 5568,
                            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            emit errorOccurred(tr("sACN 绑定失败 :%1").arg(p.localPort ? p.localPort : 5568));
            delete m_socket; m_socket = nullptr; return false;
        }
        m_universe = p.filterUniverses.isEmpty() ? 1 : p.filterUniverses.first();
        // 加入对应组播组
        m_socket->joinMulticastGroup(sacnMulticastAddress(m_universe));
        connect(m_socket, &QUdpSocket::readyRead, this, [this]{ onReadyRead(); });
        m_running = true;
        emit statusChanged(tr("sACN 监听 %1:%2, 组播 %3")
                               .arg(p.localBindAddress).arg(p.localPort ? p.localPort : 5568)
                               .arg(sacnMulticastAddress(m_universe).toString()));
        return true;
    }
    void stop() override {
        if (m_socket) {
            m_socket->leaveMulticastGroup(sacnMulticastAddress(m_universe));
            m_socket->close(); m_socket->deleteLater(); m_socket = nullptr;
        }
        m_running = false;
    }
    bool isRunning() const override { return m_running; }
    void setFilterUniverses(const QList<quint16> &u) override { if (!u.isEmpty()) m_universe = u.first(); }
    quint64 framesReceived() const override { return m_count; }

    // UI 读取 (来源信息)
    QByteArray lastCid;
    quint8 lastPriority = 0;
    quint8 lastOptions = 0;
    quint16 lastUniverse = 0;

private:
    void onReadyRead() {
        while (m_socket && m_socket->hasPendingDatagrams()) {
            QNetworkDatagram dg = m_socket->receiveDatagram();
            const QByteArray p = dg.data();
            if (p.size() < 126) continue;
            const auto *d = reinterpret_cast<const uchar*>(p.constData());
            if (std::memcmp(d + 4, "ASC-E1.17", 9) != 0) continue;
            if (!(d[21] == 0x04 && d[43] == 0x02 && d[119] == 0x02)) continue;

            LightFrame f;
            f.sourceProtocol = ProtocolType::Sacn;
            f.universe = quint16(d[115] << 8 | d[116]);
            f.universeRaw = quint8(f.universe & 0xFF);
            f.sequence = d[113];
            f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
            int startCode = d[120];
            if (startCode != 0x00) continue;   // 只处理 DMX 起始码
            const int dmpLen = ((d[117] & 0x0F) << 8) | d[118];
            int dataLen = qMin<int>(qMax(0, dmpLen - 1), 512);
            f.dmxData = p.mid(121, dataLen);
            if (f.dmxData.size() < qMin(512, p.size() - 121))
                f.dmxData = p.mid(121, qMin(512, p.size() - 121));

            lastCid = p.mid(22, 16);
            lastPriority = d[108];
            lastOptions = d[114];
            lastUniverse = f.universe;
            m_count++;
            emit frameReceived(f);
        }
    }

    QUdpSocket *m_socket = nullptr;
    bool m_running = false;
    quint64 m_count = 0;
    quint16 m_universe = 1;
};

// sACN 输出 Sink (E1.31 发送)
class SacnSinkImpl : public IProtocolSink {
public:
    ~SacnSinkImpl() override { stop(); }
    ProtocolType protocolType() const override { return ProtocolType::Sacn; }
    QString protocolName() const override { return "sACN (E1.31)"; }

    bool start(const ConnectionParams &p) override {
        if (m_running) stop();
        m_socket = new QUdpSocket(this);
        if (!m_socket->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
            emit errorOccurred(tr("sACN 发送 socket 绑定失败"));
            delete m_socket; m_socket = nullptr; return false;
        }
        m_port = p.remotePort ? p.remotePort : 5568;
        universe = p.filterUniverses.isEmpty() ? 1 : p.filterUniverses.first();
        m_target = p.remoteAddress.isEmpty()
                       ? sacnMulticastAddress(universe)
                       : QHostAddress(p.remoteAddress);
        if (m_cid.size() != 16) m_cid = makeTestCid();
        m_running = true;
        emit started(tr("sACN → %1:%2, Universe %3, 优先级 %4")
                         .arg(m_target.toString()).arg(m_port).arg(universe).arg(priority));
        return true;
    }
    void stop() override {
        if (m_socket) { m_socket->close(); m_socket->deleteLater(); m_socket = nullptr; }
        m_running = false;
        emit stopped();
    }
    bool isRunning() const override { return m_running; }

    bool sendFrame(const LightFrame &f) override {
        if (!m_running || !m_socket) return false;
        QByteArray pkt = buildE131Packet(universe, f.dmxData, m_cid, priority, m_seq++, 0x00);
        qint64 sent = m_socket->writeDatagram(pkt, m_target, m_port);
        if (sent < 0) { emit sendError(m_socket->errorString()); return false; }
        m_sent++;
        emit frameSent(universe, 1);
        return true;
    }
    quint64 framesSent() const override { return m_sent; }

    quint16 universe = 1;
    quint8  priority = 100;

private:
    QUdpSocket *m_socket = nullptr;
    bool m_running = false;
    quint64 m_sent = 0;
    quint8 m_seq = 0;
    quint16 m_port = 5568;
    QHostAddress m_target;
    QByteArray m_cid;
};

// ArtRDM 输出 Sink (Art-Net OpCode 0x6000 ArtRDM)
class ArtRdmSinkImpl : public IProtocolSink {
public:
    ~ArtRdmSinkImpl() override { stop(); }
    ProtocolType protocolType() const override { return ProtocolType::ArtNet; }
    QString protocolName() const override { return "RDM (ArtRDM)"; }

    bool start(const ConnectionParams &p) override {
        if (m_running) stop();
        m_socket = new QUdpSocket(this);
        if (!m_socket->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
            emit errorOccurred(tr("RDM 发送 socket 绑定失败"));
            delete m_socket; m_socket = nullptr; return false;
        }
        m_target = QHostAddress(p.remoteAddress.isEmpty() ? "127.0.0.1" : p.remoteAddress);
        m_port = p.remotePort ? p.remotePort : ArtNet::DEFAULT_PORT;
        m_running = true;
        emit started(tr("ArtRDM → %1:%2").arg(m_target.toString()).arg(m_port));
        return true;
    }
    void stop() override {
        if (m_socket) { m_socket->close(); m_socket->deleteLater(); m_socket = nullptr; }
        m_running = false;
        emit stopped();
    }
    bool isRunning() const override { return m_running; }

    bool sendFrame(const LightFrame &f) override {
        if (!m_running || !m_socket) return false;
        QByteArray pkt;
        pkt.append("Art-Net", 8);            // ID
        pkt.append(char(0x00)); pkt.append(char(0x60));  // OpCode 0x6000 (LE)
        pkt.append(char(0x00)); pkt.append(char(0x0E));  // ProtVer 14
        pkt.append(char(0x01));              // RDM version
        // RDM 负载: 若帧带 rdmPayload 用之, 否则发一个 DISC_UNIQUE_BRANCH 发现包骨架
        if (f.hasRdm && !f.rdmPayload.isEmpty()) {
            pkt.append(f.rdmPayload);
        } else {
            QByteArray disc(26, 0);
            disc[0] = char(0xCC);            // Start code
            disc[1] = char(0x01);            // Sub start code (RDM)
            disc[2] = char(0x11);            // Message length (lo)
            disc[3] = char(0x00);            // Message length (hi)
            disc[4] = char(0x00);            // Dest UID (broadcast)
            disc[5] = char(0x00); disc[6] = char(0x00); disc[7] = char(0x00);
            disc[8] = char(0x00); disc[9] = char(0x00); disc[10] = char(0x00);
            disc[11] = char(0x00);           // Src UID placeholder
            disc[12] = char(0x00); disc[13] = char(0x00); disc[14] = char(0x00);
            disc[15] = char(0x00); disc[16] = char(0x00); disc[17] = char(0x00);
            disc[18] = char(0x00);           // Transaction
            disc[19] = char(0x00);           // Port ID
            disc[20] = char(0x00);           // Message count
            disc[21] = char(0x00);           // Sub device
            disc[22] = char(0x10);           // Command class: DISCOVERY
            disc[23] = char(0x00);           // PID: DISC_UNIQUE_BRANCH
            disc[24] = char(0x06);           // PDL lo
            disc[25] = char(0x00);           // PDL hi
            pkt.append(disc);
        }
        qint64 sent = m_socket->writeDatagram(pkt, m_target, m_port);
        if (sent < 0) { emit sendError(m_socket->errorString()); return false; }
        m_sent++;
        emit frameSent(0, 1);
        return true;
    }
    quint64 framesSent() const override { return m_sent; }

private:
    QUdpSocket *m_socket = nullptr;
    bool m_running = false;
    quint64 m_sent = 0;
    QHostAddress m_target;
    quint16 m_port = ArtNet::DEFAULT_PORT;
};

// ============================================================
// 辅助: 创建 SCADA 卡片 (带标题 + 可自定义 body)
// ============================================================
QFrame *ArtNetRecorderMainWindow::createCard(const QString &title, QWidget *content) {
    auto *card = new QFrame(content->parentWidget());
    card->setProperty("role", "card");
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setProperty("role", "card-title");
    v->addWidget(titleLabel);

    auto *sep = new QFrame(card);
    sep->setProperty("role", "card-header-sep");
    sep->setFrameShape(QFrame::HLine);
    v->addWidget(sep);

    auto *body = new QFrame(card);
    body->setProperty("role", "card-body");
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->addWidget(content);
    v->addWidget(body, 1);

    return card;
}

QLabel *ArtNetRecorderMainWindow::createValNumber(const QString &val, int size) {
    auto *l = new QLabel(val);
    if (size >= 20) l->setProperty("role", "val-number");
    else            l->setProperty("role", "val-number-sm");
    return l;
}
QLabel *ArtNetRecorderMainWindow::createValLabel(const QString &text) {
    auto *l = new QLabel(text);
    l->setProperty("role", "val-label");
    return l;
}

// 通道电平面板: 顶部参数行 (域/子网/Universe/帧率/总帧/时间码) + 16 通道柱状图
QWidget *ArtNetRecorderMainWindow::createChannelPanel(QLabel **lblInfo,
                                                      DmxChannelBars **bars,
                                                      QWidget *parent) {
    auto *wrap = new QWidget(parent);
    auto *v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(3);

    auto *info = new QLabel(tr("域 0 · 子网 0 · Universe 0 · 帧率 --\n总帧 0 · 时间码 --:--:--:--"), wrap);
    info->setStyleSheet("color: #7f96b0; font-size: 11px;");
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *b = new DmxChannelBars(wrap);

    v->addWidget(info);
    v->addWidget(b);
    *lblInfo = info;
    *bars = b;
    return wrap;
}

void ArtNetRecorderMainWindow::updateChannelPanel(QLabel *lblInfo, quint16 universe,
                                                  double fps, quint64 totalFrames,
                                                  qint64 relTsUs) {
    if (!lblInfo) return;

    // Art-Net 15 位 Universe: Net(7) | Sub-Net(4) | Universe(4)
    const int net   = (universe >> 8) & 0x7F;
    const int sub   = (universe >> 4) & 0x0F;
    const int uni   = universe & 0x0F;

    QString fpsText = fps > 0.05 ? QString::number(fps, 'f', 1) + " fps" : "--";

    QString tc = "--:--:--:--";
    if (relTsUs >= 0) {
        const qint64 totalSec = relTsUs / 1000000;
        const int h = int(totalSec / 3600);
        const int m = int((totalSec / 60) % 60);
        const int s = int(totalSec % 60);
        const int f = fps > 0.05
                          ? int((relTsUs % 1000000) * fps / 1000000.0)
                          : 0;
        tc = QString("%1:%2:%3:%4")
                 .arg(h, 2, 10, QChar('0'))
                 .arg(m, 2, 10, QChar('0'))
                 .arg(s, 2, 10, QChar('0'))
                 .arg(f, 2, 10, QChar('0'));
    }

    lblInfo->setText(QString("域 %1 · 子网 %2 · Universe %3 · 帧率 %4\n总帧 %5 · 时间码 %6")
                         .arg(net).arg(sub).arg(uni).arg(fpsText)
                         .arg(totalFrames).arg(tc));
}

// ===== 实时输入 / 实时输出页 =====
// 单行信息格式 (对应参考图): net / subnet / Universe / 帧率 / 总帧 / 时间码 / 时间长度
QString ArtNetRecorderMainWindow::formatLiveInfoLine(quint16 universe, double fps,
                                                     quint64 totalFrames, qint64 relTsUs) {
    const int net = (universe >> 8) & 0x7F;
    const int sub = (universe >> 4) & 0x0F;
    const int uni = universe & 0x0F;

    const QString fpsText = fps > 0.05 ? QString::number(fps, 'f', 1) + " fps" : "--";

    QString tc = "--:--:--:--";
    qint64 secs = 0;
    if (relTsUs > 0) {
        secs = relTsUs / 1000000;
        const int h = int(secs / 3600);
        const int m = int((secs / 60) % 60);
        const int s = int(secs % 60);
        const int f = fps > 0.05 ? int((relTsUs % 1000000) * fps / 1000000.0) : 0;
        tc = QString("%1:%2:%3:%4")
                 .arg(h, 2, 10, QChar('0'))
                 .arg(m, 2, 10, QChar('0'))
                 .arg(s, 2, 10, QChar('0'))
                 .arg(f, 2, 10, QChar('0'));
    }

    const int lh = int(secs / 3600);
    const int lm = int((secs / 60) % 60);
    const int ls = int(secs % 60);
    const QString dur = QString("%1:%2:%3")
                            .arg(lh, 2, 10, QChar('0'))
                            .arg(lm, 2, 10, QChar('0'))
                            .arg(ls, 2, 10, QChar('0'));

    return QString("net：%1    subnet：%2    Universe：%3 · 帧率: %4 · 总帧: %5    时间码: %6    时间长度：%7")
        .arg(net).arg(sub).arg(uni).arg(fpsText).arg(totalFrames).arg(tc).arg(dur);
}

// ============================================================
// 实时输入/输出: 多域(Universe)缓存与切换显示
// 解码器一次最多接收 16 个域, 每个域各自缓存最新 512 通道
// ============================================================
void ArtNetRecorderMainWindow::updateLiveUniverseCombo(bool isInput, quint16 universe) {
    QComboBox *cmb = isInput ? m_cmbLiveInUniverse : m_cmbLiveOutUniverse;
    if (!cmb || cmb->findData(universe) >= 0) return;

    // 首个域到达时清掉"等待数据"占位项
    if (cmb->count() == 1 && cmb->itemData(0).toUInt() == 0xFFFF)
        cmb->clear();

    const quint16 net = (universe >> 8) & 0x7F;
    const quint16 sub = (universe >> 4) & 0x0F;
    const quint16 uni = universe & 0x0F;
    cmb->addItem(tr("域 %1  (net %2 / subnet %3 / Universe %4)")
                     .arg(universe).arg(net).arg(sub).arg(uni), universe);

    // Art-Net 4 最多 32768 个域, 下拉列表可容纳大量条目
    cmb->setMaxVisibleItems(20);
    QLabel *cnt = isInput ? m_lblLiveInUniCount : m_lblLiveOutUniCount;
    if (cnt) {
        const int n = (isInput ? m_liveInMap : m_liveOutMap).size();
        cnt->setText(tr("已收 %1 个域").arg(n));
    }

    quint16 &sel = isInput ? m_liveInUniverse : m_liveOutUniverse;
    if (sel == 0xFFFF) {
        // 自动选中第一个到达的域
        sel = universe;
        cmb->setCurrentIndex(cmb->count() - 1);
    }
}

void ArtNetRecorderMainWindow::feedLiveUniverse(bool isInput, quint16 universe,
                                                const QByteArray &dmx, qint64 tsUs) {
    QHash<quint16, LiveUniverseData> &map = isInput ? m_liveInMap : m_liveOutMap;
    LiveUniverseData &d = map[universe];
    d.dmx = dmx;
    d.frames++;
    if (d.firstTsUs == 0) d.firstTsUs = tsUs;
    d.lastTsUs = tsUs;
    if (d.lastTsUs > d.firstTsUs)
        d.fps = double(d.frames - 1) * 1000000.0 / double(d.lastTsUs - d.firstTsUs);

    if (isInput) m_liveInActiveUs = tsUs;
    else         m_liveOutActiveUs = tsUs;

    updateLiveUniverseCombo(isInput, universe);

    const quint16 sel = isInput ? m_liveInUniverse : m_liveOutUniverse;
    if (sel == universe) applyLiveUniverse(isInput);
}

void ArtNetRecorderMainWindow::applyLiveUniverse(bool isInput) {
    DmxChannelBars *bars = isInput ? m_liveInBars : m_liveOutBars;
    QLabel *info = isInput ? m_liveInInfo : m_liveOutInfo;
    if (!bars) return;

    const quint16 sel = isInput ? m_liveInUniverse : m_liveOutUniverse;
    const QHash<quint16, LiveUniverseData> &map = isInput ? m_liveInMap : m_liveOutMap;
    auto it = map.constFind(sel);
    if (it == map.constEnd()) {
        bars->clear();
        if (info) info->setText(formatLiveInfoLine(0, 0.0, 0, 0));
        return;
    }

    bars->setData(it->dmx);
    if (info)
        info->setText(formatLiveInfoLine(sel, it->fps, it->frames,
                                         it->lastTsUs - it->firstTsUs));
}

QWidget *ArtNetRecorderMainWindow::buildPageLiveInput()  { return buildLivePage(true); }
QWidget *ArtNetRecorderMainWindow::buildPageLiveOutput() { return buildLivePage(false); }

QWidget *ArtNetRecorderMainWindow::buildLivePage(bool isInput) {
    auto *page = new QWidget;
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    // --- 顶部信息卡 ---
    auto *infoBody = new QWidget;
    auto *infoLay = new QVBoxLayout(infoBody);
    infoLay->setContentsMargins(0, 0, 0, 0);
    auto *info = new QLabel(formatLiveInfoLine(0, 0.0, 0, 0));
    info->setStyleSheet("color: #8fb3d9; font-size: 14px; font-family: Consolas;");
    info->setWordWrap(true);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLay->addWidget(info);
    root->addWidget(createCard(isInput ? tr("实时输入状态") : tr("实时输出状态"), infoBody));

    // --- 通道卡 (分页) ---
    auto *chanBody = new QWidget;
    auto *chanLay = new QVBoxLayout(chanBody);
    chanLay->setContentsMargins(0, 0, 0, 0);
    chanLay->setSpacing(8);

    auto *navRow = new QHBoxLayout;
    auto *btnPrev = new QPushButton(tr("◀ 上一页"));  btnPrev->setProperty("action", "icon");
    auto *btnNext = new QPushButton(tr("下一页 ▶"));  btnNext->setProperty("action", "icon");
    auto *pageLbl = new QLabel;
    pageLbl->setAlignment(Qt::AlignCenter);
    pageLbl->setStyleSheet("color: #00d4ff; font-family: Consolas; font-size: 12px;");

    // 域切换 (Art-Net 4 最多 32768 个域, 用下拉框 + 跳转框选择要监视的域)
    auto *cmbUni = new QComboBox;
    cmbUni->setMinimumWidth(260);
    cmbUni->setMaxVisibleItems(20);
    cmbUni->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    cmbUni->addItem(tr("域 -- (等待数据...)"), 0xFFFF);

    auto *spinJump = new QSpinBox;
    spinJump->setRange(0, 32767);
    spinJump->setKeyboardTracking(false);
    spinJump->setPrefix(tr("跳转域 "));
    spinJump->setFixedWidth(110);
    spinJump->setToolTip(tr("输入 Universe 编号后回车, 直接切换到该域 (需已收到该域数据)"));

    auto *uniCount = new QLabel(tr("已收 0 个域"));
    uniCount->setStyleSheet("color: #8fb3d9; font-size: 11px;");

    navRow->addWidget(new QLabel(tr("域:")));
    navRow->addWidget(cmbUni);
    navRow->addWidget(spinJump);
    navRow->addWidget(uniCount);
    navRow->addStretch();
    navRow->addWidget(btnPrev);
    navRow->addWidget(pageLbl);
    navRow->addWidget(btnNext);
    navRow->addStretch();
    chanLay->addLayout(navRow);

    connect(cmbUni, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, isInput](int){
                QComboBox *c = isInput ? m_cmbLiveInUniverse : m_cmbLiveOutUniverse;
                if (!c) return;
                const quint16 u = quint16(c->currentData().toUInt());
                if (u == 0xFFFF) return;
                if (isInput) m_liveInUniverse = u; else m_liveOutUniverse = u;
                applyLiveUniverse(isInput);
            });

    // 输入 Universe 编号直接跳转
    connect(spinJump, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, isInput](int v){
                QComboBox *c = isInput ? m_cmbLiveInUniverse : m_cmbLiveOutUniverse;
                if (!c) return;
                const int idx = c->findData(quint32(v));
                if (idx >= 0) c->setCurrentIndex(idx);
            });

    auto *bars = new DmxChannelBars(chanBody);
    chanLay->addWidget(bars, 1);
    root->addWidget(createCard(tr("通道值 (每页 16 通道) · 鼠标拖动推子可直接设定通道值"), chanBody), 1);

    // 拖动推子时把结果写入日志 (限流 150ms, 避免刷屏)
    auto *logThrottle = new QElapsedTimer;
    logThrottle->start();
    connect(bars, &DmxChannelBars::channelChanged, this,
            [this, logThrottle](int ch, int v){
                if (logThrottle->elapsed() < 150) return;
                logThrottle->restart();
                appendLog(tr("推子 CH%1 = %2").arg(ch + 1).arg(v), "info");
            });

    // 绑定成员
    if (isInput) {
        m_liveInInfo = info; m_liveInPage = pageLbl; m_liveInBars = bars;
        m_cmbLiveInUniverse = cmbUni;
        m_lblLiveInUniCount = uniCount;
    } else {
        m_liveOutInfo = info; m_liveOutPage = pageLbl; m_liveOutBars = bars;
        m_cmbLiveOutUniverse = cmbUni;
        m_lblLiveOutUniCount = uniCount;
    }

    auto applyPage = [this, isInput]{
        const int perPage = 16;
        const int total = 512;
        const int pages = (total + perPage - 1) / perPage;
        int &idx = isInput ? m_liveInPageIdx : m_liveOutPageIdx;
        idx = qBound(0, idx, pages - 1);
        DmxChannelBars *b = isInput ? m_liveInBars : m_liveOutBars;
        QLabel *lbl = isInput ? m_liveInPage : m_liveOutPage;
        if (b) b->setChannelWindow(idx * perPage, perPage);
        if (lbl)
            lbl->setText(tr("CH %1 - %2 / 512     第 %3 / %4 页")
                             .arg(idx * perPage + 1).arg(idx * perPage + perPage)
                             .arg(idx + 1).arg(pages));
    };

    connect(btnPrev, &QPushButton::clicked, this, [this, applyPage, isInput]{
        int &idx = isInput ? m_liveInPageIdx : m_liveOutPageIdx;
        --idx;
        applyPage();
    });
    connect(btnNext, &QPushButton::clicked, this, [this, applyPage, isInput]{
        int &idx = isInput ? m_liveInPageIdx : m_liveOutPageIdx;
        ++idx;
        applyPage();
    });

    if (isInput) m_liveInPageIdx = 0; else m_liveOutPageIdx = 0;
    applyPage();
    return page;
}

// ============================================================
// 构造
// ============================================================
ArtNetRecorderMainWindow::ArtNetRecorderMainWindow(QWidget *parent)
    : TestWindowBase(parent)
{
    // 无标题窗口: 标题栏留空, 仅保留系统框架

    // 启动定时刷新 (UI 数字 + 顶部时钟)
    m_uiTimer = new QTimer(this);
    m_uiTimer->setInterval(250);
    connect(m_uiTimer, &QTimer::timeout, this, &ArtNetRecorderMainWindow::tickUpdate);
    m_uiTimer->start();
}

ArtNetRecorderMainWindow::~ArtNetRecorderMainWindow() {
    if (m_mtcTimer) { m_mtcTimer->stop(); }
#ifdef Q_OS_WIN
    if (m_midiOutHandle) {
        auto h = reinterpret_cast<HMIDIOUT>(m_midiOutHandle);
        midiOutReset(h);
        midiOutClose(h);
        m_midiOutHandle = nullptr;
    }
#endif
    if (m_rdmReceiver) { m_rdmReceiver->stop(); delete m_rdmReceiver; m_rdmReceiver = nullptr; }
    if (m_clock)    { m_clock->stop();    delete m_clock;    m_clock = nullptr; }
    if (m_receiver) { delete m_receiver;  m_receiver = nullptr; }
    if (m_engine)   { m_engine->stop();   delete m_engine;   m_engine = nullptr; }
}

// ============================================================
// 重写 TestWindowBase::setupContent — 搭建左侧导航 + 右侧 Stacked
// ============================================================
void ArtNetRecorderMainWindow::setupContent() {
    resize(1280, 800);

    // 复用 TestWindowBase 已安装在 m_contentArea 上的 QVBoxLayout (0 边距)
    // 注意: 不能对同一 QWidget 重复 new QLayout / delete 已安装的 layout,
    //       否则 QWidget 内部指针悬空, 首次绘制即崩溃 (0xC0000005)
    auto *outer = qobject_cast<QVBoxLayout*>(m_contentArea->layout());
    if (!outer) {
        outer = new QVBoxLayout(m_contentArea);
    }
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // === 顶部模式指示 (Live/Simulation) ===
    auto *modeRow = new QFrame(m_contentArea);
    modeRow->setProperty("role", "topbar");
    auto *modeLayout = new QHBoxLayout(modeRow);
    modeLayout->setContentsMargins(16, 0, 16, 0);

    auto *breadcrumb = new QLabel(tr("ArtNet Recorder  /  录制仪表盘"), modeRow);
    breadcrumb->setProperty("role", "breadcrumb");
    modeLayout->addWidget(breadcrumb);

    modeLayout->addStretch();

    auto *modeBadge = new QLabel(tr("● SIMULATION"), modeRow);
    modeBadge->setProperty("role", "mode-indicator");
    modeBadge->setProperty("mode", "sim");
    modeLayout->addWidget(modeBadge);

    m_lblTopClock = new QLabel(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"), modeRow);
    m_lblTopClock->setProperty("role", "clock");
    modeLayout->addWidget(m_lblTopClock);

    outer->addWidget(modeRow);

    // === 左侧导航 + 右侧 Stacked ===
    auto *navStackRow = new QWidget(m_contentArea);
    auto *navStackLayout = new QHBoxLayout(navStackRow);
    navStackLayout->setContentsMargins(0, 0, 0, 0);
    navStackLayout->setSpacing(0);

    m_navList = new QListWidget(navStackRow);
    m_navList->setFixedWidth(210);
    m_navList->setProperty("role", "sidebar");
    m_navList->setIconSize(QSize(18, 18));

    m_stack = new QStackedWidget(navStackRow);

    navStackLayout->addWidget(m_navList);
    navStackLayout->addWidget(m_stack, 1);
    outer->addWidget(navStackRow, 1);

    // === 构建 7 个页面 ===
    // 每个页面套一层 QScrollArea: 窗口变小时页面跟随缩放 (widgetResizable),
    // 内容缩到最小尺寸以下时出现滚动条, 避免卡片里的文字被裁掉.
    auto wrapScroll = [](QWidget *page) -> QWidget* {
        auto *sa = new QScrollArea();
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        sa->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        sa->setWidget(page);
        return sa;
    };
    m_stack->addWidget(wrapScroll(buildPageDashboard()));
    m_stack->addWidget(wrapScroll(buildPageSyncClock()));
    m_stack->addWidget(wrapScroll(buildPageInputData()));   // 协议输入 (ArtNet/DMX/NPU/RDM/ArtRecFile)
    m_stack->addWidget(wrapScroll(buildPageOutputData()));  // 协议输出 (ArtNet/DMX/NPU/RDM)
    m_stack->addWidget(buildPageLiveInput());               // 实时输入数据 (独立页)
    m_stack->addWidget(buildPageLiveOutput());              // 实时输出数据 (独立页)
    m_stack->addWidget(wrapScroll(buildPageNetTest(ProtocolType::ArtNet)));  // Art-Net 收发测试
    m_stack->addWidget(wrapScroll(buildPageNetTest(ProtocolType::Npu)));     // NPU 收发测试
    m_stack->addWidget(wrapScroll(buildPageNetTest(ProtocolType::Sacn)));    // sACN 收发测试
    m_stack->addWidget(wrapScroll(buildPageRdmTest()));                      // RDM 请求/应答测试
    m_stack->addWidget(wrapScroll(buildPageDmxTest()));     // DMX512 专项测试
    m_stack->addWidget(wrapScroll(buildPageArtToDmx()));    // Art-Net → DMX512 转换测试
    m_stack->addWidget(wrapScroll(buildPageDmxToArt()));    // DMX512 → Art-Net 转换测试
    m_stack->addWidget(wrapScroll(buildPageNpuToDmx()));    // NPU → DMX512 转换测试
    m_stack->addWidget(wrapScroll(buildPageOutput()));      // 转发输出 (老页面, 保留)
    m_stack->addWidget(wrapScroll(buildPageFileMgr()));
    m_stack->addWidget(wrapScroll(buildPageSystem()));

    // === 导航项 ===
    QList<QPair<QString, int>> navItems = {
        {tr("📊  录制仪表盘"),  0},
        {tr("⏱   时间码源"),    1},
        {tr("📥  输入数据"),    2},
        {tr("📤  输出数据"),    3},
        {tr("📶  实时输入"),    4},
        {tr("📶  实时输出"),    5},
        {tr("🌐  Art-Net 测试"),6},
        {tr("🌐  NPU 测试"),    7},
        {tr("🌐  sACN 测试"),   8},
        {tr("🔵  RDM 测试"),    9},
        {tr("🔌  DMX512 测试"), 10},
        {tr("🔄  Art-Net 转 DMX"), 11},
        {tr("🔄  DMX 转 Art-Net"), 12},
        {tr("🔄  NPU 转 DMX"),  13},
        {tr("📡  转发输出"),    14},
        {tr("💾  文件管理"),    15},
        {tr("⚙   系统状态"),    16},
    };
    for (const auto &pair : navItems) {
        auto *item = new QListWidgetItem(pair.first);
        item->setData(Qt::UserRole, pair.second);
        m_navList->addItem(item);
        m_navBaseTexts.append(pair.first);
    }
    connect(m_navList, &QListWidget::currentRowChanged,
            this, &ArtNetRecorderMainWindow::onNavChanged);
    m_navList->setCurrentRow(qEnvironmentVariableIntValue("ARTNET_UI_START_ROW"));

    // 启动时检测第三方解码库
#if defined(ARTNET_WITH_FFMPEG)
    logInfo(tr("FFmpeg 解码库已检测到: %1 (avcodec %2 / avformat %3)")
                .arg(QString::fromLatin1(av_version_info()))
                .arg(LIBAVCODEC_VERSION_MAJOR).arg(LIBAVFORMAT_VERSION_MAJOR));
#else
    appendLog(tr("未检测到 FFmpeg 解码库, 音视频时间码/波形不可用 (NO_FFMPEG)"), "warn");
#endif

    // 配置持久化: 启动加载, 退出保存
    connect(qApp, &QCoreApplication::aboutToQuit, this, &ArtNetRecorderMainWindow::saveUiConfig);
    loadUiConfig();

    setStatusNeutral(tr("就绪 — ArtNet Recorder SCADA"));
}

// ============================================================
// 配置文件 (ui_config.json, 与 exe 同目录)
// ============================================================
QString ArtNetRecorderMainWindow::uiConfigPath() const {
    return QCoreApplication::applicationDirPath() + "/ui_config.json";
}

void ArtNetRecorderMainWindow::saveUiConfig() {
    QJsonObject o;
    o.insert("version", 1);

    QJsonObject win;
    win.insert("width", width());
    win.insert("height", height());
    o.insert("window", win);

    if (m_cmbClockType)      o.insert("clockTypeIndex", m_cmbClockType->currentIndex());
    if (m_spinFps)           o.insert("expectFps", m_spinFps->value());
    if (m_spinArtNetPort)    o.insert("artnetPort", m_spinArtNetPort->value());
    if (m_editBindAddr)      o.insert("bindAddr", m_editBindAddr->text());
    if (m_editMediaFile)     o.insert("mediaFile", m_editMediaFile->text());
    if (m_chkLoopOnEnd)      o.insert("loopOnEnd", m_chkLoopOnEnd->isChecked());
    if (m_spinFileFps)       o.insert("mediaFps", m_spinFileFps->value());
    if (m_chkFileRateHeader) o.insert("fileRateHeader", m_chkFileRateHeader->isChecked());
    if (m_cmbMidiDevice)     o.insert("midiIn", m_cmbMidiDevice->currentText());
    if (m_cmbMidiOut)        o.insert("midiOut", m_cmbMidiOut->currentText());
    if (m_spinMidiChannel)   o.insert("midiChannel", m_spinMidiChannel->value());

    QFile f(uiConfigPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.close();
        logInfo(tr("配置已保存: %1").arg(uiConfigPath()));
    }
}

void ArtNetRecorderMainWindow::loadUiConfig() {
    QFile f(uiConfigPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;

    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    if (o.isEmpty()) return;

    const QJsonObject win = o.value("window").toObject();
    if (win.contains("width") && win.contains("height"))
        resize(qMax(1024, win.value("width").toInt()),
               qMax(680, win.value("height").toInt()));

    if (m_spinFps)           m_spinFps->setValue(o.value("expectFps").toInt(m_spinFps->value()));
    if (m_spinArtNetPort)    m_spinArtNetPort->setValue(o.value("artnetPort").toInt(m_spinArtNetPort->value()));
    if (m_editBindAddr)      m_editBindAddr->setText(o.value("bindAddr").toString());
    if (m_editMediaFile)     m_editMediaFile->setText(o.value("mediaFile").toString());
    if (m_chkLoopOnEnd)      m_chkLoopOnEnd->setChecked(o.value("loopOnEnd").toBool());
    if (m_spinFileFps)       m_spinFileFps->setValue(o.value("mediaFps").toInt(m_spinFileFps->value()));
    if (m_chkFileRateHeader) m_chkFileRateHeader->setChecked(o.value("fileRateHeader").toBool());
    if (m_spinMidiChannel)   m_spinMidiChannel->setValue(o.value("midiChannel").toInt(m_spinMidiChannel->value()));

    auto selectByText = [](QComboBox *combo, const QString &text) {
        if (!combo || text.isEmpty()) return;
        const int idx = combo->findText(text);
        if (idx >= 0) combo->setCurrentIndex(idx);
    };
    selectByText(m_cmbMidiDevice, o.value("midiIn").toString());
    selectByText(m_cmbMidiOut, o.value("midiOut").toString());

    if (m_cmbClockType) {
        const int idx = o.value("clockTypeIndex").toInt(0);
        if (idx >= 0 && idx < m_cmbClockType->count()) m_cmbClockType->setCurrentIndex(idx);
    }
    updateClockConfigRows();

    logInfo(tr("已加载配置: %1").arg(uiConfigPath()));
}

void ArtNetRecorderMainWindow::onNavChanged(int row) {
    if (row >= 0 && row < m_stack->count()) m_stack->setCurrentIndex(row);
}

// ============================================================
// 页面 1: 录制仪表盘 — 6 张卡片栅格
// 左上: 录制引擎状态 / 右上: 时间码源状态
// 左中: ArtNet DMX 实时 / 右中: 转发输出状态
// 左下: 配置速览 / 右下: 文件管理快速入口
// ============================================================
QWidget *ArtNetRecorderMainWindow::buildPageDashboard() {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setHorizontalSpacing(14);
    root->setVerticalSpacing(14);

    // === 卡片 A: 录制引擎状态 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(6);
        form->setLabelAlignment(Qt::AlignRight);

        m_lblEngineState = new QLabel(tr("IDLE"));
        m_lblEngineState->setProperty("role", "badge");
        m_lblEngineState->setProperty("badge", "idle");
        form->addRow(createValLabel(tr("引擎状态:")), m_lblEngineState);

        auto *frameRow = new QHBoxLayout();
        m_lblRecordFrames = createValNumber(tr("0"));
        frameRow->addWidget(m_lblRecordFrames);
        frameRow->addWidget(createValLabel(tr("帧")), 0, Qt::AlignBottom);
        frameRow->addStretch();
        form->addRow(createValLabel(tr("已录制帧:")), [&]{ auto *w=new QWidget(); w->setLayout(frameRow); return w; }());

        auto *secRow = new QHBoxLayout();
        m_lblRecordSecs = createValNumber(tr("0.00"), 16);
        secRow->addWidget(m_lblRecordSecs);
        secRow->addWidget(createValLabel(tr("秒")), 0, Qt::AlignBottom);
        secRow->addStretch();
        form->addRow(createValLabel(tr("录制时长:")), [&]{ auto *w=new QWidget(); w->setLayout(secRow); return w; }());

        auto *btnRow = new QHBoxLayout();
        m_btnStart->setText(tr("▶ 开始录制"));
        m_btnStop->setText(tr("■ 停止录制"));
        btnRow->addWidget(m_btnStart);
        btnRow->addWidget(m_btnStop);
        btnRow->addWidget(m_btnReset);
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        root->addWidget(createCard(tr("录制引擎"), body), 0, 0);
    }

    // === 卡片 B: 时间码源 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(6);
        form->setLabelAlignment(Qt::AlignRight);

        m_lblClockType = new QLabel(tr("--"));
        m_lblClockType->setProperty("role", "badge");
        m_lblClockType->setProperty("badge", "idle");
        form->addRow(createValLabel(tr("时钟类型:")), m_lblClockType);

        auto *fpsRow = new QHBoxLayout();
        m_lblClockFps = createValNumber(tr("--"), 16);
        fpsRow->addWidget(m_lblClockFps);
        fpsRow->addWidget(createValLabel(tr("fps")), 0, Qt::AlignBottom);
        fpsRow->addStretch();
        form->addRow(createValLabel(tr("帧率:")), [&]{ auto *w=new QWidget(); w->setLayout(fpsRow); return w; }());

        auto *tsRow = new QHBoxLayout();
        m_lblClockTs = createValNumber(tr("--"), 14);
        tsRow->addWidget(m_lblClockTs);
        tsRow->addStretch();
        form->addRow(createValLabel(tr("时间戳 (us):")), [&]{ auto *w=new QWidget(); w->setLayout(tsRow); return w; }());

        auto *frameIdxRow = new QHBoxLayout();
        m_lblClockFrameIdx = createValNumber(tr("--"), 16);
        frameIdxRow->addWidget(m_lblClockFrameIdx);
        frameIdxRow->addStretch();
        form->addRow(createValLabel(tr("帧序号:")), [&]{ auto *w=new QWidget(); w->setLayout(frameIdxRow); return w; }());

        auto *tickRow = new QHBoxLayout();
        m_lblClockCount = createValNumber(tr("0"), 14);
        tickRow->addWidget(m_lblClockCount);
        tickRow->addStretch();
        form->addRow(createValLabel(tr("总 Tick:")), [&]{ auto *w=new QWidget(); w->setLayout(tickRow); return w; }());

        root->addWidget(createCard(tr("时间码源"), body), 0, 1);
    }

    // === 卡片 C: ArtNet DMX 实时 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(6);
        form->setLabelAlignment(Qt::AlignRight);

        m_lblArtNetStatus = new QLabel(tr("未启动"));
        m_lblArtNetStatus->setProperty("role", "badge");
        m_lblArtNetStatus->setProperty("badge", "warn");
        form->addRow(createValLabel(tr("接收器:")), m_lblArtNetStatus);

        m_lblArtNetPort = new QLabel(tr("6454"));
        m_lblArtNetPort->setProperty("role", "val-number-sm");
        form->addRow(createValLabel(tr("监听端口:")), m_lblArtNetPort);

        auto *pktRow = new QHBoxLayout();
        m_lblArtNetPkts = createValNumber(tr("0"), 16);
        pktRow->addWidget(m_lblArtNetPkts);
        pktRow->addStretch();
        form->addRow(createValLabel(tr("包计数:")), [&]{ auto *w=new QWidget(); w->setLayout(pktRow); return w; }());

        m_lblLastUniverse = new QLabel(tr("--"));
        m_lblLastUniverse->setProperty("role", "val-number-sm");
        form->addRow(createValLabel(tr("最近 Universe:")), m_lblLastUniverse);

        m_lblLastUniverseData = new QLabel(tr("--"));
        m_lblLastUniverseData->setStyleSheet("font-family: Consolas, monospace; color: #00d4ff;");
        m_lblLastUniverseData->setWordWrap(true);
        m_lblLastUniverseData->setToolTip(tr("最近收到的 DMX 前 16 字节 (HEX)"));
        form->addRow(createValLabel(tr("数据预览:")), m_lblLastUniverseData);

        root->addWidget(createCard(tr("ArtNet DMX 实时"), body), 1, 0);
    }

    // === 卡片 D: 转发输出状态 ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(8);

        m_lblOutputStatus = new QLabel(tr("未配置"));
        m_lblOutputStatus->setProperty("role", "badge");
        m_lblOutputStatus->setProperty("badge", "idle");
        v->addWidget(m_lblOutputStatus, 0, Qt::AlignLeft);

        // 3 个预设输出目标卡片 (纯展示, 连接时点亮)
        auto *grid = new QGridLayout();
        grid->setSpacing(8);

        auto makeTargetRow = [](const QString &name, const QString &colorStyle) {
            auto *box = new QFrame();
            box->setObjectName("targetBox");   // 限定选择器, 避免 QFrame 样式命中内部 QLabel
            box->setStyleSheet(QString(
                "QFrame#targetBox { background-color: #122034; border: 1px solid #2a3f55; border-radius: 4px; padding: 6px; }"
            ));
            auto *inner = new QVBoxLayout(box);
            inner->setContentsMargins(8, 6, 8, 6);
            auto *n = new QLabel(name);
            n->setStyleSheet(colorStyle.isEmpty() ? "color: #8899aa; font-weight: bold;" : colorStyle);
            inner->addWidget(n);
            return box;
        };

        grid->addWidget(makeTargetRow(tr("ArtNet Sender"), "color: #00d4ff; font-weight: bold;"), 0, 0);
        grid->addWidget(makeTargetRow(tr("DMX512 Sender"), "color: #ffa726; font-weight: bold;"), 0, 1);
        grid->addWidget(makeTargetRow(tr("NPU Sender"), "color: #ba68c8; font-weight: bold;"), 0, 2);

        v->addLayout(grid);
        v->addStretch();

        root->addWidget(createCard(tr("转发输出"), body), 1, 1);
    }

    // === 卡片 E: 文件管理快速入口 ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(8);

        auto *btnOpen = new QPushButton(tr("📂 打开 .artrec 文件"), body);
        btnOpen->setProperty("action", "icon");
        connect(btnOpen, &QPushButton::clicked, this, &ArtNetRecorderMainWindow::onBtnOpenFileClicked);
        v->addWidget(btnOpen);

        auto *btnPlay = new QPushButton(tr("▶ 模拟回放文件"), body);
        btnPlay->setProperty("action", "icon");
        connect(btnPlay, &QPushButton::clicked, this, &ArtNetRecorderMainWindow::onBtnPlayFileClicked);
        v->addWidget(btnPlay);

        v->addStretch();
        root->addWidget(createCard(tr("文件管理"), body), 2, 0);
    }

    // === 卡片 F: 时钟 Tick 分布 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(8);
        form->setLabelAlignment(Qt::AlignRight);

        auto *fpsRow2 = new QHBoxLayout();
        auto *lblEffFps = createValNumber(tr("--"), 16);
        fpsRow2->addWidget(lblEffFps);
        fpsRow2->addWidget(createValLabel(tr("fps")), 0, Qt::AlignBottom);
        fpsRow2->addStretch();
        form->addRow(createValLabel(tr("有效频率:")), [&]{ auto *w=new QWidget(); w->setLayout(fpsRow2); return w; }());

        auto *statRow = new QHBoxLayout();
        auto *lblPktsSec = createValNumber(tr("0"), 16);
        statRow->addWidget(lblPktsSec);
        statRow->addWidget(createValLabel(tr("包/秒")), 0, Qt::AlignBottom);
        statRow->addStretch();
        form->addRow(createValLabel(tr("ArtNet 接收率:")), [&]{ auto *w=new QWidget(); w->setLayout(statRow); return w; }());

        // 保存引用给 tickUpdate — 简化: 不存成员, 用已有 m_lblArtNetPkts + m_lblClockFps 近似
        // 实际有效 fps 计算太复杂, 先略过

        form->addRow(createValLabel(tr("提示:")), new QLabel(tr("时间码源页可切换更多时钟类型")));

        root->addWidget(createCard(tr("运行统计"), body), 2, 1);
    }

    // 3 行 x 2 列栅格, 等宽
    root->setColumnStretch(0, 1);
    root->setColumnStretch(1, 1);
    root->setRowStretch(0, 1);
    root->setRowStretch(1, 1);
    root->setRowStretch(2, 1);

    return page;
}

// ============================================================
// 页面 2: 时间码源 — 时钟参数配置面板
// ============================================================
QWidget *ArtNetRecorderMainWindow::buildPageSyncClock() {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setHorizontalSpacing(14);
    root->setVerticalSpacing(14);

    // === 配置卡片 (时间码源单选) ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(12);

        auto *selForm = new QFormLayout();
        selForm->setContentsMargins(0, 0, 0, 0);
        selForm->setSpacing(12);
        selForm->setLabelAlignment(Qt::AlignRight);

        m_cmbClockType = new QComboBox(body);
        m_cmbClockType->addItem(tr("Internal Timer (内部定时器)"),
                                int(SyncClockType::InternalTimer));
        m_cmbClockType->addItem(tr("ArtNet TimeCode (OP 0x0D)"),
                                int(SyncClockType::ArtNetTimeCode));
        m_cmbClockType->addItem(tr("MIDI Time Code"),
                                int(SyncClockType::MidiMTC));
        m_cmbClockType->addItem(tr("Audio File / LTC (音频时间码)"),
                                int(SyncClockType::AudioFile));
        m_cmbClockType->addItem(tr("Video File (视频时间码)"),
                                int(SyncClockType::VideoFile));
        m_cmbClockType->addItem(tr("Timecode File (.tc/.lts)"),
                                int(SyncClockType::TimecodeFile));
        selForm->addRow(tr("时间码源 (单选):"), m_cmbClockType);

        auto *hint = new QLabel(tr("一次只能选择一个时间码源, 切换后仅显示该源需要的配置项"));
        hint->setStyleSheet("color: #4b6584; font-size: 11px;");
        hint->setWordWrap(true);
        selForm->addRow(QString(), hint);

        v->addLayout(selForm);

        // --- 配置组 1: Internal Timer ---
        m_grpClockTimer = new QWidget(body);
        {
            auto *f = new QFormLayout(m_grpClockTimer);
            f->setContentsMargins(0, 0, 0, 0);
            f->setSpacing(12);
            f->setLabelAlignment(Qt::AlignRight);

            m_spinFps = new QSpinBox(m_grpClockTimer);
            m_spinFps->setRange(1, 120);
            m_spinFps->setValue(30);
            f->addRow(tr("期望帧率 (fps):"), m_spinFps);
        }
        v->addWidget(m_grpClockTimer);

        // --- 配置组 2: ArtNet TimeCode ---
        m_grpClockArtNet = new QWidget(body);
        {
            auto *f = new QFormLayout(m_grpClockArtNet);
            f->setContentsMargins(0, 0, 0, 0);
            f->setSpacing(12);
            f->setLabelAlignment(Qt::AlignRight);

            m_editBindAddr = new QLineEdit(m_grpClockArtNet);
            m_editBindAddr->setText("0.0.0.0");
            f->addRow(tr("绑定地址:"), m_editBindAddr);

            m_spinArtNetPort = new QSpinBox(m_grpClockArtNet);
            m_spinArtNetPort->setRange(1024, 65535);
            m_spinArtNetPort->setValue(6454);
            f->addRow(tr("ArtNet 端口:"), m_spinArtNetPort);
        }
        v->addWidget(m_grpClockArtNet);

        // --- 配置组 3: MIDI Time Code ---
        m_grpClockMidi = new QWidget(body);
        {
            auto *f = new QFormLayout(m_grpClockMidi);
            f->setContentsMargins(0, 0, 0, 0);
            f->setSpacing(12);
            f->setLabelAlignment(Qt::AlignRight);

            auto *midiRow = new QHBoxLayout();
            m_cmbMidiDevice = new QComboBox(m_grpClockMidi);
            m_btnRefreshMidi = new QPushButton(tr("刷新"), m_grpClockMidi);
            m_btnRefreshMidi->setProperty("action", "icon");
            connect(m_btnRefreshMidi, &QPushButton::clicked, this,
                    &ArtNetRecorderMainWindow::refreshMidiDevices);
            midiRow->addWidget(m_cmbMidiDevice, 1);
            midiRow->addWidget(m_btnRefreshMidi);
            f->addRow(tr("MIDI 输入设备:"),
                      [&]{ auto *w = new QWidget(m_grpClockMidi); w->setLayout(midiRow); return w; }());

            m_spinMidiChannel = new QSpinBox(m_grpClockMidi);
            m_spinMidiChannel->setRange(0, 15);
            f->addRow(tr("MTC 通道 (0-15):"), m_spinMidiChannel);

            // --- 内建 MTC 模拟发送: 往虚拟端口发 quarter-frame, 用于验证输入链路 ---
            auto *outRow = new QHBoxLayout();            m_cmbMidiOut = new QComboBox(m_grpClockMidi);
            auto *btnRefreshOut = new QPushButton(tr("刷新"), m_grpClockMidi);
            btnRefreshOut->setProperty("action", "icon");
            connect(btnRefreshOut, &QPushButton::clicked, this,
                    &ArtNetRecorderMainWindow::refreshMidiOutDevices);
            outRow->addWidget(m_cmbMidiOut, 1);
            outRow->addWidget(btnRefreshOut);
            f->addRow(tr("模拟输出端口:"),
                      [&]{ auto *w = new QWidget(m_grpClockMidi); w->setLayout(outRow); return w; }());

            auto *tcRow = new QHBoxLayout();
            tcRow->setSpacing(6);
            auto makeSpin = [&](int maxV) {
                auto *sp = new QSpinBox(m_grpClockMidi);
                sp->setRange(0, maxV);
                sp->setValue(0);
                sp->setFixedWidth(54);
                return sp;
            };
            m_spinMtcH = makeSpin(23);
            m_spinMtcM = makeSpin(59);
            m_spinMtcS = makeSpin(59);
            m_spinMtcF = makeSpin(29);
            const char *tcLabels[4] = {"H", "M", "S", "F"};
            QSpinBox *tcSpins[4] = {m_spinMtcH, m_spinMtcM, m_spinMtcS, m_spinMtcF};
            for (int i = 0; i < 4; ++i) {
                auto *lb = new QLabel(tcLabels[i], m_grpClockMidi);
                lb->setStyleSheet("color:#7f8fa6;font-size:11px;");
                tcRow->addWidget(lb);
                tcRow->addWidget(tcSpins[i]);
            }
            tcRow->addStretch();
            f->addRow(tr("起始时间码:"),
                      [&]{ auto *w = new QWidget(m_grpClockMidi); w->setLayout(tcRow); return w; }());

            m_btnMtcSend = new QPushButton(tr("▶ 开始模拟发送"), m_grpClockMidi);
            m_btnMtcSend->setProperty("action", "start");
            connect(m_btnMtcSend, &QPushButton::clicked, this,
                    &ArtNetRecorderMainWindow::toggleMtcSimulation);
            f->addRow(tr("MTC 模拟:"), m_btnMtcSend);
        }
        v->addWidget(m_grpClockMidi);

        // --- 配置组 4: 文件类时间码 ---
        m_grpClockFile = new QWidget(body);
        {
            auto *f = new QFormLayout(m_grpClockFile);
            f->setContentsMargins(0, 0, 0, 0);
            f->setSpacing(12);
            f->setLabelAlignment(Qt::AlignRight);

            auto *fileRow = new QHBoxLayout();
            m_editMediaFile = new QLineEdit(m_grpClockFile);
            m_editMediaFile->setPlaceholderText(tr("视频/音频/.tc 文件路径"));
            m_btnBrowseMedia = new QPushButton(tr("浏览..."), m_grpClockFile);
            m_btnBrowseMedia->setProperty("action", "icon");
            connect(m_btnBrowseMedia, &QPushButton::clicked, m_grpClockFile, [this]{
                const auto type = SyncClockType(m_cmbClockType->currentData().toInt());
                QString filter = tr("所有文件 (*.*)");
                if (type == SyncClockType::AudioFile)
                    filter = tr("音频文件 (*.wav *.mp3 *.flac *.aac *.m4a *.ogg);;所有文件 (*.*)");
                else if (type == SyncClockType::VideoFile)
                    filter = tr("视频文件 (*.mp4 *.mov *.mkv *.avi *.webm *.m4v);;所有文件 (*.*)");
                else if (type == SyncClockType::TimecodeFile)
                    filter = tr("时间码文件 (*.tc *.lts *.srt *.txt);;所有文件 (*.*)");

                const QString p = QFileDialog::getOpenFileName(this, tr("选择媒体/时间码文件"), QString(), filter);
                if (p.isEmpty()) return;
                m_editMediaFile->setText(p);
                // 音视频源: 同步载入播放器 (显示画面/波形)
                if (m_mediaPlayer && type != SyncClockType::TimecodeFile) {
                    m_mediaPlayer->openFile(p);
                    logInfo(tr("已载入媒体: %1").arg(QFileInfo(p).fileName()));
                }
            });
            fileRow->addWidget(m_editMediaFile, 1);
            fileRow->addWidget(m_btnBrowseMedia);
            f->addRow(tr("媒体文件:"), [&]{ auto *w=new QWidget(m_grpClockFile); w->setLayout(fileRow); return w; }());

            m_spinFileFps = new QSpinBox(m_grpClockFile);
            m_spinFileFps->setRange(1, 120);
            m_spinFileFps->setValue(30);
            connect(m_spinFileFps, QOverload<int>::of(&QSpinBox::valueChanged), this,
                    [this](int v) { if (m_mediaPlayer) m_mediaPlayer->setTimecodeFrameRate(v); });
            f->addRow(tr("媒体时码帧率:"), m_spinFileFps);

            m_chkFileRateHeader = new QCheckBox(tr("文件首行包含 FPS 头"), m_grpClockFile);
            m_chkFileRateHeader->setChecked(true);
            f->addRow(tr("时间码格式:"), m_chkFileRateHeader);

            m_chkLoopOnEnd = new QCheckBox(tr("到达末尾后循环"), m_grpClockFile);
            f->addRow(tr("循环:"), m_chkLoopOnEnd);
        }
        v->addWidget(m_grpClockFile);

        // 按钮
        auto *btnRow = new QHBoxLayout();
        auto *btnStartClk = new QPushButton(tr("▶ 启动时钟"));
        btnStartClk->setProperty("action", "start");
        auto *btnStopClk  = new QPushButton(tr("■ 停止时钟"));
        btnStopClk->setProperty("action", "stop");
        btnRow->addWidget(btnStartClk);
        btnRow->addWidget(btnStopClk);
        btnRow->addStretch();
        v->addLayout(btnRow);
        v->addStretch();   // 多余空间留在底部, 配置项紧贴顶部

        // 按钮动作 — 直接复用 m_clock 成员 (和仪表盘页面共享)
        connect(btnStartClk, &QPushButton::clicked, this, &ArtNetRecorderMainWindow::onStartClicked);
        connect(btnStopClk,  &QPushButton::clicked, this, &ArtNetRecorderMainWindow::onStopClicked);

        // 切换时间码源 → 只显示该源的配置
        connect(m_cmbClockType, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int){ updateClockConfigRows(); });
        updateClockConfigRows();

        // 左列: 配置卡片 (跨两行); 右列: 实时时码 + 媒体播放器
        root->addWidget(createCard(tr("时间码源配置"), body), 0, 0, 2, 1);
    }

    // === 实时时钟状态 (右侧大卡片) ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(14);

        auto *form = new QFormLayout();
        form->setSpacing(12);
        form->setLabelAlignment(Qt::AlignRight);

        // 大数字面板
        auto *bigRow = new QHBoxLayout();
        auto *tsBig = createValNumber(tr("--"));
        m_lblClockBigTs = tsBig;
        auto *tsUnit = createValLabel(tr("us  (微秒)"));
        bigRow->addWidget(tsBig);
        bigRow->addWidget(tsUnit, 0, Qt::AlignBottom);
        bigRow->addStretch();
        form->addRow(createValLabel(tr("当前时间戳:")), [&]{ auto *w=new QWidget(); w->setLayout(bigRow); return w; }());

        auto *frameBigRow = new QHBoxLayout();
        auto *frameBig = createValNumber(tr("--"));
        m_lblClockBigFrame = frameBig;
        frameBigRow->addWidget(frameBig);
        frameBigRow->addWidget(createValLabel(tr("帧序号")), 0, Qt::AlignBottom);
        frameBigRow->addStretch();
        form->addRow(createValLabel(tr("当前帧序号:")), [&]{ auto *w=new QWidget(); w->setLayout(frameBigRow); return w; }());

        auto *tcRow = new QHBoxLayout();
        auto *tcBig = createValNumber(tr("--:--:--:--"), 18);
        m_lblClockBigTc = tcBig;
        tcRow->addWidget(tcBig);
        tcRow->addStretch();
        form->addRow(createValLabel(tr("SMPTE 时间码:")), [&]{ auto *w=new QWidget(); w->setLayout(tcRow); return w; }());

        auto *fpsRow = new QHBoxLayout();
        auto *fpsBig = createValNumber(tr("--"));
        m_lblClockBigFps = fpsBig;
        fpsRow->addWidget(fpsBig);
        fpsRow->addWidget(createValLabel(tr("fps  (帧率)")), 0, Qt::AlignBottom);
        fpsRow->addStretch();
        form->addRow(createValLabel(tr("帧率:")), [&]{ auto *w=new QWidget(); w->setLayout(fpsRow); return w; }());

        auto *frameCountRow = new QHBoxLayout();
        auto *frameCountBig = createValNumber(tr("--"));
        m_lblClockBigFrameCount = frameCountBig;
        frameCountRow->addWidget(frameCountBig);
        frameCountRow->addWidget(createValLabel(tr("帧")), 0, Qt::AlignBottom);
        frameCountRow->addSpacing(18);
        auto *totalFramesBig = createValNumber(tr("--"));
        m_lblClockBigTotalFrames = totalFramesBig;
        frameCountRow->addWidget(totalFramesBig);
        frameCountRow->addWidget(createValLabel(tr("帧 (总)")), 0, Qt::AlignBottom);
        frameCountRow->addStretch();
        form->addRow(createValLabel(tr("当前帧数 / 总帧数:")),
                     [&]{ auto *w=new QWidget(); w->setLayout(frameCountRow); return w; }());

        v->addLayout(form);
        v->addStretch();

        root->addWidget(createCard(tr("实时时间码显示"), body), 0, 1);
    }

    // === 媒体播放器 (音频波形 / 视频画面) — 仅音频/视频时间码源显示 ===
    {
        m_mediaPlayer = new MediaPlayerPanel();
        m_mediaPlayer->setMinimumHeight(320);
        m_grpMediaPlayer = createCard(tr("媒体播放器 — 视频画面"), m_mediaPlayer);

        // 音频波形独立卡片: 放在"实时时间码显示"右侧
        QWidget *waveCard = createCard(tr("音频波形"), m_mediaPlayer->waveformView());
        root->addWidget(waveCard, 0, 2);

        // 播放器放在右列下方 (左列时间码配置纵向跨满)
        root->addWidget(m_grpMediaPlayer, 1, 1, 1, 2);
        root->setRowStretch(0, 0);
        root->setRowStretch(1, 1);

        // 播放器播放时: 实时把播放位置换算为时间戳/帧序号/SMPTE/帧率
        {
            const auto toSmpte = [](qint64 ms, int fps) {
                const qint64 totalFrames = ms * fps / 1000;
                const int ff = int(totalFrames % fps);
                const qint64 secs = totalFrames / fps;
                return QString("%1:%2:%3:%4")
                    .arg(secs / 3600, 2, 10, QChar('0'))
                    .arg((secs / 60) % 60, 2, 10, QChar('0'))
                    .arg(secs % 60, 2, 10, QChar('0'))
                    .arg(ff, 2, 10, QChar('0'));
            };
            auto applyPlayerPosition = [this, toSmpte](qint64 ms) {
                if (!m_mediaPlayer || !m_lblClockBigTs || !m_lblClockBigFrame || !m_lblClockBigTc)
                    return;
                const int fps = qMax(1, m_mediaPlayer->frameRate());
                const qint64 curFrames = ms * fps / 1000;
                m_lblClockBigTs->setText(QString::number(ms * 1000));
                m_lblClockBigFrame->setText(QString::number(curFrames));
                m_lblClockBigTc->setText(toSmpte(ms, fps));
                if (m_lblClockBigFrameCount)
                    m_lblClockBigFrameCount->setText(QString::number(curFrames));
                if (m_lblClockBigTotalFrames) {
                    const qint64 durMs = m_mediaPlayer->durationMs();
                    m_lblClockBigTotalFrames->setText(
                        durMs > 0 ? QString::number(durMs * fps / 1000) : QString("--"));
                }
            };

            connect(m_mediaPlayer, &MediaPlayerPanel::positionChanged, this, applyPlayerPosition);
            connect(m_mediaPlayer, &MediaPlayerPanel::mediaInfoChanged, this,
                    [this, applyPlayerPosition](int fps, bool){
                        if (m_lblClockBigFps) m_lblClockBigFps->setText(QString::number(fps));
                        if (m_mediaPlayer) applyPlayerPosition(m_mediaPlayer->positionMs());
                    });
        }

        // 播放器内打开文件时, 同步到"媒体文件"配置项
        connect(m_mediaPlayer, &MediaPlayerPanel::fileOpened, this, [this](const QString &path){
            if (m_editMediaFile) m_editMediaFile->setText(path);
            if (m_spinFileFps && m_mediaPlayer)
                m_mediaPlayer->setTimecodeFrameRate(m_spinFileFps->value());
        });
    }

    // 自动化测试钩子: ARTNET_UI_MEDIA_FILE=/path/to/media.wav 预载媒体并切到音频时间码源
    const QString mediaEnv = qEnvironmentVariable("ARTNET_UI_MEDIA_FILE");
    if (!mediaEnv.isEmpty() && m_mediaPlayer && m_editMediaFile) {
        m_editMediaFile->setText(mediaEnv);
        const QString suffix = QFileInfo(mediaEnv).suffix().toLower();
        const bool isVideoExt = QStringList{"mp4", "mov", "mkv", "avi", "webm", "m4v"}.contains(suffix);
        const int idx = m_cmbClockType->findData(
            int(isVideoExt ? SyncClockType::VideoFile : SyncClockType::AudioFile));
        if (idx >= 0) m_cmbClockType->setCurrentIndex(idx);
        m_mediaPlayer->openFile(mediaEnv);

        // 测试钩子: ARTNET_UI_MEDIA_AUTOPLAY=1 自动开始播放
        if (qEnvironmentVariableIsSet("ARTNET_UI_MEDIA_AUTOPLAY")) {
            QTimer::singleShot(800, this, [this]{ if (m_mediaPlayer) m_mediaPlayer->play(); });
        }
    }

    root->setColumnStretch(0, 1);
    root->setColumnStretch(1, 1);
    root->setColumnStretch(2, 1);
    root->setRowStretch(0, 0);
    root->setRowStretch(1, 1);
    return page;
}

// ============================================================
// 时间码源单选: 只显示当前源对应的配置项
// ============================================================
void ArtNetRecorderMainWindow::updateClockConfigRows() {
    if (!m_cmbClockType) return;
    const auto type = SyncClockType(m_cmbClockType->currentData().toInt());

    const bool isTimer  = (type == SyncClockType::InternalTimer);
    const bool isArtNet = (type == SyncClockType::ArtNetTimeCode);
    const bool isMidi   = (type == SyncClockType::MidiMTC);
    const bool isFile   = (type == SyncClockType::TimecodeFile
                           || type == SyncClockType::VideoFile
                           || type == SyncClockType::AudioFile);
    const bool isMedia  = (type == SyncClockType::AudioFile
                           || type == SyncClockType::VideoFile);

    if (m_grpClockTimer)  m_grpClockTimer->setVisible(isTimer);
    if (m_grpClockArtNet) m_grpClockArtNet->setVisible(isArtNet);
    if (m_grpClockMidi)   m_grpClockMidi->setVisible(isMidi);
    if (m_grpClockFile)   m_grpClockFile->setVisible(isFile);
    if (m_grpMediaPlayer) m_grpMediaPlayer->setVisible(true);   // 播放器常显, 便于随时试听/看波形
    // "文件首行包含 FPS 头" 只对时间码文件有意义
    if (m_chkFileRateHeader) m_chkFileRateHeader->setEnabled(type == SyncClockType::TimecodeFile);

    // 切到音视频源: 若已配置文件则直接载入播放器
    if (isMedia && m_mediaPlayer) {
        const QString path = m_editMediaFile ? m_editMediaFile->text().trimmed() : QString();
        if (!path.isEmpty() && m_mediaPlayer->filePath() != path)
            m_mediaPlayer->openFile(path);
        if (m_spinFileFps)
            m_mediaPlayer->setTimecodeFrameRate(m_spinFileFps->value());
    }

    // 切到 MIDI 源时刷新一次本机设备列表
    if (isMidi && m_cmbMidiDevice) {
        if (m_cmbMidiDevice->count() == 0) refreshMidiDevices();
        if (m_cmbMidiOut && m_cmbMidiOut->count() == 0) refreshMidiOutDevices();
    }
}

// ============================================================
// 刷新本地 MIDI 输入设备列表
// ============================================================
void ArtNetRecorderMainWindow::refreshMidiDevices() {
    if (!m_cmbMidiDevice) return;

    const QString keep = m_cmbMidiDevice->currentData().toString();
    m_cmbMidiDevice->clear();
    m_cmbMidiDevice->addItem(tr("（自动选择第一个可用设备）"), QString());

    const QStringList devices = enumerateMidiInputDevices();
    for (const QString &name : devices)
        m_cmbMidiDevice->addItem(name, name);

    if (devices.isEmpty()) {
        m_cmbMidiDevice->addItem(tr("（未检测到本地 MIDI 输入设备）"), QString());
        logWarn(tr("未检测到本地 MIDI 输入设备"));
    } else {
        logInfo(tr("检测到 %1 个 MIDI 输入设备: %2")
                    .arg(devices.size()).arg(devices.join(", ")));
    }

    const int idx = m_cmbMidiDevice->findData(keep);
    m_cmbMidiDevice->setCurrentIndex(idx > 0 ? idx : 0);
}

// ============================================================
// 刷新本地 MIDI 输出设备列表 (MTC 模拟发送用)
// ============================================================
void ArtNetRecorderMainWindow::refreshMidiOutDevices() {
    if (!m_cmbMidiOut) return;

    const QString keep = m_cmbMidiOut->currentText();
    m_cmbMidiOut->clear();
    const QStringList devices = enumerateMidiOutputDevices();
    for (const QString &name : devices) m_cmbMidiOut->addItem(name);
    if (devices.isEmpty()) m_cmbMidiOut->addItem(tr("（未检测到 MIDI 输出设备）"));

    const int idx = m_cmbMidiOut->findText(keep);
    if (idx > 0) m_cmbMidiOut->setCurrentIndex(idx);
    logInfo(tr("检测到 %1 个 MIDI 输出设备").arg(devices.size()));
}

// ============================================================
// 开/关 MTC quarter-frame 模拟发送 (30fps × 2 QF/帧 ≈ 16ms 一个)
// ============================================================
void ArtNetRecorderMainWindow::toggleMtcSimulation() {
#ifdef Q_OS_WIN
    if (m_midiOutHandle) {
        if (m_mtcTimer) m_mtcTimer->stop();
        auto h = reinterpret_cast<HMIDIOUT>(m_midiOutHandle);
        midiOutReset(h);
        midiOutClose(h);
        m_midiOutHandle = nullptr;
        m_btnMtcSend->setText(tr("▶ 开始模拟发送"));
        m_btnMtcSend->setProperty("action", "start");
        m_btnMtcSend->style()->unpolish(m_btnMtcSend);
        m_btnMtcSend->style()->polish(m_btnMtcSend);
        logInfo(tr("MTC 模拟发送已停止"));
        return;
    }

    const QStringList outs = enumerateMidiOutputDevices();
    if (outs.isEmpty()) { logError(tr("未检测到 MIDI 输出设备, 无法模拟发送")); return; }
    const int devIdx = m_cmbMidiOut->currentIndex();
    if (devIdx < 0 || devIdx >= outs.size()) { logError(tr("请先选择 MIDI 输出端口")); return; }

    HMIDIOUT h = nullptr;
    if (midiOutOpen(&h, UINT(devIdx), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        logError(tr("midiOutOpen 失败 (端口 %1)").arg(outs.at(devIdx)));
        return;
    }
    m_midiOutHandle = h;
    m_mtcHour  = m_spinMtcH->value();
    m_mtcMin   = m_spinMtcM->value();
    m_mtcSec   = m_spinMtcS->value();
    m_mtcFrame = m_spinMtcF->value();
    m_mtcQfIndex = 0;

    if (!m_mtcTimer) {
        m_mtcTimer = new QTimer(this);
        m_mtcTimer->setTimerType(Qt::PreciseTimer);
        connect(m_mtcTimer, &QTimer::timeout, this, &ArtNetRecorderMainWindow::onMtcSimTick);
    }
    m_mtcTimer->start(16);
    m_btnMtcSend->setText(tr("■ 停止模拟发送"));
    m_btnMtcSend->setProperty("action", "stop");
    m_btnMtcSend->style()->unpolish(m_btnMtcSend);
    m_btnMtcSend->style()->polish(m_btnMtcSend);
    logInfo(tr("MTC 模拟发送已开始 → %1, 起始 %2:%3:%4:%5")
                .arg(outs.at(devIdx)).arg(m_mtcHour, 2, 10, QChar('0'))
                .arg(m_mtcMin, 2, 10, QChar('0')).arg(m_mtcSec, 2, 10, QChar('0'))
                .arg(m_mtcFrame, 2, 10, QChar('0')));
#else
    logError(tr("当前平台未实现 MTC 模拟发送"));
#endif
}

// ============================================================
// 每个 tick 发一个 MTC quarter-frame, 8 个为一周期 (时间码前进 2 帧)
// ============================================================
void ArtNetRecorderMainWindow::onMtcSimTick() {
#ifdef Q_OS_WIN
    if (!m_midiOutHandle) return;
    auto h = reinterpret_cast<HMIDIOUT>(m_midiOutHandle);

    quint8 value = 0;
    switch (m_mtcQfIndex) {
        case 0: value = quint8(m_mtcFrame & 0x0F); break;
        case 1: value = quint8((m_mtcFrame >> 4) & 0x01); break;
        case 2: value = quint8(m_mtcSec % 10); break;
        case 3: value = quint8(m_mtcSec / 10); break;
        case 4: value = quint8(m_mtcMin % 10); break;
        case 5: value = quint8(m_mtcMin / 10); break;
        case 6: value = quint8(m_mtcHour % 10); break;
        case 7: value = quint8((m_mtcHour / 10) | 0x02); break; // 0x02 = 30fps 码率
        default: break;
    }
    const quint8 qfByte = quint8((m_mtcQfIndex << 4) | (value & 0x0F));
    const DWORD msg = 0xF1 | (DWORD(qfByte) << 8);
    midiOutShortMsg(h, msg);

    m_mtcQfIndex = quint8((m_mtcQfIndex + 1) & 0x07);
    if (m_mtcQfIndex == 0) {           // 一个周期结束 → 时间码前进 2 帧
        m_mtcFrame += 2;
        if (m_mtcFrame >= 30) {
            m_mtcFrame -= 30;
            if (++m_mtcSec >= 60) {
                m_mtcSec = 0;
                if (++m_mtcMin >= 60) {
                    m_mtcMin = 0;
                    if (++m_mtcHour >= 24) m_mtcHour = 0;
                }
            }
        }
    }
#endif
}

// ============================================================
// 页面 3: 转发输出 — OutputManager targets 表格
// ============================================================
QWidget *ArtNetRecorderMainWindow::buildPageOutput() {
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    // 顶部操作按钮
    auto *btnRow = new QHBoxLayout();
    m_btnAddArtNet = new QPushButton(tr("+ 添加 ArtNet 目标"));
    m_btnAddDmx    = new QPushButton(tr("+ 添加 DMX512 目标"));
    m_btnRemove    = new QPushButton(tr("🗑 移除选中"));
    m_btnRemove->setProperty("action", "icon");
    btnRow->addWidget(m_btnAddArtNet);
    btnRow->addWidget(m_btnAddDmx);
    btnRow->addWidget(m_btnRemove);
    btnRow->addStretch();

    // 右侧状态标签
    auto *statusLbl = new QLabel(tr("转发输出配置 — 运行时由 RecorderEngine 调用 OutputManager"));
    statusLbl->setStyleSheet("color: #4b6584; font-size: 11px;");
    btnRow->addWidget(statusLbl);

    root->addLayout(btnRow);

    // Target 列表表格
    m_tblOutputTargets = new QTableWidget(page);
    m_tblOutputTargets->setColumnCount(6);
    m_tblOutputTargets->setHorizontalHeaderLabels({
        tr("ID"), tr("类型"), tr("目标 (IP/端口)"), tr("已启用"), tr("过滤 Universes"), tr("状态")
    });
    m_tblOutputTargets->horizontalHeader()->setStretchLastSection(true);
    m_tblOutputTargets->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tblOutputTargets->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tblOutputTargets->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tblOutputTargets->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // 预置 2 行示例数据 (展示用)
    QList<QList<QString>> demoRows = {
        {"1", "ArtNet",  "127.0.0.1:6454", "是", "全部 (0,1,2,3)", "就绪"},
        {"2", "DMX512",  "COM3 @ 250k",   "是", "Universe 1",     "就绪"},
    };
    m_tblOutputTargets->setRowCount(demoRows.size());
    for (int r = 0; r < demoRows.size(); ++r)
        for (int c = 0; c < demoRows[r].size(); ++c)
            m_tblOutputTargets->setItem(r, c, new QTableWidgetItem(demoRows[r][c]));

    root->addWidget(m_tblOutputTargets, 1);

    // 底部说明
    auto *note = new QLabel(
        tr("提示: 此页展示 OutputManager 管理的所有 OutputTargetConfig. "
           "实际启动/停止由 RecorderEngine 在 start()/stop() 时统一调用."));
    note->setStyleSheet("color: #4b6584; font-size: 11px; padding: 4px;");
    note->setWordWrap(true);
    root->addWidget(note);

    return page;
}

// ============================================================
// 页面: DMX512 专项测试 (端口 / 环回 / 时序 / 走灯)
// ============================================================

// 按 DMX512 电气参数打开串口: 250000 8N2, 无流控
static bool openDmxPort(QSerialPort *sp, const QString &name) {
    if (!sp) return false;
    if (sp->isOpen()) sp->close();
    sp->setPortName(name);
    sp->setBaudRate(250000);
    sp->setDataBits(QSerialPort::Data8);
    sp->setStopBits(QSerialPort::TwoStop);
    sp->setParity(QSerialPort::NoParity);
    sp->setFlowControl(QSerialPort::NoFlowControl);
    return sp->open(QIODevice::ReadWrite);
}

void ArtNetRecorderMainWindow::refreshDmxTestPorts() {
    if (!m_dmxTxPort || !m_dmxRxPort) return;

    const QString curTx = m_dmxTxPort->currentText();
    const QString curRx = m_dmxRxPort->currentText();
    m_dmxTxPort->clear();
    m_dmxRxPort->clear();
    m_dmxRxPort->addItem(tr("(不接收)"));

    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        const QString name = info.portName();
        m_dmxTxPort->addItem(name);
        m_dmxRxPort->addItem(name);
    }
    if (ports.isEmpty()) {
        m_dmxTxPort->addItem(tr("(未检测到串口)"));
        appendLog(tr("DMX512 测试: 未检测到可用串口"), "warn");
    } else {
        appendLog(tr("DMX512 测试: 检测到 %1 个串口").arg(ports.size()), "info");
    }

    const int iTx = m_dmxTxPort->findText(curTx);
    if (iTx >= 0) m_dmxTxPort->setCurrentIndex(iTx);
    const int iRx = m_dmxRxPort->findText(curRx);
    if (iRx >= 0) m_dmxRxPort->setCurrentIndex(iRx);
}

void ArtNetRecorderMainWindow::startDmxTestTx() {
    if (m_dmxRxTestMode) stopDmxRxTest();   // 收发互斥

    const QString tx = m_dmxTxPort->currentText();
    if (tx.isEmpty() || tx.startsWith("(")) {
        appendLog(tr("DMX512 测试: 请先选择发送端口"), "error");
        return;
    }

    m_dmxTxSerial = new QSerialPort(this);
    if (!openDmxPort(m_dmxTxSerial, tx)) {
        appendLog(tr("DMX512 测试: 打开 %1 失败 (%2)")
                      .arg(tx, m_dmxTxSerial->errorString()), "error");
        delete m_dmxTxSerial;
        m_dmxTxSerial = nullptr;
        return;
    }

    m_dmxTxBuf = QByteArray(512, 0);
    m_dmxRxAccum.clear();
    m_dmxRxRaw.clear();
    m_dmxTxCount = 0; m_dmxRxCount = 0; m_dmxMismatch = 0; m_dmxTxBytes = 0;
    m_dmxIntervalUs = 0; m_dmxLastRxUs = 0;
    m_dmxScanCh = 0; m_dmxRamp = 0; m_dmxRampUp = true;
    m_dmxLastStepMs = QDateTime::currentMSecsSinceEpoch();

    // 可选接收端口 (环回测试需要 TX/RX 两个物理口或环回线)
    QString rxInfo;
    const QString rx = m_dmxRxPort->currentText();
    if (rx == tx) {
        appendLog(tr("DMX512 测试: 收发同一端口无法环回, 本次仅发送"), "warn");
        rxInfo = tr("<span style='color:#ffb300'> · 环回未开启</span>");
    } else if (!rx.isEmpty() && !rx.startsWith("(")) {
        m_dmxRxSerial = new QSerialPort(this);
        if (openDmxPort(m_dmxRxSerial, rx)) {
            connect(m_dmxRxSerial, &QSerialPort::readyRead, this,
                    [this]{ onDmxTestRxData(); });
            appendLog(tr("DMX512 测试: 环回接收已开启 (%1)").arg(rx), "info");
            rxInfo = tr("<span style='color:#00d4ff'> · 环回 %1</span>").arg(rx);
        } else {
            appendLog(tr("DMX512 测试: 打开接收端口 %1 失败 (%2)")
                          .arg(rx, m_dmxRxSerial->errorString()), "warn");
            delete m_dmxRxSerial;
            m_dmxRxSerial = nullptr;
            rxInfo = tr("<span style='color:#ffb300'> · 环回 %1 打开失败</span>").arg(rx);
        }
    }

    // 没有物理环回但需要观察实时输入页时, 自动开启软件环回模拟
    if (!m_dmxRxSerial && m_dmxLoopbackSim && !m_dmxLoopbackSim->isChecked()
        && m_dmxFeedIn && m_dmxFeedIn->isChecked()) {
        m_dmxLoopbackSim->setChecked(true);
        rxInfo += tr("<span style='color:#00d4ff'> · 模拟回显</span>");
    }

    m_dmxTxTimer->start(qMax(1, 1000 / qMax(1, m_dmxFrameRate->value())));
    if (m_dmxLoopbackSim && m_dmxLoopbackSim->isChecked()) {
        appendLog(m_dmxRxSerial
                      ? tr("DMX512 测试: 已使用物理环回, 软件环回模拟不生效")
                      : tr("DMX512 测试: 无物理环回, 软件环回模拟已开启 (发送帧直接写入实时输入页)"),
                  "warn");
    }
    m_btnDmxTx->setText(tr("■ 停止发送"));
    if (m_lblDmxState)
        m_lblDmxState->setText(tr("<span style='color:#00e676'>发送中 → %1 · %2 fps%3</span>")
                                   .arg(tx).arg(m_dmxFrameRate->value()).arg(rxInfo));
    const int bm = m_dmxBreakMode ? m_dmxBreakMode->currentIndex() : 0;
    const QString breakText = bm == 1 ? tr(", 波特率模拟 Break: 57600 发 0x00 → 250000 发 513 字节")
                              : (bm == 2 ? tr(", 无 Break") : QString());
    appendLog(tr("DMX512 测试: 开始在 %1 发送 (250000 8N2, %2 fps%3)")
                  .arg(tx).arg(m_dmxFrameRate->value()).arg(breakText), "info");
}

void ArtNetRecorderMainWindow::stopDmxTestTx() {
    if (m_dmxTxTimer) m_dmxTxTimer->stop();
    if (m_dmxTxSerial) {
        m_dmxTxSerial->setBreakEnabled(false);
        m_dmxTxSerial->close();
        m_dmxTxSerial->deleteLater();
        m_dmxTxSerial = nullptr;
    }
    if (m_dmxRxSerial) {
        m_dmxRxSerial->close();
        m_dmxRxSerial->deleteLater();
        m_dmxRxSerial = nullptr;
    }
    m_dmxRxTestMode = false;
    if (m_btnDmxTx) m_btnDmxTx->setText(tr("▶ 开始发送"));
    if (m_btnDmxRxTest) m_btnDmxRxTest->setText(tr("▶ 接收测试"));
    if (m_lblDmxState)
        m_lblDmxState->setText(tr("<span style='color:#8fb3d9'>已停止</span>"));
    appendLog(tr("DMX512 测试: 已停止 (发送 %1 帧, 接收 %2 帧)")
                  .arg(m_dmxTxCount).arg(m_dmxRxCount), "info");
}

void ArtNetRecorderMainWindow::startDmxRxTest() {
    if (m_dmxTxSerial && m_dmxTxSerial->isOpen()) stopDmxTestTx();   // 收发互斥

    const QString rx = m_dmxRxPort->currentText();
    if (rx.isEmpty() || rx.startsWith("(")) {
        appendLog(tr("DMX512 测试: 接收测试请先在「接收端口 (环回)」选择串口"), "error");
        return;
    }

    m_dmxRxSerial = new QSerialPort(this);
    if (!openDmxPort(m_dmxRxSerial, rx)) {
        appendLog(tr("DMX512 测试: 打开接收端口 %1 失败 (%2)")
                      .arg(rx, m_dmxRxSerial->errorString()), "error");
        delete m_dmxRxSerial;
        m_dmxRxSerial = nullptr;
        return;
    }
    connect(m_dmxRxSerial, &QSerialPort::readyRead, this, [this]{ onDmxTestRxData(); });

    // 只统计接收, 清空本次计数
    m_dmxRxAccum.clear();
    m_dmxRxRaw.clear();
    m_dmxTxCount = 0; m_dmxRxCount = 0; m_dmxMismatch = 0;
    m_dmxIntervalUs = 0; m_dmxLastRxUs = 0;
    m_dmxRxTestMode = true;

    if (m_btnDmxRxTest) m_btnDmxRxTest->setText(tr("■ 停止接收"));
    if (m_lblDmxState)
        m_lblDmxState->setText(tr("<span style='color:#00d4ff'>接收测试中 ← %1</span>").arg(rx));
    appendLog(tr("DMX512 测试: 开始接收测试 %1 (250000 8N2, 仅接收不发送)").arg(rx), "info");
}

void ArtNetRecorderMainWindow::stopDmxRxTest() {
    if (m_dmxRxSerial) {
        m_dmxRxSerial->close();
        m_dmxRxSerial->deleteLater();
        m_dmxRxSerial = nullptr;
    }
    m_dmxRxTestMode = false;
    if (m_btnDmxRxTest) m_btnDmxRxTest->setText(tr("▶ 接收测试"));
    if (m_lblDmxState)
        m_lblDmxState->setText(tr("<span style='color:#8fb3d9'>已停止</span>"));
    appendLog(tr("DMX512 测试: 接收测试已停止 (共接收 %1 帧)").arg(m_dmxRxCount), "info");
}

void ArtNetRecorderMainWindow::writeDmxTestFrame() {
    if (!m_dmxTxSerial || !m_dmxTxSerial->isOpen()) return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int mode = m_dmxMode ? m_dmxMode->currentIndex() : 0;   // 0 恒值 1 扫描 2 走灯
    const int ch = m_dmxChannel ? (m_dmxChannel->value() - 1) : 0;
    const int stepMs = m_dmxStepMs ? m_dmxStepMs->value() : 100;

    if (mode == 0) {
        // 恒值: 指定通道保持设定电平
        m_dmxTxBuf.fill(0);
        if (ch >= 0 && ch < 512) m_dmxTxBuf[ch] = char(m_dmxLevel->value());
    } else if (mode == 1) {
        // 单通道扫描: 每步点亮下一个通道
        if (nowMs - m_dmxLastStepMs >= stepMs) {
            m_dmxLastStepMs = nowMs;
            m_dmxScanCh = (m_dmxScanCh + 1) % 512;
        }
        m_dmxTxBuf.fill(0);
        m_dmxTxBuf[m_dmxScanCh] = char(255);
    } else {
        // 逐通道走灯: 指定通道 0→255→0 往返
        if (nowMs - m_dmxLastStepMs >= stepMs) {
            m_dmxLastStepMs = nowMs;
            m_dmxRamp += m_dmxRampUp ? 5 : -5;
            if (m_dmxRamp >= 255) { m_dmxRamp = 255; m_dmxRampUp = false; }
            if (m_dmxRamp <= 0)   { m_dmxRamp = 0;   m_dmxRampUp = true; }
        }
        m_dmxTxBuf.fill(0);
        if (ch >= 0 && ch < 512) m_dmxTxBuf[ch] = char(m_dmxRamp);
    }

    QByteArray frame;
    frame.reserve(513);
    frame.append(char(0x00));      // DMX 起始码
    frame.append(m_dmxTxBuf);

    const int breakMode = m_dmxBreakMode ? m_dmxBreakMode->currentIndex() : 0;
    if (breakMode == 0) {
        m_dmxTxSerial->setBreakEnabled(true);     // Break
        QThread::usleep(120);                     // Break 时长 (~88us 以上)
        m_dmxTxSerial->setBreakEnabled(false);    // MAB
        QThread::usleep(12);
    } else if (breakMode == 1) {
        // 普通 RS485 芯片无 Break:
        //   1) 切到 57600 发送 1 个 0x00 (起始位 + 8 数据位 = 9 个低电平位 ≈ 156us) 作为 Break;
        //   2) 该字节的 2 个停止位即 MAB;
        //   3) 切回 250000 后仍完整发送 513 字节: 1 字节起始码 0x00 + 512 通道数据。
        // 低波特率的这个 0x00 只用于模拟 Break, 不计入后续 513 字节。
        m_dmxTxSerial->setBaudRate(57600);
        m_dmxTxSerial->write(QByteArray(1, char(0x00)));
        m_dmxTxSerial->flush();
        QThread::usleep(250);
        m_dmxTxSerial->setBaudRate(250000);
        QThread::usleep(30);
    }
    // breakMode == 2: 不发送 Break, 仅发起始码 + 数据

    const qint64 written = m_dmxTxSerial->write(frame);
    m_dmxTxSerial->flush();
    if (written > 0) m_dmxTxBytes += quint64(written);
    m_dmxTxCount++;

    // 勾选「发送数据 → 实时输出页」时, 把本帧通道数据送入实时输出监视 (域 0)
    if (m_dmxFeedOut && m_dmxFeedOut->isChecked())
        feedLiveUniverse(false, 0, m_dmxTxBuf, nowMs * 1000);

    // 软件环回模拟: 没有可用环回硬件时, 把刚发送的帧当作接收帧, 供实时输入页观察
    if (m_dmxLoopbackSim && m_dmxLoopbackSim->isChecked() && !m_dmxRxSerial) {
        const qint64 nowUs = nowMs * 1000;
        m_dmxRxCount++;
        if (m_dmxLastRxUs > 0) m_dmxIntervalUs = nowUs - m_dmxLastRxUs;
        m_dmxLastRxUs = nowUs;
        if (m_dmxFeedIn && m_dmxFeedIn->isChecked())
            feedLiveUniverse(true, 0, m_dmxTxBuf, nowUs);
    }
}

void ArtNetRecorderMainWindow::onDmxTestRxData() {
    if (!m_dmxRxSerial) return;
    const QByteArray chunk = m_dmxRxSerial->readAll();
    m_dmxRxAccum.append(chunk);
    if (!chunk.isEmpty()) {
        m_dmxRxRaw.append(chunk);
        if (m_dmxRxRaw.size() > 64) m_dmxRxRaw = m_dmxRxRaw.right(64);
    }

    // 接收帧格式: 0=513(起始码+512) 1=514(起始码前多 1 字节) 2=512(无起始码)
    int frameLen = 513, dataOff = 1;
    const int fmt = m_dmxRxFormat ? m_dmxRxFormat->currentIndex() : 0;
    if (fmt == 1)      { frameLen = 514; dataOff = 2; }
    else if (fmt == 2) { frameLen = 512; dataOff = 0; }

    while (m_dmxRxAccum.size() >= frameLen) {
        const QByteArray frame = m_dmxRxAccum.left(frameLen);
        // 起始码不符时只丢 1 字节重新同步, 避免整帧错位后一直错 (
        // 无真正 Break 的 USB-RS485 常见多 1 个 0x00 或缺少起始码)
        if (dataOff > 0 && uchar(frame[dataOff - 1]) != 0x00) {
            m_dmxRxAccum.remove(0, 1);
            continue;
        }
        m_dmxRxAccum.remove(0, frameLen);

        QByteArray dmx = frame.mid(dataOff, 512);
        if (dmx.size() < 512) dmx.append(QByteArray(512 - dmx.size(), 0));

        m_dmxRxCount++;
        const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        if (m_dmxLastRxUs > 0) m_dmxIntervalUs = nowUs - m_dmxLastRxUs;
        m_dmxLastRxUs = nowUs;

        // 勾选「接收数据 → 实时输入页」时, 把环回收到的通道数据送入实时输入监视 (域 0)
        if (m_dmxFeedIn && m_dmxFeedIn->isChecked())
            feedLiveUniverse(true, 0, dmx, nowUs);

        // 环回比对 (仅发送测试时有基准; 独立接收测试不参与比对)
        if (m_dmxTxSerial && m_dmxTxSerial->isOpen()) {
            for (int i = 0; i < 512; ++i)
                if (dmx[i] != m_dmxTxBuf[i]) m_dmxMismatch++;
        }
    }

    // 防止异常数据无限累积
    if (m_dmxRxAccum.size() > 4096) m_dmxRxAccum.clear();
}

void ArtNetRecorderMainWindow::updateDmxTestStats() {
    if (m_lblDmxTxFrames) m_lblDmxTxFrames->setText(QString::number(m_dmxTxCount));
    if (m_lblDmxRxFrames) m_lblDmxRxFrames->setText(QString::number(m_dmxRxCount));
    if (m_lblDmxMismatch) m_lblDmxMismatch->setText(QString::number(m_dmxMismatch));

    if (m_lblDmxInterval) {
        if (m_dmxIntervalUs > 0)
            m_lblDmxInterval->setText(QString("%1 ms").arg(m_dmxIntervalUs / 1000.0, 0, 'f', 1));
        else
            m_lblDmxInterval->setText("-- ms");
    }
    if (m_lblDmxRate) {
        if (m_dmxIntervalUs > 0)
            m_lblDmxRate->setText(QString("%1 Hz").arg(1e6 / double(m_dmxIntervalUs), 0, 'f', 1));
        else
            m_lblDmxRate->setText("-- Hz");
    }
    if (m_lblDmxHex) {
        m_lblDmxHex->setText(m_dmxRxRaw.isEmpty()
                                 ? QString("--")
                                 : QString::fromLatin1(m_dmxRxRaw.toHex(' ')).toUpper());
    }
    if (m_lblDmxTxBytes) m_lblDmxTxBytes->setText(QString::number(m_dmxTxBytes));
}

// ============================================================
// Art-Net / NPU 收发测试页 (共用实现)
// ============================================================
static QString netTestName(ProtocolType t) {
    if (t == ProtocolType::ArtNet) return QStringLiteral("Art-Net");
    if (t == ProtocolType::Sacn)   return QStringLiteral("sACN");
    return QStringLiteral("NPU");
}

static quint16 netTestRxPort(ProtocolType t) {
    if (t == ProtocolType::ArtNet) return ArtNet::DEFAULT_PORT;
    if (t == ProtocolType::Sacn)   return 5568;
    return NpuProtocol::DEFAULT_PORT;
}

static void resetNetTestCounters(NetTestPage *p) {
    p->txBuf = QByteArray(512, 0);
    p->rxAccum.clear();
    p->txCount = p->rxCount = p->mismatch = 0;
    p->lastRxUs = p->intervalUs = 0;
    p->lastStepMs = QDateTime::currentMSecsSinceEpoch();
    p->scanCh = 0; p->ramp = 0; p->rampUp = true;
}

static void fillNetTestPattern(NetTestPage *p, qint64 nowMs) {
    // 0 恒值 1 单通道扫描 2 逐通道走灯 3 全通道阶梯 4 全通道恒值渐变 5 分组扫描(8×64)
    const int mode = p->cmbMode ? p->cmbMode->currentIndex() : 0;
    const int ch = p->spinChannel ? (p->spinChannel->value() - 1) : 0;
    const int stepMs = p->spinStepMs ? p->spinStepMs->value() : 100;
    const bool stepDue = (nowMs - p->lastStepMs >= stepMs);
    if (stepDue) p->lastStepMs = nowMs;

    p->txBuf.fill(0);
    if (mode == 0) {
        if (ch >= 0 && ch < 512) p->txBuf[ch] = char(p->spinLevel->value());
    } else if (mode == 1) {
        if (stepDue) p->scanCh = (p->scanCh + 1) % 512;
        p->txBuf[p->scanCh] = char(255);
    } else if (mode == 2) {
        if (stepDue) {
            p->ramp += p->rampUp ? 5 : -5;
            if (p->ramp >= 255) { p->ramp = 255; p->rampUp = false; }
            if (p->ramp <= 0)   { p->ramp = 0;   p->rampUp = true; }
        }
        if (ch >= 0 && ch < 512) p->txBuf[ch] = char(p->ramp);
    } else if (mode == 3) {
        // 512 通道全阶梯: 0..255 循环两轮, 校验全通道数据完整性
        for (int i = 0; i < 512; ++i) p->txBuf[i] = char(i & 0xFF);
    } else if (mode == 4) {
        // 512 通道同步渐变: 全部通道一起升降
        if (stepDue) {
            p->ramp += p->rampUp ? 5 : -5;
            if (p->ramp >= 255) { p->ramp = 255; p->rampUp = false; }
            if (p->ramp <= 0)   { p->ramp = 0;   p->rampUp = true; }
        }
        p->txBuf.fill(char(p->ramp));
    } else {
        // 8 组 × 64 通道分组扫描: 当前组 64 通道全亮, 依次轮转
        if (stepDue) p->scanCh = (p->scanCh + 1) % 8;
        for (int i = p->scanCh * 64; i < p->scanCh * 64 + 64; ++i)
            p->txBuf[i] = char(p->spinLevel ? p->spinLevel->value() : 255);
    }
}

static QByteArray buildArtNetTestPacket(quint16 universe, const QByteArray &dmx, quint8 seq) {
    ArtNetDmxTxPacket pkt;
    pkt.sequence = seq;
    pkt.setUniverse(universe);
    pkt.setDmx(reinterpret_cast<const uint8_t*>(dmx.constData()), qMin<int>(512, dmx.size()));
    return QByteArray(reinterpret_cast<const char*>(&pkt), sizeof(pkt));
}

static QByteArray buildNpuTestPacket(quint16 universe, const QByteArray &dmx) {
    QByteArray pkt;
    pkt.reserve(4 + 512);
    const quint16 length = 514;                       // cmd + universe + 512
    pkt.append(char(length & 0xFF));
    pkt.append(char((length >> 8) & 0xFF));
    pkt.append(char(NpuProtocol::OP_DMX));
    pkt.append(char(universe & 0xFF));

    QByteArray d = dmx;
    if (d.size() < 512) d.append(QByteArray(512 - d.size(), 0));
    else if (d.size() > 512) d = d.left(512);
    pkt.append(d);
    return pkt;
}

static void updateNetTestState(NetTestPage *p) {
    if (!p || !p->lblState) return;
    const bool tx = (p->txSocket != nullptr);
    const bool rx = p->rxTestMode;
    const quint16 lp = p->spinListenPort ? quint16(p->spinListenPort->value())
                                         : netTestRxPort(p->type);
    if (tx && rx) {
        p->lblState->setText(
            QString("<span style='color:#00e676'>发送中 → %1:%2 · %3 fps</span>"
                    " <span style='color:#00d4ff'>+ 接收 UDP %4</span>")
                .arg(p->editTargetIp->text()).arg(p->spinTargetPort->value())
                .arg(p->spinFrameRate->value()).arg(lp));
    } else if (tx) {
        p->lblState->setText(QString("<span style='color:#00e676'>发送中 → %1:%2 · %3 fps</span>")
                                 .arg(p->editTargetIp->text()).arg(p->spinTargetPort->value())
                                 .arg(p->spinFrameRate->value()));
    } else if (rx) {
        p->lblState->setText(QString("<span style='color:#00d4ff'>接收测试中 ← UDP %1</span>")
                                 .arg(lp));
    } else {
        p->lblState->setText(QString("<span style='color:#8fb3d9'>已停止</span>"));
    }
}

void ArtNetRecorderMainWindow::startNetTestTx(NetTestPage *p) {
    if (!p) return;
    const QString name = netTestName(p->type);

    if (p->txSocket) return;
    p->txSocket = new QUdpSocket(this);
    if (!p->txSocket->bind(QHostAddress(QHostAddress::AnyIPv4), 0, QUdpSocket::ShareAddress)) {
        appendLog(tr("%1 测试: 发送 socket 绑定失败 (%2)").arg(name, p->txSocket->errorString()), "error");
        delete p->txSocket;
        p->txSocket = nullptr;
        return;
    }

    const QHostAddress target(p->editTargetIp->text().trimmed());
    if (target.isNull()) {
        appendLog(tr("%1 测试: 目标 IP 无效 (%2)").arg(name, p->editTargetIp->text()), "error");
        p->txSocket->close();
        delete p->txSocket;
        p->txSocket = nullptr;
        return;
    }

    resetNetTestCounters(p);
    if (p->type == ProtocolType::Sacn && p->cid.size() != 16)
        p->cid = makeTestCid();
    p->btnTx->setText(tr("■ 停止发送"));
    updateNetTestState(p);
    p->txTimer->start(qMax(1, 1000 / qMax(1, p->spinFrameRate->value())));
    appendLog(tr("%1 测试: 开始在 %2:%3 发送 (%4 fps, Universe %5)")
                  .arg(name, target.toString()).arg(p->spinTargetPort->value())
                  .arg(p->spinFrameRate->value()).arg(p->spinUniverse->value()), "info");
}

void ArtNetRecorderMainWindow::stopNetTestTx(NetTestPage *p) {
    if (!p) return;
    if (p->txTimer) p->txTimer->stop();
    if (p->txSocket) {
        p->txSocket->close();
        p->txSocket->deleteLater();
        p->txSocket = nullptr;
    }
    if (p->btnTx) p->btnTx->setText(tr("▶ 开始发送"));
    if (p->btnLoop && !p->rxTestMode) p->btnLoop->setText(tr("▶ 回环测试"));
    p->loopStartMs = 0;
    updateNetTestState(p);
    appendLog(tr("%1 测试: 已停止发送 (发送 %2 帧)").arg(netTestName(p->type)).arg(p->txCount), "info");
}

void ArtNetRecorderMainWindow::startNetTestRx(NetTestPage *p) {
    if (!p || p->rxTestMode) return;
    const QString name = netTestName(p->type);
    const quint16 port = p->spinListenPort ? quint16(p->spinListenPort->value())
                                           : netTestRxPort(p->type);

    p->rxSocket = new QUdpSocket(this);
    if (!p->rxSocket->bind(QHostAddress(QHostAddress::AnyIPv4), port,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        appendLog(tr("%1 测试: 接收绑定 UDP %2 失败 (%3)")
                      .arg(name).arg(port).arg(p->rxSocket->errorString()), "error");
        delete p->rxSocket;
        p->rxSocket = nullptr;
        return;
    }
    connect(p->rxSocket, &QUdpSocket::readyRead, this, [this, p]{ onNetTestRx(p); });

    // sACN: 加入对应组播组 (239.255.x.x), 同时保留单播接收能力
    if (p->type == ProtocolType::Sacn) {
        p->lastUniverse = quint16(p->spinUniverse->value());
        if (!p->rxSocket->joinMulticastGroup(sacnMulticastAddress(p->lastUniverse)))
            appendLog(tr("sACN 测试: 加入组播 %1 失败 (%2), 仅接收单播")
                          .arg(sacnMulticastAddress(p->lastUniverse).toString(),
                               p->rxSocket->errorString()), "warn");
    }

    // 只重置接收统计, 不影响正在进行的发送计数
    p->rxAccum.clear();
    p->rxCount = 0; p->mismatch = 0;
    p->lastRxUs = 0; p->intervalUs = 0;
    p->rxTestMode = true;

    if (p->btnRx) p->btnRx->setText(tr("■ 停止接收"));
    updateNetTestState(p);
    appendLog(tr("%1 测试: 开始接收测试 UDP %2 (仅接收)").arg(name).arg(port), "info");
}

void ArtNetRecorderMainWindow::stopNetTestRx(NetTestPage *p) {
    if (!p) return;
    if (p->rxSocket) {
        if (p->type == ProtocolType::Sacn)
            p->rxSocket->leaveMulticastGroup(sacnMulticastAddress(p->lastUniverse));
        p->rxSocket->close();
        p->rxSocket->deleteLater();
        p->rxSocket = nullptr;
    }
    p->rxTestMode = false;
    if (p->btnRx) p->btnRx->setText(tr("▶ 接收测试"));
    if (p->btnLoop && !p->txSocket) p->btnLoop->setText(tr("▶ 回环测试"));
    p->loopStartMs = 0;
    updateNetTestState(p);
    appendLog(tr("%1 测试: 接收测试已停止 (共接收 %2 帧)")
                  .arg(netTestName(p->type)).arg(p->rxCount), "info");
}

void ArtNetRecorderMainWindow::writeNetTestFrame(NetTestPage *p) {
    if (!p || !p->txSocket) return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    fillNetTestPattern(p, nowMs);

    const QHostAddress target(p->editTargetIp->text().trimmed());
    if (target.isNull()) return;

    const int uniMax = p->spinUniverse->maximum();
    const int uniBase = p->spinUniverse->value();
    const int uniCount = p->spinUniverseCount ? p->spinUniverseCount->value() : 1;
    const bool feedOut = p->chkFeedOut && p->chkFeedOut->isChecked();
    // 从起始域连续发送 N 个域 (8 域测试), 各域携带相同 512 通道图案
    for (int k = 0; k < uniCount; ++k) {
        const int u = uniBase + k;
        if (u > uniMax) break;
        const quint16 uni = quint16(u);
        QByteArray pkt;
        if (p->type == ProtocolType::ArtNet) {
            pkt = buildArtNetTestPacket(uni, p->txBuf, quint8(p->txCount & 0xFF));
        } else if (p->type == ProtocolType::Sacn) {
            const quint8 prio = quint8(p->spinPriority ? p->spinPriority->value() : 100);
            pkt = buildE131Packet(uni, p->txBuf, p->cid, prio, quint8(p->txCount & 0xFF), 0);
        } else {
            pkt = buildNpuTestPacket(uni, p->txBuf);
        }

        const qint64 sent = p->txSocket->writeDatagram(pkt, target, quint16(p->spinTargetPort->value()));
        if (sent <= 0) return;

        p->txCount++;
        p->lastUniverse = uni;
        if (feedOut)
            feedLiveUniverse(false, uni, p->txBuf, nowMs * 1000);
    }
}

void ArtNetRecorderMainWindow::onNetTestRx(NetTestPage *p) {
    if (!p || !p->rxSocket) return;

    while (p->rxSocket->hasPendingDatagrams()) {
        const qint64 sz = p->rxSocket->pendingDatagramSize();
        if (sz <= 0) break;
        QByteArray data;
        data.resize(int(sz));
        if (p->rxSocket->readDatagram(data.data(), data.size()) < 0) break;

        QByteArray dmx;
        quint16 uni = 0;
        if (p->type == ProtocolType::ArtNet) {
            if (data.size() < 18) continue;
            if (std::memcmp(data.constData(), "Art-Net\0", 8) != 0) continue;
            const auto *d = reinterpret_cast<const uchar*>(data.constData());
            const quint16 op = quint16(d[8] | (d[9] << 8));
            if (op != ArtNet::OP_DMX) continue;
            const int len = int(d[16] | (d[17] << 8));
            if (len <= 0) continue;
            uni = quint16(d[14] | (d[15] << 8));
            dmx = data.mid(18, qMin(len, 512));
        } else if (p->type == ProtocolType::Sacn) {
            if (data.size() < 126) continue;
            if (std::memcmp(data.constData() + 4, "ASC-E1.17", 9) != 0) continue;
            const auto *d = reinterpret_cast<const uchar*>(data.constData());
            if (!(d[21] == 0x04 && d[43] == 0x02 && d[119] == 0x02)) continue;
            if (d[120] != 0x00) continue;                 // 只处理 DMX 起始码
            uni = quint16((d[115] << 8) | d[116]);
            const int dmpLen = ((d[117] & 0x0F) << 8) | d[118];
            dmx = data.mid(121, qMin(qMax(0, dmpLen - 1), 512));
        } else {
            if (data.size() < 4 + 512) continue;
            const auto *d = reinterpret_cast<const uchar*>(data.constData());
            if (d[2] != NpuProtocol::OP_DMX) continue;
            uni = quint16(d[3]);
            dmx = data.mid(4, 512);
        }
        if (dmx.size() < 512) dmx.append(QByteArray(512 - dmx.size(), 0));

        p->rxCount++;
        const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        if (p->lastRxUs > 0) p->intervalUs = nowUs - p->lastRxUs;
        p->lastRxUs = nowUs;

        // 环回比对: 仅发送侧在运行时才有基准
        if (p->txSocket) {
            for (int i = 0; i < 512; ++i)
                if (dmx[i] != p->txBuf[i]) p->mismatch++;
        }

        if (p->chkFeedIn && p->chkFeedIn->isChecked())
            feedLiveUniverse(true, uni, dmx, nowUs);
    }
}

void ArtNetRecorderMainWindow::updateNetTestStats(NetTestPage *p) {
    if (!p) return;
    if (p->lblTxFrames) p->lblTxFrames->setText(QString::number(p->txCount));
    if (p->lblRxFrames) p->lblRxFrames->setText(QString::number(p->rxCount));
    if (p->lblMismatch) p->lblMismatch->setText(QString::number(p->mismatch));

    // 回环运行时用「接收帧数 / 运行时长」的平均值显示, 避免单次间隔抖动 (USB/事件循环会成批到达)
    const bool bothRunning = (p->txSocket != nullptr) && p->rxTestMode;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (bothRunning && p->loopStartMs == 0) p->loopStartMs = nowMs;
    double avgFps = 0.0;
    if (bothRunning && p->loopStartMs > 0) {
        const qint64 el = nowMs - p->loopStartMs;
        if (el > 0) avgFps = p->rxCount * 1000.0 / double(el);
    }

    if (p->lblInterval) {
        const double ms = avgFps > 0 ? 1000.0 / avgFps
                                     : (p->intervalUs > 0 ? p->intervalUs / 1000.0 : 0.0);
        p->lblInterval->setText(ms > 0 ? QString("%1 ms").arg(ms, 0, 'f', 1)
                                       : QString("-- ms"));
    }
    if (p->lblRate) {
        const double hz = avgFps > 0 ? avgFps
                                     : (p->intervalUs > 0 ? 1e6 / double(p->intervalUs) : 0.0);
        p->lblRate->setText(hz > 0 ? QString("%1 Hz").arg(hz, 0, 'f', 1)
                                   : QString("-- Hz"));
    }

    // ---- 回环自动判定: 发送与接收同时运行时, 采集 3 秒后给出结论 ----
    if (p->lblVerdict) {
        const bool both = (p->txSocket != nullptr) && p->rxTestMode;
        QString text;
        QString color = "#8fb3d9";
        int state = 0;
        if (!both) {
            text = tr("未运行回环（发送与接收同时运行后自动判定）");
        } else {
            if (p->loopStartMs == 0) p->loopStartMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - p->loopStartMs;
            if (elapsed < 3000) {
                text = tr("采集中… 剩余 %1 秒（至少采集 3 秒）")
                           .arg((3000 - elapsed + 999) / 1000);
                state = 1;
            } else {
                const double fpsSet  = (p->spinFrameRate ? p->spinFrameRate->value() : 0.0)
                                     * double(p->spinUniverseCount ? p->spinUniverseCount->value() : 1);
                const double fpsAvg  = elapsed > 0 ? p->rxCount * 1000.0 / double(elapsed) : 0.0;
                const bool okCount = p->txCount > 0 && (p->rxCount + 2 >= p->txCount);
                const bool okRate  = fpsAvg > 0
                                     && qAbs(fpsAvg - fpsSet) <= qMax(3.0, fpsSet * 0.15);
                const bool levelMode = p->cmbMode && p->cmbMode->currentIndex() == 0;
                const bool okData = !levelMode || p->mismatch == 0;
                if (okCount && okRate && okData) {
                    text = tr("通过 — 收 %1 / 发 %2 帧，不一致 %3 字节，实测平均 %4 Hz")
                               .arg(p->rxCount).arg(p->txCount).arg(p->mismatch)
                               .arg(fpsAvg, 0, 'f', 1);
                    color = "#00e676";
                    state = 2;
                } else {
                    QStringList why;
                    if (!okCount) why << tr("接收帧数偏少");
                    if (!okRate)  why << tr("刷新率偏差大");
                    if (!okData)  why << tr("数据不一致");
                    text = tr("不通过 — %1（收 %2 / 发 %3 帧，不一致 %4 字节，实测平均 %5 Hz）")
                               .arg(why.join(tr("、")))
                               .arg(p->rxCount).arg(p->txCount).arg(p->mismatch)
                               .arg(fpsAvg, 0, 'f', 1);
                    color = "#ff5252";
                    state = 3;
                }
            }
        }
        p->lblVerdict->setText(QString("<span style='color:%1'>%2</span>")
                                   .arg(color, text.toHtmlEscaped()));
        if (state != p->verdictState && (state == 2 || state == 3)) {
            appendLog(tr("%1 回环自动判定: %2").arg(netTestName(p->type), text),
                      state == 2 ? "info" : "warn");
        }
        p->verdictState = state;
    }
}

QWidget *ArtNetRecorderMainWindow::buildPageNetTest(ProtocolType type) {
    const bool isArtNet = (type == ProtocolType::ArtNet);
    const bool isSacn   = (type == ProtocolType::Sacn);
    auto *p = new NetTestPage;
    p->type = type;
    if (isArtNet)      m_artnetTest = p;
    else if (isSacn)   m_sacnTest = p;
    else               m_npuTest = p;

    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto *hint = new QLabel(
        isArtNet
            ? tr("Art-Net 收发测试 — UDP 发送 ArtDMX (默认 6454) + 本机接收比对 + 时序测量 + 走灯。"
                 "自测时目标 IP 填 127.0.0.1 即可在本机环回；对真实节点请填写其 IP。")
            : (isSacn
                   ? tr("sACN (E1.31) 收发测试 — UDP 发送 638 字节 E1.31 包 (默认 5568) + 本机接收比对 + 时序测量 + 走灯。"
                        "自测时目标 IP 填 127.0.0.1 即可在本机环回；组播接收会加入 239.255.x.x。")
                   : tr("NPU 收发测试 — UDP 发送 Ma-Net2 DMX 包 (默认 9999) + 本机接收比对 + 时序测量 + 走灯。"
                        "自测时目标 IP 填 127.0.0.1 即可在本机环回；对真实 NPU 节点请填写其 IP。")));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#8fb3d9; font-size:12px;");
    root->addWidget(hint);

    // ---- 目标与发送参数 ----
    auto *cfgBody = new QWidget;
    auto *cfg = new QGridLayout(cfgBody);
    cfg->setContentsMargins(0, 0, 0, 0);
    cfg->setHorizontalSpacing(12);
    cfg->setVerticalSpacing(8);

    p->editTargetIp = new QLineEdit(isArtNet ? "127.0.0.1" : "127.0.0.1");
    p->editTargetIp->setMinimumWidth(140);
    p->spinTargetPort = new QSpinBox;
    p->spinTargetPort->setRange(1, 65535);
    p->spinTargetPort->setValue(netTestRxPort(type));
    p->spinListenPort = new QSpinBox;
    p->spinListenPort->setRange(1, 65535);
    p->spinListenPort->setValue(netTestRxPort(type));
    p->spinListenPort->setToolTip(tr("接收测试绑定的本地 UDP 端口；回环时目标端口应与之相同（修改后需停止再重新开始接收）"));
    p->spinUniverse = new QSpinBox;
    p->spinUniverse->setRange(0, isArtNet ? 32767 : (isSacn ? 63999 : 255));
    p->spinUniverse->setValue(isSacn ? 1 : 0);
    p->spinUniverseCount = new QSpinBox;
    p->spinUniverseCount->setRange(1, 8);
    p->spinUniverseCount->setValue(1);
    p->spinUniverseCount->setSuffix(tr(" 域"));
    p->spinUniverseCount->setToolTip(tr("从起始域开始连续发送 N 个域 (含 8 域测试); 回环接收率 = 刷新率 × 域数"));
    p->spinFrameRate = new QSpinBox;
    p->spinFrameRate->setRange(1, 100);
    p->spinFrameRate->setValue(40);
    p->spinFrameRate->setSuffix(" fps");
    p->cmbMode = new QComboBox;
    p->cmbMode->addItem(tr("恒值输出"));
    p->cmbMode->addItem(tr("单通道扫描"));
    p->cmbMode->addItem(tr("逐通道走灯"));
    p->cmbMode->addItem(tr("全通道阶梯 (512)"));
    p->cmbMode->addItem(tr("全通道渐变 (512)"));
    p->cmbMode->addItem(tr("分组扫描 (8×64)"));
    p->spinChannel = new QSpinBox; p->spinChannel->setRange(1, 512); p->spinChannel->setValue(1);
    p->spinLevel = new QSpinBox;   p->spinLevel->setRange(0, 255);  p->spinLevel->setValue(255);
    p->spinStepMs = new QSpinBox;  p->spinStepMs->setRange(10, 5000); p->spinStepMs->setValue(100);
    p->spinStepMs->setSuffix(" ms");

    p->btnTx = new QPushButton(tr("▶ 开始发送"));
    p->btnTx->setProperty("action", "start");
    p->btnRx = new QPushButton(tr("▶ 接收测试"));
    p->btnRx->setProperty("action", "tool");
    p->btnRx->setToolTip(tr("只监听「接收端口」接收数据, 不发送; 可与发送同时运行以做本机环回"));
    p->btnLoop = new QPushButton(tr("▶ 回环测试"));
    p->btnLoop->setProperty("action", "tool");
    p->btnLoop->setToolTip(tr("一键回环: 目标 IP 设为 127.0.0.1、目标端口设为接收端口, 同时开始接收与发送"));
    p->lblState = new QLabel(tr("<span style='color:#8fb3d9'>已停止</span>"));

    cfg->addWidget(new QLabel(tr("目标 IP:")), 0, 0);
    cfg->addWidget(p->editTargetIp, 0, 1);
    cfg->addWidget(new QLabel(tr("目标端口:")), 0, 2);
    cfg->addWidget(p->spinTargetPort, 0, 3);
    cfg->addWidget(new QLabel(tr("接收端口:")), 0, 4);
    cfg->addWidget(p->spinListenPort, 0, 5);
    cfg->addWidget(new QLabel(isArtNet ? tr("起始域:") : tr("Universe:")), 0, 6);
    cfg->addWidget(p->spinUniverse, 0, 7);
    cfg->addWidget(new QLabel(tr("域数量:")), 0, 8);
    cfg->addWidget(p->spinUniverseCount, 0, 9);

    cfg->addWidget(new QLabel(tr("刷新率:")), 1, 0);
    cfg->addWidget(p->spinFrameRate, 1, 1);
    if (isSacn) {
        p->spinPriority = new QSpinBox;
        p->spinPriority->setRange(0, 200);
        p->spinPriority->setValue(100);
        cfg->addWidget(new QLabel(tr("优先级:")), 1, 2);
        cfg->addWidget(p->spinPriority, 1, 3);
        cfg->addWidget(new QLabel(tr("测试模式:")), 1, 4);
        cfg->addWidget(p->cmbMode, 1, 5);
    } else {
        cfg->addWidget(new QLabel(tr("测试模式:")), 1, 2);
        cfg->addWidget(p->cmbMode, 1, 3, 1, 3);
    }

    cfg->addWidget(new QLabel(tr("通道 / 电平 / 步进:")), 2, 0);
    auto *rowCtl = new QHBoxLayout;
    rowCtl->setContentsMargins(0, 0, 0, 0);
    rowCtl->addWidget(p->spinChannel); rowCtl->addWidget(p->spinLevel);
    rowCtl->addWidget(p->spinStepMs);  rowCtl->addStretch();
    cfg->addLayout(rowCtl, 2, 1, 1, 5);

    p->chkFeedOut = new QCheckBox(tr("发送数据 → 实时输出页"));
    p->chkFeedOut->setChecked(true);
    p->chkFeedIn = new QCheckBox(tr("接收数据 → 实时输入页"));
    p->chkFeedIn->setChecked(true);
    auto *rowFeed = new QHBoxLayout;
    rowFeed->setContentsMargins(0, 0, 0, 0);
    rowFeed->addWidget(p->chkFeedOut);
    rowFeed->addSpacing(24);
    rowFeed->addWidget(p->chkFeedIn);
    rowFeed->addStretch();
    cfg->addLayout(rowFeed, 3, 0, 1, 6);

    auto *rowBtns = new QHBoxLayout;
    rowBtns->setContentsMargins(0, 0, 0, 0);
    rowBtns->addWidget(p->btnTx);
    rowBtns->addWidget(p->btnRx);
    rowBtns->addWidget(p->btnLoop);
    rowBtns->addWidget(p->lblState);
    rowBtns->addStretch();
    cfg->addLayout(rowBtns, 4, 0, 1, 6);

    cfg->setColumnStretch(5, 1);
    cfg->setColumnStretch(7, 1);
    root->addWidget(createCard(tr("目标与发送参数"), cfgBody), 0);

    // ---- 测试结果 ----
    auto *statBody = new QWidget;
    auto *st = new QGridLayout(statBody);
    st->setContentsMargins(0, 0, 0, 0);
    st->setHorizontalSpacing(18);
    st->setVerticalSpacing(10);

    auto makeStat = [](const QString &title, QLabel **out, QGridLayout *lay, int row, int col) {
        auto *cap = new QLabel(title);
        cap->setStyleSheet("color:#8fb3d9; font-size:12px;");
        auto *val = new QLabel("--");
        val->setStyleSheet("color:#00d4ff; font-size:18px; font-weight:bold; font-family:Consolas;");
        auto *box = new QVBoxLayout;
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        box->addWidget(cap);
        box->addWidget(val);
        lay->addLayout(box, row, col);
        *out = val;
    };

    makeStat(tr("发送帧数"),       &p->lblTxFrames, st, 0, 0);
    makeStat(tr("接收帧数"),       &p->lblRxFrames, st, 0, 1);
    makeStat(tr("环回不一致字节"), &p->lblMismatch, st, 0, 2);
    makeStat(tr("实测帧间隔"),     &p->lblInterval, st, 1, 0);
    makeStat(tr("实测刷新率"),     &p->lblRate,     st, 1, 1);

    auto *verdictCap = new QLabel(tr("回环自动判定"));
    verdictCap->setStyleSheet("color:#8fb3d9; font-size:12px;");
    p->lblVerdict = new QLabel(tr("<span style='color:#8fb3d9'>未运行回环（发送与接收同时运行后自动判定）</span>"));
    p->lblVerdict->setWordWrap(true);
    p->lblVerdict->setTextInteractionFlags(Qt::TextSelectableByMouse);
    p->lblVerdict->setStyleSheet("font-size:13px;");
    st->addWidget(verdictCap, 2, 0, 1, 3);
    st->addWidget(p->lblVerdict, 3, 0, 1, 3);

    root->addWidget(createCard(tr("测试结果 (接收 / 时序)"), statBody), 0);
    root->addStretch();

    // ---- 定时器与信号 ----
    p->txTimer = new QTimer(this);
    p->txTimer->setTimerType(Qt::PreciseTimer);   // 保证 25ms/40fps 等周期不被系统粗定时器拉长
    connect(p->txTimer, &QTimer::timeout, this, [this, p]{ writeNetTestFrame(p); });

    p->statTimer = new QTimer(this);
    connect(p->statTimer, &QTimer::timeout, this, [this, p]{ updateNetTestStats(p); });
    p->statTimer->start(300);

    connect(p->btnTx, &QPushButton::clicked, this, [this, p]{
        if (p->txSocket) stopNetTestTx(p);
        else startNetTestTx(p);
    });
    connect(p->btnRx, &QPushButton::clicked, this, [this, p]{
        if (p->rxTestMode) stopNetTestRx(p);
        else startNetTestRx(p);
    });
    connect(p->btnLoop, &QPushButton::clicked, this, [this, p]{
        if (p->txSocket || p->rxTestMode) {
            if (p->txSocket) stopNetTestTx(p);
            if (p->rxTestMode) stopNetTestRx(p);
            p->btnLoop->setText(tr("▶ 回环测试"));
            return;
        }
        const quint16 lp = quint16(p->spinListenPort->value());
        p->editTargetIp->setText("127.0.0.1");
        p->spinTargetPort->setValue(lp);
        startNetTestRx(p);
        if (!p->rxTestMode) return;
        startNetTestTx(p);
        if (!p->txSocket) { stopNetTestRx(p); return; }
        p->btnLoop->setText(tr("■ 停止回环"));
        appendLog(tr("%1 测试: 回环模式 — 发送 127.0.0.1:%2, 本地监听 %2")
                      .arg(netTestName(p->type)).arg(lp), "info");
    });

    // 自动化验证入口: ARTNET_UI_NETTEST_AUTOSTART=artnet|sacn|npu
    const QString token = isArtNet ? "artnet" : (isSacn ? "sacn" : "npu");
    const QString autoStart = qEnvironmentVariable("ARTNET_UI_NETTEST_AUTOSTART");
    if (autoStart == token)
        QTimer::singleShot(700, this, [this, p]{ startNetTestTx(p); });
    const QString autoRx = qEnvironmentVariable("ARTNET_UI_NETTEST_RXTEST");
    if (autoRx == token)
        QTimer::singleShot(800, this, [this, p]{ startNetTestRx(p); });
    if (qEnvironmentVariable("ARTNET_UI_NETTEST_LOOP") == token)
        QTimer::singleShot(900, this, [p]{ p->btnLoop->click(); });

    return page;
}

// ============================================================
// RDM (ArtRDM 0x6000) 请求 / 响应测试页
// ============================================================
static const uchar kRdmTestUid[6] = {0x4C, 0x41, 0x52, 0x4E, 0x00, 0x01}; // "LARN"+0001

static QByteArray buildRdmRequestPayload(bool discovery) {
    const int params = discovery ? 12 : 0;          // DISC 参数: lower/upper UID
    QByteArray rdm(26 + params, 0);
    rdm[0] = char(0xCC);                            // RDM start code
    rdm[1] = char(0x01);                            // Sub start code
    const quint16 msgLen = quint16(22 + params);    // 长度字段后的字节数
    rdm[2] = char(msgLen & 0xFF);
    rdm[3] = char(msgLen >> 8);

    for (int i = 0; i < 6; ++i)                     // Dest UID = 广播 (全 0)
        rdm[4 + i] = 0;
    for (int i = 0; i < 6; ++i)                     // Source UID
        rdm[10 + i] = char(kRdmTestUid[i]);

    rdm[16] = 0;                                    // Transaction
    rdm[17] = 0;                                    // Port ID
    rdm[18] = 0;                                    // Message count
    rdm[19] = 0; rdm[20] = 0;                       // Sub device

    if (discovery) {
        rdm[21] = char(0x10);                       // Command class: DISCOVERY
        rdm[22] = 0; rdm[23] = 0;                   // PID: DISC_UNIQUE_BRANCH
        rdm[24] = 12; rdm[25] = 0;                  // PDL = 12
        for (int i = 0; i < 6; ++i) rdm[26 + i] = 0;              // lower UID
        for (int i = 0; i < 6; ++i) rdm[32 + i] = char(0xFF);     // upper UID
    } else {
        rdm[21] = char(0x20);                       // Command class: GET
        rdm[22] = 0x60; rdm[23] = 0;                // PID: DEVICE_INFO (0x0060)
        rdm[24] = 0; rdm[25] = 0;                   // PDL = 0
    }
    return rdm;
}

static QByteArray buildArtRdmPacket(const QByteArray &rdmPayload) {
    QByteArray pkt;
    pkt.append("Art-Net", 8);
    pkt.append(char(0x00)); pkt.append(char(0x60)); // OpCode 0x6000 (Little Endian)
    pkt.append(char(0x00)); pkt.append(char(0x0E)); // ProtVer 14
    pkt.append(char(0x01));                          // RDM version
    pkt.append(rdmPayload);
    return pkt;
}

void ArtNetRecorderMainWindow::startRdmTestRx() {
    if (m_rdmListening) return;
    m_rdmTestRxSock = new QUdpSocket(this);
    if (!m_rdmTestRxSock->bind(QHostAddress(QHostAddress::AnyIPv4), ArtNet::DEFAULT_PORT,
                               QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        appendLog(tr("RDM 测试: 绑定 UDP 6454 失败 (%1)").arg(m_rdmTestRxSock->errorString()), "error");
        delete m_rdmTestRxSock;
        m_rdmTestRxSock = nullptr;
        return;
    }
    connect(m_rdmTestRxSock, &QUdpSocket::readyRead, this, [this]{ onRdmTestRx(); });

    m_rdmListening = true;
    m_rdmRxCount = 0;
    m_rdmOtherCount = 0;
    m_rdmRespIntervalUs = 0;
    m_rdmLastPayload.clear();
    if (m_btnRdmListen) m_btnRdmListen->setText(tr("■ 停止监听"));
    if (m_lblRdmState)
        m_lblRdmState->setText(tr("<span style='color:#00d4ff'>监听中 ← UDP 6454</span>"));
    appendLog(tr("RDM 测试: 开始监听 ArtRDM (UDP 6454)"), "info");
}

void ArtNetRecorderMainWindow::stopRdmTestRx() {
    if (!m_rdmListening) return;
    if (m_rdmTestRxSock) {
        m_rdmTestRxSock->close();
        m_rdmTestRxSock->deleteLater();
        m_rdmTestRxSock = nullptr;
    }
    m_rdmListening = false;
    if (m_btnRdmListen) m_btnRdmListen->setText(tr("▶ 开始监听"));
    if (m_lblRdmState)
        m_lblRdmState->setText(tr("<span style='color:#8fb3d9'>已停止</span>"));
    appendLog(tr("RDM 测试: 停止监听 (共接收 %1 包)").arg(m_rdmRxCount), "info");
}

void ArtNetRecorderMainWindow::sendRdmTestRequest(bool discovery) {
    if (!m_rdmTestTxSock) {
        m_rdmTestTxSock = new QUdpSocket(this);
        if (!m_rdmTestTxSock->bind(QHostAddress(QHostAddress::AnyIPv4), 0, QUdpSocket::ShareAddress)) {
            appendLog(tr("RDM 测试: 发送 socket 绑定失败 (%1)").arg(m_rdmTestTxSock->errorString()), "error");
            delete m_rdmTestTxSock;
            m_rdmTestTxSock = nullptr;
            return;
        }
    }
    const QHostAddress target(m_rdmTestIp->text().trimmed());
    if (target.isNull()) {
        appendLog(tr("RDM 测试: 目标 IP 无效"), "error");
        return;
    }
    const quint16 port = quint16(m_rdmTestPort->value());
    const QByteArray pkt = buildArtRdmPacket(buildRdmRequestPayload(discovery));
    if (m_rdmTestTxSock->writeDatagram(pkt, target, port) <= 0) {
        appendLog(tr("RDM 测试: 发送失败 (%1)").arg(m_rdmTestTxSock->errorString()), "error");
        return;
    }
    m_rdmTxCount++;
    m_rdmLastReqUs = QDateTime::currentMSecsSinceEpoch() * 1000;
    appendLog(tr("RDM 测试: 发送 %1 → %2:%3")
                  .arg(discovery ? "DISC_UNIQUE_BRANCH" : "GET DEVICE_INFO")
                  .arg(target.toString()).arg(port), "info");
}

void ArtNetRecorderMainWindow::onRdmTestRx() {
    if (!m_rdmTestRxSock) return;
    while (m_rdmTestRxSock->hasPendingDatagrams()) {
        const qint64 sz = m_rdmTestRxSock->pendingDatagramSize();
        if (sz <= 0) break;
        QByteArray data;
        data.resize(int(sz));
        if (m_rdmTestRxSock->readDatagram(data.data(), data.size()) < 0) break;

        if (data.size() < 14) continue;
        if (std::memcmp(data.constData(), "Art-Net\0", 8) != 0) continue;
        const auto *d = reinterpret_cast<const uchar*>(data.constData());
        const quint16 op = quint16(d[8] | (d[9] << 8));
        if (op != 0x6000 && op != 0x6100) { m_rdmOtherCount++; continue; }

        int off = 12;                                  // ArtRDM: RDM ver 之后
        if (op == 0x6100) off += 2;                    // ArtRDMSub: 额外 2 字节 universe
        if (data.size() < off + 3) continue;
        if (uchar(data[off]) != 0xCC || uchar(data[off + 1]) != 0x01) continue;

        m_rdmRxCount++;
        const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        if (m_rdmLastReqUs > 0) m_rdmRespIntervalUs = nowUs - m_rdmLastReqUs;
        m_rdmLastPayload = data.mid(off, 32);

        if (m_rdmLastPayload.size() >= 16) {
            const auto *r = reinterpret_cast<const uchar*>(m_rdmLastPayload.constData());
            QString uid;
            for (int i = 0; i < 6; ++i)
                uid += QString("%1%2").arg(r[10 + i] >> 4, 1, 16)
                                      .arg(r[10 + i] & 0x0F, 1, 16)
                                      .toUpper() + (i < 5 ? ":" : "");
            if (m_lblRdmSrcUid) m_lblRdmSrcUid->setText(uid);
        }
        if (m_lblRdmHex) {
            QString hex = m_rdmLastPayload.toHex(' ').toUpper();
            m_lblRdmHex->setText(hex);
        }
    }
}

void ArtNetRecorderMainWindow::updateRdmTestStats() {
    if (m_lblRdmTxCount) m_lblRdmTxCount->setText(QString::number(m_rdmTxCount));
    if (m_lblRdmRxCount) m_lblRdmRxCount->setText(QString::number(m_rdmRxCount));
    if (m_lblRdmInterval)
        m_lblRdmInterval->setText(m_rdmRespIntervalUs > 0
                                      ? QString("%1 ms").arg(m_rdmRespIntervalUs / 1000.0, 0, 'f', 1)
                                      : QString("-- ms"));
}

QWidget *ArtNetRecorderMainWindow::buildPageRdmTest() {
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto *hint = new QLabel(tr("RDM 测试 — 通过 ArtRDM (OpCode 0x6000) 发送发现 / 设备信息请求，并监听 UDP 6454 接收应答。"
                               "RDM 依赖网关或支持 RDM 的设备应答；没有真实 RDM 设备时只会增加发送请求数。"));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#8fb3d9; font-size:12px;");
    root->addWidget(hint);

    // ---- 目标与操作 ----
    auto *cfgBody = new QWidget;
    auto *cfg = new QGridLayout(cfgBody);
    cfg->setContentsMargins(0, 0, 0, 0);
    cfg->setHorizontalSpacing(12);
    cfg->setVerticalSpacing(8);

    m_rdmTestIp = new QLineEdit("127.0.0.1");
    m_rdmTestIp->setMinimumWidth(150);
    m_rdmTestPort = new QSpinBox;
    m_rdmTestPort->setRange(1, 65535);
    m_rdmTestPort->setValue(ArtNet::DEFAULT_PORT);

    m_btnRdmListen = new QPushButton(tr("▶ 开始监听"));
    m_btnRdmListen->setProperty("action", "tool");
    m_btnRdmDisc = new QPushButton(tr("🔨 发送 DISC_UNIQUE_BRANCH"));
    m_btnRdmDisc->setProperty("action", "icon");
    m_btnRdmGetInfo = new QPushButton(tr("🔨 发送 GET DEVICE_INFO"));
    m_btnRdmGetInfo->setProperty("action", "icon");
    m_lblRdmState = new QLabel(tr("<span style='color:#8fb3d9'>已停止</span>"));

    cfg->addWidget(new QLabel(tr("网关 IP:")), 0, 0);
    cfg->addWidget(m_rdmTestIp, 0, 1);
    cfg->addWidget(new QLabel(tr("端口:")), 0, 2);
    cfg->addWidget(m_rdmTestPort, 0, 3);
    cfg->setColumnStretch(4, 1);

    auto *rowBtns = new QHBoxLayout;
    rowBtns->setContentsMargins(0, 0, 0, 0);
    rowBtns->addWidget(m_btnRdmListen);
    rowBtns->addWidget(m_btnRdmDisc);
    rowBtns->addWidget(m_btnRdmGetInfo);
    rowBtns->addWidget(m_lblRdmState);
    rowBtns->addStretch();
    cfg->addLayout(rowBtns, 1, 0, 1, 5);
    root->addWidget(createCard(tr("目标与操作"), cfgBody), 0);

    // ---- 测试结果 ----
    auto *statBody = new QWidget;
    auto *st = new QGridLayout(statBody);
    st->setContentsMargins(0, 0, 0, 0);
    st->setHorizontalSpacing(18);
    st->setVerticalSpacing(10);

    auto makeStat = [](const QString &title, QLabel **out, QGridLayout *lay, int row, int col) {
        auto *cap = new QLabel(title);
        cap->setStyleSheet("color:#8fb3d9; font-size:12px;");
        auto *val = new QLabel("--");
        val->setStyleSheet("color:#00d4ff; font-size:18px; font-weight:bold; font-family:Consolas;");
        auto *box = new QVBoxLayout;
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        box->addWidget(cap);
        box->addWidget(val);
        lay->addLayout(box, row, col);
        *out = val;
    };

    makeStat(tr("发送请求数"),  &m_lblRdmTxCount, st, 0, 0);
    makeStat(tr("接收 RDM 包"), &m_lblRdmRxCount, st, 0, 1);
    makeStat(tr("响应间隔"),    &m_lblRdmInterval, st, 0, 2);
    makeStat(tr("来源 UID"),    &m_lblRdmSrcUid, st, 0, 3);
    st->setColumnStretch(4, 1);

    auto *hexCap = new QLabel(tr("最近 RDM 负载 (HEX)"));
    hexCap->setStyleSheet("color:#8fb3d9; font-size:12px;");
    m_lblRdmHex = new QLabel("--");
    m_lblRdmHex->setWordWrap(true);
    m_lblRdmHex->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lblRdmHex->setStyleSheet("color:#7df0ff; font-size:12px; font-family:Consolas;");
    st->addWidget(hexCap, 1, 0, 1, 5);
    st->addWidget(m_lblRdmHex, 2, 0, 1, 5);

    root->addWidget(createCard(tr("测试结果 (请求 / 应答)"), statBody), 0);
    root->addStretch();

    m_rdmTestStatTimer = new QTimer(this);
    connect(m_rdmTestStatTimer, &QTimer::timeout, this, [this]{ updateRdmTestStats(); });
    m_rdmTestStatTimer->start(300);

    connect(m_btnRdmListen, &QPushButton::clicked, this, [this]{
        if (m_rdmListening) stopRdmTestRx();
        else startRdmTestRx();
    });
    connect(m_btnRdmDisc, &QPushButton::clicked, this, [this]{ sendRdmTestRequest(true); });
    connect(m_btnRdmGetInfo, &QPushButton::clicked, this, [this]{ sendRdmTestRequest(false); });

    if (qEnvironmentVariableIntValue("ARTNET_UI_RDM_AUTOTEST") == 1)
        QTimer::singleShot(800, this, [this]{ startRdmTestRx(); sendRdmTestRequest(true); });

    return page;
}

// ============================================================
// 页面: Art-Net → DMX512 转换测试
//   接收 ArtDMX 数据报, 按 8 路输出的域段映射转成 DMX512 串口帧发出.
// ============================================================
void ArtNetRecorderMainWindow::refreshArtToDmxPorts() {
    // 一次枚举, 刷新全部 8 行的串口下拉, 尽量保留各行已选项
    QStringList keeps;
    for (int i = 0; i < kArtToDmxOuts; ++i)
        keeps << (m_brOuts[i].cmbPort ? m_brOuts[i].cmbPort->currentData().toString()
                                      : QString());
    const auto ports = QSerialPortInfo::availablePorts();
    for (int i = 0; i < kArtToDmxOuts; ++i) {
        QComboBox *cmb = m_brOuts[i].cmbPort;
        if (!cmb) continue;
        cmb->clear();
        for (const QSerialPortInfo &pi : ports) {
            cmb->addItem(pi.portName() + (pi.description().isEmpty()
                                              ? QString()
                                              : "  (" + pi.description() + ")"),
                         pi.portName());
        }
        if (cmb->count() == 0)
            cmb->addItem(tr("(无可用串口)"), QString());
        const int idx = cmb->findData(keeps.at(i));
        if (idx >= 0) cmb->setCurrentIndex(idx);
    }
}

void ArtNetRecorderMainWindow::startArtToDmx() {
    stopArtToDmx();
    if (!m_brListenPort) return;

    // 对每个"启用且选了串口"的行创建一个 Dmx512Sender 并 open;
    // 单行失败只记警告, 不影响其余行
    int opened = 0;
    QStringList openedDesc;
    for (int i = 0; i < kArtToDmxOuts; ++i) {
        ArtToDmxOutput &row = m_brOuts[i];
        if (!row.chkEnabled || !row.chkEnabled->isChecked()) continue;
        const QString portName = row.cmbPort ? row.cmbPort->currentData().toString() : QString();
        if (portName.isEmpty()) {
            appendLog(tr("Art-Net→DMX: 输出 %1 已启用但未选择串口").arg(i + 1), "warn");
            continue;
        }
        auto *sender = new Dmx512Sender(this);
        if (!sender->open(portName)) {
            appendLog(tr("Art-Net→DMX: 打开串口 %1 失败 (输出 %2)").arg(portName).arg(i + 1),
                      "error");
            delete sender;
            continue;
        }
        row.sender = sender;
        ++opened;
        openedDesc << tr("输出%1 %2 域%3起×%4")
                      .arg(i + 1).arg(portName)
                      .arg(row.spinStart->value()).arg(row.spinSpan->value());
    }
    if (opened == 0) {
        appendLog(tr("Art-Net→DMX: 没有成功打开的 DMX 输出串口, 请检查启用行与端口选择"), "error");
        return;
    }

    m_brSocket = new QUdpSocket(this);
    const quint16 port = quint16(m_brListenPort->value());
    QHostAddress bindAddr = QHostAddress::AnyIPv4;
    QHostAddress joinGroup;   // 非空 = 监听 IP 填的是组播组地址
    const QString ipText = m_brListenIp ? m_brListenIp->text().trimmed() : QString();
    if (!ipText.isEmpty() && ipText != QLatin1String("0.0.0.0")) {
        QHostAddress specific(ipText);
        if (specific.isNull()) {
            appendLog(tr("Art-Net→DMX: 监听 IP 无效: %1").arg(ipText), "error");
            stopArtToDmx();
            return;
        }
        const quint8 firstOctet = quint8(specific.toIPv4Address() >> 24);
        if (firstOctet >= 224 && firstOctet <= 239) {
            // 组播组地址: 绑 Any + 加组, 只收发往该组的包
            joinGroup = specific;
        } else {
            bindAddr = specific;
        }
    }
    if (!m_brSocket->bind(bindAddr, port,
                          QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        appendLog(tr("Art-Net→DMX: 绑定 UDP %1:%2 失败 (%3)")
                      .arg(bindAddr.toString()).arg(port).arg(m_brSocket->errorString()),
                  "error");
        m_brSocket->deleteLater(); m_brSocket = nullptr;
        stopArtToDmx();
        return;
    }
    // Art-Net 标准组播 239.255.0.0: 未指定组播组时默认加该组, 否则收不到控台组播包;
    // 绑定单播 IP 时广播 (255.255.255.255) 可能收不到, 建议用 0.0.0.0 或组播组地址
    {
        const QHostAddress group = joinGroup.isNull()
            ? QHostAddress(QStringLiteral("239.255.0.0")) : joinGroup;
        bool joined;
        if (bindAddr == QHostAddress::AnyIPv4) {
            joined = m_brSocket->joinMulticastGroup(group);
        } else {
            // 指定网卡 IP: 在该接口上加组
            QNetworkInterface iface;
            const QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
            for (const QNetworkInterface &ni : ifaces) {
                const bool hit = std::any_of(ni.addressEntries().constBegin(),
                                             ni.addressEntries().constEnd(),
                                             [&bindAddr](const QNetworkAddressEntry &e) {
                                                 return e.ip() == bindAddr;
                                             });
                if (hit) { iface = ni; break; }
            }
            joined = iface.isValid() ? m_brSocket->joinMulticastGroup(group, iface)
                                     : m_brSocket->joinMulticastGroup(group);
        }
        appendLog(joined
            ? tr("Art-Net→DMX: 已监听 %1:%2, 加入组播组 %3")
                  .arg(bindAddr.toString()).arg(port).arg(group.toString())
            : tr("Art-Net→DMX: 已监听 %1:%2, 但加入组播组 %3 失败 (仅收单播/广播)")
                  .arg(bindAddr.toString()).arg(port).arg(group.toString()),
            joined ? "info" : "warn");
    }
    connect(m_brSocket, &QUdpSocket::readyRead, this, [this]{ onArtToDmxDatagram(); });

    m_brRxCount = m_brDropCount = 0;
    m_brLastLatencyUs = 0;
    m_brLastUni = 0;
    m_brLastDmx.clear();

    m_btnBrToggle->setText(tr("■ 停止转换"));
    m_lblBrState->setText(tr("<span style='color:#00e676'>转换中</span> "
                             "<span style='color:#8fb3d9'>UDP %1 · %2 路串口输出</span>")
                              .arg(port).arg(opened));
    appendLog(tr("Art-Net→DMX: 已开始 UDP %1 → %2 路串口 (250000 8N2): %3")
                  .arg(port).arg(opened).arg(openedDesc.join(", ")), "info");
}

void ArtNetRecorderMainWindow::stopArtToDmx() {
    if (m_brSocket) {
        m_brSocket->close();
        m_brSocket->deleteLater();
        m_brSocket = nullptr;
    }
    for (int i = 0; i < kArtToDmxOuts; ++i) {
        if (m_brOuts[i].sender) {
            m_brOuts[i].sender->close();
            m_brOuts[i].sender->deleteLater();
            m_brOuts[i].sender = nullptr;
        }
    }
    if (m_btnBrToggle) m_btnBrToggle->setText(tr("▶ 开始转换"));
    if (m_lblBrState)
        m_lblBrState->setText(tr("<span style='color:#8fb3d9'>未启动</span>"));
}

void ArtNetRecorderMainWindow::onArtToDmxDatagram() {
    if (!m_brSocket) return;

    while (m_brSocket->hasPendingDatagrams()) {
        const qint64 sz = m_brSocket->pendingDatagramSize();
        if (sz <= 0) break;
        QByteArray data;
        data.resize(int(sz));
        const qint64 recvUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        if (m_brSocket->readDatagram(data.data(), data.size()) < 0) break;

        if (data.size() < 18) { ++m_brDropCount; continue; }
        if (std::memcmp(data.constData(), "Art-Net\0", 8) != 0) { ++m_brDropCount; continue; }
        const auto *d = reinterpret_cast<const uchar*>(data.constData());
        if (quint16(d[8] | (d[9] << 8)) != ArtNet::OP_DMX) { ++m_brDropCount; continue; }

        const quint16 uni = quint16(d[14] | ((d[15] & 0x7F) << 8));
        const int len = int(d[16] | (d[17] << 8));
        if (len <= 0) { ++m_brDropCount; continue; }

        m_brRxCount++;
        m_brLastUni = uni;
        m_brLastDmx = data.mid(18, qMin(len, 512));
        if (m_brLastDmx.size() < 512)
            m_brLastDmx.append(QByteArray(512 - m_brLastDmx.size(), 0));

        // 路由: 第一个 启用 && sender 已打开 && universe ∈ [起始, 起始+覆盖数) 的行
        Dmx512Sender *target = nullptr;
        for (int i = 0; i < kArtToDmxOuts; ++i) {
            ArtToDmxOutput &row = m_brOuts[i];
            if (!row.chkEnabled || !row.chkEnabled->isChecked()
                || !row.sender || !row.sender->isOpen()) continue;
            const int start = row.spinStart ? row.spinStart->value() : 0;
            const int span  = row.spinSpan  ? row.spinSpan->value()  : 1;
            if (int(uni) >= start && int(uni) < start + span) { target = row.sender; break; }
        }
        if (!target) { ++m_brDropCount; continue; }

        target->sendFrame(m_brLastDmx);
        m_brLastLatencyUs = QDateTime::currentMSecsSinceEpoch() * 1000 - recvUs;
        if (m_brFeedOut && m_brFeedOut->isChecked())
            feedLiveUniverse(false, uni, m_brLastDmx, recvUs);
    }
}

QWidget *ArtNetRecorderMainWindow::buildPageArtToDmx() {
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto *hint = new QLabel(tr("Art-Net → DMX512 转换测试 — 监听 ArtDMX 数据报, 支持最多 8 路 DMX 串口输出; "
                               "每行按「起始 Universe + 覆盖域数」划分域段, 收到帧后路由到第一个匹配段所属串口 "
                               "(同一帧只发一路, 不匹配任何启用段的帧计入丢弃). BREAK/MAB 由发送器按 250000 8N2 "
                               "标准自动处理. 可用本机「Art-Net 测试」页发送数据来自环验证."));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#8fb3d9; font-size:12px;");
    root->addWidget(hint);

    auto *cfgBody = new QWidget;
    auto *cfg = new QVBoxLayout(cfgBody);
    cfg->setContentsMargins(0, 0, 0, 0);
    cfg->setSpacing(8);

    // ---- 顶部: 监听端口 / 输出选项 / 刷新 ----
    auto *rowTop = new QHBoxLayout;
    rowTop->setContentsMargins(0, 0, 0, 0);
    m_brListenIp = new QLineEdit; m_brListenIp->setText("0.0.0.0");
    m_brListenIp->setPlaceholderText(tr("0.0.0.0 / 本机 IP / 组播组 239.255.0.0"));
    m_brListenIp->setToolTip(tr("接收 Art-Net 包的本地监听地址:<br>"
        "0.0.0.0 = 所有网卡 (单播+广播, 并自动加入组播组 239.255.0.0)<br>"
        "本机网卡 IP = 只收发往该地址的单播包<br>"
        "239.x.x.x = 组播组地址, 只收发往该组的组播包<br>"
        "修改后需重启桥接生效"));
    m_brListenIp->setFixedWidth(120);
    m_brListenPort = new QSpinBox; m_brListenPort->setRange(1, 65535); m_brListenPort->setValue(6454);
    m_brFeedOut   = new QCheckBox(tr("转换结果 → 实时输出页"));
    m_brFeedOut->setChecked(true);
    auto *btnRefresh = new QPushButton(tr("刷新端口"));
    btnRefresh->setProperty("action", "tool");
    rowTop->addWidget(new QLabel(tr("监听 IP:")));
    rowTop->addWidget(m_brListenIp);
    rowTop->addSpacing(10);
    rowTop->addWidget(new QLabel(tr("监听 UDP 端口:")));
    rowTop->addWidget(m_brListenPort);
    rowTop->addSpacing(18);
    rowTop->addWidget(m_brFeedOut);
    rowTop->addStretch();
    rowTop->addWidget(btnRefresh);
    cfg->addLayout(rowTop);

    // ---- 输出配置表: 8 行 (启用 / 串口 / 起始 Universe / 覆盖域数 / 已发帧数) ----
    auto *tbl = new QGridLayout;
    tbl->setContentsMargins(0, 0, 0, 0);
    tbl->setHorizontalSpacing(10);
    tbl->setVerticalSpacing(6);

    auto makeHead = [](const QString &t) {
        auto *l = new QLabel(t);
        l->setStyleSheet("color:#8fb3d9; font-size:12px;");
        return l;
    };
    tbl->addWidget(makeHead(tr("#")),              0, 0);
    tbl->addWidget(makeHead(tr("启用")),           0, 1);
    tbl->addWidget(makeHead(tr("DMX 输出串口")),   0, 2);
    tbl->addWidget(makeHead(tr("起始 Universe")),  0, 3);
    tbl->addWidget(makeHead(tr("覆盖域数")),       0, 4);
    tbl->addWidget(makeHead(tr("已发帧数")),       0, 5);

    for (int i = 0; i < kArtToDmxOuts; ++i) {
        ArtToDmxOutput &row = m_brOuts[i];
        auto *idx = makeHead(QString::number(i + 1));
        idx->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.chkEnabled = new QCheckBox;
        row.chkEnabled->setChecked(i == 0);   // 第 1 行默认启用, 沿用原单串口默认值
        row.cmbPort = new QComboBox; row.cmbPort->setMinimumWidth(170);
        row.spinStart = new QSpinBox; row.spinStart->setRange(0, 32767); row.spinStart->setValue(i);
        row.spinSpan  = new QSpinBox; row.spinSpan->setRange(1, 8);      row.spinSpan->setValue(1);
        row.lblSent = new QLabel("--");
        row.lblSent->setStyleSheet("color:#00d4ff; font-size:13px; font-weight:bold; font-family:Consolas;");
        tbl->addWidget(idx, i + 1, 0);
        tbl->addWidget(row.chkEnabled, i + 1, 1, Qt::AlignCenter);
        tbl->addWidget(row.cmbPort, i + 1, 2);
        tbl->addWidget(row.spinStart, i + 1, 3);
        tbl->addWidget(row.spinSpan, i + 1, 4);
        tbl->addWidget(row.lblSent, i + 1, 5);
    }
    tbl->setColumnStretch(6, 1);
    cfg->addLayout(tbl);

    m_btnBrToggle = new QPushButton(tr("▶ 开始转换"));
    m_btnBrToggle->setProperty("action", "start");
    m_lblBrState = new QLabel(tr("<span style='color:#8fb3d9'>未启动</span>"));
    auto *rowBtn = new QHBoxLayout;
    rowBtn->setContentsMargins(0, 0, 0, 0);
    rowBtn->addWidget(m_btnBrToggle);
    rowBtn->addWidget(m_lblBrState);
    rowBtn->addStretch();
    cfg->addLayout(rowBtn);

    root->addWidget(createCard(tr("转换参数"), cfgBody), 0);

    // ---- 转换结果 ----
    auto *statBody = new QWidget;
    auto *st = new QGridLayout(statBody);
    st->setContentsMargins(0, 0, 0, 0);
    st->setHorizontalSpacing(18);
    st->setVerticalSpacing(10);

    auto makeStat = [](const QString &title, QLabel **out, QGridLayout *lay, int row, int col) {
        auto *cap = new QLabel(title);
        cap->setStyleSheet("color:#8fb3d9; font-size:12px;");
        auto *val = new QLabel("--");
        val->setStyleSheet("color:#00d4ff; font-size:18px; font-weight:bold; font-family:Consolas;");
        auto *box = new QVBoxLayout;
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        box->addWidget(cap);
        box->addWidget(val);
        lay->addLayout(box, row, col);
        *out = val;
    };

    makeStat(tr("接收 Art-Net 帧"), &m_lblBrRxFrames, st, 0, 0);
    makeStat(tr("发送 DMX 帧"),    &m_lblBrTxFrames, st, 0, 1);
    makeStat(tr("丢弃帧"),         &m_lblBrDropped,  st, 0, 2);
    makeStat(tr("最近域"),         &m_lblBrLastUni,  st, 1, 0);
    makeStat(tr("转换延迟"),       &m_lblBrLatency,  st, 1, 1);
    makeStat(tr("CH1"),            &m_lblBrCh1,      st, 1, 2);

    root->addWidget(createCard(tr("转换结果"), statBody), 0);
    root->addStretch();

    m_brStatTimer = new QTimer(this);
    connect(m_brStatTimer, &QTimer::timeout, this, [this]{
        quint64 totalTx = 0;
        for (int i = 0; i < kArtToDmxOuts; ++i) {
            const quint64 sent = m_brOuts[i].sender ? m_brOuts[i].sender->totalSentFrames() : 0;
            totalTx += sent;
            if (m_brOuts[i].lblSent)
                m_brOuts[i].lblSent->setText(m_brOuts[i].sender ? QString::number(sent)
                                                                : QStringLiteral("--"));
        }
        if (m_lblBrRxFrames) m_lblBrRxFrames->setText(QString::number(m_brRxCount));
        if (m_lblBrTxFrames) m_lblBrTxFrames->setText(QString::number(totalTx));
        if (m_lblBrDropped)  m_lblBrDropped->setText(QString::number(m_brDropCount));
        if (m_lblBrLastUni)  m_lblBrLastUni->setText(m_brRxCount ? QString::number(m_brLastUni) : "--");
        if (m_lblBrLatency)  m_lblBrLatency->setText(m_brLastLatencyUs > 0
                                                         ? QString("%1 µs").arg(m_brLastLatencyUs)
                                                         : "--");
        if (m_lblBrCh1)      m_lblBrCh1->setText(m_brLastDmx.size() >= 1
                                                     ? QString::number(uchar(m_brLastDmx.at(0)))
                                                     : "--");
    });
    m_brStatTimer->start(300);

    connect(btnRefresh, &QPushButton::clicked, this, [this]{ refreshArtToDmxPorts(); });
    connect(m_btnBrToggle, &QPushButton::clicked, this, [this]{
        if (m_brSocket) stopArtToDmx();
        else startArtToDmx();
    });

    refreshArtToDmxPorts();
    if (qEnvironmentVariableIntValue("ARTNET_UI_BRIDGE_AUTOSTART") == 1)
        QTimer::singleShot(700, this, [this]{ startArtToDmx(); });

    return page;
}

// ============================================================
// 页面: DMX512 → Art-Net 转换测试
//   最多 8 路串口接收 DMX512 帧, 各行按目标域转成 ArtDMX 数据报送往全局目标地址.
// ============================================================
void ArtNetRecorderMainWindow::refreshD2aPorts() {
    // 一次枚举, 刷新全部 8 行的串口下拉, 尽量保留各行已选项
    QStringList keeps;
    for (int i = 0; i < kDmxToArtIns; ++i)
        keeps << (m_d2aIns[i].cmbPort ? m_d2aIns[i].cmbPort->currentData().toString()
                                      : QString());
    const auto ports = QSerialPortInfo::availablePorts();
    for (int i = 0; i < kDmxToArtIns; ++i) {
        QComboBox *cmb = m_d2aIns[i].cmbPort;
        if (!cmb) continue;
        cmb->clear();
        for (const QSerialPortInfo &pi : ports) {
            cmb->addItem(pi.portName() + (pi.description().isEmpty()
                                              ? QString()
                                              : "  (" + pi.description() + ")"),
                         pi.portName());
        }
        if (cmb->count() == 0)
            cmb->addItem(tr("(无可用串口)"), QString());
        const int idx = cmb->findData(keeps.at(i));
        if (idx >= 0) cmb->setCurrentIndex(idx);
    }
}

void ArtNetRecorderMainWindow::startDmxToArt() {
    stopDmxToArt();

    // 对每个"启用且选了串口"的行开一个 QSerialPort; 单行失败只记日志, 不影响其余行
    int opened = 0;
    QStringList openedDesc;
    for (int i = 0; i < kDmxToArtIns; ++i) {
        DmxToArtInput &row = m_d2aIns[i];
        row.accum.clear();
        row.rxCount = 0;
        row.seq = 0;
        if (!row.chkEnabled || !row.chkEnabled->isChecked()) continue;
        const QString portName = row.cmbPort ? row.cmbPort->currentData().toString() : QString();
        if (portName.isEmpty()) {
            appendLog(tr("DMX→Art-Net: 输入 %1 已启用但未选择串口").arg(i + 1), "warn");
            continue;
        }
        row.serial = new QSerialPort(this);
        if (!openDmxPort(row.serial, portName)) {
            appendLog(tr("DMX→Art-Net: 打开串口 %1 失败 (输入 %2, %3)")
                          .arg(portName, row.serial->errorString()).arg(i + 1), "error");
            delete row.serial; row.serial = nullptr;
            continue;
        }
        const int idx = i;
        connect(row.serial, &QSerialPort::readyRead, this, [this, idx]{ onD2aSerialData(idx); });
        ++opened;
        openedDesc << tr("输入%1 %2 → 域%3")
                      .arg(i + 1).arg(portName).arg(row.spinUni->value());
    }
    if (opened == 0) {
        appendLog(tr("DMX→Art-Net: 没有成功打开的 DMX 输入串口, 请检查启用行与端口选择"), "error");
        return;
    }

    m_d2aSocket = new QUdpSocket(this);
    if (!m_d2aSocket->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        appendLog(tr("DMX→Art-Net: 创建发送 socket 失败"), "error");
        m_d2aSocket->deleteLater(); m_d2aSocket = nullptr;
        stopDmxToArt();
        return;
    }

    m_d2aLastDmx.clear();
    m_d2aTxCount = m_d2aDropCount = 0;
    m_d2aLatencyUs = 0;
    m_d2aLastUni = 0;

    m_btnD2aToggle->setText(tr("■ 停止转换"));
    m_lblD2aState->setText(tr("<span style='color:#00e676'>转换中</span> "
                              "<span style='color:#8fb3d9'>%1 路串口 → UDP %2:%3</span>")
                               .arg(opened)
                               .arg(m_d2aTargetIp->text().trimmed())
                               .arg(m_d2aTargetPort->value()));
    appendLog(tr("DMX→Art-Net: 已开始 %1 路串口 (250000 8N2) → %2:%3: %4")
                  .arg(opened)
                  .arg(m_d2aTargetIp->text().trimmed())
                  .arg(m_d2aTargetPort->value())
                  .arg(openedDesc.join(", ")), "info");
}

void ArtNetRecorderMainWindow::stopDmxToArt() {
    for (int i = 0; i < kDmxToArtIns; ++i) {
        if (m_d2aIns[i].serial) {
            m_d2aIns[i].serial->close();
            m_d2aIns[i].serial->deleteLater();
            m_d2aIns[i].serial = nullptr;
        }
        m_d2aIns[i].accum.clear();
    }
    if (m_d2aSocket) {
        m_d2aSocket->close();
        m_d2aSocket->deleteLater();
        m_d2aSocket = nullptr;
    }
    if (m_btnD2aToggle) m_btnD2aToggle->setText(tr("▶ 开始转换"));
    if (m_lblD2aState)
        m_lblD2aState->setText(tr("<span style='color:#8fb3d9'>未启动</span>"));
}

void ArtNetRecorderMainWindow::onD2aSerialData(int row) {
    if (row < 0 || row >= kDmxToArtIns) return;
    DmxToArtInput &in = m_d2aIns[row];
    if (!in.serial || !m_d2aSocket) return;
    in.accum.append(in.serial->readAll());

    const int fmt = m_d2aFormat ? m_d2aFormat->currentIndex() : 0;
    const int frameLen = (fmt == 1) ? 514 : (fmt == 2 ? 512 : 513);

    while (in.accum.size() >= frameLen) {
        const QByteArray frame = in.accum.left(frameLen);
        QByteArray dmx;
        if (frameLen == 512) {
            dmx = frame;
        } else {
            // 兼容起始码前多一个字节的适配器
            if (uchar(frame.at(0)) == 0x00)      dmx = frame.mid(1, 512);
            else if (uchar(frame.at(1)) == 0x00) dmx = frame.mid(2, 512);
            else { in.accum.remove(0, 1); ++m_d2aDropCount; continue; }
        }
        in.accum.remove(0, frameLen);
        if (dmx.size() < 512) dmx.append(QByteArray(512 - dmx.size(), 0));

        const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        in.rxCount++;
        m_d2aLastDmx = dmx.left(512);
        m_d2aLastUni = quint16(in.spinUni ? in.spinUni->value() : 0);

        if (m_d2aFeedIn && m_d2aFeedIn->isChecked())
            feedLiveUniverse(true, m_d2aLastUni, m_d2aLastDmx, nowUs);

        convertDmxToArt(row, m_d2aLastDmx);
        m_d2aLatencyUs = QDateTime::currentMSecsSinceEpoch() * 1000 - nowUs;
    }

    if (in.accum.size() > frameLen * 4)
        in.accum = in.accum.right(frameLen * 2);
}

void ArtNetRecorderMainWindow::convertDmxToArt(int row, const QByteArray &dmx) {
    if (!m_d2aSocket || row < 0 || row >= kDmxToArtIns) return;
    const QHostAddress target(m_d2aTargetIp ? m_d2aTargetIp->text().trimmed() : "127.0.0.1");
    if (target.isNull()) { ++m_d2aDropCount; return; }

    DmxToArtInput &in = m_d2aIns[row];
    const quint16 uni = quint16(in.spinUni ? in.spinUni->value() : 0);
    const QByteArray pkt = buildArtNetTestPacket(uni, dmx, in.seq++);
    const qint64 sent = m_d2aSocket->writeDatagram(
        pkt, target, quint16(m_d2aTargetPort ? m_d2aTargetPort->value() : 6454));
    if (sent <= 0) { ++m_d2aDropCount; return; }

    m_d2aTxCount++;
    if (m_d2aFeedOut && m_d2aFeedOut->isChecked())
        feedLiveUniverse(false, uni, dmx, QDateTime::currentMSecsSinceEpoch() * 1000);
}

QWidget *ArtNetRecorderMainWindow::buildPageDmxToArt() {
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto *hint = new QLabel(tr("DMX512 → Art-Net 转换测试 — 支持最多 8 路 DMX 串口同时输入 (250000 8N2), "
                               "每路按各自「目标 Universe」打包成 ArtDMX 数据报, 立即发往全局目标 IP:端口; "
                               "帧格式与目标地址对所有输入行共用. 可用本机「Art-Net 测试 → 接收测试」验证输出."));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#8fb3d9; font-size:12px;");
    root->addWidget(hint);

    auto *cfgBody = new QWidget;
    auto *cfg = new QVBoxLayout(cfgBody);
    cfg->setContentsMargins(0, 0, 0, 0);
    cfg->setSpacing(8);

    m_d2aFormat = new QComboBox;
    m_d2aFormat->addItem(tr("513 字节（起始码 + 512 数据）"));
    m_d2aFormat->addItem(tr("514 字节（起始码前多 1 字节）"));
    m_d2aFormat->addItem(tr("512 字节（无起始码）"));
    m_d2aTargetIp = new QLineEdit("127.0.0.1"); m_d2aTargetIp->setMinimumWidth(140);
    m_d2aTargetPort = new QSpinBox; m_d2aTargetPort->setRange(1, 65535); m_d2aTargetPort->setValue(6454);
    m_d2aFeedIn = new QCheckBox(tr("接收数据 → 实时输入页"));
    m_d2aFeedIn->setChecked(true);
    m_d2aFeedOut = new QCheckBox(tr("发送数据 → 实时输出页"));
    m_d2aFeedOut->setChecked(true);

    auto *btnRefresh = new QPushButton(tr("刷新端口"));
    btnRefresh->setProperty("action", "tool");
    m_btnD2aToggle = new QPushButton(tr("▶ 开始转换"));
    m_btnD2aToggle->setProperty("action", "start");
    m_lblD2aState = new QLabel(tr("<span style='color:#8fb3d9'>未启动</span>"));

    // ---- 顶部: 全局目标地址 / 帧格式 / 刷新 ----
    auto *rowTop = new QHBoxLayout;
    rowTop->setContentsMargins(0, 0, 0, 0);
    rowTop->addWidget(new QLabel(tr("目标 IP:")));
    rowTop->addWidget(m_d2aTargetIp);
    rowTop->addWidget(new QLabel(tr("目标端口:")));
    rowTop->addWidget(m_d2aTargetPort);
    rowTop->addSpacing(18);
    rowTop->addWidget(new QLabel(tr("帧格式:")));
    rowTop->addWidget(m_d2aFormat);
    rowTop->addStretch();
    rowTop->addWidget(btnRefresh);
    cfg->addLayout(rowTop);

    // ---- 输入配置表: 8 行 (启用 / 串口 / 目标 Universe / 收到帧数) ----
    auto *tbl = new QGridLayout;
    tbl->setContentsMargins(0, 0, 0, 0);
    tbl->setHorizontalSpacing(10);
    tbl->setVerticalSpacing(6);

    auto makeHead = [](const QString &t) {
        auto *l = new QLabel(t);
        l->setStyleSheet("color:#8fb3d9; font-size:12px;");
        return l;
    };
    tbl->addWidget(makeHead(tr("#")),               0, 0);
    tbl->addWidget(makeHead(tr("启用")),            0, 1);
    tbl->addWidget(makeHead(tr("DMX 输入串口")),    0, 2);
    tbl->addWidget(makeHead(tr("目标 Universe")),   0, 3);
    tbl->addWidget(makeHead(tr("收到帧数")),        0, 4);

    for (int i = 0; i < kDmxToArtIns; ++i) {
        DmxToArtInput &row = m_d2aIns[i];
        auto *idx = makeHead(QString::number(i + 1));
        idx->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.chkEnabled = new QCheckBox;
        row.chkEnabled->setChecked(i == 0);   // 第 1 行默认启用, 沿用原单串口逻辑
        row.cmbPort = new QComboBox; row.cmbPort->setMinimumWidth(170);
        row.spinUni = new QSpinBox; row.spinUni->setRange(0, 32767); row.spinUni->setValue(i);
        row.lblRx = new QLabel("--");
        row.lblRx->setStyleSheet("color:#00d4ff; font-size:13px; font-weight:bold; font-family:Consolas;");
        tbl->addWidget(idx, i + 1, 0);
        tbl->addWidget(row.chkEnabled, i + 1, 1, Qt::AlignCenter);
        tbl->addWidget(row.cmbPort, i + 1, 2);
        tbl->addWidget(row.spinUni, i + 1, 3);
        tbl->addWidget(row.lblRx, i + 1, 4);
    }
    tbl->setColumnStretch(5, 1);
    cfg->addLayout(tbl);

    auto *rowOpt = new QHBoxLayout;
    rowOpt->setContentsMargins(0, 0, 0, 0);
    rowOpt->addWidget(m_d2aFeedIn);
    rowOpt->addSpacing(24);
    rowOpt->addWidget(m_d2aFeedOut);
    rowOpt->addStretch();
    cfg->addLayout(rowOpt);

    auto *rowBtn = new QHBoxLayout;
    rowBtn->setContentsMargins(0, 0, 0, 0);
    rowBtn->addWidget(m_btnD2aToggle);
    rowBtn->addWidget(m_lblD2aState);
    rowBtn->addStretch();
    cfg->addLayout(rowBtn);

    root->addWidget(createCard(tr("转换参数"), cfgBody), 0);

    auto *statBody = new QWidget;
    auto *st = new QGridLayout(statBody);
    st->setContentsMargins(0, 0, 0, 0);
    st->setHorizontalSpacing(18);
    st->setVerticalSpacing(10);

    auto makeStat = [](const QString &title, QLabel **out, QGridLayout *lay, int row, int col) {
        auto *cap = new QLabel(title);
        cap->setStyleSheet("color:#8fb3d9; font-size:12px;");
        auto *val = new QLabel("--");
        val->setStyleSheet("color:#00d4ff; font-size:18px; font-weight:bold; font-family:Consolas;");
        auto *box = new QVBoxLayout;
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        box->addWidget(cap);
        box->addWidget(val);
        lay->addLayout(box, row, col);
        *out = val;
    };

    makeStat(tr("接收 DMX 帧"),    &m_lblD2aRx,      st, 0, 0);
    makeStat(tr("发送 Art-Net 帧"), &m_lblD2aTx,      st, 0, 1);
    makeStat(tr("丢弃帧"),         &m_lblD2aDrop,    st, 0, 2);
    makeStat(tr("目标域"),         &m_lblD2aUni,     st, 1, 0);
    makeStat(tr("转换延迟"),       &m_lblD2aLatency, st, 1, 1);
    makeStat(tr("CH1"),            &m_lblD2aCh1,     st, 1, 2);

    root->addWidget(createCard(tr("转换结果"), statBody), 0);
    root->addStretch();

    m_d2aStatTimer = new QTimer(this);
    connect(m_d2aStatTimer, &QTimer::timeout, this, [this]{
        quint64 totalRx = 0;
        for (int i = 0; i < kDmxToArtIns; ++i) {
            totalRx += m_d2aIns[i].rxCount;
            if (m_d2aIns[i].lblRx)
                m_d2aIns[i].lblRx->setText(m_d2aIns[i].serial
                                               ? QString::number(m_d2aIns[i].rxCount)
                                               : QStringLiteral("--"));
        }
        if (m_lblD2aRx)      m_lblD2aRx->setText(QString::number(totalRx));
        if (m_lblD2aTx)      m_lblD2aTx->setText(QString::number(m_d2aTxCount));
        if (m_lblD2aDrop)    m_lblD2aDrop->setText(QString::number(m_d2aDropCount));
        if (m_lblD2aUni)     m_lblD2aUni->setText(totalRx ? QString::number(m_d2aLastUni) : "--");
        if (m_lblD2aLatency) m_lblD2aLatency->setText(m_d2aLatencyUs > 0
                                                          ? QString("%1 µs").arg(m_d2aLatencyUs)
                                                          : "--");
        if (m_lblD2aCh1)     m_lblD2aCh1->setText(m_d2aLastDmx.size() >= 1
                                                      ? QString::number(uchar(m_d2aLastDmx.at(0)))
                                                      : "--");
    });
    m_d2aStatTimer->start(300);

    connect(btnRefresh, &QPushButton::clicked, this, [this]{ refreshD2aPorts(); });
    connect(m_btnD2aToggle, &QPushButton::clicked, this, [this]{
        if (m_d2aSocket) stopDmxToArt();
        else startDmxToArt();
    });

    refreshD2aPorts();
    if (qEnvironmentVariableIntValue("ARTNET_UI_D2A_AUTOSTART") == 1)
        QTimer::singleShot(700, this, [this]{ startDmxToArt(); });
    // 自动化验证: 注入一帧合成 DMX 数据, 走完整的解析/转换路径
    if (qEnvironmentVariableIntValue("ARTNET_UI_D2A_SELFTEST") == 1)
        QTimer::singleShot(1500, this, [this]{
            if (!m_d2aIns[0].serial) return;
            QByteArray f(1, char(0x00));
            f.append(QByteArray(512, 0));
            f[1] = char(200);
            m_d2aIns[0].accum.append(f);
            onD2aSerialData(0);
        });

    return page;
}

// ============================================================
// 页面: NPU → DMX512 转换测试
//   复用生产 NpuSource 接收 MA NPU (Ma-Net2) DMX 数据包 (单播/广播, 默认 UDP 9999),
//   按 8 路输出的域段映射 (universeRaw 0..255) 转成 DMX512 串口帧发出.
// ============================================================
void ArtNetRecorderMainWindow::refreshNp2dPorts() {
    // 一次枚举, 刷新全部 8 行的串口下拉, 尽量保留各行已选项
    QStringList keeps;
    for (int i = 0; i < kNp2dOuts; ++i)
        keeps << (m_np2dOuts[i].cmbPort ? m_np2dOuts[i].cmbPort->currentData().toString()
                                        : QString());
    const auto ports = QSerialPortInfo::availablePorts();
    for (int i = 0; i < kNp2dOuts; ++i) {
        QComboBox *cmb = m_np2dOuts[i].cmbPort;
        if (!cmb) continue;
        cmb->clear();
        for (const QSerialPortInfo &pi : ports) {
            cmb->addItem(pi.portName() + (pi.description().isEmpty()
                                              ? QString()
                                              : "  (" + pi.description() + ")"),
                         pi.portName());
        }
        if (cmb->count() == 0)
            cmb->addItem(tr("(无可用串口)"), QString());
        const int idx = cmb->findData(keeps.at(i));
        if (idx >= 0) cmb->setCurrentIndex(idx);
    }
}

void ArtNetRecorderMainWindow::startNpuToDmx() {
    stopNpuToDmx();
    if (!m_np2dListenPort) return;

    // 对每个"启用且选了串口"的行创建一个 Dmx512Sender 并 open;
    // 单行失败只记日志, 不影响其余行
    int opened = 0;
    QStringList openedDesc;
    for (int i = 0; i < kNp2dOuts; ++i) {
        Np2dOutput &row = m_np2dOuts[i];
        if (!row.chkEnabled || !row.chkEnabled->isChecked()) continue;
        const QString portName = row.cmbPort ? row.cmbPort->currentData().toString() : QString();
        if (portName.isEmpty()) {
            appendLog(tr("NPU→DMX: 输出 %1 已启用但未选择串口").arg(i + 1), "warn");
            continue;
        }
        auto *sender = new Dmx512Sender(this);
        if (!sender->open(portName)) {
            appendLog(tr("NPU→DMX: 打开串口 %1 失败 (输出 %2)").arg(portName).arg(i + 1),
                      "error");
            delete sender;
            continue;
        }
        row.sender = sender;
        ++opened;
        openedDesc << tr("输出%1 %2 域%3起×%4")
                      .arg(i + 1).arg(portName)
                      .arg(row.spinStart->value()).arg(row.spinSpan->value());
    }
    if (opened == 0) {
        appendLog(tr("NPU→DMX: 没有成功打开的 DMX 输出串口, 请检查启用行与端口选择"), "error");
        return;
    }

    // 复用生产 NpuSource: 自管 socket + 包解析, 经 frameReceived 送出 LightFrame
    const QString bindIp = m_np2dListenIp && !m_np2dListenIp->text().trimmed().isEmpty()
                               ? m_np2dListenIp->text().trimmed() : QStringLiteral("0.0.0.0");
    m_np2dSource = new NpuSource(this);
    connect(m_np2dSource, &NpuSource::frameReceived,
            this, [this](const LightFrame &f){ onNp2dFrame(f); });
    ConnectionParams params;
    params.protocol = ProtocolType::Npu;
    params.localBindAddress = bindIp;
    params.localPort = quint16(m_np2dListenPort->value());
    if (!m_np2dSource->start(params)) {
        appendLog(tr("NPU→DMX: 绑定 NPU 监听 %1:%2 失败").arg(bindIp).arg(params.localPort),
                  "error");
        m_np2dSource->deleteLater(); m_np2dSource = nullptr;
        stopNpuToDmx();
        return;
    }

    m_np2dRxCount = m_np2dDropCount = 0;
    m_np2dLastUni = 0;
    m_np2dLastDmx.clear();

    m_btnNp2dToggle->setText(tr("■ 停止转换"));
    m_lblNp2dState->setText(tr("<span style='color:#00e676'>转换中</span> "
                               "<span style='color:#8fb3d9'>NPU %1:%2 → %3 路串口输出</span>")
                                .arg(bindIp).arg(params.localPort).arg(opened));
    appendLog(tr("NPU→DMX: 已开始 %1:%2 → %3 路串口 (250000 8N2): %4")
                  .arg(bindIp).arg(params.localPort).arg(opened).arg(openedDesc.join(", ")),
              "info");
}

void ArtNetRecorderMainWindow::stopNpuToDmx() {
    if (m_np2dSource) {
        m_np2dSource->stop();
        m_np2dSource->deleteLater();
        m_np2dSource = nullptr;
    }
    for (int i = 0; i < kNp2dOuts; ++i) {
        if (m_np2dOuts[i].sender) {
            m_np2dOuts[i].sender->close();
            m_np2dOuts[i].sender->deleteLater();
            m_np2dOuts[i].sender = nullptr;
        }
    }
    if (m_btnNp2dToggle) m_btnNp2dToggle->setText(tr("▶ 开始转换"));
    if (m_lblNp2dState)
        m_lblNp2dState->setText(tr("<span style='color:#8fb3d9'>未启动</span>"));
}

void ArtNetRecorderMainWindow::onNp2dFrame(const LightFrame &frame) {
    m_np2dRxCount++;
    const int uni = frame.universeRaw;   // NPU 原始域编号 0..255
    m_np2dLastUni = quint16(uni);
    m_np2dLastDmx = frame.dmxData;
    if (m_np2dLastDmx.size() < 512)
        m_np2dLastDmx.append(QByteArray(512 - m_np2dLastDmx.size(), 0));

    // 路由: 第一个 启用 && sender 已打开 && universe ∈ [起始, 起始+覆盖数) 的行
    Dmx512Sender *target = nullptr;
    for (int i = 0; i < kNp2dOuts; ++i) {
        Np2dOutput &row = m_np2dOuts[i];
        if (!row.chkEnabled || !row.chkEnabled->isChecked()
            || !row.sender || !row.sender->isOpen()) continue;
        const int start = row.spinStart ? row.spinStart->value() : 0;
        const int span  = row.spinSpan  ? row.spinSpan->value()  : 1;
        if (uni >= start && uni < start + span) { target = row.sender; break; }
    }
    if (!target) { ++m_np2dDropCount; return; }

    target->sendFrame(m_np2dLastDmx);
    if (m_np2dFeedOut && m_np2dFeedOut->isChecked())
        feedLiveUniverse(false, quint16(uni), m_np2dLastDmx,
                         QDateTime::currentMSecsSinceEpoch() * 1000);
}

QWidget *ArtNetRecorderMainWindow::buildPageNpuToDmx() {
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto *hint = new QLabel(tr("NPU → DMX512 转换测试 — 监听 MA NPU (Ma-Net2) DMX 数据包 (单播/广播, 默认 UDP 9999), "
                               "支持最多 8 路 DMX 串口输出; 每行按「起始 Universe (0..255) + 覆盖域数」划分域段, "
                               "收到帧后路由到第一个匹配段所属串口 (同一帧只发一路, 不匹配任何启用段的帧计入丢弃). "
                               "BREAK/MAB 由发送器按 250000 8N2 标准自动处理."));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#8fb3d9; font-size:12px;");
    root->addWidget(hint);

    auto *cfgBody = new QWidget;
    auto *cfg = new QVBoxLayout(cfgBody);
    cfg->setContentsMargins(0, 0, 0, 0);
    cfg->setSpacing(8);

    // ---- 顶部: 监听 IP / 端口 / 输出选项 / 刷新 ----
    auto *rowTop = new QHBoxLayout;
    rowTop->setContentsMargins(0, 0, 0, 0);
    m_np2dListenIp = new QLineEdit("0.0.0.0"); m_np2dListenIp->setMinimumWidth(120);
    m_np2dListenPort = new QSpinBox; m_np2dListenPort->setRange(1, 65535);
    m_np2dListenPort->setValue(NpuProtocol::DEFAULT_PORT);
    m_np2dFeedOut   = new QCheckBox(tr("转换结果 → 实时输出页"));
    m_np2dFeedOut->setChecked(true);
    auto *btnRefresh = new QPushButton(tr("刷新端口"));
    btnRefresh->setProperty("action", "tool");
    rowTop->addWidget(new QLabel(tr("监听 IP:")));
    rowTop->addWidget(m_np2dListenIp);
    rowTop->addWidget(new QLabel(tr("监听 UDP 端口:")));
    rowTop->addWidget(m_np2dListenPort);
    rowTop->addSpacing(18);
    rowTop->addWidget(m_np2dFeedOut);
    rowTop->addStretch();
    rowTop->addWidget(btnRefresh);
    cfg->addLayout(rowTop);

    // ---- 输出配置表: 8 行 (启用 / 串口 / 起始 Universe / 覆盖域数 / 已发帧数) ----
    auto *tbl = new QGridLayout;
    tbl->setContentsMargins(0, 0, 0, 0);
    tbl->setHorizontalSpacing(10);
    tbl->setVerticalSpacing(6);

    auto makeHead = [](const QString &t) {
        auto *l = new QLabel(t);
        l->setStyleSheet("color:#8fb3d9; font-size:12px;");
        return l;
    };
    tbl->addWidget(makeHead(tr("#")),              0, 0);
    tbl->addWidget(makeHead(tr("启用")),           0, 1);
    tbl->addWidget(makeHead(tr("DMX 输出串口")),   0, 2);
    tbl->addWidget(makeHead(tr("起始 Universe")),  0, 3);
    tbl->addWidget(makeHead(tr("覆盖域数")),       0, 4);
    tbl->addWidget(makeHead(tr("已发帧数")),       0, 5);

    for (int i = 0; i < kNp2dOuts; ++i) {
        Np2dOutput &row = m_np2dOuts[i];
        auto *idx = makeHead(QString::number(i + 1));
        idx->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.chkEnabled = new QCheckBox;
        row.chkEnabled->setChecked(i == 0);   // 第 1 行默认启用
        row.cmbPort = new QComboBox; row.cmbPort->setMinimumWidth(170);
        row.spinStart = new QSpinBox; row.spinStart->setRange(0, 255); row.spinStart->setValue(i);
        row.spinSpan  = new QSpinBox; row.spinSpan->setRange(1, 8);    row.spinSpan->setValue(1);
        row.lblSent = new QLabel("--");
        row.lblSent->setStyleSheet("color:#00d4ff; font-size:13px; font-weight:bold; font-family:Consolas;");
        tbl->addWidget(idx, i + 1, 0);
        tbl->addWidget(row.chkEnabled, i + 1, 1, Qt::AlignCenter);
        tbl->addWidget(row.cmbPort, i + 1, 2);
        tbl->addWidget(row.spinStart, i + 1, 3);
        tbl->addWidget(row.spinSpan, i + 1, 4);
        tbl->addWidget(row.lblSent, i + 1, 5);
    }
    tbl->setColumnStretch(6, 1);
    cfg->addLayout(tbl);

    m_btnNp2dToggle = new QPushButton(tr("▶ 开始转换"));
    m_btnNp2dToggle->setProperty("action", "start");
    m_lblNp2dState = new QLabel(tr("<span style='color:#8fb3d9'>未启动</span>"));
    auto *rowBtn = new QHBoxLayout;
    rowBtn->setContentsMargins(0, 0, 0, 0);
    rowBtn->addWidget(m_btnNp2dToggle);
    rowBtn->addWidget(m_lblNp2dState);
    rowBtn->addStretch();
    cfg->addLayout(rowBtn);

    root->addWidget(createCard(tr("转换参数"), cfgBody), 0);

    // ---- 转换结果 ----
    auto *statBody = new QWidget;
    auto *st = new QGridLayout(statBody);
    st->setContentsMargins(0, 0, 0, 0);
    st->setHorizontalSpacing(18);
    st->setVerticalSpacing(10);

    auto makeStat = [](const QString &title, QLabel **out, QGridLayout *lay, int row, int col) {
        auto *cap = new QLabel(title);
        cap->setStyleSheet("color:#8fb3d9; font-size:12px;");
        auto *val = new QLabel("--");
        val->setStyleSheet("color:#00d4ff; font-size:18px; font-weight:bold; font-family:Consolas;");
        auto *box = new QVBoxLayout;
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        box->addWidget(cap);
        box->addWidget(val);
        lay->addLayout(box, row, col);
        *out = val;
    };

    makeStat(tr("接收 NPU 帧"),  &m_lblNp2dRxFrames, st, 0, 0);
    makeStat(tr("发送 DMX 帧"),  &m_lblNp2dTxFrames, st, 0, 1);
    makeStat(tr("丢弃帧"),       &m_lblNp2dDropped,  st, 0, 2);
    makeStat(tr("最近域"),       &m_lblNp2dLastUni,  st, 1, 0);
    makeStat(tr("CH1"),          &m_lblNp2dCh1,      st, 1, 1);

    root->addWidget(createCard(tr("转换结果"), statBody), 0);
    root->addStretch();

    m_np2dStatTimer = new QTimer(this);
    connect(m_np2dStatTimer, &QTimer::timeout, this, [this]{
        quint64 totalTx = 0;
        for (int i = 0; i < kNp2dOuts; ++i) {
            const quint64 sent = m_np2dOuts[i].sender ? m_np2dOuts[i].sender->totalSentFrames() : 0;
            totalTx += sent;
            if (m_np2dOuts[i].lblSent)
                m_np2dOuts[i].lblSent->setText(m_np2dOuts[i].sender ? QString::number(sent)
                                                                    : QStringLiteral("--"));
        }
        if (m_lblNp2dRxFrames) m_lblNp2dRxFrames->setText(QString::number(m_np2dRxCount));
        if (m_lblNp2dTxFrames) m_lblNp2dTxFrames->setText(QString::number(totalTx));
        if (m_lblNp2dDropped)  m_lblNp2dDropped->setText(QString::number(m_np2dDropCount));
        if (m_lblNp2dLastUni)  m_lblNp2dLastUni->setText(m_np2dRxCount ? QString::number(m_np2dLastUni) : "--");
        if (m_lblNp2dCh1)      m_lblNp2dCh1->setText(m_np2dLastDmx.size() >= 1
                                                        ? QString::number(uchar(m_np2dLastDmx.at(0)))
                                                        : "--");
    });
    m_np2dStatTimer->start(300);

    connect(btnRefresh, &QPushButton::clicked, this, [this]{ refreshNp2dPorts(); });
    connect(m_btnNp2dToggle, &QPushButton::clicked, this, [this]{
        if (m_np2dSource) stopNpuToDmx();
        else startNpuToDmx();
    });

    refreshNp2dPorts();
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildPageDmxTest() {
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto *hint = new QLabel(tr("DMX512 独立测试 — 串口发送 (250000 8N2, Break/MAB) + 环回接收比对 + 时序测量 + 走灯。"
                               "环回测试需把发送口与接收口接成环回（或使用两个物理口对连）。"));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#8fb3d9; font-size:12px;");
    root->addWidget(hint);

    // ---- 端口与参数 ----
    auto *cfgBody = new QWidget;
    auto *cfg = new QGridLayout(cfgBody);
    cfg->setContentsMargins(0, 0, 0, 0);
    cfg->setHorizontalSpacing(12);
    cfg->setVerticalSpacing(8);

    m_dmxTxPort = new QComboBox;  m_dmxTxPort->setMinimumWidth(150);
    m_dmxRxPort = new QComboBox;  m_dmxRxPort->setMinimumWidth(150);
    m_dmxFrameRate = new QSpinBox; m_dmxFrameRate->setRange(1, 44); m_dmxFrameRate->setValue(40);
    m_dmxFrameRate->setSuffix(" fps");
    m_dmxMode = new QComboBox;
    m_dmxMode->addItem(tr("恒值输出"));
    m_dmxMode->addItem(tr("单通道扫描"));
    m_dmxMode->addItem(tr("逐通道走灯"));
    m_dmxChannel = new QSpinBox;   m_dmxChannel->setRange(1, 512); m_dmxChannel->setValue(1);
    m_dmxLevel   = new QSpinBox;   m_dmxLevel->setRange(0, 255);   m_dmxLevel->setValue(255);
    m_dmxStepMs  = new QSpinBox;   m_dmxStepMs->setRange(10, 5000); m_dmxStepMs->setValue(100);
    m_dmxStepMs->setSuffix(" ms");

    auto *btnRefresh = new QPushButton(tr("刷新端口"));
    btnRefresh->setProperty("action", "tool");
    m_btnDmxTx = new QPushButton(tr("▶ 开始发送"));
    m_btnDmxTx->setProperty("action", "start");
    m_btnDmxRxTest = new QPushButton(tr("▶ 接收测试"));
    m_btnDmxRxTest->setProperty("action", "tool");
    m_btnDmxRxTest->setToolTip(tr("只用「接收端口 (环回)」接收外部 DMX512 帧, 不发送; 与开始发送互斥"));

    m_lblDmxState = new QLabel(tr("<span style='color:#8fb3d9'>未启动</span>"));

    cfg->addWidget(new QLabel(tr("发送端口:")), 0, 0);
    cfg->addWidget(m_dmxTxPort, 0, 1);
    cfg->addWidget(btnRefresh, 0, 2);
    cfg->addWidget(new QLabel(tr("接收端口 (环回):")), 0, 3);
    cfg->addWidget(m_dmxRxPort, 0, 4);

    cfg->addWidget(new QLabel(tr("刷新率:")), 1, 0);
    cfg->addWidget(m_dmxFrameRate, 1, 1);
    cfg->addWidget(new QLabel(tr("测试模式:")), 1, 3);
    cfg->addWidget(m_dmxMode, 1, 4);

    cfg->addWidget(new QLabel(tr("通道 / 电平 / 步进:")), 2, 0);
    auto *rowCtl = new QHBoxLayout;
    rowCtl->setContentsMargins(0, 0, 0, 0);
    rowCtl->addWidget(m_dmxChannel); rowCtl->addWidget(m_dmxLevel);
    rowCtl->addWidget(m_dmxStepMs);  rowCtl->addStretch();
    cfg->addLayout(rowCtl, 2, 1, 1, 2);

    // Break 方式 — 普通 RS485 芯片无 Break, 可用低波特率发送 0x00 模拟
    m_dmxBreakMode = new QComboBox;
    m_dmxBreakMode->addItem(tr("串口 Break（标准 DMX 接口）"));
    m_dmxBreakMode->addItem(tr("波特率模拟（普通 RS485）"));
    m_dmxBreakMode->addItem(tr("不发送 Break"));
    m_dmxBreakMode->setToolTip(tr("普通 USB-RS485 芯片没有真 Break：选「波特率模拟」会先切到 57600 发送 1 个 0x00 产生约 156µs 低电平，再切回 250000 发送起始码与数据"));
    cfg->addWidget(new QLabel(tr("Break 方式:")), 2, 3);
    cfg->addWidget(m_dmxBreakMode, 2, 4);

    // 接收帧格式 — 适配不同 USB-RS485 (部分芯片无真 Break, 会多发一个 0x00)
    m_dmxRxFormat = new QComboBox;
    m_dmxRxFormat->addItem(tr("513 字节（起始码 + 512 数据）"));
    m_dmxRxFormat->addItem(tr("514 字节（起始码前多 1 字节）"));
    m_dmxRxFormat->addItem(tr("512 字节（无起始码）"));
    m_dmxRxFormat->setToolTip(tr("有帧数但通道值全为 0 时，依次尝试「514」或「512」；USB-RS485 芯片通常无法产生真正的 DMX Break"));
    cfg->addWidget(new QLabel(tr("接收帧格式:")), 3, 0);
    cfg->addWidget(m_dmxRxFormat, 3, 1, 1, 2);

    // 数据送入实时监视页 — 在实时输入/输出页可直接看到 512 通道数值
    m_dmxFeedOut = new QCheckBox(tr("发送数据 → 实时输出页（域 0）"));
    m_dmxFeedOut->setChecked(true);
    m_dmxFeedOut->setToolTip(tr("勾选后, 每帧发送的 512 通道数据会同步写入实时输出页, 可在那里查看推子数值"));
    m_dmxFeedIn = new QCheckBox(tr("接收数据 → 实时输入页（域 0）"));
    m_dmxFeedIn->setChecked(true);
    m_dmxFeedIn->setToolTip(tr("勾选后, 环回接收到的 512 通道数据会同步写入实时输入页, 可在那里查看推子数值"));
    m_dmxLoopbackSim = new QCheckBox(tr("软件环回模拟"));
    m_dmxLoopbackSim->setChecked(false);
    m_dmxLoopbackSim->setToolTip(tr("没有可用环回硬件时勾选: 把发送帧直接当作接收帧写入实时输入页, 并计入接收帧数/帧间隔(模拟值, 不验证物理链路)"));
    auto *rowFeed = new QHBoxLayout;
    rowFeed->setContentsMargins(0, 0, 0, 0);
    rowFeed->addWidget(m_dmxFeedOut);
    rowFeed->addSpacing(24);
    rowFeed->addWidget(m_dmxFeedIn);
    rowFeed->addSpacing(24);
    rowFeed->addWidget(m_dmxLoopbackSim);
    rowFeed->addStretch();
    cfg->addLayout(rowFeed, 4, 0, 1, 5);

    auto *rowBtns = new QHBoxLayout;
    rowBtns->setContentsMargins(0, 0, 0, 0);
    rowBtns->addWidget(m_btnDmxTx);
    rowBtns->addWidget(m_btnDmxRxTest);
    rowBtns->addWidget(m_lblDmxState);
    rowBtns->addStretch();
    cfg->addLayout(rowBtns, 5, 0, 1, 5);

    cfg->setColumnStretch(4, 1);
    root->addWidget(createCard(tr("端口与发送参数"), cfgBody), 0);

    // ---- 测试结果 ----
    auto *statBody = new QWidget;
    auto *st = new QGridLayout(statBody);
    st->setContentsMargins(0, 0, 0, 0);
    st->setHorizontalSpacing(18);
    st->setVerticalSpacing(10);

    auto makeStat = [](const QString &title, QLabel **out, QWidget *parent, QGridLayout *lay,
                       int row, int col) {
        auto *cap = new QLabel(title);
        cap->setStyleSheet("color:#8fb3d9; font-size:12px;");
        auto *val = new QLabel("--");
        val->setStyleSheet("color:#00d4ff; font-size:18px; font-weight:bold; font-family:Consolas;");
        auto *box = new QVBoxLayout;
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        box->addWidget(cap);
        box->addWidget(val);
        lay->addLayout(box, row, col);
        *out = val;
        Q_UNUSED(parent);
    };

    makeStat(tr("发送帧数"),   &m_lblDmxTxFrames, statBody, st, 0, 0);
    makeStat(tr("接收帧数"),   &m_lblDmxRxFrames, statBody, st, 0, 1);
    makeStat(tr("环回不一致字节"), &m_lblDmxMismatch, statBody, st, 0, 2);
    makeStat(tr("实测帧间隔"), &m_lblDmxInterval, statBody, st, 1, 0);
    makeStat(tr("实测刷新率"), &m_lblDmxRate,     statBody, st, 1, 1);
    makeStat(tr("已发送字节"), &m_lblDmxTxBytes,  statBody, st, 1, 2);

    auto *hexCap = new QLabel(tr("最近接收原始字节 (HEX, 前 64 字节)"));
    hexCap->setStyleSheet("color:#8fb3d9; font-size:12px;");
    m_lblDmxHex = new QLabel("--");
    m_lblDmxHex->setWordWrap(true);
    m_lblDmxHex->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lblDmxHex->setStyleSheet("color:#7df0ff; font-size:12px; font-family:Consolas;");
    st->addWidget(hexCap, 2, 0, 1, 5);
    st->addWidget(m_lblDmxHex, 3, 0, 1, 5);

    root->addWidget(createCard(tr("测试结果 (环回 / 时序)"), statBody), 0);
    root->addStretch();

    // ---- 定时器与信号 ----
    m_dmxTxTimer = new QTimer(this);
    connect(m_dmxTxTimer, &QTimer::timeout, this, [this]{ writeDmxTestFrame(); });

    m_dmxStatTimer = new QTimer(this);
    connect(m_dmxStatTimer, &QTimer::timeout, this, [this]{ updateDmxTestStats(); });
    m_dmxStatTimer->start(300);

    connect(btnRefresh, &QPushButton::clicked, this, [this]{ refreshDmxTestPorts(); });
    connect(m_btnDmxTx, &QPushButton::clicked, this, [this]{
        if (m_dmxTxSerial && m_dmxTxSerial->isOpen()) stopDmxTestTx();
        else startDmxTestTx();
    });
    connect(m_btnDmxRxTest, &QPushButton::clicked, this, [this]{
        if (m_dmxRxTestMode) stopDmxRxTest();
        else startDmxRxTest();
    });

    refreshDmxTestPorts();

    // 自动化验证入口: ARTNET_UI_DMX_AUTOSTART=1 时自动开始发送
    if (qEnvironmentVariableIntValue("ARTNET_UI_DMX_SIM_LOOPBACK") == 1 && m_dmxLoopbackSim)
        m_dmxLoopbackSim->setChecked(true);
    if (qEnvironmentVariableIntValue("ARTNET_UI_DMX_AUTOSTART") == 1)
        QTimer::singleShot(600, this, [this]{ startDmxTestTx(); });
    if (qEnvironmentVariableIntValue("ARTNET_UI_DMX_RXTEST") == 1)
        QTimer::singleShot(700, this, [this]{
            if (m_dmxRxPort->count() > 1) {
                m_dmxRxPort->setCurrentIndex(1);
                startDmxRxTest();
            }
        });

    return page;
}

// ============================================================
// 页面 4: 文件管理 — ArtRec 文件验证 + 预览
// ============================================================
QWidget *ArtNetRecorderMainWindow::buildPageFileMgr() {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setHorizontalSpacing(14);
    root->setVerticalSpacing(14);

    // === Header 信息卡片 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(6);
        form->setLabelAlignment(Qt::AlignRight);

        auto *openRow = new QHBoxLayout();
        auto *btnOpenArtRec = new QPushButton(tr("📂 打开 .artrec 文件"));
        btnOpenArtRec->setProperty("action", "start");
        openRow->addWidget(btnOpenArtRec);
        openRow->addStretch();
        form->addRow(tr("文件:"), [&]{ auto *w=new QWidget(); w->setLayout(openRow); return w; }());
        connect(btnOpenArtRec, &QPushButton::clicked, this, &ArtNetRecorderMainWindow::onBtnOpenFileClicked);

        m_lblFilePath = new QLabel(tr("-- 未打开 --"));
        m_lblFilePath->setStyleSheet("color: #8899aa;");
        m_lblFilePath->setWordWrap(true);
        form->addRow(tr("路径:"), m_lblFilePath);

        m_lblFileHeaderVersion   = createValLabel(tr("--"));
        m_lblFileFps             = createValLabel(tr("--"));
        m_lblFileTotalFrames     = createValLabel(tr("--"));
        m_lblFileDuration        = createValLabel(tr("--"));
        m_lblFileUniverseCount   = createValLabel(tr("--"));

        form->addRow(tr("版本:"),   m_lblFileHeaderVersion);
        form->addRow(tr("帧率:"),   m_lblFileFps);
        form->addRow(tr("总帧:"),   m_lblFileTotalFrames);
        form->addRow(tr("时长:"),   m_lblFileDuration);
        form->addRow(tr("Universe 数:"), m_lblFileUniverseCount);

        root->addWidget(createCard(tr("ArtRec 文件信息"), body), 0, 0);
    }

    // === 帧预览表格 ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(0, 0, 0, 0);

        m_tblFileFrames = new QTableWidget(body);
        m_tblFileFrames->setColumnCount(4);
        m_tblFileFrames->setHorizontalHeaderLabels({
            tr("帧号"), tr("时间戳 (us)"), tr("Frame Index"), tr("Universe 数")
        });
        m_tblFileFrames->horizontalHeader()->setStretchLastSection(true);
        m_tblFileFrames->setEditTriggers(QAbstractItemView::NoEditTriggers);
        v->addWidget(m_tblFileFrames);

        root->addWidget(createCard(tr("帧数据预览 (前 200 帧)"), body), 0, 1);
    }

    root->setColumnStretch(0, 1);
    root->setColumnStretch(1, 2);
    return page;
}

// ============================================================
// 页面 5: 系统状态 — 运行环境 / 版本 / 运行时长
// ============================================================
QWidget *ArtNetRecorderMainWindow::buildPageSystem() {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setHorizontalSpacing(14);
    root->setVerticalSpacing(14);

    // === 运行环境卡片 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(6);
        form->setLabelAlignment(Qt::AlignRight);

        m_lblAppVersion = createValLabel(tr("v1.2.0"));
        m_lblBuildType  = createValLabel(tr("Release"));
        m_lblPlatform   = createValLabel(tr("Windows x86_64"));
        m_lblQtVersion  = createValLabel(QString::fromLatin1(QT_VERSION_STR));
        m_lblRuntime    = createValLabel(tr("00:00:00"));

        form->addRow(tr("应用版本:"),   m_lblAppVersion);
        form->addRow(tr("构建类型:"),   m_lblBuildType);
        form->addRow(tr("平台:"),       m_lblPlatform);
        form->addRow(tr("Qt 版本:"),    m_lblQtVersion);
        form->addRow(tr("本次运行时长:"), m_lblRuntime);

        root->addWidget(createCard(tr("运行环境"), body), 0, 0);
    }

    // === 模块状态卡片 ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(10);

        auto makeLine = [](const QString &name, const QString &badge, const QString &color) {
            auto *row = new QHBoxLayout();
            row->addWidget(new QLabel(name));
            row->addStretch();
            auto *b = new QLabel(badge);
            b->setProperty("role", "badge");
            b->setProperty("badge", color);
            row->addWidget(b);
            auto *w = new QWidget(); w->setLayout(row);
            return w;
        };

        v->addWidget(makeLine(tr("RecorderEngine"),  tr("OK"),       "ok"));
        v->addWidget(makeLine(tr("SyncClockFactory"), tr("OK"),       "ok"));
        v->addWidget(makeLine(tr("ArtNetReceiver"),   tr("未启动"),   "warn"));
        v->addWidget(makeLine(tr("OutputManager"),    tr("OK"),       "ok"));
        v->addWidget(makeLine(tr("ArtRecStorage"),    tr("OK"),       "ok"));
        v->addWidget(makeLine(tr("Logger"),           tr("INFO"),     "info"));
#if defined(ARTNET_WITH_FFMPEG)
        v->addWidget(makeLine(tr("FFmpeg 解码库"),
                              tr("%1 · avcodec %2")
                                  .arg(QString::fromLatin1(av_version_info()))
                                  .arg(LIBAVCODEC_VERSION_MAJOR),
                              "ok"));
#else
        v->addWidget(makeLine(tr("FFmpeg 解码库"), tr("未编译 (NO_FFMPEG)"), "warn"));
#endif
        v->addStretch();

        root->addWidget(createCard(tr("模块状态"), body), 0, 1);
    }

    root->setColumnStretch(0, 1);
    root->setColumnStretch(1, 1);
    return page;
}

// ============================================================
// 槽函数: 时钟 Tick / 引擎状态 / DMX 帧 / UI 定时刷新
// ============================================================
void ArtNetRecorderMainWindow::onClockTick(qint64 frameIdx, qint64 tsUs) {
    m_tickCount++;
    m_lblClockType->setText(syncClockTypeToString(m_clock->clockType()));
    m_lblClockType->setProperty("badge", "ok");
    m_lblClockType->style()->unpolish(m_lblClockType);
    m_lblClockType->style()->polish(m_lblClockType);
    m_lblClockFps->setText(QString::number(m_clock->frameRate()));
    m_lblClockTs->setText(QString::number(tsUs));
    m_lblClockFrameIdx->setText(QString::number(frameIdx));
    m_lblClockCount->setText(QString::number(m_tickCount));

    updateClockDisplay(tsUs, frameIdx, m_clock->frameRate());

    if (m_tickCount % 30 == 0) {
        logInfo(tr("Clock tick #%1  frame=%2  ts=%3 us")
                    .arg(m_tickCount).arg(frameIdx).arg(tsUs));
    }
}

// 右侧"实时时间码显示"大数字 (时钟 tick / 播放器时码共用)
void ArtNetRecorderMainWindow::updateClockDisplay(qint64 tsUs, qint64 frameIdx, int fps) {
    const int rate = qMax(1, fps);
    if (m_lblClockBigTs)    m_lblClockBigTs->setText(QString::number(tsUs));
    if (m_lblClockBigFrame) m_lblClockBigFrame->setText(QString::number(frameIdx));
    if (m_lblClockBigTc) {
        const qint64 totalFrames = qMax<qint64>(0, tsUs * rate / 1000000);
        const int ff = int(totalFrames % rate);
        const qint64 totalSec = totalFrames / rate;
        m_lblClockBigTc->setText(QString("%1:%2:%3:%4")
            .arg(int(totalSec / 3600), 2, 10, QChar('0'))
            .arg(int((totalSec / 60) % 60), 2, 10, QChar('0'))
            .arg(int(totalSec % 60), 2, 10, QChar('0'))
            .arg(ff, 2, 10, QChar('0')));
    }
}

void ArtNetRecorderMainWindow::onEngineStateChanged(RecordState state) {
    QString s; QString badge;
    switch (state) {
        case RecordState::Idle:        s = "IDLE";      badge = "idle";   break;
        case RecordState::WaitingSync: s = "WAIT SYNC"; badge = "warn";   break;
        case RecordState::Recording:   s = "RECORDING"; badge = "ok";    break;
        case RecordState::Paused:      s = "PAUSED";    badge = "warn";   break;
        case RecordState::Stopped:     s = "STOPPED";   badge = "info";   break;
    }
    m_lblEngineState->setText(s);
    m_lblEngineState->setProperty("badge", badge);
    m_lblEngineState->style()->unpolish(m_lblEngineState);
    m_lblEngineState->style()->polish(m_lblEngineState);

    if (state == RecordState::Recording) {
        logSuccess(tr("RecorderEngine 进入 RECORDING"));
        setStatusOk(tr("录制中"));
    } else if (state == RecordState::Stopped) {
        logInfo(tr("RecorderEngine 已停止"));
        setStatusNeutral(tr("已停止"));
    }
}

void ArtNetRecorderMainWindow::onEngineFrameRecorded(qint64 frameIdx, qint64 tsUs) {
    Q_UNUSED(tsUs);
    m_lblRecordFrames->setText(QString::number(frameIdx + 1));
}

void ArtNetRecorderMainWindow::onEngineFinished(qint64 frames, qint64 durationUs) {
    m_lblRecordFrames->setText(QString::number(frames));
    m_lblRecordSecs->setText(QString::number(durationUs / 1000.0, 'f', 2));
    logSuccess(tr("录制完成 — %1 帧, %2 ms").arg(frames).arg(durationUs / 1000));
}

void ArtNetRecorderMainWindow::onEngineError(const QString &err) {
    logError(tr("引擎错误: %1").arg(err));
    setStatusError(err);
}

void ArtNetRecorderMainWindow::onArtNetDmxReceived(const ArtNetDmxFrame &frame) {
    m_dmxPacketCount++;
    m_lblArtNetPkts->setText(QString::number(m_dmxPacketCount));

    // Universe (encoded)
    m_lblLastUniverse->setText(QString("0x%1").arg(quint16(frame.universe), 0, 16));

    // 前 16 字节 HEX 预览 (dmxData 是 quint8[512] 固定数组, frame.length 是实际长度)
    int take = qMin<int>(16, frame.length);
    QString hex;
    for (int i = 0; i < take; ++i)
        hex += QString("%1 ").arg(frame.dmxData[i], 2, 16, QChar('0')).toUpper();
    m_lblLastUniverseData->setText(hex.trimmed());
    m_lblLastUniverseData->setToolTip(
        tr("Universe 0x%1, %2 字节有效").arg(quint16(frame.universe), 0, 16).arg(frame.length));

    if (m_lblArtNetStatus->property("badge").toString() != "ok") {
        m_lblArtNetStatus->setText(tr("收集中..."));
        m_lblArtNetStatus->setProperty("badge", "ok");
        m_lblArtNetStatus->style()->unpolish(m_lblArtNetStatus);
        m_lblArtNetStatus->style()->polish(m_lblArtNetStatus);
    }
}

void ArtNetRecorderMainWindow::onClockLooped(int cnt) {
    logSuccess(tr("时钟循环回零 — 第 %1 次").arg(cnt));
}

// 播放器时码 (FFmpeg 不可用时的退化模式): 用播放位置驱动时间码
void ArtNetRecorderMainWindow::onPlayerClockTick() {
    if (!m_mediaPlayer) return;

    const int fps = m_spinFileFps ? qMax(1, m_spinFileFps->value()) : 30;
    const qint64 posMs    = m_mediaPlayer->positionMs();
    const qint64 tsUs     = posMs * 1000;
    const qint64 frameIdx = posMs * fps / 1000;

    m_tickCount++;
    if (m_lblClockType) {
        const auto type = SyncClockType(m_cmbClockType->currentData().toInt());
        m_lblClockType->setText(syncClockTypeToString(type));
        m_lblClockType->setProperty("badge", "ok");
        m_lblClockType->style()->unpolish(m_lblClockType);
        m_lblClockType->style()->polish(m_lblClockType);
    }
    if (m_lblClockFps)      m_lblClockFps->setText(QString::number(fps));
    if (m_lblClockTs)       m_lblClockTs->setText(QString::number(tsUs));
    if (m_lblClockFrameIdx) m_lblClockFrameIdx->setText(QString::number(frameIdx));
    if (m_lblClockCount)    m_lblClockCount->setText(QString::number(m_tickCount));
    updateClockDisplay(tsUs, frameIdx, fps);

    if (m_tickCount % 30 == 0)
        logInfo(tr("播放器 tick #%1  frame=%2  ts=%3 us")
                    .arg(m_tickCount).arg(frameIdx).arg(tsUs));

    // 媒体播放结束 → 自动停止
    if (!m_mediaPlayer->isPlaying()) {
        const qint64 dur = m_mediaPlayer->durationMs();
        if (dur > 0 && posMs >= dur - 100) {
            logWarn(tr("媒体播放结束"));
            onStopClicked();
        }
    }
}

// ============================================================
// 启动 / 停止 / 重置 (覆盖 TestWindowBase)
// ============================================================
void ArtNetRecorderMainWindow::onStartClicked() {
    // 创建时钟 (按时间码源页配置)
    if (m_clock) {
        logWarn(tr("已有一个时钟在运行, 请先停止"));
        return;
    }

    auto type = SyncClockType(m_cmbClockType->currentData().toInt());

    // 时间码源只能选一种: 启动前校验该源必需的配置
    if (type == SyncClockType::TimecodeFile || type == SyncClockType::VideoFile
        || type == SyncClockType::AudioFile) {
        if (m_editMediaFile->text().trimmed().isEmpty()) {
            logError(tr("请先选择时间码/媒体文件 (%1)")
                         .arg(syncClockTypeToString(type)));
            setStatusError(tr("缺少文件配置"));
            return;
        }
    } else if (type == SyncClockType::ArtNetTimeCode) {
        if (m_editBindAddr->text().trimmed().isEmpty()) {
            logError(tr("请填写 ArtNet 绑定地址"));
            setStatusError(tr("缺少绑定地址"));
            return;
        }
    } else if (type == SyncClockType::Unknown) {
        logError(tr("未选择时间码源"));
        setStatusError(tr("未选择时间码源"));
        return;
    }

    const bool isMediaSrc = (type == SyncClockType::AudioFile
                             || type == SyncClockType::VideoFile);

    SyncClockConfig cfg;
    cfg.clockType    = type;
    cfg.frameRate    = (isMediaSrc && m_spinFileFps) ? m_spinFileFps->value()
                                                     : m_spinFps->value();
    cfg.artNetPort   = quint16(m_spinArtNetPort->value());
    cfg.artNetBindAddress = m_editBindAddr->text();
    cfg.mediaFilePath = m_editMediaFile->text();
    cfg.loopOnEnd    = m_chkLoopOnEnd->isChecked();
    cfg.midiInputDeviceName = m_cmbMidiDevice->currentData().toString().trimmed();
    // "自动选择第一个" → 显式指定 index 0, 触发真实 MIDI 绑定
    if (type == SyncClockType::MidiMTC && cfg.midiInputDeviceName.isEmpty())
        cfg.midiInputDeviceIndex = 0;
    cfg.midiInputChannel    = quint8(m_spinMidiChannel->value());
    cfg.timecodeFileHasRateHeader = m_chkFileRateHeader->isChecked();

    // 媒体源的退化路径: 生产时钟不可用 (未启用 FFmpeg) 时,
    // 用本地播放器的播放位置驱动时间码, 保证 UI 可测
    auto startPlayerClock = [this, &cfg, type, isMediaSrc]() -> bool {
        const QString mediaPath = m_editMediaFile ? m_editMediaFile->text().trimmed() : QString();
        if (!isMediaSrc || !m_mediaPlayer || mediaPath.isEmpty()) return false;

        if (m_mediaPlayer->filePath() != mediaPath)
            m_mediaPlayer->openFile(mediaPath);
        m_mediaPlayer->setTimecodeFrameRate(cfg.frameRate);
        m_mediaPlayer->seekTo(0);
        m_mediaPlayer->play();

        m_usingPlayerClock = true;
        m_tickCount = 0;
        if (!m_playerClockTimer) {
            m_playerClockTimer = new QTimer(this);
            connect(m_playerClockTimer, &QTimer::timeout,
                    this, &ArtNetRecorderMainWindow::onPlayerClockTick);
        }
        m_playerClockTimer->start(qMax(5, 1000 / qMax(1, cfg.frameRate)));
        m_startWallUs = QDateTime::currentMSecsSinceEpoch() * 1000;

        logWarn(tr("生产时钟不可用 (当前构建未启用 FFmpeg), 改用本地播放器驱动时码"));
        logSuccess(tr("播放器时码已启动 — %1 @ %2 fps")
                       .arg(syncClockTypeToString(type)).arg(cfg.frameRate));
        setStatusOk(tr("播放器时码运行中"));
        setStartEnabled(false);
        setStopEnabled(true);
        m_cmbClockType->setEnabled(false);
        return true;
    };

    m_clock = SyncClockFactory::instance().create(type);
    if (!m_clock) {
        if (startPlayerClock()) return;
        logError(tr("SyncClockFactory::create 失败, type=%1").arg(int(type)));
        setStatusError(tr("创建时钟失败"));
        return;
    }

    connect(m_clock, &ISyncClock::frameTick, this, &ArtNetRecorderMainWindow::onClockTick);
    connect(m_clock, &ISyncClock::clockLooped, this, &ArtNetRecorderMainWindow::onClockLooped);
    connect(m_clock, &ISyncClock::clockFinished, this, [this]{
        logWarn(tr("时钟到达末尾 (clockFinished)"));
        onStopClicked();   // 时钟已自行停止, 同步 UI 状态
    });
    connect(m_clock, &ISyncClock::statusChanged, this, [this](const QString &s){ logInfo(s); });
    connect(m_clock, &ISyncClock::errorOccurred, this, [this](const QString &e){ logError(e); });

    m_tickCount = 0;
    if (!m_clock->start(cfg)) {
        delete m_clock; m_clock = nullptr;
        if (startPlayerClock()) return;

        logError(tr("时钟 start() 失败 — 端口/文件检查"));
        setStatusError(tr("时钟启动失败"));
        return;
    }

    m_startWallUs = QDateTime::currentMSecsSinceEpoch() * 1000;

    // 音视频源: 让播放器同步从 0 开始播放, 画面/波形跟随时钟
    if (isMediaSrc && m_mediaPlayer) {
        const QString mediaPath = m_editMediaFile ? m_editMediaFile->text().trimmed() : QString();
        if (!mediaPath.isEmpty()) {
            if (m_mediaPlayer->filePath() != mediaPath)
                m_mediaPlayer->openFile(mediaPath);
            m_mediaPlayer->setTimecodeFrameRate(cfg.frameRate);
            m_mediaPlayer->seekTo(0);
            m_mediaPlayer->play();
        }
    }

    logSuccess(tr("时钟已启动 — %1 @ %2 fps")
                   .arg(syncClockTypeToString(type)).arg(cfg.frameRate));
    setStatusOk(tr("时钟运行中 — %1").arg(syncClockTypeToString(type)));
    setStartEnabled(false);
    setStopEnabled(true);
    m_cmbClockType->setEnabled(false);   // 运行中不允许切换时间码源
}

void ArtNetRecorderMainWindow::onStopClicked() {
    if (m_playerClockTimer) m_playerClockTimer->stop();
    if (m_usingPlayerClock) {
        if (m_mediaPlayer) m_mediaPlayer->pause();
        m_usingPlayerClock = false;
        logInfo(tr("播放器时码已停止 — 共 %1 tick").arg(m_tickCount));
    }
    if (m_clock) {
        m_clock->stop();
        logInfo(tr("时钟已停止 — 共 %1 tick").arg(m_tickCount));
        m_clock->deleteLater();
        m_clock = nullptr;
    }
    if (m_cmbClockType) m_cmbClockType->setEnabled(true);
    setStatusNeutral(tr("已停止"));
    setStartEnabled(true);
    setStopEnabled(false);
}

void ArtNetRecorderMainWindow::onResetClicked() {
    onStopClicked();
    m_tickCount = 0;
    m_dmxPacketCount = 0;
    m_lblClockCount->setText("0");
    m_lblClockTs->setText("--");
    m_lblClockFrameIdx->setText("--");
    m_lblRecordFrames->setText("0");
    m_lblRecordSecs->setText("0.00");
    m_lblArtNetPkts->setText("0");
    m_lblLastUniverse->setText("--");
    m_lblLastUniverseData->setText("--");
    m_lblArtNetStatus->setText(tr("未启动"));
    m_lblArtNetStatus->setProperty("badge", "warn");
    m_lblArtNetStatus->style()->unpolish(m_lblArtNetStatus);
    m_lblArtNetStatus->style()->polish(m_lblArtNetStatus);
    setStatusNeutral(tr("已重置"));
    logInfo(tr("面板已重置"));
}

// ============================================================
// 文件: 打开 + 预览
// ============================================================
void ArtNetRecorderMainWindow::onBtnOpenFileClicked() {
    auto path = QFileDialog::getOpenFileName(this, tr("选择 .artrec 文件"),
                                             QString(),
                                             tr("ArtRec Files (*.artrec);;All (*)"));
    if (path.isEmpty()) return;

    ArtRecReader reader;
    if (!reader.open(path)) {
        logError(tr("ArtRecReader 打开失败: %1").arg(path));
        setStatusError(tr("文件打开失败"));
        return;
    }

    const auto &h = reader.header();
    m_lblFilePath->setText(path);
    m_lblFileHeaderVersion->setText(QString("0x%1").arg(h.version, 0, 16));
    m_lblFileFps->setText(QString::number(h.frameRate));
    m_lblFileTotalFrames->setText(QString::number(h.totalFrames));
    m_lblFileDuration->setText(tr("%1 ms").arg(h.totalDurationUs / 1000));
    m_lblFileUniverseCount->setText(QString::number(h.universeCount));

    // 帧预览
    m_tblFileFrames->setRowCount(0);
    int row = 0;
    qint64 frameIdx, tsUs;
    QMap<int, QByteArray> data;
    while (row < 200 && reader.readNextFrame(frameIdx, tsUs, data)) {
        m_tblFileFrames->insertRow(row);
        m_tblFileFrames->setItem(row, 0, new QTableWidgetItem(QString::number(frameIdx)));
        m_tblFileFrames->setItem(row, 1, new QTableWidgetItem(QString::number(tsUs)));
        m_tblFileFrames->setItem(row, 2, new QTableWidgetItem(QString::number(row)));
        m_tblFileFrames->setItem(row, 3, new QTableWidgetItem(QString::number(data.size())));
        row++;
    }

    logSuccess(tr("已打开 .artrec — %1 帧, %2 宇宙").arg(h.totalFrames).arg(h.universeCount));
    setStatusOk(tr("已加载 %1").arg(QFileInfo(path).fileName()));
}

void ArtNetRecorderMainWindow::onBtnPlayFileClicked() {
    logInfo(tr("模拟回放 — 请先在 '时间码源' 页面选择 VideoFile/TimecodeFile"));
}

// ============================================================
// 页面 3: 输入数据 — 协议 Source Tab
// ============================================================
QWidget *ArtNetRecorderMainWindow::buildPageInputData() {
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    // 说明
    auto *hint = new QLabel(tr("协议输入端测试 — 直接连接 IProtocolSource (ArtNet / NPU / DMX512 / RDM / ArtRecFile) "
                                "实时显示收到的帧、Universe 分布、DMX 通道峰值."));
    hint->setStyleSheet("color: #4b6584; font-size: 11px; padding: 2px 4px;");
    hint->setWordWrap(true);
    root->addWidget(hint);

    m_inputTabs = new QTabWidget(page);
    m_inputTabs->setDocumentMode(true);

    // 6 个 tab
    auto *artnetData = new ProtocolInputTab{ProtocolType::ArtNet};
    auto *npuData    = new ProtocolInputTab{ProtocolType::Npu};
    auto *dmxData    = new ProtocolInputTab{ProtocolType::Dmx512};
    auto *sacnData   = new ProtocolInputTab{ProtocolType::Sacn};
    auto *rdmData    = new ProtocolInputTab{ProtocolType::ArtNet};   // RDM 走 Art-Net 通道
    auto *fileData   = new ProtocolInputTab{ProtocolType::ArtRecFile};

    artnetData->key = "artnet";
    npuData->key    = "npu";
    dmxData->key    = "dmx512";
    sacnData->key   = "sacn";
    rdmData->key    = "rdm";
    fileData->key   = "artrecfile";

    m_inputTabsData[ProtocolType::ArtNet]     = artnetData;
    m_inputTabsData[ProtocolType::Npu]        = npuData;
    m_inputTabsData[ProtocolType::Dmx512]     = dmxData;
    m_inputTabsData[ProtocolType::Sacn]       = sacnData;
    m_inputTabsData[ProtocolType::ArtRecFile] = fileData;

    m_inputTabs->addTab(buildInputTabArtNet(artnetData),     tr("🎨  ArtNet (UDP 6454)"));
    m_inputTabs->addTab(buildInputTabNpu(npuData),           tr("🟠  MA NPU (Ma-Net2 UDP 9999)"));
    m_inputTabs->addTab(buildInputTabDmx512(dmxData),        tr("🔌  DMX512 (串口)"));
    m_inputTabs->addTab(buildInputTabSacn(sacnData),         tr("🟣  sACN / E1.31 (UDP 5568)"));
    m_inputTabs->addTab(buildInputTabRdm(rdmData),           tr("🔵  RDM (ArtRDM 0x6000)"));
    m_inputTabs->addTab(buildInputTabArtRecFile(fileData),   tr("📂  ArtRec File (离线重演)"));

    root->addWidget(m_inputTabs, 1);
    return page;
}

// --- Tab 工厂: 参数配置 + 实时面板 + Universe 峰值表 ---
QWidget *ArtNetRecorderMainWindow::buildInputTabArtNet(ProtocolInputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14, 14, 14, 14);
    root->setHorizontalSpacing(12);
    root->setVerticalSpacing(12);

    // === 配置 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(6);
        form->setLabelAlignment(Qt::AlignRight);

        data->editBindAddr = new QLineEdit("0.0.0.0", body);
        data->spinPort     = new QSpinBox(body); data->spinPort->setRange(1024, 65535); data->spinPort->setValue(6454);

        form->addRow(tr("绑定地址:"), data->editBindAddr);
        form->addRow(tr("监听端口:"), data->spinPort);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 开始接收")); data->btnStart->setProperty("action", "start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action", "stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startInputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopInputTab(data); });

        root->addWidget(createCard(tr("ArtNet 参数"), body), 0, 0);
    }

    // === 实时面板 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(6);
        form->setLabelAlignment(Qt::AlignRight);

        data->lblStatus = new QLabel(tr("未启动"));
        data->lblStatus->setProperty("role", "badge");
        data->lblStatus->setProperty("badge", "warn");
        form->addRow(createValLabel(tr("状态:")), data->lblStatus);

        auto *c1 = new QHBoxLayout();
        data->lblFrameCount = createValNumber(tr("0"), 18);
        c1->addWidget(data->lblFrameCount); c1->addStretch();
        form->addRow(createValLabel(tr("总帧:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());

        auto *c2 = new QHBoxLayout();
        data->lblUniverse = createValNumber(tr("--"), 16);
        c2->addWidget(data->lblUniverse); c2->addStretch();
        form->addRow(createValLabel(tr("最近 Universe:")), [&]{ auto *w=new QWidget(); w->setLayout(c2); return w; }());

        auto *c3 = new QHBoxLayout();
        data->lblTs = createValNumber(tr("--"), 14);
        c3->addWidget(data->lblTs); c3->addStretch();
        form->addRow(createValLabel(tr("时间戳 (us):")), [&]{ auto *w=new QWidget(); w->setLayout(c3); return w; }());

        data->lblDataHex = new QLabel(tr("--"));
        data->lblDataHex->setStyleSheet("font-family: Consolas, monospace; color: #00d4ff; font-size: 11px;");
        data->lblDataHex->setWordWrap(true);
        form->addRow(createValLabel(tr("DMX 前 16B:")), data->lblDataHex);

        root->addWidget(createCard(tr("实时数据"), body), 0, 1);

        // === 数据监视卡片 (512 通道, 跨整行) ===
        auto *dataCard = createCard(tr("输入数据监视 (512 CH)"),
                                    createChannelPanel(&data->lblChanInfo, &data->bars, page));
        dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        root->addWidget(dataCard, 1, 0, 1, 2);

    }

    // === Universe 通道峰值表 ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(0, 0, 0, 0);

        data->tblUniverses = new QTableWidget(body);
        data->tblUniverses->setColumnCount(4);
        data->tblUniverses->setHorizontalHeaderLabels({
            tr("Universe"), tr("总帧"), tr("通道峰值 CH0-7"), tr("通道峰值 CH8-15")
        });
        data->tblUniverses->horizontalHeader()->setStretchLastSection(true);
        data->tblUniverses->setEditTriggers(QAbstractItemView::NoEditTriggers);
        data->tblUniverses->setSelectionBehavior(QAbstractItemView::SelectRows);
        v->addWidget(data->tblUniverses);

        auto *peakCard = createCard(tr("Universe 通道峰值分布"), body);
        peakCard->setMinimumHeight(90);
        peakCard->setMaximumHeight(90);
        root->addWidget(peakCard, 2, 0, 1, 2);
    }

    root->setColumnStretch(0, 1);
    root->setColumnStretch(1, 1);
    root->setRowStretch(0, 0);
    root->setRowStretch(1, 1);
    return page;
}

// 其他 4 个输入 tab 结构相同, 只有默认参数和协议名不同 — 简化实现 (占位 + 共享实时面板)
QWidget *ArtNetRecorderMainWindow::buildInputTabDmx512(ProtocolInputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14, 14, 14, 14);
    root->setHorizontalSpacing(12);
    root->setVerticalSpacing(12);

    // 配置
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(6);
        form->setLabelAlignment(Qt::AlignRight);

        data->editSerialPort = new QLineEdit("COM3", body);

        auto *baud = new QComboBox(body);
        baud->addItem("250000 (DMX512 默认)", 250000);
        baud->addItem("115200", 115200);

        form->addRow(tr("串口:"), data->editSerialPort);
        form->addRow(tr("波特率:"), baud);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 开始接收")); data->btnStart->setProperty("action", "start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action", "stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startInputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopInputTab(data); });

        root->addWidget(createCard(tr("DMX512 参数"), body), 0, 0);
    }

    // 实时面板 (复用 ArtNet 模板)
    auto *rt = new QWidget();
    auto *form = new QFormLayout(rt);
    form->setContentsMargins(0, 0, 0, 0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
    data->lblStatus=new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
    data->lblFrameCount=createValNumber(tr("0"),18); data->lblUniverse=createValNumber(tr("--"),16);
    data->lblTs=createValNumber(tr("--"),14);
    data->lblDataHex=new QLabel(tr("--")); data->lblDataHex->setStyleSheet("font-family: Consolas, monospace; color: #ffa726; font-size: 11px;");
    data->lblDataHex->setWordWrap(true);
    form->addRow(createValLabel(tr("状态:")), data->lblStatus);
    auto *c1=new QHBoxLayout(); c1->addWidget(data->lblFrameCount); c1->addStretch();
    form->addRow(createValLabel(tr("总帧:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());
    auto *c2=new QHBoxLayout(); c2->addWidget(data->lblUniverse); c2->addStretch();
    form->addRow(createValLabel(tr("线路号:")), [&]{ auto *w=new QWidget(); w->setLayout(c2); return w; }());
    auto *c3=new QHBoxLayout(); c3->addWidget(data->lblTs); c3->addStretch();
    form->addRow(createValLabel(tr("时间戳 (us):")), [&]{ auto *w=new QWidget(); w->setLayout(c3); return w; }());
    form->addRow(createValLabel(tr("数据预览:")), data->lblDataHex);
    root->addWidget(createCard(tr("实时数据"), rt), 0, 1);

    // === 数据监视卡片 (512 通道, 跨整行) ===
    auto *dataCard = createCard(tr("输入数据监视 (512 CH)"),
                                createChannelPanel(&data->lblChanInfo, &data->bars, page));
    dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    root->addWidget(dataCard, 1, 0, 1, 2);


    // Universe 表 (DMX512 没有 Universe 概念, 这里表示"线路号")
    auto *tblBody = new QWidget();
    auto *tv = new QVBoxLayout(tblBody); tv->setContentsMargins(0,0,0,0);
    data->tblUniverses = new QTableWidget(tblBody);
    data->tblUniverses->setColumnCount(3);
    data->tblUniverses->setHorizontalHeaderLabels({tr("线路/线路号"), tr("总帧"), tr("通道峰值 CH0-15")});
    data->tblUniverses->horizontalHeader()->setStretchLastSection(true);
    data->tblUniverses->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tv->addWidget(data->tblUniverses);
    auto *peakCard = createCard(tr("线路通道峰值分布"), tblBody);
    peakCard->setMinimumHeight(90);
        peakCard->setMaximumHeight(90);
    root->addWidget(peakCard, 2, 0, 1, 2);

    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    root->setRowStretch(0,0); root->setRowStretch(1,1);
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildInputTabNpu(ProtocolInputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14);
    root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    // 配置
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
        data->editBindAddr = new QLineEdit("0.0.0.0", body);
        data->spinPort = new QSpinBox(body); data->spinPort->setRange(1024,65535); data->spinPort->setValue(9999);
        form->addRow(tr("绑定地址:"), data->editBindAddr);
        form->addRow(tr("监听端口:"), data->spinPort);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 开始接收")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startInputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopInputTab(data); });

        root->addWidget(createCard(tr("MA NPU 参数"), body), 0, 0);
    }

    // 实时面板
    auto *rt = new QWidget();
    auto *form = new QFormLayout(rt);
    form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
    data->lblStatus=new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
    data->lblFrameCount=createValNumber(tr("0"),18); data->lblUniverse=createValNumber(tr("--"),16);
    data->lblTs=createValNumber(tr("--"),14);
    data->lblDataHex=new QLabel(tr("--")); data->lblDataHex->setStyleSheet("font-family: Consolas, monospace; color: #ba68c8; font-size: 11px;");
    data->lblDataHex->setWordWrap(true);
    form->addRow(createValLabel(tr("状态:")), data->lblStatus);
    auto *c1=new QHBoxLayout(); c1->addWidget(data->lblFrameCount); c1->addStretch();
    form->addRow(createValLabel(tr("DMX 包总数:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());
    auto *c2=new QHBoxLayout(); c2->addWidget(data->lblUniverse); c2->addStretch();
    form->addRow(createValLabel(tr("最近 Universe:")), [&]{ auto *w=new QWidget(); w->setLayout(c2); return w; }());
    auto *c3=new QHBoxLayout(); c3->addWidget(data->lblTs); c3->addStretch();
    form->addRow(createValLabel(tr("时间戳 (us):")), [&]{ auto *w=new QWidget(); w->setLayout(c3); return w; }());
    form->addRow(createValLabel(tr("DMX 前 16B:")), data->lblDataHex);
    root->addWidget(createCard(tr("实时数据"), rt), 0, 1);

    // === 数据监视卡片 (512 通道, 跨整行) ===
    auto *dataCard = createCard(tr("输入数据监视 (512 CH)"),
                                createChannelPanel(&data->lblChanInfo, &data->bars, page));
    dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    root->addWidget(dataCard, 1, 0, 1, 2);


    // Universe 表
    auto *tblBody = new QWidget();
    auto *tv = new QVBoxLayout(tblBody); tv->setContentsMargins(0,0,0,0);
    data->tblUniverses = new QTableWidget(tblBody);
    data->tblUniverses->setColumnCount(4);
    data->tblUniverses->setHorizontalHeaderLabels({tr("Universe (0-255)"), tr("DMX 包数"), tr("其他 OpCode"), tr("通道峰值 CH0-15")});
    data->tblUniverses->horizontalHeader()->setStretchLastSection(true);
    data->tblUniverses->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tv->addWidget(data->tblUniverses);
    auto *peakCard = createCard(tr("Universe 分布 (NPU)"), tblBody);
    peakCard->setMinimumHeight(90);
        peakCard->setMaximumHeight(90);
    root->addWidget(peakCard, 2, 0, 1, 2);

    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    root->setRowStretch(0,0); root->setRowStretch(1,1);
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildInputTabSacn(ProtocolInputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14);
    root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    // === 配置 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->editBindAddr = new QLineEdit("0.0.0.0", body);
        data->spinPort = new QSpinBox(body); data->spinPort->setRange(1024,65535); data->spinPort->setValue(5568);
        data->spinUniverse = new QSpinBox(body); data->spinUniverse->setRange(1, 63999); data->spinUniverse->setValue(1);
        data->spinPriority = new QSpinBox(body); data->spinPriority->setRange(0, 200); data->spinPriority->setValue(0);
        data->spinPriority->setSpecialValueText(tr("全部"));

        form->addRow(tr("绑定地址:"), data->editBindAddr);
        form->addRow(tr("UDP 端口:"), data->spinPort);
        form->addRow(tr("Universe:"), data->spinUniverse);
        form->addRow(tr("优先级过滤:"), data->spinPriority);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 开始接收")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startInputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopInputTab(data); });

        root->addWidget(createCard(tr("sACN (E1.31) 参数"), body), 0, 0);
    }

    // === 实时面板 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->lblStatus = new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
        form->addRow(createValLabel(tr("状态:")), data->lblStatus);

        auto *c1 = new QHBoxLayout(); data->lblFrameCount = createValNumber(tr("0"),18);
        c1->addWidget(data->lblFrameCount); c1->addStretch();
        form->addRow(createValLabel(tr("sACN 包数:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());

        auto *c2 = new QHBoxLayout(); data->lblUniverse = createValNumber(tr("--"),16);
        c2->addWidget(data->lblUniverse); c2->addStretch();
        form->addRow(createValLabel(tr("Universe:")), [&]{ auto *w=new QWidget(); w->setLayout(c2); return w; }());

        auto *c3 = new QHBoxLayout(); data->lblTs = createValNumber(tr("--"),14);
        c3->addWidget(data->lblTs); c3->addStretch();
        form->addRow(createValLabel(tr("序列号:")), [&]{ auto *w=new QWidget(); w->setLayout(c3); return w; }());

        data->lblExtra = new QLabel(tr("--"));
        data->lblExtra->setStyleSheet("font-family: Consolas, monospace; color: #ba68c8; font-size: 11px;");
        form->addRow(createValLabel(tr("来源 CID:")), data->lblExtra);

        data->lblDataHex = new QLabel(tr("--"));
        data->lblDataHex->setStyleSheet("font-family: Consolas, monospace; color: #00d4ff; font-size: 11px;");
        data->lblDataHex->setWordWrap(true);
        form->addRow(createValLabel(tr("DMX 前 16B:")), data->lblDataHex);

        root->addWidget(createCard(tr("实时数据"), body), 0, 1);

        // === 数据监视卡片 (512 通道, 跨整行) ===
        auto *dataCard = createCard(tr("输入数据监视 (512 CH)"),
                                    createChannelPanel(&data->lblChanInfo, &data->bars, page));
        dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        root->addWidget(dataCard, 1, 0, 1, 2);

    }

    // === Universe 统计表 ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body); v->setContentsMargins(0,0,0,0);
        data->tblUniverses = new QTableWidget(body);
        data->tblUniverses->setColumnCount(4);
        data->tblUniverses->setHorizontalHeaderLabels({tr("Universe"), tr("包数"), tr("优先级"), tr("来源 CID (前 8B)")});
        data->tblUniverses->horizontalHeader()->setStretchLastSection(true);
        data->tblUniverses->setEditTriggers(QAbstractItemView::NoEditTriggers);
        data->tblUniverses->setSelectionBehavior(QAbstractItemView::SelectRows);
        v->addWidget(data->tblUniverses);
        auto *peakCard = createCard(tr("sACN Universe 分布"), body);
        peakCard->setMinimumHeight(90);
        peakCard->setMaximumHeight(90);
        root->addWidget(peakCard, 2, 0, 1, 2);
    }

    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    root->setRowStretch(0,0); root->setRowStretch(1,1);
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildInputTabRdm(ProtocolInputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14);
    root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    // === 配置 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->editBindAddr = new QLineEdit("0.0.0.0", body);
        data->spinPort = new QSpinBox(body); data->spinPort->setRange(1024,65535); data->spinPort->setValue(6454);
        form->addRow(tr("绑定地址:"), data->editBindAddr);
        form->addRow(tr("Art-Net 端口:"), data->spinPort);

        auto *note = new QLabel(tr("RDM 复用 Art-Net 通道: 监听 ArtRDM (0x6000) / ArtRDMSub (0x6100) 包"));
        note->setStyleSheet("color: #4b6584; font-size: 11px;"); note->setWordWrap(true);
        form->addRow(tr("说明:"), note);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 开始接收")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startInputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopInputTab(data); });

        root->addWidget(createCard(tr("RDM 参数"), body), 0, 0);
    }

    // === 实时面板 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->lblStatus = new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
        form->addRow(createValLabel(tr("状态:")), data->lblStatus);

        auto *c1 = new QHBoxLayout(); data->lblFrameCount = createValNumber(tr("0"),18);
        c1->addWidget(data->lblFrameCount); c1->addStretch();
        form->addRow(createValLabel(tr("RDM 包数:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());

        data->lblExtra = createValNumber(tr("0"), 16);
        form->addRow(createValLabel(tr("同时 DMX 包:")), data->lblExtra);

        auto *c2 = new QHBoxLayout(); data->lblUniverse = createValNumber(tr("--"),16);
        c2->addWidget(data->lblUniverse); c2->addStretch();
        form->addRow(createValLabel(tr("最近 OpCode:")), [&]{ auto *w=new QWidget(); w->setLayout(c2); return w; }());

        auto *c3 = new QHBoxLayout(); data->lblTs = createValNumber(tr("--"),14);
        c3->addWidget(data->lblTs); c3->addStretch();
        form->addRow(createValLabel(tr("最近包大小:")), [&]{ auto *w=new QWidget(); w->setLayout(c3); return w; }());

        data->lblDataHex = new QLabel(tr("--"));
        data->lblDataHex->setStyleSheet("font-family: Consolas, monospace; color: #4fc3f7; font-size: 11px;");
        data->lblDataHex->setWordWrap(true);
        form->addRow(createValLabel(tr("RDM 数据 HEX:")), data->lblDataHex);

        root->addWidget(createCard(tr("实时数据"), body), 0, 1);

        // === 数据监视卡片 (512 通道, 跨整行) ===
        auto *dataCard = createCard(tr("输入数据监视 (512 CH)"),
                                    createChannelPanel(&data->lblChanInfo, &data->bars, page));
        dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        root->addWidget(dataCard, 1, 0, 1, 2);

    }

    // === OpCode 统计表 ===
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body); v->setContentsMargins(0,0,0,0);
        data->tblUniverses = new QTableWidget(body);
        data->tblUniverses->setColumnCount(3);
        data->tblUniverses->setHorizontalHeaderLabels({tr("OpCode"), tr("说明"), tr("包数")});
        data->tblUniverses->horizontalHeader()->setStretchLastSection(true);
        data->tblUniverses->setEditTriggers(QAbstractItemView::NoEditTriggers);
        data->tblUniverses->setSelectionBehavior(QAbstractItemView::SelectRows);
        v->addWidget(data->tblUniverses);
        auto *peakCard = createCard(tr("Art-Net OpCode 分布"), body);
        peakCard->setMinimumHeight(90);
        peakCard->setMaximumHeight(90);
        root->addWidget(peakCard, 2, 0, 1, 2);
    }

    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    root->setRowStretch(0,0); root->setRowStretch(1,1);
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildInputTabArtRecFile(ProtocolInputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14);
    root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    // 配置
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->editFilePath = new QLineEdit(body);
        data->editFilePath->setPlaceholderText(tr("选择 .artrec 录制文件"));
        auto *browseRow = new QHBoxLayout();
        data->btnBrowseFile = new QPushButton(tr("浏览...")); data->btnBrowseFile->setProperty("action", "icon");
        browseRow->addWidget(data->editFilePath, 1); browseRow->addWidget(data->btnBrowseFile);
        form->addRow(tr("文件:"), [&]{ auto *w=new QWidget(); w->setLayout(browseRow); return w; }());

        data->spinSpeed = new QDoubleSpinBox(body); data->spinSpeed->setRange(0.1, 10.0); data->spinSpeed->setValue(1.0); data->spinSpeed->setSingleStep(0.1);
        data->chkLoop = new QCheckBox(tr("到末尾循环重演 (loopPlayback)"), body);

        form->addRow(tr("重演速度:"), data->spinSpeed);
        form->addRow(tr("循环:"), data->chkLoop);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 开始重演")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnBrowseFile, &QPushButton::clicked, body, [this,data]{
            auto p = QFileDialog::getOpenFileName(this, tr("选择 .artrec 文件"), QString(), tr("ArtRec (*.artrec);;All (*)"));
            if (!p.isEmpty()) data->editFilePath->setText(p);
        });
        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startInputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopInputTab(data); });

        root->addWidget(createCard(tr("ArtRec File 重演参数"), body), 0, 0);
    }

    // 实时面板
    auto *rt = new QWidget();
    auto *form = new QFormLayout(rt);
    form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
    data->lblStatus=new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
    data->lblFrameCount=createValNumber(tr("0"),18); data->lblUniverse=createValNumber(tr("--"),16);
    data->lblTs=createValNumber(tr("--"),14);
    data->lblDataHex=new QLabel(tr("--")); data->lblDataHex->setStyleSheet("font-family: Consolas, monospace; color: #66bb6a; font-size: 11px;");
    data->lblDataHex->setWordWrap(true);
    form->addRow(createValLabel(tr("状态:")), data->lblStatus);
    auto *c1=new QHBoxLayout(); c1->addWidget(data->lblFrameCount); c1->addStretch();
    form->addRow(createValLabel(tr("已 emit LightFrame:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());
    auto *c2=new QHBoxLayout(); c2->addWidget(data->lblUniverse); c2->addStretch();
    form->addRow(createValLabel(tr("当前 Universe:")), [&]{ auto *w=new QWidget(); w->setLayout(c2); return w; }());
    auto *c3=new QHBoxLayout(); c3->addWidget(data->lblTs); c3->addStretch();
    form->addRow(createValLabel(tr("帧时间戳 (us):")), [&]{ auto *w=new QWidget(); w->setLayout(c3); return w; }());
    form->addRow(createValLabel(tr("DMX 前 16B:")), data->lblDataHex);
    root->addWidget(createCard(tr("重演实时数据"), rt), 0, 1);

    // === 数据监视卡片 (512 通道, 跨整行) ===
    auto *dataCard = createCard(tr("输入数据监视 (512 CH)"),
                                createChannelPanel(&data->lblChanInfo, &data->bars, page));
    dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    root->addWidget(dataCard, 1, 0, 1, 2);


    // Universe 表
    auto *tblBody = new QWidget();
    auto *tv = new QVBoxLayout(tblBody); tv->setContentsMargins(0,0,0,0);
    data->tblUniverses = new QTableWidget(tblBody);
    data->tblUniverses->setColumnCount(3);
    data->tblUniverses->setHorizontalHeaderLabels({tr("Universe"), tr("文件内帧数"), tr("通道峰值 CH0-15")});
    data->tblUniverses->horizontalHeader()->setStretchLastSection(true);
    data->tblUniverses->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tv->addWidget(data->tblUniverses);
    auto *peakCard = createCard(tr("文件内 Universe 分布"), tblBody);
    peakCard->setMinimumHeight(90);
        peakCard->setMaximumHeight(90);
    root->addWidget(peakCard, 2, 0, 1, 2);

    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    root->setRowStretch(0,0); root->setRowStretch(1,1);
    return page;
}

// --- 输入 tab 启动/停止 ---
void ArtNetRecorderMainWindow::startInputTab(ProtocolInputTab *tab) {
    if (tab->source) { stopInputTab(tab); }
    if (m_rdmReceiver) { m_rdmReceiver->stop(); m_rdmReceiver->deleteLater(); m_rdmReceiver = nullptr; }

    // ===== RDM 输入: 生产 ArtNetReceiver (ArtRDM 0x6000) =====
    if (tab->key == "rdm") {
        m_rdmReceiver = new ArtNetReceiver(this);
        connect(m_rdmReceiver, &ArtNetReceiver::rdmFrameReceived, this, [this,tab](const QByteArray &rdm){
            tab->frameCount++;
            tab->lblFrameCount->setText(QString::number(tab->frameCount));
            int take = qMin<int>(32, rdm.size());
            QString hex;
            const auto *d = reinterpret_cast<const uchar*>(rdm.constData());
            for (int i = 0; i < take; ++i) hex += QString("%1 ").arg(d[i], 2, 16, QChar('0')).toUpper();
            tab->lblDataHex->setText(hex.trimmed());
        });
        connect(m_rdmReceiver, &ArtNetReceiver::dmxFrameReceived, this, [this,tab](const ArtNetDmxFrame &){
            tab->dmxCount++;
            tab->lblExtra->setText(QString::number(tab->dmxCount));
        });
        connect(m_rdmReceiver, &ArtNetReceiver::rawPacketReceived, this, [this,tab](quint16 op, const QByteArray &pkt){
            tab->otherCount++;
            tab->lblUniverse->setText(QString("0x%1").arg(op, 4, 16, QChar('0')).toUpper());
            tab->lblTs->setText(QString("%1 B").arg(pkt.size()));

            if ((tab->otherCount % 20) == 0 && tab->tblUniverses) {
                auto *tbl = tab->tblUniverses;
                QString key = QString("0x%1").arg(op, 4, 16, QChar('0')).toUpper();
                QString desc;
                switch (op) {
                    case 0x5000: desc = "ArtDMX"; break;
                    case 0x6000: desc = "ArtRDM"; break;
                    case 0x6100: desc = "ArtRDMSub"; break;
                    case 0x2000: desc = "ArtPoll"; break;
                    case 0x2100: desc = "ArtPollReply"; break;
                    case 0x8000: desc = "ArtSync"; break;
                    case 0x9700: desc = "ArtTodRequest"; break;
                    default: desc = "其他"; break;
                }
                int row = -1;
                for (int r = 0; r < tbl->rowCount(); ++r) {
                    auto *it = tbl->item(r, 0);
                    if (it && it->text() == key) { row = r; break; }
                }
                if (row < 0) {
                    row = tbl->rowCount();
                    tbl->insertRow(row);
                    tbl->setItem(row, 0, new QTableWidgetItem(key));
                    tbl->setItem(row, 1, new QTableWidgetItem(desc));
                    tbl->setItem(row, 2, new QTableWidgetItem("1"));
                } else {
                    int cur = tbl->item(row, 2)->text().toInt();
                    tbl->item(row, 2)->setText(QString::number(cur + 20));
                }
            }
        });
        connect(m_rdmReceiver, &ArtNetReceiver::statusChanged, this, [this,tab](const QString &s){
            onInputTabStatusChanged(tab, s);
        });

        quint16 port = tab->spinPort ? quint16(tab->spinPort->value()) : ArtNet::DEFAULT_PORT;
        QString bindAddr = tab->editBindAddr ? tab->editBindAddr->text() : "0.0.0.0";
        if (!m_rdmReceiver->start(port, bindAddr)) {
            tab->lblStatus->setText(tr("启动失败"));
            tab->lblStatus->setProperty("badge","error"); tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
            logError(tr("RDM 接收启动失败: %1").arg(m_rdmReceiver->lastError()));
            m_rdmReceiver->deleteLater(); m_rdmReceiver = nullptr;
            return;
        }
        tab->frameCount = tab->dmxCount = tab->otherCount = 0;
        tab->lblFrameCount->setText("0"); tab->lblExtra->setText("0");
        tab->btnStart->setEnabled(false); tab->btnStop->setEnabled(true);
        tab->lblStatus->setText(tr("运行中"));
        tab->lblStatus->setProperty("badge","ok"); tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
        logSuccess(tr("RDM 接收已启动 — Art-Net :%1").arg(port));
        return;
    }

    // ===== 其他协议: ProtocolFactory / 内嵌 sACN =====
    if (tab->key == "sacn")
        tab->source = new SacnSourceImpl();
    else
        tab->source = ProtocolFactory::createSource(tab->type, this);

    if (!tab->source) {
        tab->lblStatus->setText(tr("创建失败"));
        tab->lblStatus->setProperty("badge","error"); tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
        logError(tr("创建 Source 失败, key=%1").arg(tab->key));
        return;
    }

    ConnectionParams params;
    params.protocol = tab->type;
    if (tab->editBindAddr)       params.localBindAddress = tab->editBindAddr->text();
    if (tab->spinPort)           params.localPort = quint16(tab->spinPort->value());
    if (tab->editSerialPort)     params.serialPortName = tab->editSerialPort->text();
    if (tab->editFilePath)       params.filePath = tab->editFilePath->text();
    if (tab->spinSpeed)          params.playbackSpeed = tab->spinSpeed->value();
    if (tab->chkLoop)            params.loopPlayback = tab->chkLoop->isChecked();
    if (tab->spinUniverse)       params.filterUniverses = { quint16(tab->spinUniverse->value()) };

    connect(tab->source, &IProtocolSource::frameReceived, this, [this,tab](const LightFrame &f){
        onInputTabFrameReceived(tab, f);
    });
    connect(tab->source, &IProtocolSource::statusChanged, this, [this,tab](const QString &s){
        onInputTabStatusChanged(tab, s);
    });
    connect(tab->source, &IProtocolSource::errorOccurred, this, [this,tab](const QString &e){
        onInputTabError(tab, e);
    });

    if (!tab->source->start(params)) {
        tab->lblStatus->setText(tr("启动失败"));
        tab->lblStatus->setProperty("badge","error"); tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
        logError(tr("%1 Source start() 失败").arg(protocolTypeToString(tab->type)));
        delete tab->source; tab->source = nullptr;
        return;
    }

    tab->frameCount = 0;
    tab->btnStart->setEnabled(false); tab->btnStop->setEnabled(true);
    tab->lblStatus->setText(tr("运行中"));
    tab->lblStatus->setProperty("badge","ok"); tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
    logSuccess(tr("%1 Source 已启动").arg(protocolTypeToString(tab->type)));
}

void ArtNetRecorderMainWindow::stopInputTab(ProtocolInputTab *tab) {
    if (tab->key == "rdm") {
        if (m_rdmReceiver) {
            m_rdmReceiver->stop();
            m_rdmReceiver->deleteLater();
            m_rdmReceiver = nullptr;
        }
        tab->btnStart->setEnabled(true); tab->btnStop->setEnabled(false);
        tab->lblStatus->setText(tr("已停止"));
        tab->lblStatus->setProperty("badge","idle"); tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
        logInfo(tr("RDM 接收已停止 — RDM %1 包, DMX %2 包").arg(tab->frameCount).arg(tab->dmxCount));
        return;
    }
    if (!tab->source) return;
    tab->source->stop();
    tab->source->deleteLater();
    tab->source = nullptr;
    tab->btnStart->setEnabled(true); tab->btnStop->setEnabled(false);
    tab->lblStatus->setText(tr("已停止"));
    tab->lblStatus->setProperty("badge","idle"); tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
    logInfo(tr("%1 Source 已停止 — 共接收 %2 帧").arg(protocolTypeToString(tab->type)).arg(tab->frameCount));
}

// --- 输入 tab 槽函数 (实时数据驱动) ---
void ArtNetRecorderMainWindow::onInputTabFrameReceived(ProtocolInputTab *tab, const LightFrame &frame) {
    // sACN 专属: 优先级过滤 + 来源 CID 显示
    bool isSacn = (tab->key == "sacn");
    SacnSourceImpl *sacnImpl = isSacn ? static_cast<SacnSourceImpl*>(tab->source) : nullptr;
    if (sacnImpl) {
        if (tab->spinPriority && tab->spinPriority->value() > 0
            && sacnImpl->lastPriority < quint8(tab->spinPriority->value()))
            return;   // 低于过滤阈值, 丢弃

        QString cidHex;
        for (int i = 0; i < qMin<int>(8, sacnImpl->lastCid.size()); ++i)
            cidHex += QString("%1").arg(uchar(sacnImpl->lastCid[i]), 2, 16, QChar('0')).toUpper();
        tab->lblExtra->setText(cidHex + "…");
        tab->lblTs->setText(QString::number(frame.sequence));
    }

    tab->frameCount++;
    tab->lblFrameCount->setText(QString::number(tab->frameCount));
    if (isSacn)
        tab->lblUniverse->setText(QString::number(frame.universe));
    else
        tab->lblUniverse->setText(QString("0x%1").arg(quint16(frame.universe), 0, 16));
    if (!isSacn)
        tab->lblTs->setText(QString::number(frame.timestampUs));

    // HEX 预览
    int take = qMin<int>(16, frame.dmxData.size());
    QString hex;
    const auto *d = reinterpret_cast<const uchar*>(frame.dmxData.constData());
    for (int i = 0; i < take; ++i) hex += QString("%1 ").arg(d[i], 2, 16, QChar('0')).toUpper();
    tab->lblDataHex->setText(hex.trimmed());

    // 通道电平面板 (柱状图 + 域/子网/Universe/帧率/总帧/时间码)
    if (tab->bars) tab->bars->setData(frame.dmxData);
    if (tab->firstTsUs == 0) tab->firstTsUs = frame.timestampUs;
    if (tab->lastTsUs > 0) {
        const double dt = (frame.timestampUs - tab->lastTsUs) / 1e6;
        if (dt > 1e-6) {
            const double inst = 1.0 / dt;
            tab->fps = (tab->fps <= 0.001) ? inst : tab->fps * 0.9 + inst * 0.1;
        }
    }
    tab->lastTsUs = frame.timestampUs;
    updateChannelPanel(tab->lblChanInfo, frame.universe, tab->fps, tab->frameCount,
                       frame.timestampUs - tab->firstTsUs);

    // 同步到"实时输入"独立页 (按域缓存, 可在页面上切换域)
    feedLiveUniverse(true, frame.universe, frame.dmxData, frame.timestampUs);

    // Universe 峰值表更新 (每秒最多更新一次表格, 降低刷新开销)
    auto &peak = tab->universePeaks[frame.universe];
    if (peak.size() < 512) peak.resize(512);
    for (int i = 0; i < frame.dmxData.size() && i < 512; ++i) {
        if (uchar(frame.dmxData[i]) > peak[i]) peak[i] = uchar(frame.dmxData[i]);
    }

    // 每 30 帧刷一次表
    if (tab->frameCount % 30 == 0) {
        auto *tbl = tab->tblUniverses;
        if (tbl) {
            // 先查是否已有此行
            int row = -1;
            for (int r = 0; r < tbl->rowCount(); ++r) {
                auto *it = tbl->item(r, 0);
                if (it && it->text() == QString("0x%1").arg(quint16(frame.universe), 0, 16)) { row = r; break; }
            }
            if (row < 0) {
                row = tbl->rowCount();
                tbl->insertRow(row);
                tbl->setItem(row, 0, new QTableWidgetItem(QString("0x%1").arg(quint16(frame.universe), 0, 16)));
                tbl->setItem(row, 1, new QTableWidgetItem("1"));
            } else {
                int cur = tbl->item(row, 1)->text().toInt();
                tbl->item(row, 1)->setText(QString::number(cur + 30));
            }
            // 通道峰值 CH0-7 / CH8-15
            QString h1, h2;
            for (int i = 0; i < 8; ++i)  h1 += QString("%1 ").arg(peak[i], 3).rightJustified(3);
            for (int i = 8; i < 16; ++i) h2 += QString("%1 ").arg(peak[i], 3).rightJustified(3);
            tbl->setItem(row, 2, new QTableWidgetItem(h1.trimmed()));
            tbl->setItem(row, 3, new QTableWidgetItem(h2.trimmed()));
        }
    }
}
void ArtNetRecorderMainWindow::onInputTabStatusChanged(ProtocolInputTab *tab, const QString &s) {
    logInfo(tr("[%1] %2").arg(protocolTypeToString(tab->type), s));
}
void ArtNetRecorderMainWindow::onInputTabError(ProtocolInputTab *tab, const QString &e) {
    logError(tr("[%1] %2").arg(protocolTypeToString(tab->type), e));
    tab->lblStatus->setText(tr("错误"));
    tab->lblStatus->setProperty("badge","error"); tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
}

// ============================================================
// 页面 4: 输出数据 — 协议 Sink Tab
// ============================================================
QWidget *ArtNetRecorderMainWindow::buildPageOutputData() {
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(16,16,16,16);
    root->setSpacing(14);

    auto *hint = new QLabel(tr("协议输出端测试 — 连接 IProtocolSink (ArtNet / NPU / DMX512 / sACN / RDM) "
                                "实时显示发送计数、目标状态. 可配合 Source 页面做 Source→Sink 回环测试."));
    hint->setStyleSheet("color: #4b6584; font-size: 11px; padding: 2px 4px;");
    hint->setWordWrap(true);
    root->addWidget(hint);

    m_outputTabs = new QTabWidget(page);
    m_outputTabs->setDocumentMode(true);

    auto *artnet = new ProtocolOutputTab{ProtocolType::ArtNet};
    auto *npu    = new ProtocolOutputTab{ProtocolType::Npu};
    auto *dmx    = new ProtocolOutputTab{ProtocolType::Dmx512};
    auto *sacn   = new ProtocolOutputTab{ProtocolType::Sacn};
    auto *rdm    = new ProtocolOutputTab{ProtocolType::ArtNet};  // RDM 走 Art-Net

    artnet->key = "artnet";
    npu->key    = "npu";
    dmx->key    = "dmx512";
    sacn->key   = "sacn";
    rdm->key    = "rdm";

    m_outputTabsData[ProtocolType::ArtNet] = artnet;
    m_outputTabsData[ProtocolType::Npu]    = npu;
    m_outputTabsData[ProtocolType::Dmx512] = dmx;
    m_outputTabsData[ProtocolType::Sacn]   = sacn;

    m_outputTabs->addTab(buildOutputTabArtNet(artnet),  tr("🎨  ArtNet (目标 IP:端口)"));
    m_outputTabs->addTab(buildOutputTabNpu(npu),        tr("🟠  MA NPU (目标 IP:9999)"));
    m_outputTabs->addTab(buildOutputTabDmx512(dmx),     tr("🔌  DMX512 (COM 口)"));
    m_outputTabs->addTab(buildOutputTabSacn(sacn),      tr("🟣  sACN / E1.31 (UDP 5568)"));
    m_outputTabs->addTab(buildOutputTabRdm(rdm),        tr("🔵  RDM (ArtRDM 0x6000)"));

    root->addWidget(m_outputTabs, 1);
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildOutputTabArtNet(ProtocolOutputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14); root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    // 配置
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->editRemoteAddr = new QLineEdit("127.0.0.1", body);
        data->spinRemotePort = new QSpinBox(body); data->spinRemotePort->setRange(1024,65535); data->spinRemotePort->setValue(6454);

        auto *targetRow = new QHBoxLayout();
        auto *btnAdd = new QPushButton(tr("+ 添加目标")); btnAdd->setProperty("action","icon");
        targetRow->addWidget(btnAdd); targetRow->addStretch();
        form->addRow(tr("目标:"), [&]{ auto *w=new QWidget(); w->setLayout(targetRow); return w; }());

        form->addRow(tr("目标 IP:"), data->editRemoteAddr);
        form->addRow(tr("目标端口:"), data->spinRemotePort);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 启动输出")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startOutputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopOutputTab(data); });

        root->addWidget(createCard(tr("ArtNet 输出目标"), body), 0, 0);
    }

    // 实时面板
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
        data->lblStatus=new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
        auto *c1=new QHBoxLayout(); data->lblSentCount=createValNumber(tr("0"),18); c1->addWidget(data->lblSentCount); c1->addStretch();
        data->lblTarget=new QLabel(tr("—"));

        form->addRow(createValLabel(tr("状态:")), data->lblStatus);
        form->addRow(createValLabel(tr("已发送帧:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());
        form->addRow(createValLabel(tr("目标地址:")), data->lblTarget);

        // (512 通道数据在下方独立卡片中显示)

        // 测试发送按钮 (发一帧默认 DMX 数据, 方便验证链路)
        auto *testRow = new QHBoxLayout();
        auto *btnTestSend = new QPushButton(tr("🔨 测试发 1 帧 (全 0)")); btnTestSend->setProperty("action","icon");
        auto *btnTestSendMax = new QPushButton(tr("🔨 测试发 1 帧 (全 255)")); btnTestSendMax->setProperty("action","icon");
        testRow->addWidget(btnTestSend); testRow->addWidget(btnTestSendMax); testRow->addStretch();
        form->addRow(tr("测试发送:"), [&]{ auto *w=new QWidget(); w->setLayout(testRow); return w; }());

        connect(btnTestSend, &QPushButton::clicked, this, [this, data]{
            if (!data->sink) return;
            LightFrame f; f.universe = 0; f.sourceProtocol = ProtocolType::ArtNet;
            f.dmxData = QByteArray(512, 0); f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
            data->sink->sendFrame(f);
            if (data->bars) data->bars->setData(f.dmxData);
            updateChannelPanel(data->lblChanInfo,
                               quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                               0.0, data->sentCount, 0);
            // 同步到"实时输出"独立页 (按域缓存, 可切换域)
            feedLiveUniverse(false,
                             quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                             f.dmxData, f.timestampUs);
        });
        connect(btnTestSendMax, &QPushButton::clicked, this, [this, data]{
            if (!data->sink) return;
            LightFrame f; f.universe = 0; f.sourceProtocol = ProtocolType::ArtNet;
            f.dmxData = QByteArray(512, 0xFF); f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
            data->sink->sendFrame(f);
            if (data->bars) data->bars->setData(f.dmxData);
            updateChannelPanel(data->lblChanInfo,
                               quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                               0.0, data->sentCount, 0);
            // 同步到"实时输出"独立页 (按域缓存, 可切换域)
            feedLiveUniverse(false,
                             quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                             f.dmxData, f.timestampUs);
        });

        root->addWidget(createCard(tr("发送状态"), body), 0, 1);

        // === 数据监视卡片 (512 通道, 跨整行) ===
        auto *dataCard = createCard(tr("输出数据监视 (512 CH)"),
                                    createChannelPanel(&data->lblChanInfo, &data->bars, page));
        dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        root->addWidget(dataCard, 2, 0, 1, 2);
        root->setRowStretch(2, 1);

    }

    // 目标列表 (ArtNetSink 特有多目标管理)
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body); v->setContentsMargins(0,0,0,0);
        auto *tbl = new QTableWidget(body);
        tbl->setColumnCount(3); tbl->setHorizontalHeaderLabels({tr("#"), tr("IP:端口"), tr("已发送")});
        tbl->horizontalHeader()->setStretchLastSection(true); tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        v->addWidget(tbl);
        root->addWidget(createCard(tr("目标列表"), body), 1, 0, 1, 2);
    }

    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    root->setRowStretch(0,0); root->setRowStretch(1,1);
    return page;
}

// NPU / DMX / RDM 输出 tab — 复用 ArtNet 结构, 只换默认参数
QWidget *ArtNetRecorderMainWindow::buildOutputTabNpu(ProtocolOutputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14); root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
        data->editRemoteAddr = new QLineEdit("127.0.0.1", body);
        data->spinRemotePort = new QSpinBox(body); data->spinRemotePort->setRange(1024,65535); data->spinRemotePort->setValue(9999);
        form->addRow(tr("目标 IP:"), data->editRemoteAddr);
        form->addRow(tr("目标端口:"), data->spinRemotePort);
        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 启动输出")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());
        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startOutputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopOutputTab(data); });
        root->addWidget(createCard(tr("MA NPU 输出目标"), body), 0, 0);
    }
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
        data->lblStatus=new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
        auto *c1=new QHBoxLayout(); data->lblSentCount=createValNumber(tr("0"),18); c1->addWidget(data->lblSentCount); c1->addStretch();
        data->lblTarget=new QLabel(tr("—"));
        form->addRow(createValLabel(tr("状态:")), data->lblStatus);
        form->addRow(createValLabel(tr("已发送帧:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());
        form->addRow(createValLabel(tr("目标地址:")), data->lblTarget);

        // (512 通道数据在下方独立卡片中显示)

        auto *testRow = new QHBoxLayout();
        auto *btnTest = new QPushButton(tr("🔨 测试发 1 帧")); btnTest->setProperty("action","icon");
        testRow->addWidget(btnTest); testRow->addStretch();
        form->addRow(tr("测试:"), [&]{ auto *w=new QWidget(); w->setLayout(testRow); return w; }());
        connect(btnTest, &QPushButton::clicked, this, [this, data]{
            if (!data->sink) return;
            LightFrame f; f.universe = 0; f.sourceProtocol = ProtocolType::Npu;
            f.dmxData = QByteArray(512, 0x80); f.timestampUs = QDateTime::currentMSecsSinceEpoch()*1000;
            data->sink->sendFrame(f);
            if (data->bars) data->bars->setData(f.dmxData);
            updateChannelPanel(data->lblChanInfo,
                               quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                               0.0, data->sentCount, 0);
            // 同步到"实时输出"独立页 (按域缓存, 可切换域)
            feedLiveUniverse(false,
                             quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                             f.dmxData, f.timestampUs);
        });
        root->addWidget(createCard(tr("发送状态"), body), 0, 1);

        // === 数据监视卡片 (512 通道, 跨整行) ===
        auto *dataCard = createCard(tr("输出数据监视 (512 CH)"),
                                    createChannelPanel(&data->lblChanInfo, &data->bars, page));
        dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        root->addWidget(dataCard, 2, 0, 1, 2);
        root->setRowStretch(2, 1);

    }
    {
        auto *body = new QWidget();
        auto *v = new QVBoxLayout(body); v->setContentsMargins(0,0,0,0);
        auto *tbl = new QTableWidget(body);
        tbl->setColumnCount(3); tbl->setHorizontalHeaderLabels({tr("#"), tr("IP:端口"), tr("已发送")});
        tbl->horizontalHeader()->setStretchLastSection(true); tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        v->addWidget(tbl);
        root->addWidget(createCard(tr("目标列表"), body), 1, 0, 1, 2);
    }
    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    root->setRowStretch(0,0); root->setRowStretch(1,1);
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildOutputTabDmx512(ProtocolOutputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14); root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
        data->editSerialPort = new QLineEdit("COM3", body);
        form->addRow(tr("串口:"), data->editSerialPort);
        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 启动输出")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());
        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startOutputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopOutputTab(data); });
        root->addWidget(createCard(tr("DMX512 输出 (COM 口)"), body), 0, 0);
    }
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);
        data->lblStatus=new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
        auto *c1=new QHBoxLayout(); data->lblSentCount=createValNumber(tr("0"),18); c1->addWidget(data->lblSentCount); c1->addStretch();
        data->lblTarget=new QLabel(tr("—"));
        form->addRow(createValLabel(tr("状态:")), data->lblStatus);
        form->addRow(createValLabel(tr("已发送帧:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());
        form->addRow(createValLabel(tr("COM 口:")), data->lblTarget);

        // (512 通道数据在下方独立卡片中显示)

        auto *testRow = new QHBoxLayout();
        auto *btnTest = new QPushButton(tr("🔨 测试发 1 帧")); btnTest->setProperty("action","icon");
        testRow->addWidget(btnTest); testRow->addStretch();
        form->addRow(tr("测试:"), [&]{ auto *w=new QWidget(); w->setLayout(testRow); return w; }());
        connect(btnTest, &QPushButton::clicked, this, [this, data]{
            if (!data->sink) return;
            LightFrame f; f.universe = 0; f.sourceProtocol = ProtocolType::Dmx512;
            f.dmxData = QByteArray(512, 0x80); f.timestampUs = QDateTime::currentMSecsSinceEpoch()*1000;
            data->sink->sendFrame(f);
            if (data->bars) data->bars->setData(f.dmxData);
            updateChannelPanel(data->lblChanInfo,
                               quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                               0.0, data->sentCount, 0);
            // 同步到"实时输出"独立页 (按域缓存, 可切换域)
            feedLiveUniverse(false,
                             quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                             f.dmxData, f.timestampUs);
        });
        root->addWidget(createCard(tr("发送状态"), body), 0, 1);

        // === 数据监视卡片 (512 通道, 跨整行) ===
        auto *dataCard = createCard(tr("输出数据监视 (512 CH)"),
                                    createChannelPanel(&data->lblChanInfo, &data->bars, page));
        dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        root->addWidget(dataCard, 2, 0, 1, 2);
        root->setRowStretch(2, 1);

    }
    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildOutputTabSacn(ProtocolOutputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14); root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    // === 配置 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->editRemoteAddr = new QLineEdit(body);
        data->editRemoteAddr->setPlaceholderText(tr("留空 = 组播 239.255.x.x"));
        data->spinRemotePort = new QSpinBox(body); data->spinRemotePort->setRange(1024,65535); data->spinRemotePort->setValue(5568);
        data->spinUniverse = new QSpinBox(body); data->spinUniverse->setRange(1, 63999); data->spinUniverse->setValue(1);
        data->spinPriority = new QSpinBox(body); data->spinPriority->setRange(0, 200); data->spinPriority->setValue(100);

        form->addRow(tr("目标 IP:"), data->editRemoteAddr);
        form->addRow(tr("目标端口:"), data->spinRemotePort);
        form->addRow(tr("Universe:"), data->spinUniverse);
        form->addRow(tr("优先级:"), data->spinPriority);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 启动输出")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startOutputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopOutputTab(data); });

        root->addWidget(createCard(tr("sACN (E1.31) 输出目标"), body), 0, 0);
    }

    // === 实时面板 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->lblStatus = new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
        auto *c1 = new QHBoxLayout(); data->lblSentCount = createValNumber(tr("0"),18);
        c1->addWidget(data->lblSentCount); c1->addStretch();
        data->lblTarget = new QLabel(tr("—"));

        form->addRow(createValLabel(tr("状态:")), data->lblStatus);
        form->addRow(createValLabel(tr("已发送包:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());
        form->addRow(createValLabel(tr("目标地址:")), data->lblTarget);

        // (512 通道数据在下方独立卡片中显示)

        auto *testRow = new QHBoxLayout();
        auto *btnRamp = new QPushButton(tr("🔨 发 1 帧渐变测试")); btnRamp->setProperty("action","icon");
        auto *btnFull = new QPushButton(tr("🔨 发 1 帧全 255")); btnFull->setProperty("action","icon");
        testRow->addWidget(btnRamp); testRow->addWidget(btnFull); testRow->addStretch();
        form->addRow(tr("测试发送:"), [&]{ auto *w=new QWidget(); w->setLayout(testRow); return w; }());

        connect(btnRamp, &QPushButton::clicked, this, [this, data]{
            if (!data->sink) return;
            LightFrame f; f.universe = 0; f.sourceProtocol = ProtocolType::Sacn;
            f.dmxData = QByteArray(512, 0);
            for (int i = 0; i < 512; ++i) f.dmxData[i] = char(i % 256);
            f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
            data->sink->sendFrame(f);
            if (data->bars) data->bars->setData(f.dmxData);
            updateChannelPanel(data->lblChanInfo,
                               quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                               0.0, data->sentCount, 0);
            // 同步到"实时输出"独立页 (按域缓存, 可切换域)
            feedLiveUniverse(false,
                             quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                             f.dmxData, f.timestampUs);
        });
        connect(btnFull, &QPushButton::clicked, this, [this, data]{
            if (!data->sink) return;
            LightFrame f; f.universe = 0; f.sourceProtocol = ProtocolType::Sacn;
            f.dmxData = QByteArray(512, char(0xFF));
            f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
            data->sink->sendFrame(f);
            if (data->bars) data->bars->setData(f.dmxData);
            updateChannelPanel(data->lblChanInfo,
                               quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                               0.0, data->sentCount, 0);
            // 同步到"实时输出"独立页 (按域缓存, 可切换域)
            feedLiveUniverse(false,
                             quint16(data->spinUniverse ? data->spinUniverse->value() : f.universe),
                             f.dmxData, f.timestampUs);
        });

        root->addWidget(createCard(tr("发送状态"), body), 0, 1);

        // === 数据监视卡片 (512 通道, 跨整行) ===
        auto *dataCard = createCard(tr("输出数据监视 (512 CH)"),
                                    createChannelPanel(&data->lblChanInfo, &data->bars, page));
        dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        root->addWidget(dataCard, 2, 0, 1, 2);
        root->setRowStretch(2, 1);

    }

    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    return page;
}

QWidget *ArtNetRecorderMainWindow::buildOutputTabRdm(ProtocolOutputTab *data) {
    auto *page = new QWidget();
    auto *root = new QGridLayout(page);
    root->setContentsMargins(14,14,14,14); root->setHorizontalSpacing(12); root->setVerticalSpacing(12);

    // === 配置 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->editRemoteAddr = new QLineEdit("127.0.0.1", body);
        data->spinRemotePort = new QSpinBox(body); data->spinRemotePort->setRange(1024,65535); data->spinRemotePort->setValue(6454);

        form->addRow(tr("网关 IP:"), data->editRemoteAddr);
        form->addRow(tr("Art-Net 端口:"), data->spinRemotePort);

        auto *note = new QLabel(tr("ArtRDM (OpCode 0x6000) — 发往 RDM 网关 / Art-Net 节点"));
        note->setStyleSheet("color: #4b6584; font-size: 11px;"); note->setWordWrap(true);
        form->addRow(tr("说明:"), note);

        auto *btnRow = new QHBoxLayout();
        data->btnStart = new QPushButton(tr("▶ 启动输出")); data->btnStart->setProperty("action","start");
        data->btnStop  = new QPushButton(tr("■ 停止"));     data->btnStop->setProperty("action","stop");
        data->btnStop->setEnabled(false);
        btnRow->addWidget(data->btnStart); btnRow->addWidget(data->btnStop); btnRow->addStretch();
        form->addRow(tr("操作:"), [&]{ auto *w=new QWidget(); w->setLayout(btnRow); return w; }());

        connect(data->btnStart, &QPushButton::clicked, this, [this,data]{ startOutputTab(data); });
        connect(data->btnStop,  &QPushButton::clicked, this, [this,data]{ stopOutputTab(data); });

        root->addWidget(createCard(tr("RDM 输出目标"), body), 0, 0);
    }

    // === 实时面板 ===
    {
        auto *body = new QWidget();
        auto *form = new QFormLayout(body);
        form->setContentsMargins(0,0,0,0); form->setSpacing(6); form->setLabelAlignment(Qt::AlignRight);

        data->lblStatus = new QLabel(tr("未启动")); data->lblStatus->setProperty("role","badge"); data->lblStatus->setProperty("badge","warn");
        auto *c1 = new QHBoxLayout(); data->lblSentCount = createValNumber(tr("0"),18);
        c1->addWidget(data->lblSentCount); c1->addStretch();
        data->lblTarget = new QLabel(tr("—"));

        form->addRow(createValLabel(tr("状态:")), data->lblStatus);
        form->addRow(createValLabel(tr("已发送 RDM:")), [&]{ auto *w=new QWidget(); w->setLayout(c1); return w; }());
        form->addRow(createValLabel(tr("目标:")), data->lblTarget);

        auto *testRow = new QHBoxLayout();
        auto *btnDisc = new QPushButton(tr("🔨 发送 DISC_UNIQUE_BRANCH")); btnDisc->setProperty("action","icon");
        auto *btnGet = new QPushButton(tr("🔨 发送 GET DEVICE_INFO"));     btnGet->setProperty("action","icon");
        testRow->addWidget(btnDisc); testRow->addWidget(btnGet); testRow->addStretch();
        form->addRow(tr("测试发送:"), [&]{ auto *w=new QWidget(); w->setLayout(testRow); return w; }());

        connect(btnDisc, &QPushButton::clicked, this, [this, data]{
            if (!data->sink) return;
            LightFrame f; f.hasRdm = true;
            f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
            data->sink->sendFrame(f);   // 空 payload → Sink 内建发现包
        });
        connect(btnGet, &QPushButton::clicked, this, [this, data]{
            if (!data->sink) return;
            LightFrame f; f.hasRdm = true;
            // 构造 GET DEVICE_INFO (PID 0x0060) 请求
            QByteArray rdm(26, 0);
            rdm[0] = char(0xCC); rdm[1] = char(0x01);
            rdm[2] = char(0x11); rdm[3] = char(0x00);
            for (int i = 4; i < 18; ++i) rdm[i] = char(0x00);  // Dest/Src UID 广播
            rdm[22] = char(0x20);   // GET_COMMAND
            rdm[23] = char(0x60);   // PID DEVICE_INFO
            f.rdmPayload = rdm;
            f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
            data->sink->sendFrame(f);
        });

        root->addWidget(createCard(tr("发送状态"), body), 0, 1);

        // === 数据监视卡片 (512 通道, 跨整行) ===
        auto *dataCard = createCard(tr("输出数据监视 (512 CH)"),
                                    createChannelPanel(&data->lblChanInfo, &data->bars, page));
        dataCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        root->addWidget(dataCard, 2, 0, 1, 2);
        root->setRowStretch(2, 1);

    }

    root->setColumnStretch(0,1); root->setColumnStretch(1,1);
    return page;
}

// --- 输出 tab 启动/停止 ---
void ArtNetRecorderMainWindow::startOutputTab(ProtocolOutputTab *tab) {
    if (tab->sink) { stopOutputTab(tab); }

    // sACN / RDM 使用 UI 内嵌 Sink (生产层暂未提供)
    if (tab->key == "sacn") {
        auto *impl = new SacnSinkImpl();
        if (tab->spinUniverse) impl->universe = quint16(tab->spinUniverse->value());
        if (tab->spinPriority) impl->priority = quint8(tab->spinPriority->value());
        tab->sink = impl;
    } else if (tab->key == "rdm") {
        tab->sink = new ArtRdmSinkImpl();
    } else {
        tab->sink = ProtocolFactory::createSink(tab->type);
    }
    if (!tab->sink) {
        tab->lblStatus->setText(tr("创建失败")); tab->lblStatus->setProperty("badge","error");
        tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
        logError(tr("创建 Sink 失败, key=%1").arg(tab->key)); return;
    }

    ConnectionParams params;
    params.protocol = tab->type;
    if (tab->editRemoteAddr)  params.remoteAddress = tab->editRemoteAddr->text();
    if (tab->spinRemotePort)  params.remotePort = quint16(tab->spinRemotePort->value());
    if (tab->editSerialPort)  params.serialPortName = tab->editSerialPort->text();
    if (tab->spinUniverse)    params.filterUniverses = { quint16(tab->spinUniverse->value()) };

    connect(tab->sink, &IProtocolSink::frameSent, this, [this,tab](quint16 u, quint64 c){
        onOutputTabFrameSent(tab, u, c);
    });
    connect(tab->sink, &IProtocolSink::started,  this, [this,tab](const QString &info){
        onOutputTabStarted(tab, info);
    });
    connect(tab->sink, &IProtocolSink::stopped,  this, [this,tab]{ onOutputTabStopped(tab); });
    connect(tab->sink, &IProtocolSink::errorOccurred, this, [this,tab](const QString &e){
        onOutputTabError(tab, e);
    });
    connect(tab->sink, &IProtocolSink::sendError, this, [this,tab](const QString &e){
        onOutputTabError(tab, e);
    });

    if (!tab->sink->start(params)) {
        tab->lblStatus->setText(tr("启动失败")); tab->lblStatus->setProperty("badge","error");
        tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
        logError(tr("%1 Sink start() 失败").arg(protocolTypeToString(tab->type)));
        delete tab->sink; tab->sink = nullptr; return;
    }

    tab->sentCount = 0;
    tab->btnStart->setEnabled(false); tab->btnStop->setEnabled(true);
    tab->lblStatus->setText(tr("运行中")); tab->lblStatus->setProperty("badge","ok");
    tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
    tab->lblTarget->setText(params.serialPortName.isEmpty()
                            ? QString("%1:%2").arg(params.remoteAddress.isEmpty() ? tr("组播") : params.remoteAddress)
                                  .arg(params.remotePort ? params.remotePort : 5568)
                            : params.serialPortName);
    logSuccess(tr("%1 Sink 已启动 — 目标 %2").arg(protocolTypeToString(tab->type), tab->lblTarget->text()));
}

void ArtNetRecorderMainWindow::stopOutputTab(ProtocolOutputTab *tab) {
    if (!tab->sink) return;
    tab->sink->stop();
    tab->sink->deleteLater();
    tab->sink = nullptr;
    tab->btnStart->setEnabled(true); tab->btnStop->setEnabled(false);
    tab->lblStatus->setText(tr("已停止")); tab->lblStatus->setProperty("badge","idle");
    tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
    logInfo(tr("%1 Sink 已停止 — 共发送 %2 帧").arg(protocolTypeToString(tab->type)).arg(tab->sentCount));
}

// --- 输出 tab 槽 ---
void ArtNetRecorderMainWindow::onOutputTabFrameSent(ProtocolOutputTab *tab, quint16 universe, quint64 count) {
    tab->sentCount += count;
    tab->lblSentCount->setText(QString::number(tab->sentCount));
    Q_UNUSED(universe);
}
void ArtNetRecorderMainWindow::onOutputTabStarted(ProtocolOutputTab *tab, const QString &info) {
    logInfo(tr("[%1] 启动成功: %2").arg(protocolTypeToString(tab->type), info));
}
void ArtNetRecorderMainWindow::onOutputTabStopped(ProtocolOutputTab *tab) {
    logInfo(tr("[%1] Sink stopped").arg(protocolTypeToString(tab->type)));
}
void ArtNetRecorderMainWindow::onOutputTabError(ProtocolOutputTab *tab, const QString &e) {
    logError(tr("[%1] %2").arg(protocolTypeToString(tab->type), e));
    tab->lblStatus->setText(tr("错误")); tab->lblStatus->setProperty("badge","error");
    tab->lblStatus->style()->unpolish(tab->lblStatus); tab->lblStatus->style()->polish(tab->lblStatus);
}

// ============================================================
// 定时刷新 UI — 顶部时钟 + 运行时长 (250ms)
// ============================================================
void ArtNetRecorderMainWindow::tickUpdate() {
    m_lblTopClock->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));

    if (m_startWallUs > 0) {
        qint64 elapsedMs = (QDateTime::currentMSecsSinceEpoch() * 1000 - m_startWallUs) / 1000;
        int h = elapsedMs / 3600000;
        int m = (elapsedMs % 3600000) / 60000;
        int s = (elapsedMs % 60000) / 1000;
        m_lblRuntime->setText(QString("%1:%2:%3")
                                   .arg(h, 2, 10, QChar('0'))
                                   .arg(m, 2, 10, QChar('0'))
                                   .arg(s, 2, 10, QChar('0')));
    }

    updateNavIndicators();
}

// ============================================================
// 导航运行标记: 测试页显示「● 测试中」, 实时页有数据时显示「● 有数据」
// ============================================================
void ArtNetRecorderMainWindow::updateNavIndicators() {
    if (!m_navList || m_navBaseTexts.size() < m_navList->count()) return;

    const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000;
    auto setMark = [this](int row, const QString &mark, const QColor &color) {
        if (row < 0 || row >= m_navList->count()) return;
        QListWidgetItem *it = m_navList->item(row);
        if (!it) return;
        const QString base = m_navBaseTexts.value(row);
        const QString text = mark.isEmpty() ? base : (base + QStringLiteral("  ") + mark);
        if (it->text() != text) it->setText(text);
        if (mark.isEmpty()) {
            it->setData(Qt::ForegroundRole, QVariant());
        } else if (it->foreground().color() != color) {
            it->setForeground(color);
        }
    };

    const bool artnetRun = m_artnetTest && (m_artnetTest->txSocket || m_artnetTest->rxTestMode);
    const bool npuRun    = m_npuTest && (m_npuTest->txSocket || m_npuTest->rxTestMode);
    const bool sacnRun   = m_sacnTest && (m_sacnTest->txSocket || m_sacnTest->rxTestMode);
    const bool dmxRun    = (m_dmxTxSerial != nullptr) || m_dmxRxTestMode;
    const bool brRun     = (m_brSocket != nullptr);
    const bool d2aRun    = (m_d2aSocket != nullptr);
    const bool np2dRun   = (m_np2dSource != nullptr);
    const bool rdmRun    = m_rdmListening;

    const QColor runColor("#00e676");
    setMark(6,  artnetRun ? QStringLiteral("● 测试中") : QString(), runColor);
    setMark(7,  npuRun    ? QStringLiteral("● 测试中") : QString(), runColor);
    setMark(8,  sacnRun   ? QStringLiteral("● 测试中") : QString(), runColor);
    setMark(9,  rdmRun    ? QStringLiteral("● 测试中") : QString(), runColor);
    setMark(10, dmxRun    ? QStringLiteral("● 测试中") : QString(), runColor);
    setMark(11, brRun     ? QStringLiteral("● 转换中") : QString(), QColor("#ffb300"));
    setMark(12, d2aRun    ? QStringLiteral("● 转换中") : QString(), QColor("#ffb300"));
    setMark(13, np2dRun   ? QStringLiteral("● 转换中") : QString(), QColor("#ffb300"));

    const bool inActive  = m_liveInActiveUs  > 0 && (nowUs - m_liveInActiveUs)  < 2000000;
    const bool outActive = m_liveOutActiveUs > 0 && (nowUs - m_liveOutActiveUs) < 2000000;
    const QColor dataColor("#00d4ff");
    setMark(4, inActive  ? QStringLiteral("● 有数据") : QString(), dataColor);
    setMark(5, outActive ? QStringLiteral("● 有数据") : QString(), dataColor);
}
