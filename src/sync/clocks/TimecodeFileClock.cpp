// TimecodeFileClock.cpp
#include "TimecodeFileClock.h"
#include "../../core/Logger.h"
#include <QTextStream>
#include <QRegularExpression>

TimecodeFileClock::TimecodeFileClock(QObject *parent)
    : ISyncClock(parent) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &TimecodeFileClock::onTick);
}

TimecodeFileClock::~TimecodeFileClock() { stop(); }

bool TimecodeFileClock::parseLine(const QString &line, int fileFps, Line &out) {
    // 匹配 HH:MM:SS:FF 或 HH:MM:SS
    static const QRegularExpression re(
        R"((\d{2}):(\d{2}):(\d{2})(?:[:.,](\d{1,2}))?)");
    auto m = re.match(line);
    if (!m.hasMatch()) return false;

    int hh = m.captured(1).toInt();
    int mm = m.captured(2).toInt();
    int ss = m.captured(3).toInt();
    int ff = m.captured(4).isEmpty() ? 0 : m.captured(4).toInt();

    out.frameIndex = ((hh*3600LL + mm*60 + ss) * fileFps + ff);
    out.timestampUs = out.frameIndex * 1000000LL / fileFps;
    return true;
}

bool TimecodeFileClock::start(const SyncClockConfig &config) {
    m_config = config;
    m_frameRate = config.frameRate > 0 ? config.frameRate : 30;
    m_fileFps   = m_frameRate;
    m_lastTsUs.store(-1);
    m_currentFrame = -1;

    if (config.mediaFilePath.isEmpty()) {
        emit errorOccurred(tr("TimecodeFileClock 需要 mediaFilePath"));
        return false;
    }

    QFile f(config.mediaFilePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit errorOccurred(tr("TimecodeFileClock: 无法打开 %1").arg(config.mediaFilePath));
        return false;
    }

    QTextStream ts(&f);
    m_lines.clear();

    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (line.isEmpty()) continue;

        // 头: "#FPS=30" / "#FPS: 25"
        if (line.startsWith("#FPS", Qt::CaseInsensitive) ||
            line.startsWith("FPS=") || line.startsWith("FPS:")) {
            static const QRegularExpression fpsRe(R"(FPS\s*[:=]\s*(\d{2,3}))", QRegularExpression::CaseInsensitiveOption);
            auto fm = fpsRe.match(line);
            if (fm.hasMatch()) {
                m_fileFps = fm.captured(1).toInt();
                LOG_INFO(QString("[TimecodeFileClock] 文件 FPS 头: %1").arg(m_fileFps));
            }
            continue;
        }
        if (line.startsWith('#')) continue;

        Line ln;
        if (parseLine(line, m_fileFps, ln)) {
            m_lines.push_back(ln);
        }
    }
    f.close();

    if (m_lines.empty()) {
        emit errorOccurred(tr("TimecodeFileClock: 文件里没解析到有效时间码行"));
        return false;
    }

    // 起始时间 = 文件首行的绝对帧 (减去第一个, 让起始=0)
    qint64 offsetStart = m_lines.front().timestampUs;
    for (auto &ln : m_lines) ln.timestampUs -= offsetStart;
    m_currentFrame = 0;

    int tickMs = qMax(1, 1000 / m_frameRate);
    m_timer->start(tickMs);
    m_running.store(true);

    LOG_INFO(QString("[TimecodeFileClock] start: %1, lines=%2, fileFps=%3, clockFps=%4")
                 .arg(config.mediaFilePath).arg(m_lines.size()).arg(m_fileFps).arg(m_frameRate));
    return true;
}

void TimecodeFileClock::stop() {
    m_timer->stop();
    m_running.store(false);
    m_lastTsUs.store(-1);
}

qint64 TimecodeFileClock::currentTimestampUs() const { return m_lastTsUs.load(); }

void TimecodeFileClock::onTick() {
    // 把 m_currentFrame 映射到 m_lines 里的行
    // 简化: tick 直接按配置帧率 advance, 行与帧一一对应, 读完 → loop 或 end
    if (m_currentFrame >= qint64(m_lines.size())) {
        if (m_config.loopOnEnd) {
            m_currentFrame = 0;
            LOG_INFO("[TimecodeFileClock] loop");
            emit clockLooped(1);
        } else {
            m_running.store(false);
            m_timer->stop();
            emit clockFinished();
            return;
        }
    }

    const auto &ln = m_lines[m_currentFrame];
    m_lastTsUs.store(ln.timestampUs);
    emit frameTick(m_currentFrame, ln.timestampUs);
    ++m_currentFrame;
}
