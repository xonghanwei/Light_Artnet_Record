// XmlStorage.cpp
#include "XmlStorage.h"
#include <QFile>
#include <QTextStream>
#include <QDomDocument>
#include <QDomElement>

XmlStorage::XmlStorage(QObject *parent) : IStorageFormat(parent) {}
XmlStorage::~XmlStorage() { close(); }

// ======== 写入 ========

bool XmlStorage::create(const QString &filePath,
                       const StorageMeta &meta,
                       const QList<StorageUniverse> &universes) {
    close();
    m_filePath = filePath;

    m_doc = QDomDocument();
    m_doc.appendChild(m_doc.createProcessingInstruction(
        QStringLiteral("xml"), QStringLiteral("version=\"1.0\" encoding=\"UTF-8\"")));

    // <recording version="1">
    m_root = m_doc.createElement(QStringLiteral("recording"));
    m_root.setAttribute(QStringLiteral("version"), 1);
    m_doc.appendChild(m_root);

    // <meta>
    QDomElement metaEl = m_doc.createElement(QStringLiteral("meta"));
    metaEl.setAttribute(QStringLiteral("formatVersion"), int(meta.formatVersion));
    metaEl.setAttribute(QStringLiteral("frameRate"), int(meta.frameRate));
    metaEl.setAttribute(QStringLiteral("universeCount"), int(meta.universeCount));
    metaEl.setAttribute(QStringLiteral("totalFrames"), qint64(meta.totalFrames));
    metaEl.setAttribute(QStringLiteral("totalDurationUs"), qint64(meta.totalDurationUs));
    metaEl.setAttribute(QStringLiteral("createTimestamp"), qint64(meta.createTimestamp));
    if(!meta.mediaFilePath.isEmpty())
        metaEl.setAttribute(QStringLiteral("mediaFilePath"), meta.mediaFilePath);
    m_root.appendChild(metaEl);

    // <universes>
    QDomElement unisEl = m_doc.createElement(QStringLiteral("universes"));
    for(const auto &u : universes) {
        QDomElement uEl = m_doc.createElement(QStringLiteral("universe"));
        uEl.setAttribute(QStringLiteral("encodedUniverse"), int(u.encodedUniverse));
        uEl.setAttribute(QStringLiteral("channelCount"), int(u.channelCount));
        unisEl.appendChild(uEl);
    }
    m_root.appendChild(unisEl);

    // <frames>
    m_framesEl = m_doc.createElement(QStringLiteral("frames"));
    m_root.appendChild(m_framesEl);

    m_meta = meta;
    m_universes = universes;
    m_valid = true;
    m_openedForWrite = true;
    return true;
}

bool XmlStorage::writeFrame(const StorageFrame &frame) {
    if(!m_openedForWrite || !m_valid) return false;

    QDomElement fEl = m_doc.createElement(QStringLiteral("frame"));
    fEl.setAttribute(QStringLiteral("index"), frame.frameIndex);
    fEl.setAttribute(QStringLiteral("timestampUs"), qint64(frame.timestampUs));
    fEl.setAttribute(QStringLiteral("dataChanged"), frame.dataChanged ? 1 : 0);

    // <universeData index="0"> base64 </universeData>
    for(auto it = frame.universeData.constBegin(); it != frame.universeData.constEnd(); ++it) {
        QDomElement udEl = m_doc.createElement(QStringLiteral("universeData"));
        udEl.setAttribute(QStringLiteral("index"), it.key());
        udEl.appendChild(m_doc.createTextNode(QString::fromLatin1(it.value().toBase64())));
        fEl.appendChild(udEl);
    }

    if(frame.hasRDM && !frame.rdmData.isEmpty()) {
        QDomElement rdmEl = m_doc.createElement(QStringLiteral("rdmData"));
        rdmEl.appendChild(m_doc.createTextNode(QString::fromLatin1(frame.rdmData.toBase64())));
        fEl.appendChild(rdmEl);
    }

    m_framesEl.appendChild(fEl);
    return true;
}

bool XmlStorage::finalize(quint32 totalFrames, quint64 totalDurationUs,
                         const QString &mediaFilePath) {
    if(!m_openedForWrite) return false;

    // 回写 meta
    QDomElement metaEl = m_root.firstChildElement(QStringLiteral("meta"));
    metaEl.setAttribute(QStringLiteral("totalFrames"), qint64(totalFrames));
    metaEl.setAttribute(QStringLiteral("totalDurationUs"), qint64(totalDurationUs));
    if(!mediaFilePath.isEmpty())
        metaEl.setAttribute(QStringLiteral("mediaFilePath"), mediaFilePath);

    // 写文件
    QFile f(m_filePath);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit errorOccurred(tr("XmlStorage finalize open fail: %1").arg(f.errorString()));
        return false;
    }
    QTextStream out(&f);
    out.setCodec("UTF-8");
    m_doc.save(out, 2);
    f.close();

    m_meta.totalFrames = totalFrames;
    m_meta.totalDurationUs = totalDurationUs;
    m_meta.mediaFilePath = mediaFilePath;

    m_openedForWrite = false;
    m_valid = false;
    return true;
}

