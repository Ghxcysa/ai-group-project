@echo off
REM 编译 Windows sidecar 二进制（在 Windows 上运行）
REM 需要安装 MSVC（Visual Studio Build Tools）或 MinGW

setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set PROJECT_ROOT=%SCRIPT_DIR%..
set CPP_DIR=%PROJECT_ROOT%\..\code
set BINARIES_DIR=%PROJECT_ROOT%\src-tauri\binaries
set TRIPLE=x86_64-pc-windows-msvc

echo =^> 编译 C++ optimal_sample...
cd /d "%CPP_DIR%"

REM 优先尝试 MSVC cl.exe
where cl >nul 2>&1
if %errorlevel% == 0 (
    echo    使用 MSVC cl.exe
    cl /std:c++17 /O2 /EHsc SampleSelectSystem.cpp main.cpp /Fe:optimal_sample.exe
) else (
    REM 回退到 MinGW g++
    where g++ >nul 2>&1
    if %errorlevel% == 0 (
        echo    使用 MinGW g++
        g++ -std=c++17 -O2 -o optimal_sample.exe SampleSelectSystem.cpp main.cpp
        set TRIPLE=x86_64-pc-windows-gnu
    ) else (
        echo [错误] 未找到 cl.exe 或 g++，请安装 MSVC 或 MinGW
        exit /b 1
    )
)

echo =^> 复制到 binaries 目录...
if not exist "%BINARIES_DIR%" mkdir "%BINARIES_DIR%"
copy /y optimal_sample.exe "%BINARIES_DIR%\optimal_sample-%TRIPLE%.exe"

echo =^> 完成！已生成: %BINARIES_DIR%\optimal_sample-%TRIPLE%.exe
