// Logger.cpp
#include "Logger.h"
#include <iostream>

Logger::Logger() {
}

Logger::~Logger() {
    if (m_file.isOpen()) {
        m_fileStream.flush();
        m_file.close();
    }
}

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

void Logger::setLevel(LogLevel level) {
    QMutexLocker locker(&m_mutex);
    m_level = level;
}

LogLevel Logger::level() const {
    return m_level;
}

void Logger::setLogFile(const QString &filePath) {
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) {
        m_fileStream.flush();
        m_file.close();
    }
    m_file.setFileName(filePath);
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        m_fileStream.setDevice(&m_file);
    }
}

void Logger::enableConsole(bool enable) {
    QMutexLocker locker(&m_mutex);
    m_consoleEnabled = enable;
}

void Logger::log(LogLevel level, const QString &message) {
    if (level < m_level) return;
    
    QMutexLocker locker(&m_mutex);
    
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString line = QString("[%1] [%2] %3")
        .arg(timestamp)
        .arg(levelToString(level), -7)
        .arg(message);
    
    if (m_consoleEnabled) {
        std::cout << line.toStdString() << std::endl;
    }
    
    if (m_file.isOpen()) {
        m_fileStream << line << Qt::endl;
        m_fileStream.flush();
    }
}

QString Logger::levelToString(LogLevel level) {
    switch(level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

// 便捷方法
void Logger::debug(const QString &msg) {
    instance().log(LogLevel::Debug, msg);
}

void Logger::info(const QString &msg) {
    instance().log(LogLevel::Info, msg);
}

void Logger::warning(const QString &msg) {
    instance().log(LogLevel::Warning, msg);
}

void Logger::error(const QString &msg) {
    instance().log(LogLevel::Error, msg);
}

void Logger::fatal(const QString &msg) {
    instance().log(LogLevel::Fatal, msg);
}
