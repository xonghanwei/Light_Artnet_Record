// OutputManager.cpp
#include "OutputManager.h"
#include "../core/Logger.h"
#include "../protocol/sinks/NpuSink.h"

OutputManager::OutputManager(QObject *parent)
    : QObject(parent) {
    m_artNetSender = new ArtNetSender(this);
    m_dmxSender = new Dmx512Sender(this);
}

OutputManager::~OutputManager() {
    stop();
}

bool OutputManager::start() {
    if (m_running) return true;
    
    bool anyArtNet = false;
    bool anyDmx = false;
    bool anyNpu = false;
    
    for (const auto &t : m_targets) {
        if (!t.config.enabled) continue;
        switch (t.config.type) {
            case OutputType::ArtNet:  anyArtNet = true; break;
            case OutputType::DMX512:  anyDmx = true; break;
            case OutputType::Npu:     anyNpu = true; break;
        }
    }
    
    // 启动 ArtNetSender (只需启动一次, 它管理多个 target)
    if (anyArtNet) {
        if (!m_artNetSender->start()) {
            LOG_ERROR("ArtNetSender 启动失败");
            return false;
        }
        
        // 注册 ArtNet targets
        for (auto &t : m_targets) {
            if (!t.config.enabled || t.config.type != OutputType::ArtNet) continue;
            m_artNetSender->addTarget(t.config.artNetIp, t.config.artNetPort, 
                                      t.config.filterUniverses);
            t.active = true;
        }
    }
    
    // 启动 Dmx512Sender (每个串口独立)
    // 但 Dmx512Sender 只管理一个串口, 所以我们需要多路复用
    // 目前版本: 只支持一个 DMX512 目标, 后续可扩展为多个串口
    if (anyDmx) {
        // 找到第一个 DMX512 目标
        Target *firstDmx = nullptr;
        for (auto &t : m_targets) {
            if (t.config.enabled && t.config.type == OutputType::DMX512) {
                firstDmx = &t;
                break;
            }
        }
        if (firstDmx) {
            if (!m_dmxSender->open(firstDmx->config.dmxPortName,
                                    firstDmx->config.dmxBaudRate)) {
                LOG_ERROR(QString("DMX512 串口打开失败: %1")
                    .arg(firstDmx->config.dmxPortName));
                emit errorOccurred(firstDmx->id, OutputType::DMX512,
                    QString("无法打开串口 %1").arg(firstDmx->config.dmxPortName));
            } else {
                firstDmx->active = true;
            }
        }
    }
    
    // 启动 NPU Sink (和 ArtNet 一样: 一个实例, 多个 target)
    if (anyNpu) {
        NpuSink *npuSink = new NpuSink();
        ConnectionParams npuParams;
        if (!npuSink->start(npuParams)) {
            LOG_ERROR("NPU Sink 启动失败");
            delete npuSink;
        } else {
            for (auto &t : m_targets) {
                if (!t.config.enabled || t.config.type != OutputType::Npu) continue;
                npuSink->addTarget(t.config.artNetIp, t.config.artNetPort);
                t.active = true;
            }
            m_npuSinks.append(npuSink);
        }
    }
    
    m_running = true;
    LOG_INFO(QString("OutputManager 已启动 (%1 个目标活跃)")
        .arg(enabledTargetCount()));
    emit allStarted();
    
    return true;
}

void OutputManager::stop() {
    if (!m_running) return;
    
    m_running = false;
    
    m_artNetSender->clearTargets();
    m_artNetSender->stop();
    
    m_dmxSender->close();
    
    for (auto *sink : m_npuSinks) {
        sink->stop();
        delete sink;
    }
    m_npuSinks.clear();
    
    for (auto &t : m_targets) t.active = false;
    
    LOG_INFO("OutputManager 已停止");
    emit allStopped();
}

int OutputManager::enabledTargetCount() const {
    int count = 0;
    for (const auto &t : m_targets) {
        if (t.config.enabled) count++;
    }
    return count;
}

int OutputManager::addArtNetTarget(const QString &ip, quint16 port,
                                    const QList<quint16> &filterUniverses) {
    OutputTargetConfig cfg;
    cfg.type = OutputType::ArtNet;
    cfg.artNetIp = ip;
    cfg.artNetPort = port;
    cfg.filterUniverses = filterUniverses;
    return addTarget(cfg);
}

int OutputManager::addDmx512Target(const QString &portName, qint32 baudRate,
                                   const QList<quint16> &filterUniverses) {
    OutputTargetConfig cfg;
    cfg.type = OutputType::DMX512;
    cfg.dmxPortName = portName;
    cfg.dmxBaudRate = baudRate;
    cfg.filterUniverses = filterUniverses;
    return addTarget(cfg);
}