void XmlStorage::close() {
    m_openedForWrite = false;
    m_openedForRead = false;
    m_valid = false;
    m_doc = QDomDocument();
    m_root = QDomElement();
    m_framesEl = QDomElement();
    m_frameNodes.clear();
}

// ======== 读取 ========

bool XmlStorage::open(const QString &filePath) {
    close();

    QFile f(filePath);
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit errorOccurred(tr("XmlStorage open fail: %1").arg(f.errorString()));
        return false;
    }

    QString errMsg;
    int errLine, errCol;
    if(!m_doc.setContent(&f, &errMsg, &errLine, &errCol)) {
        emit errorOccurred(tr("XmlStorage parse fail at %1:%2: %3").arg(errLine).arg(errCol).arg(errMsg));
        f.close();
        return false;
    }
    f.close();

    m_root = m_doc.firstChildElement(QStringLiteral("recording"));
    if(m_root.isNull()) {
        emit errorOccurred(tr("XmlStorage: missing <recording> root"));
        return false;
    }

    // meta
    QDomElement metaEl = m_root.firstChildElement(QStringLiteral("meta"));
    m_meta.formatVersion = quint16(metaEl.attribute(QStringLiteral("formatVersion")).toInt());
    m_meta.frameRate = quint16(metaEl.attribute(QStringLiteral("frameRate")).toInt());
    m_meta.universeCount = quint16(metaEl.attribute(QStringLiteral("universeCount")).toInt());
    m_meta.totalFrames = quint32(metaEl.attribute(QStringLiteral("totalFrames")).toULongLong());
    m_meta.totalDurationUs = quint64(metaEl.attribute(QStringLiteral("totalDurationUs")).toULongLong());
    m_meta.createTimestamp = quint64(metaEl.attribute(QStringLiteral("createTimestamp")).toULongLong());
    m_meta.mediaFilePath = metaEl.attribute(QStringLiteral("mediaFilePath"));

    // universes
    m_universes.clear();
    QDomElement unisEl = m_root.firstChildElement(QStringLiteral("universes"));
    QDomNodeList uList = unisEl.elementsByTagName(QStringLiteral("universe"));
    for(int i = 0; i < uList.size(); ++i) {
        QDomElement uEl = uList.at(i).toElement();
        m_universes.append(StorageUniverse(
            quint16(uEl.attribute(QStringLiteral("encodedUniverse")).toInt()),
            quint16(uEl.attribute(QStringLiteral("channelCount")).toInt() ?
                    uEl.attribute(QStringLiteral("channelCount")).toInt() : 512)
        ));
    }

    // 预展开 frame 节点便于随机读取
    QDomElement framesEl = m_root.firstChildElement(QStringLiteral("frames"));
    QDomNodeList fList = framesEl.elementsByTagName(QStringLiteral("frame"));
    m_frameNodes.clear();
    for(int i = 0; i < fList.size(); ++i)
        m_frameNodes.append(fList.at(i).toElement());

    m_filePath = filePath;
    m_readCursor = 0;
    m_openedForRead = true;
    m_valid = true;
    return true;
}

bool XmlStorage::readNextFrame(StorageFrame &frame) {
    if(!m_openedForRead) return false;
    if(m_readCursor >= m_frameNodes.size()) return false;

    QDomElement fEl = m_frameNodes[m_readCursor];
    frame.frameIndex = fEl.attribute(QStringLiteral("index")).toLongLong();
    frame.timestampUs = fEl.attribute(QStringLiteral("timestampUs")).toLongLong();
    frame.dataChanged = fEl.attribute(QStringLiteral("dataChanged"), QStringLiteral("1")).toInt() != 0;

    frame.universeData.clear();
    QDomNodeList udList = fEl.elementsByTagName(QStringLiteral("universeData"));
    for(int i = 0; i < udList.size(); ++i) {
        QDomElement udEl = udList.at(i).toElement();
        int idx = udEl.attribute(QStringLiteral("index")).toInt();
        QByteArray data = QByteArray::fromBase64(udEl.text().toLatin1());
        frame.universeData[idx] = data;
    }

    QDomNodeList rdmList = fEl.elementsByTagName(QStringLiteral("rdmData"));
    frame.hasRDM = !rdmList.isEmpty();
    if(frame.hasRDM)
        frame.rdmData = QByteArray::fromBase64(rdmList.at(0).toElement().text().toLatin1());

    ++m_readCursor;
    return true;
}

bool XmlStorage::seekToFrame(qint64 frameIndex) {
    if(!m_openedForRead) return false;
    m_readCursor = qBound(0, int(frameIndex), m_frameNodes.size());
    return true;
}
