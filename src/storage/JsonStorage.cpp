// JsonStorage.cpp
#include "JsonStorage.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDataStream>
#include <QIODevice>

JsonStorage::JsonStorage(QObject *parent) : IStorageFormat(parent) {}
JsonStorage::~JsonStorage() { close(); }

// ======== helper: QByteArray → QJsonArray (uint8 数组) ========
static QJsonArray bytesToJson(const QByteArray &data) {
    QJsonArray arr;
    for(int i = 0; i < data.size(); ++i)
        arr.append(quint8(data[i]));
    return arr;
}
static QByteArray jsonToBytes(const QJsonArray &arr) {
    QByteArray data;
    data.reserve(arr.size());
    for(const auto &v : arr)
        data.append(char(v.toInt() & 0xFF));
    return data;
}

// ======== 写入 ========

bool JsonStorage::create(const QString &filePath,
                        const StorageMeta &meta,
                        const QList<StorageUniverse> &universes) {
    close();
    m_filePath = filePath;

    // 构建 root
    QJsonObject root;
    root["version"] = 1;

    // meta
    QJsonObject mj;
    mj["formatVersion"] = meta.formatVersion;
    mj["frameRate"] = meta.frameRate;
    mj["universeCount"] = int(meta.universeCount);
    mj["totalFrames"] = qint64(meta.totalFrames);
    mj["totalDurationUs"] = qint64(meta.totalDurationUs);
    mj["createTimestamp"] = qint64(meta.createTimestamp);
    if(!meta.mediaFilePath.isEmpty())
        mj["mediaFilePath"] = meta.mediaFilePath;
    root["meta"] = mj;

    // universes
    QJsonArray uj;
    for(const auto &u : universes) {
        QJsonObject o;
        o["encodedUniverse"] = int(u.encodedUniverse);
        o["channelCount"] = int(u.channelCount);
        uj.append(o);
    }
    root["universes"] = uj;

    root["frames"] = QJsonArray();

    m_root = root;
    m_framesArray = m_root["frames"].toArray();
    m_meta = meta;
    m_universes = universes;
    m_valid = true;
    m_openedForWrite = true;
    return true;
}

bool JsonStorage::writeFrame(const StorageFrame &frame) {
    if(!m_openedForWrite || !m_valid) return false;

    QJsonObject fj;
    fj["frameIndex"] = frame.frameIndex;
    fj["timestampUs"] = qint64(frame.timestampUs);
    fj["dataChanged"] = frame.dataChanged;

    QJsonObject udj;
    for(auto it = frame.universeData.constBegin(); it != frame.universeData.constEnd(); ++it) {
        udj[QString::number(it.key())] = bytesToJson(it.value());
    }
    fj["universeData"] = udj;

    if(frame.hasRDM && !frame.rdmData.isEmpty())
        fj["rdmData"] = bytesToJson(frame.rdmData);

    m_framesArray.append(fj);
    return true;
}

bool JsonStorage::finalize(quint32 totalFrames, quint64 totalDurationUs,
                          const QString &mediaFilePath) {
    if(!m_openedForWrite) return false;

    // 回写统计到 meta
    QJsonObject mj = m_root["meta"].toObject();
    mj["totalFrames"] = qint64(totalFrames);
    mj["totalDurationUs"] = qint64(totalDurationUs);
    if(!mediaFilePath.isEmpty())
        mj["mediaFilePath"] = mediaFilePath;
    m_root["meta"] = mj;
    m_root["frames"] = m_framesArray;

    // 写文件
    QFile f(m_filePath);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit errorOccurred(tr("JsonStorage finalize open fail: %1").arg(f.errorString()));
        return false;
    }

    QJsonDocument doc(m_root);
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();

    m_meta.totalFrames = totalFrames;
    m_meta.totalDurationUs = totalDurationUs;
    m_meta.mediaFilePath = mediaFilePath;

    m_openedForWrite = false;
    m_valid = false;
    return true;
}

void JsonStorage::close() {
    if(m_openedForWrite) {
        // 警告: 未 finalize 就关闭 → 文件不会被写出
        m_openedForWrite = false;
    }
    if(m_openedForRead) {
        m_openedForRead = false;
    }
    m_valid = false;
    m_root = QJsonObject();
    m_framesArray = QJsonArray();
}

// ======== 读取 ========

bool JsonStorage::open(const QString &filePath) {
    close();

    QFile f(filePath);
    if(!f.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("JsonStorage open fail: %1").arg(f.errorString()));
        return false;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();

    if(err.error != QJsonParseError::NoError || !doc.isObject()) {
        emit errorOccurred(tr("JsonStorage parse fail: %1").arg(err.errorString()));
        return false;
    }

    m_root = doc.object();

    // 解析 meta
    QJsonObject mj = m_root["meta"].toObject();
    m_meta.formatVersion = quint16(mj["formatVersion"].toInt());
    m_meta.frameRate = quint16(mj["frameRate"].toInt());
    m_meta.universeCount = quint16(mj["universeCount"].toInt());
    m_meta.totalFrames = quint32(mj["totalFrames"].toVariant().toULongLong());
    m_meta.totalDurationUs = quint64(mj["totalDurationUs"].toVariant().toULongLong());
    m_meta.createTimestamp = quint64(mj["createTimestamp"].toVariant().toULongLong());
    m_meta.mediaFilePath = mj["mediaFilePath"].toString();

    // 解析 universes
    m_universes.clear();
    QJsonArray uj = m_root["universes"].toArray();
    for(const auto &uo : uj) {
        QJsonObject o = uo.toObject();
        m_universes.append(StorageUniverse(
            quint16(o["encodedUniverse"].toInt()),
            quint16(o["channelCount"].toInt(512))
        ));
    }

    m_framesArray = m_root["frames"].toArray();
    m_filePath = filePath;
    m_readCursor = 0;
    m_openedForRead = true;
    m_valid = true;
    return true;
}

bool JsonStorage::readNextFrame(StorageFrame &frame) {
    if(!m_openedForRead) return false;
    if(m_readCursor >= m_framesArray.size()) return false;

    QJsonObject fj = m_framesArray[m_readCursor].toObject();
    frame.frameIndex = fj["frameIndex"].toVariant().toLongLong();
    frame.timestampUs = fj["timestampUs"].toVariant().toLongLong();
    frame.dataChanged = fj["dataChanged"].toBool(true);

    QJsonObject udj = fj["universeData"].toObject();
    frame.universeData.clear();
    for(auto it = udj.constBegin(); it != udj.constEnd(); ++it) {
        frame.universeData[it.key().toInt()] = jsonToBytes(it.value().toArray());
    }

    frame.hasRDM = fj.contains("rdmData");
    if(frame.hasRDM)
        frame.rdmData = jsonToBytes(fj["rdmData"].toArray());

    ++m_readCursor;
    return true;
}

bool JsonStorage::seekToFrame(qint64 frameIndex) {
    if(!m_openedForRead) return false;
    m_readCursor = qBound(0, int(frameIndex), m_framesArray.size());
    return true;
}
