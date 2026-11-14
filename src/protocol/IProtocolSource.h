// IProtocolSource.h - 协议接收抽象接口 (所有接收协议的基类)
#ifndef IPROTOCOL_SOURCE_H
#define IPROTOCOL_SOURCE_H

#include "../core/Common.h"
#include "ProtocolTypes.h"
#include <QObject>
#include <QString>

/**
 * @brief 协议接收抽象接口
 * 
 * 设计: 继承 QObject 以支持 Qt 信号槽 (frameReceived)
 *       纯虚函数: start/stop/setFilterUniverses 由各协议实现
 *       
 *       各协议 Source 实现职责:
 *         1. 绑定端口 / 打开串口
 *         2. 接收网络包 / 串口数据
 *         3. 解析协议帧头 (ArtNet "Art-Net\0", NPU 长度前缀, DMX512 BREAK)
 *         4. 提取 DMX 数据 → 填充 LightFrame
 *         5. 发射 frameReceived 信号
 */
class IProtocolSource : public QObject {
    Q_OBJECT
public:
    explicit IProtocolSource(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~IProtocolSource() = default;
    
    /** 协议类型标识 */
    virtual ProtocolType protocolType() const = 0;
    
    /** 协议可读名称 */
    virtual QString protocolName() const { 
        return protocolTypeToString(protocolType()); 
    }
    
    /**
     * @brief 启动接收
     * @param params 连接参数
     */
    virtual bool start(const ConnectionParams &params) = 0;
    
    /** 停止接收 */
    virtual void stop() = 0;
    
    /** 是否正在接收 */
    virtual bool isRunning() const = 0;
    
    /**
     * @brief 设置 Universe 过滤 (空=接收所有)
     * 注意: universe 参数是协议原始值, 由 Source 自己理解
     */
    virtual void setFilterUniverses(const QList<quint16> &universes) = 0;
    
    /** 统计信息 */
    virtual quint64 framesReceived() const = 0;
    
signals:
    /**
     * @brief 收到一帧协议无关的 LightFrame
     * 
     * 这是 Source 给上层 (RecorderEngine) 推数据的唯一通道。
     * 上层不需要知道数据来自 ArtNet / NPU / 还是 DMX512。
     */
    void frameReceived(const LightFrame &frame);
    
    /**
     * @brief 状态变化 / 错误
     */
    void statusChanged(const QString &status);
    void errorOccurred(const QString &error);
};

#endif // IPROTOCOL_SOURCE_H
