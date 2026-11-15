# NAND FTL Simulator

A simplified SSD Flash Translation Layer simulator in C++17 with a Dear ImGui + ImPlot GUI.

## Layout
```
core/  ftl_core library (NAND model, FTL, GC) - no GUI dependencies
cli/   ftl_cli  - headless runs and sanity checks
gui/   ftl_gui  - ImGui/ImPlot front end
```
GLFW, Dear ImGui and ImPlot are downloaded automatically by CMake (FetchContent) on first configure.

## Prerequisites
- CMake 3.20+, a C++17 compiler, git
- **Windows:** Visual Studio 2019/2022 with "Desktop development with C++". Nothing else needed.
- **macOS:** `xcode-select --install` and `brew install cmake`
- **Linux (Ubuntu/Debian):**
  ```
  sudo apt install build-essential cmake git libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
  ```

## Build and run
```
cmake -S . -B build
cmake --build build --config Release
```
- Linux/macOS: `./build/ftl_cli` and `./build/ftl_gui`
- Windows: `build\Release\ftl_cli.exe` and `build\Release\ftl_gui.exe`

To build only the core and CLI (no GUI download): `cmake -S . -B build -DFTL_BUILD_GUI=OFF`
