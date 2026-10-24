@echo off
REM 构建 ATK A9 Battery（需要 MinGW-w64 gcc 在 PATH 中）
setlocal
cd /d "%~dp0"

where gcc >nul 2>nul
if errorlevel 1 (
    echo [ERROR] gcc not found. Install MinGW-w64 and add it to PATH.
    exit /b 1
)

gcc -O2 -s -municode -mwindows -o ATKA9Battery.exe src\atkbat.c ^
    -lhid -lsetupapi -lshell32 -luser32 -lgdi32 -ladvapi32

if errorlevel 1 (
    echo [ERROR] build failed.
    exit /b 1
)

for %%F in (ATKA9Battery.exe) do echo [OK] %%~nxF  %%~zF bytes
