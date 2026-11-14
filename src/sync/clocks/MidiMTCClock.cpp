// MidiMTCClock.cpp
//
// 真实 MIDI 绑定 (Windows / WinMM):
//   midiInOpen(设备, CALLBACK_FUNCTION) → midiInStart
//   回调里只做 QMetaObject::invokeMethod 排队, 解析在本对象线程执行
//   MTC Quarter-Frame = 状态字节 0xF1 + 1 字节 nibble
//
// 若 config 未指定设备 (index<0 且 name 为空) → stub 模式:
//   start() 直接返回 true, 由外部调用 onMtcQuarterFrame 手动喂数据 (单元测试用)
#include "MidiMTCClock.h"
#include "../../core/Logger.h"
#include <cstring>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <mmsystem.h>
#  include <QMetaObject>
#endif

#ifdef Q_OS_WIN
// MIDI 输入回调 (系统线程) — 只做排队, 不碰 Qt 对象状态
static void CALLBACK mtcMidiInProc(HMIDIIN, UINT wMsg, DWORD_PTR dwInstance,
                                   DWORD_PTR dwParam1, DWORD_PTR /*dwParam2*/) {
    if (wMsg != MIM_DATA) return;
    auto *self = reinterpret_cast<MidiMTCClock*>(dwInstance);
    if (!self) return;

    const DWORD data = DWORD(dwParam1);
    const quint8 status = quint8(data & 0xFF);
    if (status != 0xF1) return;                    // 仅处理 MTC Quarter Frame
    const quint8 nibble = quint8((data >> 8) & 0xFF);

    QMetaObject::invokeMethod(self, "onMtcQuarterFrame", Qt::QueuedConnection,
                              Q_ARG(quint8, nibble));
}
#endif

MidiMTCClock::MidiMTCClock(QObject *parent)
    : ISyncClock(parent) {
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(false);
    m_timeoutTimer->setInterval(2000);
    connect(m_timeoutTimer, &QTimer::timeout, this, &MidiMTCClock::onTimeout);
}

MidiMTCClock::~MidiMTCClock() { stop(); }

bool MidiMTCClock::start(const SyncClockConfig &config) {
    m_config = config;
    m_frameRate = config.frameRate > 0 ? config.frameRate : 30;
    m_lastTsUs.store(-1);
    m_qf = QFAccum();
    m_qf.fps = m_frameRate;
    m_qf.waitingRestart = false;

    const bool explicitDevice = (config.midiInputDeviceIndex >= 0)
                                || !config.midiInputDeviceName.trimmed().isEmpty();
    if (explicitDevice) {
        if (!openMidiInput(config))
            return false;
    } else {
        LOG_WARN("[MidiMTCClock] 未指定 MIDI 设备, 使用 stub 模式 (由外部喂 quarter-frame)");
    }

    m_running.store(true);
    m_timeoutTimer->start();
    emit statusChanged(explicitDevice
        ? tr("MIDI TimeCode 监听中")
        : tr("MIDI TimeCode 监听中 (stub, 未绑定设备)"));
    return true;
}

