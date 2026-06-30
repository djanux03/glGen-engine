@echo off
rem ============================================================
rem  glGen Vulkan engine - configure, build and run
rem  Double-click, or run from cmd:   run-vulkan.bat
rem ============================================================
setlocal
cd /d "%~dp0"

echo ============================================================
echo   glGen Vulkan engine
echo ============================================================

rem --- Locate the Vulkan SDK (installer normally sets VULKAN_SDK) ---
if not defined VULKAN_SDK (
    for /d %%D in ("C:\VulkanSDK\*") do set "VULKAN_SDK=%%D"
)
if not defined VULKAN_SDK (
    echo [ERROR] Vulkan SDK not found. Install it from https://vulkan.lunarg.com
    goto :fail
)
echo Using VULKAN_SDK=%VULKAN_SDK%
echo.

rem --- 1) Configure (first run downloads dependencies; needs internet) ---
echo [1/3] Configuring...
if exist "Build-vs18\CMakeCache.txt" (
    cmake -S . -B Build-vs18 -DGLGEN_BUILD_VULKAN=ON
) else (
    cmake -S . -B Build-vs18 -G "Visual Studio 18 2026" -A x64 -DGLGEN_BUILD_VULKAN=ON
)
if errorlevel 1 goto :fail

rem --- 2) Build the Vulkan engine (Release) ---
echo.
echo [2/3] Building glGenVulkanSmoke (Release)...
cmake --build Build-vs18 --config Release --target glGenVulkanSmoke
if errorlevel 1 goto :fail

rem --- 3) Run ---
echo.
echo [3/3] Launching ^(RMB: look, Scroll: zoom, WASD+Space/Ctrl: fly^)...
"Build-vs18\bin\Release\glGenVulkanSmoke.exe"
goto :done

:fail
echo.
echo [FAILED] See the messages above.
pause
exit /b 1

:done
endlocal
