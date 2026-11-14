// ProtocolFactory.h - 协议工厂 (统一创建 Source/Sink)
#ifndef PROTOCOL_FACTORY_H
#define PROTOCOL_FACTORY_H

#include "ProtocolTypes.h"
#include "IProtocolSource.h"
#include "IProtocolSink.h"
#include <memory>

/**
 * @brief 协议工厂
 * 
 * 用法:
 *   auto source = ProtocolFactory::createSource(ProtocolType::ArtNet);
 *   auto sink   = ProtocolFactory::createSink(ProtocolType::ArtNet);
 *   // 或按名称:
 *   auto source = ProtocolFactory::createSource("artnet");
 */
class ProtocolFactory {
public:
    /**
     * @brief 按 ProtocolType 创建 Source
     * @return 源对象指针 (由调用者管理生命周期, 建议用 unique_ptr)
     */
    static IProtocolSource *createSource(ProtocolType type, QObject *parent = nullptr);
    
    /**
     * @brief 按字符串名称创建 Source
     * 支持: "artnet", "npu", "dmx512"
     */
    static IProtocolSource *createSource(const QString &name, QObject *parent = nullptr);
    
    /**
     * @brief 按 ProtocolType 创建 Sink
     */
    static IProtocolSink *createSink(ProtocolType type);
    
    /**
     * @brief 按字符串名称创建 Sink
     */
    static IProtocolSink *createSink(const QString &name);
    
    /**
     * @brief 字符串 → ProtocolType
     */
    static ProtocolType nameToType(const QString &name);
    
    /**
     * @brief 列出所有已支持的协议
     */
    static QStringList supportedProtocols();
};

#endif // PROTOCOL_FACTORY_H
