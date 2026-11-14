// StorageFactory.h — 存储格式工厂
//
// 根据扩展名或 formatName 创建对应 IStorageFormat 实例.
// 支持运行时注册 (将来扩展格式时只需 registerFormat, 不改工厂)
//
#ifndef STORAGE_FACTORY_H
#define STORAGE_FACTORY_H

#include "IStorageFormat.h"
#include <QString>
#include <QMap>
#include <functional>

class StorageFactory {
public:
    using CreatorFn = std::function<IStorageFormat*(QObject*)>;

    /** 单例 */
    static StorageFactory &instance();

    /** 根据文件扩展名创建 (自动检测) */
    IStorageFormat *createByFilePath(const QString &filePath,
                                    QObject *parent = nullptr) const;

    /** 根据格式名创建: "artrec" / "json" / "xml" */
    IStorageFormat *createByName(const QString &formatName,
                                QObject *parent = nullptr) const;

    /** 运行时注册新格式 */
    void registerFormat(const QString &formatName, CreatorFn creator);

    /** 返回所有已注册格式名 */
    QStringList registeredFormats() const;

private:
    StorageFactory();

    QStringList m_formatNames;   // 保留注册顺序
    QMap<QString, CreatorFn> m_creators;
};

#endif // STORAGE_FACTORY_H
