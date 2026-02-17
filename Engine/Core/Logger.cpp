#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger &Logger::instance() {
  static Logger g;
  return g;
}

void Logger::setMinLevel(Level level) {
  std::lock_guard<std::mutex> lock(mMutex);
  mMinLevel = level;
}

void Logger::setFileSink(const std::string &path) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (mFile) {
    std::fclose(mFile);
    mFile = nullptr;
  }
  mFile = std::fopen(path.c_str(), "a");
}

void Logger::clearFileSink() {
  std::lock_guard<std::mutex> lock(mMutex);
  if (mFile) {
    std::fclose(mFile);
    mFile = nullptr;
  }
}

const char *Logger::levelName(Level level) {
  switch (level) {
  case Level::Trace:
    return "TRACE";
  case Level::Info:
    return "INFO";
  case Level::Warn:
    return "WARN";
  case Level::Error:
    return "ERROR";
  case Level::Fatal:
    return "FATAL";
  default:
    return "UNKNOWN";
  }
}

std::string Logger::nowString_() const {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t t = clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return ss.str();
}

void Logger::log(Level level, const std::string &category,
                 const std::string &message) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (level < mMinLevel)
    return;

  Entry e;
  e.sequence = ++mSequence;
  e.level = level;
  e.category = category;
  e.timestamp = nowString_();
  e.message = message;

  mEntries.push_back(e);
  if (mEntries.size() > mMaxEntries) {
    const size_t overflow = mEntries.size() - mMaxEntries;
    mEntries.erase(mEntries.begin(), mEntries.begin() + (long)overflow);
  }

  std::ostringstream line;
  line << "[" << e.timestamp << "]"
       << " [" << levelName(level) << "]"
       << " [" << e.category << "] " << e.message << "\n";

  if (level >= Level::Warn)
    std::cerr << line.str();
  else
    std::cout << line.str();

  if (mFile) {
    std::fputs(line.str().c_str(), mFile);
    std::fflush(mFile);
  }
}

std::vector<Logger::Entry> Logger::recentEntries(size_t maxCount) const {
  std::lock_guard<std::mutex> lock(mMutex);
  if (mEntries.size() <= maxCount)
    return mEntries;
  return std::vector<Entry>(mEntries.end() - (long)maxCount, mEntries.end());
}

