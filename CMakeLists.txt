cmake_minimum_required(VERSION 3.20)
project(ftl_sim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(FTL_BUILD_GUI "Build the ImGui front end" ON)

# ---------------------------------------------------------------------------
# Core simulator library (no GUI dependencies)
# ---------------------------------------------------------------------------
add_library(ftl_core STATIC
  core/nand.cpp
  core/ftl.cpp
  core/nvme.cpp
  core/workload.cpp
)
target_include_directories(ftl_core PUBLIC core)
if(MSVC)
  target_compile_options(ftl_core PRIVATE /W4)
else()
  target_compile_options(ftl_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Headless CLI (sanity checks, batch runs)
# ---------------------------------------------------------------------------
add_executable(ftl_cli cli/main.cpp)
target_link_libraries(ftl_cli PRIVATE ftl_core)

# Experiments: prints the results table for the README
add_executable(ftl_experiments cli/experiments.cpp)
target_link_libraries(ftl_experiments PRIVATE ftl_core)

# ---------------------------------------------------------------------------
# GUI: GLFW + Dear ImGui + ImPlot, fetched automatically
# ---------------------------------------------------------------------------
if(FTL_BUILD_GUI)
  include(FetchContent)
  set(FETCHCONTENT_QUIET OFF)

  # GLFW (has its own CMake)
  set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND  OFF CACHE BOOL "" FORCE)  # X11 only on Linux (works under XWayland too)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

  # Dear ImGui and ImPlot have no CMake, so we only download them
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
  FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
    GIT_SHALLOW    TRUE)

  FetchContent_MakeAvailable(glfw imgui implot)

  find_package(OpenGL REQUIRED)

  add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
  )
  target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR})
  target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

  add_executable(ftl_gui gui/main.cpp)
  target_link_libraries(ftl_gui PRIVATE ftl_core imgui_lib)
  # Modern font shipped with Dear ImGui (misc/fonts/Roboto-Medium.ttf)
  target_compile_definitions(ftl_gui PRIVATE FTL_FONT_DIR="${imgui_SOURCE_DIR}/misc/fonts")
endif()
