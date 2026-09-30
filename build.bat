@echo off
rem Builds bin\BundleRipper.exe (Release, static CRT). Needs CMake and Visual Studio 2022 C++.
setlocal
cd /d "%~dp0"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build build --config Release --parallel || exit /b 1
if not exist bin mkdir bin
copy /y build\Release\BundleRipper.exe bin\BundleRipper.exe >nul || exit /b 1
echo built bin\BundleRipper.exe
