@echo off
REM ============================================================
REM  Build script for Hierarchical Time Wheel Timer
REM  Usage:
REM    build.bat msvc       - Use MSVC (Visual Studio Developer Prompt)
REM    build.bat mingw      - Use MinGW g++
REM    build.bat clang      - Use Clang++
REM ============================================================

setlocal enabledelayedexpansion

if "%1"=="" goto :help
if "%1"=="msvc" goto :msvc
if "%1"=="mingw" goto :mingw
if "%1"=="clang" goto :clang
goto :help

:msvc
    echo [MSVC] Building...
    if not exist build mkdir build
    cl /EHsc /O2 /std:c++17 /W4 time_wheel.cpp example.cpp /Febuild\example.exe
    cl /EHsc /O2 /std:c++17 /W4 time_wheel.cpp stress_test.cpp /Febuild\stress_test.exe
    goto :done

:mingw
    echo [MinGW] Building...
    if not exist build mkdir build
    g++ -std=c++17 -O2 -Wall -Wextra -pthread time_wheel.cpp example.cpp -o build\example.exe
    g++ -std=c++17 -O2 -Wall -Wextra -pthread time_wheel.cpp stress_test.cpp -o build\stress_test.exe
    goto :done

:clang
    echo [Clang] Building...
    if not exist build mkdir build
    clang++ -std=c++17 -O2 -Wall -Wextra -pthread time_wheel.cpp example.cpp -o build\example.exe
    clang++ -std=c++17 -O2 -Wall -Wextra -pthread time_wheel.cpp stress_test.cpp -o build\stress_test.exe
    goto :done

:help
    echo Usage:
    echo   build.bat msvc       (in Visual Studio Developer Command Prompt)
    echo   build.bat mingw      (with MinGW g++ on PATH)
    echo   build.bat clang      (with Clang++ on PATH)
    echo.
    echo Or use CMake:
    echo   mkdir build ^&^& cd build ^&^& cmake .. ^&^& cmake --build . --config Release

:done
    if %ERRORLEVEL% EQU 0 (
        echo.
        echo Build complete. Binaries in build\ folder.
        echo Run: build\example.exe  or  build\stress_test.exe
    ) else (
        echo.
        echo Build FAILED with code %ERRORLEVEL%
    )
    exit /b %ERRORLEVEL%
