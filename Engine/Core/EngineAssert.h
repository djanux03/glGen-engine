#pragma once

#include <string>

void engineAssertFail(const char *expr, const char *file, int line,
                      const std::string &message);

#define ENGINE_ASSERT(expr, message)                                            \
  do {                                                                          \
    if (!(expr))                                                                \
      engineAssertFail(#expr, __FILE__, __LINE__, message);                     \
  } while (0)
