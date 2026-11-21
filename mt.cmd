@echo off
setlocal
set "MT_EXECUTABLE=%~dp0build\v2\bin\migratool-cli-0.3.exe"
if not exist "%MT_EXECUTABLE%" set "MT_EXECUTABLE=%~dp0build\delivery\bin\migratool-cli-0.3.exe"
if not exist "%MT_EXECUTABLE%" (
  1>&2 echo Migratool CLI no compilada. Ejecute cmake --build --preset release.
  exit /b 1
)
"%MT_EXECUTABLE%" %*
exit /b %ERRORLEVEL%
