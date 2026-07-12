@echo off
rem ============================================================
rem  glGenVk - the Vulkan engine runtime + editor
rem  Configure, build and run. Double-click, or from cmd:
rem      run-glgenvk.bat
rem ============================================================
setlocal
cd /d "%~dp0"

echo ============================================================
echo   glGenVk (EngineCore + VulkanRHI + editor)
echo ============================================================

rem --- Locate cmake (PATH first, then the default install dir) ---
set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
    if exist "C:\Program Files\CMake\bin\cmake.exe" (
        set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
    ) else (
        echo [ERROR] cmake not found. Install CMake or add it to PATH.
        goto :fail
    )
)

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
    "%CMAKE%" -S . -B Build-vs18 -DGLGEN_BUILD_VULKAN=ON
) else (
    "%CMAKE%" -S . -B Build-vs18 -G "Visual Studio 18 2026" -A x64 -DGLGEN_BUILD_VULKAN=ON
)
if errorlevel 1 goto :fail

rem --- 2) Build glGenVk (Release) ---
echo.
echo [2/3] Building glGenVk (Release)...
"%CMAKE%" --build Build-vs18 --config Release --target glGenVk
if errorlevel 1 goto :fail

rem --- 3) Run ---
echo.
echo [3/3] Launching...
echo    Camera: hold RMB to look, WASD+Space/Ctrl to fly (Shift = fast)
echo    Editor: W/E/R gizmo ops, double-click assets to spawn, Ctrl+S saves
"Build-vs18\bin\Release\glGenVk.exe"
goto :done

:fail
echo.
echo [FAILED] See the messages above.
pause
exit /b 1

:done
endlocal
