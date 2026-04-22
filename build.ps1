$ErrorActionPreference = "Stop"

cmake -S . -B Build-vs18 -G "Visual Studio 18 2026" -A x64
cmake --build Build-vs18 --config Release
