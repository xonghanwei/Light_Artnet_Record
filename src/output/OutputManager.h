// OutputManager.h - 统一输出管理 (ArtNet 转发 + DMX512 输出)
#ifndef OUTPUT_MANAGER_H
#define OUTPUT_MANAGER_H

#include "../core/Common.h"
#include "../protocol/ProtocolTypes.h"
#include "ArtNetSender.h"
#include "Dmx512Sender.h"
#include <QObject>
#include <QMap>
#include <QList>
#include <QMutex>

// 前置声明
class NpuSink;

/**
 * @brief 输出类型枚举
 */
enum class OutputType {
    ArtNet,   // ArtNet 网络转发 (UDP 6454)
    Npu,      // MA NPU / Ma-Net2 (UDP 9999)
    DMX512    // 串口 DMX512 输出
};

/**
 * @brief 单个输出目标的配置
 */
struct OutputTargetConfig {
    OutputType type;
    
    // ArtNet 目标
    QString artNetIp;
    quint16 artNetPort = ArtNet::DEFAULT_PORT;
    
    // DMX512 目标
    QString dmxPortName;
    qint32 dmxBaudRate = Dmx512Sender::defaultBaudRate();
    
    // 过滤 (空=全部 Universe)
    QList<quint16> filterUniverses;
    
    // 启用/停用
    bool enabled = true;
};

/**
 * @brief 统一输出管理器
 * 
 * 设计思路 (参考 CommX 转发模块经验):
 *   - 一源多目标 fan-out: 同一帧数据同时发给 ArtNet 转发和 DMX512 输出
 *   - 每个目标独立 filter + 独立启停: 可以灵活路由 Universe
 *   - 抽象层上统一接口: 调用者只需要 sendDmxFrame(), 不用关心底层是 ArtNet 还是串口
 *   - 自动生命周期管理: start/stop 时自动管理所有子目标
 * 
 * 使用示例:
 *   OutputManager mgr;
 *   mgr.addArtNetTarget("192.168.1.100", 6454, {0, 1});
 *   mgr.addDmx512Target("COM3", 250000, {0, 1, 2});
 *   mgr.start();
 *   // 录制循环中...
 *   mgr.sendMultipleUniverses(universeData);
 */
class OutputManager : public QObject {
    Q_OBJECT
public:
    explicit OutputManager(QObject *parent = nullptr);
    ~OutputManager();
    
    /**
     * @brief 启动所有已启用的输出目标
     */
    bool start();
    
    /**
     * @brief 停止所有输出目标
     */
    void stop();
    
    bool isRunning() const { return m_running; }
    
    // ============ 目标管理 ============
    
    /**
     * @brief 添加 ArtNet 网络转发目标
     * @return 目标 ID (从 1 开始)
     */
    int addArtNetTarget(const QString &ip, quint16 port = ArtNet::DEFAULT_PORT,
                        const QList<quint16> &filterUniverses = {});
    
    /**
     * @brief 添加 DMX512 串口输出目标
     * @return 目标 ID
     */
    int addDmx512Target(const QString &portName, qint32 baudRate = 250000,
                       const QList<quint16> &filterUniverses = {});
    
    /**
     * @brief 添加任意配置的目标
     * @return 目标 ID
     */
    int addTarget(const OutputTargetConfig &config);
    
    bool removeTarget(int targetId);
    void enableTarget(int targetId, bool enable);
    void clearAll();
    
    int targetCount() const { return m_targets.size(); }
    int enabledTargetCount() const;
    
    // ============ 数据发送 ============
    
    /**
     * @brief 发送单个 Universe 的 DMX 数据到所有匹配目标
     */
    void sendDmxFrame(quint16 universe, const QByteArray &dmxData);
    
    /**
     * @brief 发送多个 Universe 到所有目标 (fan-out)
     */
    void sendMultipleUniverses(const QMap<quint16, QByteArray> &universeData);
    
    // ============ 统计 ============
    
    quint64 totalPacketsSent() const { return m_totalPacketsSent; }
    
    // ============ 子组件访问 (供高级用法) ============
    
    ArtNetSender *artNetSender() const { return m_artNetSender; }
    Dmx512Sender *dmx512Sender() const { return m_dmxSender; }
    
signals:
    void allStarted();
    void allStopped();
    void targetStatusChanged(int id, OutputType type, bool enabled, const QString &info);
    void errorOccurred(int id, OutputType type, const QString &error);
    
private:
    // 目标内部结构
    struct Target {
        int id;
        OutputTargetConfig config;
        bool active = false; // 当前是否实际启动了底层组件
    };
    
    bool matchUniverse(const Target &t, quint16 universe) const;
    
    ArtNetSender *m_artNetSender = nullptr;
    Dmx512Sender *m_dmxSender = nullptr;
    QList<NpuSink*> m_npuSinks;   // NPU 协议输出 (可多实例, 对应不同目标集)
    
    QList<Target> m_targets;
    QMutex m_mutex;
    bool m_running = false;
    int m_nextTargetId = 1;
    quint64 m_totalPacketsSent = 0;
};

#endif // OUTPUT_MANAGER_H