// ============================================================
// 打开 WinMM MIDI 输入设备
// ============================================================
bool MidiMTCClock::openMidiInput(const SyncClockConfig &config) {
#ifdef Q_OS_WIN
    const UINT count = midiInGetNumDevs();
    if (count == 0) {
        emit errorOccurred(tr("未找到本机 MIDI 输入设备"));
        return false;
    }

    UINT deviceId = 0;
    QString deviceName;
    if (config.midiInputDeviceIndex >= 0) {
        if (UINT(config.midiInputDeviceIndex) >= count) {
            emit errorOccurred(tr("MIDI 设备索引 %1 超出范围 (共 %2 个)")
                                   .arg(config.midiInputDeviceIndex).arg(count));
            return false;
        }
        deviceId = UINT(config.midiInputDeviceIndex);
    } else {
        // 按名称匹配, 找不到则退回第一个
        const QString wanted = config.midiInputDeviceName.trimmed();
        bool matched = false;
        for (UINT i = 0; i < count; ++i) {
            MIDIINCAPS caps;
            std::memset(&caps, 0, sizeof(caps));
            if (midiInGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
            if (QString::fromWCharArray(caps.szPname).trimmed() == wanted) {
                deviceId = i;
                matched = true;
                break;
            }
        }
        if (!matched)
            emit errorOccurred(tr("找不到 MIDI 输入设备 \"%1\", 改用第一个").arg(wanted));
    }

    // 读取设备名 (日志/状态)
    MIDIINCAPS caps;
    std::memset(&caps, 0, sizeof(caps));
    if (midiInGetDevCaps(deviceId, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
        deviceName = QString::fromWCharArray(caps.szPname).trimmed();

    HMIDIIN handle = nullptr;
    const MMRESULT r = midiInOpen(&handle, deviceId,
                                  reinterpret_cast<DWORD_PTR>(&mtcMidiInProc),
                                  reinterpret_cast<DWORD_PTR>(this),
                                  CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        emit errorOccurred(tr("midiInOpen 失败 (code=%1)").arg(int(r)));
        return false;
    }
    midiInStart(handle);
    m_midiInHandle = handle;

    LOG_INFO(QString("[MidiMTCClock] 已绑定 MIDI 输入: %1").arg(deviceName));
    emit statusChanged(tr("MIDI TimeCode 监听中 — 设备: %1").arg(deviceName));
    return true;
#else
    Q_UNUSED(config);
    emit errorOccurred(tr("当前平台未实现 MIDI 输入绑定"));
    return false;
#endif
}

// ============================================================
void MidiMTCClock::closeMidiInput() {
#ifdef Q_OS_WIN
    if (m_midiInHandle) {
        auto handle = reinterpret_cast<HMIDIIN>(m_midiInHandle);
        midiInStop(handle);
        midiInReset(handle);
        midiInClose(handle);
        m_midiInHandle = nullptr;
        LOG_INFO("[MidiMTCClock] MIDI 输入已关闭");
    }
#endif
}


void MidiMTCClock::stop() {
    m_timeoutTimer->stop();
    m_running.store(false);
    m_lastTsUs.store(-1);
    closeMidiInput();
    LOG_INFO("[MidiMTCClock] stop");
}

qint64 MidiMTCClock::currentTimestampUs() const {
    return m_lastTsUs.load();
}

void MidiMTCClock::onMtcQuarterFrame(quint8 nibble) {
    // 收到 quarter-frame index 0 → 说明新一轮开始, 重置累积器
    int idx = nibble & 0x0F;
    quint8 val = nibble >> 4;

    if (idx == 0) m_qf.waitingRestart = false;
    if (m_qf.waitingRestart) return;  // 跳过断裂帧

    switch (idx) {
        case 0: m_qf.frame_ls = val; break;
        case 1: m_qf.frame_ms = val; break;
        case 2: m_qf.sec_ls   = val; break;
        case 3: m_qf.sec_ms   = val; break;
        case 4: m_qf.min_ls   = val; break;
        case 5: m_qf.min_ms   = val; break;
        case 6: m_qf.hour_ls  = val; break;
        case 7:
            m_qf.hour_ms = val & 0x01;
            // MTC quarter-frame fps type: 0=24, 1=25, 2=29.97, 3=30
            switch ((val >> 1) & 0x03) {
                case 0: m_qf.fps = 24; break;
                case 1: m_qf.fps = 25; break;
                case 2: m_qf.fps = 29; break;  // 29.97 近似
                case 3: m_qf.fps = 30; break;
            }
            m_qf.dropFrame = bool(val & 0x80);
            break;
    }

    m_qf.lastIndex = idx;
    m_timeoutTimer->start();  // 收到任何 qf 重置超时

    // 收到 index 7 → 一帧完整, emit
    if (idx == 7) {
        qint64 ts = m_qf.toUs();
        m_lastTsUs.store(ts);
        qint64 frameIdx = ts * m_qf.fps / 1000000;
        emit frameTick(frameIdx, ts);
    }
}

void MidiMTCClock::onMtcFullFrame(quint8 hours, quint8 minutes, quint8 seconds,
                                  quint8 frames, quint8 smpteType) {
    int fps;
    switch (smpteType) {
        case 0: fps = 24; break;
        case 1: fps = 25; break;
        case 2: fps = 29; break;
        case 3: fps = 30; break;
        default: fps = 30;
    }
    qint64 total = ((hours*3600LL + minutes*60 + seconds) * fps + frames);
    qint64 ts = total * 1000000LL / fps;
    m_lastTsUs.store(ts);
    m_qf.fps = fps;
    m_timeoutTimer->start();
    emit frameTick(total, ts);
}

void MidiMTCClock::onTimeout() {
    if (!m_running.load()) return;
    stop();   // 关闭 MIDI 句柄, 置 running=false
    emit errorOccurred(tr("MIDI TimeCode 超时 (2s 没收到新 quarter-frame)"));
    emit clockFinished();
}
