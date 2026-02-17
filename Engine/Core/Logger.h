#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

class Logger {
public:
  enum class Level { Trace = 0, Info, Warn, Error, Fatal };

  struct Entry {
    uint64_t sequence = 0;
    Level level = Level::Info;
    std::string category;
    std::string timestamp;
    std::string message;
  };

  static Logger &instance();

  void setMinLevel(Level level);
  void setFileSink(const std::string &path);
  void clearFileSink();

  void log(Level level, const std::string &category, const std::string &message);

  std::vector<Entry> recentEntries(size_t maxCount = 2000) const;

  static const char *levelName(Level level);

private:
  Logger() = default;
  std::string nowString_() const;

  mutable std::mutex mMutex;
  Level mMinLevel = Level::Trace;
  uint64_t mSequence = 0;
  std::vector<Entry> mEntries;
  size_t mMaxEntries = 8000;
  std::FILE *mFile = nullptr;
};

#define LOG_TRACE(category, message)                                            \
  Logger::instance().log(Logger::Level::Trace, category, message)
#define LOG_INFO(category, message)                                             \
  Logger::instance().log(Logger::Level::Info, category, message)
#define LOG_WARN(category, message)                                             \
  Logger::instance().log(Logger::Level::Warn, category, message)
#define LOG_ERROR(category, message)                                            \
  Logger::instance().log(Logger::Level::Error, category, message)
#define LOG_FATAL(category, message)                                            \
  Logger::instance().log(Logger::Level::Fatal, category, message)

