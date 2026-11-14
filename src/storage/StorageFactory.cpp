// StorageFactory.cpp
#include "StorageFactory.h"
#include "ArtRecStorage.h"
#include "JsonStorage.h"
#include "XmlStorage.h"

// 初始化时自动注册所有内置格式
static struct FactoryRegistrar {
    FactoryRegistrar() {
        auto &f = StorageFactory::instance();
        f.registerFormat("artrec", [](QObject *p) { return new ArtRecStorage(p); });
        f.registerFormat("json",   [](QObject *p) { return new JsonStorage(p); });
        f.registerFormat("xml",    [](QObject *p) { return new XmlStorage(p); });
    }
} g_registrar;

StorageFactory &StorageFactory::instance() {
    static StorageFactory inst;
    return inst;
}

StorageFactory::StorageFactory() = default;

void StorageFactory::registerFormat(const QString &formatName, CreatorFn creator) {
    if(!m_creators.contains(formatName))
        m_formatNames.append(formatName);
    m_creators[formatName] = std::move(creator);
}

IStorageFormat *StorageFactory::createByName(const QString &formatName,
                                            QObject *parent) const {
    auto it = m_creators.constFind(formatName.toLower());
    if(it == m_creators.constEnd())
        return nullptr;
    return it.value()(parent);
}

IStorageFormat *StorageFactory::createByFilePath(const QString &filePath,
                                               QObject *parent) const {
    // 取扩展名
    int dot = filePath.lastIndexOf('.');
    if(dot < 0) return nullptr;
    QString ext = filePath.mid(dot + 1).toLower();

    // 尝试精确匹配
    auto it = m_creators.constFind(ext);
    if(it != m_creators.constEnd())
        return it.value()(parent);

    return nullptr;
}

QStringList StorageFactory::registeredFormats() const {
    return m_formatNames;
}
