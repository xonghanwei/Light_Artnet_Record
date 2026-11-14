// ProtocolFactory.cpp
#include "ProtocolFactory.h"
#include "sources/ArtNetSource.h"
#include "sources/NpuSource.h"
#include "sources/Dmx512Source.h"
#include "sources/ArtRecFileSource.h"
#include "sinks/ArtNetSink.h"
#include "sinks/NpuSink.h"
#include "sinks/Dmx512Sink.h"

IProtocolSource *ProtocolFactory::createSource(ProtocolType type, QObject *parent) {
    switch (type) {
        case ProtocolType::ArtNet:     return new ArtNetSource(parent);
        case ProtocolType::Npu:        return new NpuSource(parent);
        case ProtocolType::Dmx512:     return new Dmx512Source(parent);
        case ProtocolType::ArtRecFile: return new ArtRecFileSource(parent);
        default: return nullptr;
    }
}

IProtocolSource *ProtocolFactory::createSource(const QString &name, QObject *parent) {
    return createSource(nameToType(name), parent);
}

IProtocolSink *ProtocolFactory::createSink(ProtocolType type) {
    switch (type) {
        case ProtocolType::ArtNet:     return new ArtNetSink();
        case ProtocolType::Npu:        return new NpuSink();
        case ProtocolType::Dmx512:     return new Dmx512Sink();
        case ProtocolType::ArtRecFile: return nullptr; // 文件重演只有 Source, 没有 Sink
        default: return nullptr;
    }
}

IProtocolSink *ProtocolFactory::createSink(const QString &name) {
    return createSink(nameToType(name));
}

ProtocolType ProtocolFactory::nameToType(const QString &name) {
    QString lower = name.toLower().trimmed();
    if (lower == "artnet" || lower == "art-net" || lower == "art_net")
        return ProtocolType::ArtNet;
    if (lower == "npu" || lower == "ma" || lower == "ma-net" || lower == "manet2")
        return ProtocolType::Npu;
    if (lower == "dmx512" || lower == "dmx" || lower == "dmx-512")
        return ProtocolType::Dmx512;
    if (lower == "artrecfile" || lower == "artrec" || lower == "file" || lower == "recording")
        return ProtocolType::ArtRecFile;
    if (lower == "sacn" || lower == "e1.31")
        return ProtocolType::Sacn;
    if (lower == "osc")
        return ProtocolType::OSC;
    return ProtocolType::Unknown;
}

QStringList ProtocolFactory::supportedProtocols() {
    return {
        "artnet",      "Art-Net (UDP 6454)",
        "npu",         "MA NPU / Ma-Net2 (UDP 9999)",
        "dmx512",      "DMX512 (串口 RS-485 / USB-DMX)",
        "artrecfile",  "ArtRec 录制文件 (离线重演)"
    };
}
