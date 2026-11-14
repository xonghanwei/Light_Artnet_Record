// SyncClockFactory.cpp
#include "SyncClockFactory.h"
#include "ISyncClock.h"
#include "clocks/InternalTimerClock.h"
#ifdef ARTNET_WITH_FFMPEG
#include "clocks/VideoFileClock.h"
#include "clocks/AudioFileClock.h"
#endif
#include "clocks/ArtNetTimeCodeClock.h"
#include "clocks/MidiMTCClock.h"
#include "clocks/TimecodeFileClock.h"

SyncClockFactory::SyncClockFactory() {
    m_supported = {
        SyncClockType::InternalTimer,
#ifdef ARTNET_WITH_FFMPEG
        SyncClockType::VideoFile,
        SyncClockType::AudioFile,
#endif
        SyncClockType::ArtNetTimeCode,
        SyncClockType::MidiMTC,
        SyncClockType::TimecodeFile,
    };
}

SyncClockFactory &SyncClockFactory::instance() {
    static SyncClockFactory inst;
    return inst;
}

ISyncClock *SyncClockFactory::create(SyncClockType type, QObject *parent) {
    switch (type) {
        case SyncClockType::InternalTimer:  return new InternalTimerClock(parent);
#ifdef ARTNET_WITH_FFMPEG
        case SyncClockType::VideoFile:      return new VideoFileClock(parent);
        case SyncClockType::AudioFile:      return new AudioFileClock(parent);
#endif
        case SyncClockType::ArtNetTimeCode: return new ArtNetTimeCodeClock(parent);
        case SyncClockType::MidiMTC:        return new MidiMTCClock(parent);
        case SyncClockType::TimecodeFile:   return new TimecodeFileClock(parent);
        default: return nullptr;
    }
}

ISyncClock *SyncClockFactory::create(const QString &name, QObject *parent) {
    return create(syncClockTypeFromString(name), parent);
}
