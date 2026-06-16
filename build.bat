@echo off
REM ============================================================
REM  Build script for Hierarchical Time Wheel Timer
REM  Usage:
REM    build.bat msvc       - Use MSVC (Visual Studio Developer Prompt)
REM    build.bat mingw      - Use MinGW g++
REM    build.bat clang      - Use Clang++
REM    build.bat cmake      - Use CMake
REM ============================================================

setlocal enabledelayedexpansion

if "%1"=="" goto :mingw
if "%1"=="msvc" goto :msvc
if "%1"=="mingw" goto :mingw
if "%1"=="clang" goto :clang
if "%1"=="cmake" goto :cmake
goto :help

:msvc
    echo [MSVC] Building...
    if not exist build mkdir build
    cl /EHsc /O2 /std:c++17 /W4 time_wheel.cpp example.cpp /Febuild\example.exe
    cl /EHsc /O2 /std:c++17 /W4 time_wheel.cpp stress_test.cpp /Febuild\stress_test.exe
    goto :done

:mingw
    echo [MinGW g++] Building...
    if not exist build mkdir build
    if exist "C:\mingw64\bin\g++.exe" set "PATH=C:\mingw64\bin;%PATH%"
    g++ -std=c++17 -O2 -Wall -c time_wheel.cpp -o build\time_wheel.o
    g++ -std=c++17 -O2 -Wall example.cpp build\time_wheel.o -o build\example.exe -static
    g++ -std=c++17 -O2 -Wall stress_test.cpp build\time_wheel.o -o build\stress_test.exe -static
    goto :done

:clang
    echo [Clang++] Building...
    if not exist build mkdir build
    clang++ -std=c++17 -O2 -Wall -c time_wheel.cpp -o build\time_wheel.o
    clang++ -std=c++17 -O2 -Wall example.cpp build\time_wheel.o -o build\example.exe
    clang++ -std=c++17 -O2 -Wall stress_test.cpp build\time_wheel.o -o build\stress_test.exe
    goto :done

:cmake
    echo [CMake] Building...
    if not exist build mkdir build
    cd build
    cmake ..
    cmake --build . --config Release
    cd ..
    goto :done

:help
    echo Usage:
    echo   build.bat mingw      (default, requires MinGW g++ on PATH or at C:\mingw64)
    echo   build.bat msvc       (in Visual Studio Developer Command Prompt)
    echo   build.bat clang      (with Clang++ on PATH)
    echo   build.bat cmake      (with CMake on PATH)

:done
    if %ERRORLEVEL% EQU 0 (
        echo.
        echo Build complete. Binaries in build\ folder.
        echo Run:
        echo   build\example.exe      - 4-scenario walkthrough
        echo   build\stress_test.exe  - full pressure test
    ) else (
        echo.
        echo Build FAILED with code %ERRORLEVEL%
    )
    exit /b %ERRORLEVEL%
