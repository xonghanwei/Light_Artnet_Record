// IProtocolSink.h - 协议输出抽象接口 (所有输出协议的基类)
#ifndef IPROTOCOL_SINK_H
#define IPROTOCOL_SINK_H

#include "../core/Common.h"
#include "ProtocolTypes.h"
#include <QObject>
#include <QString>

/**
 * @brief 协议输出抽象接口
 * 
 * 与 IProtocolSource 对称: Sink 负责"LightFrame → 各协议包"。
 * 
 * 设计: 继承 QObject 以支持 Qt 信号槽 (UI 层监听发送状态)
 *       
 *       各协议 Sink 实现职责:
 *         1. 打开 socket / 打开串口 / 连接目标
 *         2. 组装协议帧头 (ArtNet "Art-Net\0", NPU 长度前缀, DMX512 BREAK)
 *         3. 把 LightFrame 的 dmxData 填入
 *         4. 发送 → 发射 frameSent / sendError 信号
 * 
 * UI 层连接这些信号即可实时显示: 发送状态、发送计数、错误提示。
 */
class IProtocolSink : public QObject {
    Q_OBJECT
public:
    explicit IProtocolSink(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~IProtocolSink() = default;
    
    /** 协议类型标识 */
    virtual ProtocolType protocolType() const = 0;
    
    /** 协议可读名称 */
    virtual QString protocolName() const { 
        return protocolTypeToString(protocolType()); 
    }
    
    /**
     * @brief 启动输出
     * @param params 连接参数 (目标 IP/端口, 串口名等)
     */
    virtual bool start(const ConnectionParams &params) = 0;
    
    /** 停止输出 */
    virtual void stop() = 0;
    
    /** 是否正在输出 */
    virtual bool isRunning() const = 0;
    
    /**
     * @brief 发送一帧 (协议无关输入, 由 Sink 组装成各自协议)
     * @param frame 要发送的 LightFrame
     * @return 成功返回 true
     */
    virtual bool sendFrame(const LightFrame &frame) = 0;
    
    /**
     * @brief 批量发送 (同一帧数据发给多个 Universe 的场景, 由 Sink 自己 fan-out)
     */
    virtual int sendFrames(const QList<LightFrame> &frames) {
        int ok = 0;
        for (const auto &f : frames) {
            if (sendFrame(f)) ok++;
        }
        return ok;
    }
    
    /** 统计 */
    virtual quint64 framesSent() const = 0;

signals:
    /**
     * @brief 成功发送一帧 (UI 仪表盘计数用)
     * @param universe 发送到的 Universe 编号 (编码后)
     * @param frameCount 本次发送的帧数 (通常 1, fan-out 多目标时是目标数)
     */
    void frameSent(quint16 universe, quint64 frameCount);

    /**
     * @brief 启动成功 / 状态变化
     */
    void started(const QString &info);
    void stopped();
    void statusChanged(const QString &status);

    /**
     * @brief 发送失败 / 端口错误 / 连接断开 (UI 提示用)
     */
    void sendError(const QString &error);
    void errorOccurred(const QString &error);

    /**
     * @brief 周期性统计信号 (可由各 Sink 自己决定发射频率)
     * @param framesPerSec 当前发送速率 (估算)
     */
    void statsUpdated(quint64 totalSent, double framesPerSec);
};

#endif // IPROTOCOL_SINK_H