int OutputManager::addTarget(const OutputTargetConfig &config) {
    QMutexLocker locker(&m_mutex);
    
    Target t;
    t.id = m_nextTargetId++;
    t.config = config;
    t.active = false;
    
    m_targets.append(t);
    
    LOG_INFO(QString("输出目标已添加 [#%1] type=%2 %3")
        .arg(t.id)
        .arg(config.type == OutputType::ArtNet ? "ArtNet" : "DMX512")
        .arg(config.type == OutputType::ArtNet 
             ? QString("%1:%2").arg(config.artNetIp).arg(config.artNetPort)
             : config.dmxPortName));
    
    return t.id;
}

bool OutputManager::removeTarget(int targetId) {
    QMutexLocker locker(&m_mutex);
    for (int i = 0; i < m_targets.size(); ++i) {
        if (m_targets[i].id == targetId) {
            m_targets.removeAt(i);
            return true;
        }
    }
    return false;
}

void OutputManager::enableTarget(int targetId, bool enable) {
    QMutexLocker locker(&m_mutex);
    for (auto &t : m_targets) {
        if (t.id == targetId) {
            t.config.enabled = enable;
            break;
        }
    }
}

void OutputManager::clearAll() {
    QMutexLocker locker(&m_mutex);
    m_targets.clear();
}

bool OutputManager::matchUniverse(const Target &t, quint16 universe) const {
    if (!t.config.enabled) return false;
    if (t.config.filterUniverses.isEmpty()) return true;
    return t.config.filterUniverses.contains(universe);
}

void OutputManager::sendDmxFrame(quint16 universe, const QByteArray &dmxData) {
    if (!m_running) return;
    
    // ArtNet 转发
    if (m_artNetSender && m_artNetSender->isRunning()) {
        // 只有匹配的 ArtNet target 才需要这个 universe
        bool needArtNet = false;
        for (const auto &t : m_targets) {
            if (t.config.type == OutputType::ArtNet && matchUniverse(t, universe)) {
                needArtNet = true;
                break;
            }
        }
        if (needArtNet) {
            m_artNetSender->sendDmxFrame(universe, dmxData);
            m_totalPacketsSent++;
        }
    }
    
    // DMX512 输出 (目前只支持一个串口, 匹配任何活跃的 DMX512 target)
    if (m_dmxSender && m_dmxSender->isOpen()) {
        bool needDmx = false;
        Target *dmxTarget = nullptr;
        for (auto &t : m_targets) {
            if (t.config.type == OutputType::DMX512 && matchUniverse(t, universe)) {
                needDmx = true;
                dmxTarget = &t;
                break;
            }
        }
        if (needDmx && dmxTarget && dmxTarget->active) {
            // DMX512 是一帧 = 一个 Universe 的 512 通道
            m_dmxSender->sendFrame(dmxData);
        }
    }
}

void OutputManager::sendMultipleUniverses(const QMap<quint16, QByteArray> &universeData) {
    if (!m_running) return;
    
    // ArtNet: 直接 fan-out 全部 (内部有自己的 target filter)
    if (m_artNetSender && m_artNetSender->isRunning()) {
        m_artNetSender->sendMultipleUniverses(universeData);
        m_totalPacketsSent += universeData.size();
    }
    
    // NPU: 逐个 universe 构 LightFrame 发给 NpuSink
    if (!m_npuSinks.isEmpty()) {
        for (auto *sink : m_npuSinks) {
            if (!sink->isRunning()) continue;
            for (auto it = universeData.constBegin(); it != universeData.constEnd(); ++it) {
                LightFrame lf;
                lf.sourceProtocol = ProtocolType::Npu;
                lf.universe = it.key();
                lf.universeRaw = quint8(it.key() & 0xFF);
                lf.dmxData = it.value();
                sink->sendFrame(lf);
                m_totalPacketsSent++;
            }
        }
    }
    
    // DMX512: 把所有匹配的 universe 找出来
    if (m_dmxSender && m_dmxSender->isOpen()) {
        Target *dmxTarget = nullptr;
        for (auto &t : m_targets) {
            if (t.config.type == OutputType::DMX512 && t.active) {
                dmxTarget = &t;
                break;
            }
        }
        if (dmxTarget) {
            // 找到匹配的第一个 universe 发到串口
            // (DMX512 标准是单 Universe, 如果多个 universe 都要发, 
            //  这里取 filter 里的第一个; 更复杂场景后续扩展)
            quint16 targetUniverse = 0;
            bool found = false;
            
            if (!dmxTarget->config.filterUniverses.isEmpty()) {
                // 按 filter 顺序找
                for (auto u : dmxTarget->config.filterUniverses) {
                    if (universeData.contains(u)) {
                        targetUniverse = u;
                        found = true;
                        break;
                    }
                }
            } else {
                // 取第一个
                if (!universeData.isEmpty()) {
                    targetUniverse = universeData.firstKey();
                    found = true;
                }
            }
            
            if (found) {
                m_dmxSender->sendFrame(universeData.value(targetUniverse));
            }
        }
    }
}
