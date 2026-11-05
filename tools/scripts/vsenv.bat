@echo off
rem Usage: vsenv.bat <command...>  -- runs command inside the VS x64 dev environment (2026 or 2022)
set "VCV=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCV%" set "VCV=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%VCV%" >nul || exit /b 1
%*
