// Logger.h - 简单日志系统
#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QString>
#include <QTextStream>
#include <QFile>
#include <QMutex>
#include <QDateTime>

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
    Fatal = 4
};

class Logger {
public:
    static Logger& instance();
    
    void setLevel(LogLevel level);
    LogLevel level() const;
    
    void setLogFile(const QString &filePath);
    void enableConsole(bool enable);
    
    void log(LogLevel level, const QString &message);
    
    // 便捷方法
    static void debug(const QString &msg);
    static void info(const QString &msg);
    static void warning(const QString &msg);
    static void error(const QString &msg);
    static void fatal(const QString &msg);
    
private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    LogLevel m_level = LogLevel::Info;
    QFile m_file;
    QTextStream m_fileStream;
    QMutex m_mutex;
    bool m_consoleEnabled = true;
    
    static QString levelToString(LogLevel level);
};

// 宏定义
#define LOG_DEBUG(msg) Logger::debug(QString(msg))
#define LOG_INFO(msg)  Logger::info(QString(msg))
#define LOG_WARN(msg)  Logger::warning(QString(msg))
#define LOG_ERROR(msg) Logger::error(QString(msg))
#define LOG_FATAL(msg) Logger::fatal(QString(msg))

#endif // LOGGER_H
