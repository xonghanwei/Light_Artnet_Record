// IProtocolSource.cpp - 空实现文件 (触发 AUTOMOC 生成元对象)
// IProtocolSource 是抽象接口, 没有具体方法实现
// 但它继承 QObject + Q_OBJECT, 需要一个 .cpp 让 CMake AUTOMOC 生成 moc 代码

#include "IProtocolSource.h"

// 这里什么都不用写, 只要这个文件存在, CMake 的 AUTOMOC 就会为 IProtocolSource 生成 moc_IProtocolSource.cpp
// 子类 (ArtNetSource/NpuSource/Dmx512Source) 的 moc 会链接到父类的 moc 实现
