// SyncClockFactory.h - ISyncClock 单例工厂
#ifndef SYNC_CLOCK_FACTORY_H
#define SYNC_CLOCK_FACTORY_H

#include "SyncTypes.h"

class ISyncClock;

class SyncClockFactory {
public:
    static SyncClockFactory &instance();

    /**
     * @brief 按枚举创建时钟实例
     * @return 新实例 (调用方 delete), 未实现的类型返回 nullptr
     */
    ISyncClock *create(SyncClockType type, QObject *parent = nullptr);

    /** 按字符串名创建 ("internal" / "video" / "mtc" ...) */
    ISyncClock *create(const QString &name, QObject *parent = nullptr);

    /** 已实现的时钟类型列表 */
    QList<SyncClockType> supportedTypes() const {
        return m_supported;
    }

private:
    SyncClockFactory();
    ~SyncClockFactory() = default;
    SyncClockFactory(const SyncClockFactory&) = delete;
    SyncClockFactory &operator=(const SyncClockFactory&) = delete;

    QList<SyncClockType> m_supported;
};

#endif // SYNC_CLOCK_FACTORY_H
