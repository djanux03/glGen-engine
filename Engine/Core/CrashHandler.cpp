#include "CrashHandler.h"

#include "Logger.h"

#include <csignal>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
std::string gReportPath = "crash_report.txt";
std::once_flag gInstallOnce;

void writeReport(const std::string &reason) {
  std::ofstream out(gReportPath, std::ios::out | std::ios::trunc);
  if (!out.is_open())
    return;

  out << "reason=" << reason << "\n";
  out << "thread_id=" << std::this_thread::get_id() << "\n";
}

void onTerminate() {
  std::string reason = "std::terminate";
  if (auto ex = std::current_exception()) {
    try {
      std::rethrow_exception(ex);
    } catch (const std::exception &e) {
      reason += std::string(": ") + e.what();
    } catch (...) {
      reason += ": unknown exception";
    }
  }

  writeReport(reason);
  LOG_FATAL("Crash", "Captured terminate. Report: " + gReportPath);
  std::_Exit(1);
}

void onSignal(int sig) {
  // Async-signal-safe: no allocations, no mutexes, no IO streams.
  // std::to_string / LOG_FATAL / std::ofstream would deadlock or
  // double-fault if the crash occurred during allocation or while
  // holding the logger mutex.
  static const char msg[] = "glGen: fatal signal caught\n";
  // POSIX write() is async-signal-safe; on Windows _write() is the closest
  // equivalent and is safe enough for a pre-exit handler.
#ifdef _WIN32
  _write(2, msg, sizeof(msg) - 1);
#else
  (void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
#endif
  std::_Exit(128 + sig);
}
} // namespace

namespace CrashHandler {
void install(const std::string &reportPath) {
  std::call_once(gInstallOnce, [&]() {
    if (!reportPath.empty())
      gReportPath = reportPath;
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onSignal);
    std::signal(SIGSEGV, onSignal);
    std::signal(SIGILL, onSignal);
    std::signal(SIGFPE, onSignal);
  });
}
} // namespace CrashHandler

