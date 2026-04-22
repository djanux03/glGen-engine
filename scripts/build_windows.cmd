@echo off
setlocal

set "Path=%Path%"
set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
set "PROJECT=%~1"

if "%PROJECT%"=="" set "PROJECT=C:\Users\djanu\Desktop\glGen-main\Build-vs18\Runtime\glGen.vcxproj"

if not exist "%MSBUILD%" (
  echo MSBuild not found at "%MSBUILD%"
  exit /b 1
)

"%MSBUILD%" "%PROJECT%" /m /p:Configuration=Release /p:Platform=x64 /v:diag /fl /flp:logfile=C:\Users\djanu\Desktop\glGen-main\Build-vs18\msbuild_glgen.log;verbosity=diagnostic
exit /b %ERRORLEVEL%
