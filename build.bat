@echo off
REM ============================================================
REM  Hierarchical Time Wheel Timer - Build & Run Script
REM  Usage:
REM    build.bat               - build (auto-detect MinGW/MSVC)
REM    build.bat mingw         - build with MinGW g++
REM    build.bat msvc          - build with MSVC (VS Dev Prompt)
REM    build.bat cmake         - build with CMake
REM    build.bat run example   - build and run example
REM    build.bat run stress    - build and run stress test
REM ============================================================

setlocal enabledelayedexpansion

if "%1"=="run" goto :run_mode
if "%1"=="mingw" goto :mingw
if "%1"=="msvc" goto :msvc
if "%1"=="cmake" goto :cmake

REM Auto-detect
if exist "C:\mingw64\bin\g++.exe" goto :mingw
where cl >nul 2>nul && goto :msvc
where g++ >nul 2>nul && goto :mingw
goto :help

:mingw
    echo [MinGW g++] Building...
    if not exist build mkdir build
    if exist "C:\mingw64\bin\g++.exe" set "PATH=C:\mingw64\bin;%PATH%"
    g++ -std=c++17 -O2 -Wall -c time_wheel.cpp -o build\time_wheel.o
    g++ -std=c++17 -O2 -Wall example.cpp build\time_wheel.o -o build\example.exe -static -static-libgcc -static-libstdc++
    g++ -std=c++17 -O2 -Wall stress_test.cpp build\time_wheel.o -o build\stress_test.exe -static -static-libgcc -static-libstdc++
    goto :done

:msvc
    echo [MSVC] Building...
    if not exist build mkdir build
    cl /EHsc /O2 /std:c++17 /W4 time_wheel.cpp example.cpp /Febuild\example.exe
    cl /EHsc /O2 /std:c++17 /W4 time_wheel.cpp stress_test.cpp /Febuild\stress_test.exe
    goto :done

:cmake
    echo [CMake] Building...
    if not exist build mkdir build
    cd build
    cmake ..
    cmake --build . --config Release
    cd ..
    goto :done

:run_mode
    if "%2"=="" goto :help
    if "%2"=="example" (
        call %~dpnx0 mingw
        echo.
        echo === Running example.exe ===
        build\example.exe
    ) else if "%2"=="stress" (
        call %~dpnx0 mingw
        echo.
        echo === Running stress_test.exe ===
        build\stress_test.exe
    ) else (
        goto :help
    )
    goto :eof

:help
    echo Usage:
    echo   build.bat                    - auto-detect and build
    echo   build.bat mingw              - build with MinGW g++
    echo   build.bat msvc               - build with MSVC (in VS Dev Prompt)
    echo   build.bat cmake              - build with CMake
    echo   build.bat run example        - build + run example demo
    echo   build.bat run stress         - build + run full stress test
    echo.
    echo Outputs:
    echo   build\example.exe            - interactive demo (6 scenarios)
    echo   build\stress_test.exe        - full stress test (9 scenarios)
    echo.
    exit /b 1

:done
    if %ERRORLEVEL% EQU 0 (
        echo.
        echo ========================================================
        echo  Build successful! Binaries in build\ folder
        echo ========================================================
        echo   build\example.exe      - feature demo (6 scenarios)
        echo   build\stress_test.exe  - full stress test (9 scenarios)
        echo ========================================================
        echo.
        echo Run:
        echo   build\example.exe      (quick feature walkthrough)
        echo   build\stress_test.exe  (full validation + stats report)
    ) else (
        echo.
        echo Build FAILED with code %ERRORLEVEL%
        exit /b %ERRORLEVEL%
    )
    exit /b 0
