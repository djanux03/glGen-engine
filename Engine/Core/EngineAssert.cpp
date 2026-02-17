#include "EngineAssert.h"

#include "Logger.h"

#include <cstdlib>

void engineAssertFail(const char *expr, const char *file, int line,
                      const std::string &message) {
  LOG_FATAL("Assert", std::string("Assertion failed: ") + expr + " @ " + file +
                          ":" + std::to_string(line) + " - " + message);
  std::abort();
}
