cmake_minimum_required(VERSION 3.20)
project(ndd-mdview LANGUAGES CXX)

# ---------------------------------------------------------------------------
#  静态 CRT（/MT）
#
#  宿主 Notepad-- 目录里只有 14.32 的 msvcp140.dll。云端 runner 的 VS2022 更新，
#  编译出的 /MD 会要求比 14.32 更新的 msvcp140，插件装进宿主后加载器会报
#  「找不到入口点」。/MT 自带 CRT，插件不依赖任何 VC 运行库。
# ---------------------------------------------------------------------------
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

add_library(ndd-mdview SHARED
    src/ndd-mdview.cpp
    src/pluginGl.h
)

# Qt 资源：assets/*.qrc
#   ndd-mdview.qrc —— 仓库里自带的（markdown-it）
#   katex.qrc      —— CI 生成的（KaTeX 的 js/css/字体）
# 用 glob 是为了让 CI 生成的那份也能自动进来。
file(GLOB NDD_QRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.qrc")
if(NDD_QRC)
    target_sources(ndd-mdview PRIVATE ${NDD_QRC})
endif()

target_include_directories(ndd-mdview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(ndd-mdview PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# /utf-8：源码里带中文注释/字符串
target_compile_options(ndd-mdview PRIVATE /utf-8 /W3 /EHsc)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)
target_link_libraries(ndd-mdview PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)

# ---------------------------------------------------------------------------
#  WebView2
#  头文件 + 静态 loader，由 CI 从 NuGet 拉取到 3rdparty/webview2：
#      3rdparty/webview2/include/WebView2.h
#      3rdparty/webview2/x64/WebView2LoaderStatic.lib
# ---------------------------------------------------------------------------
if(NOT DEFINED WEBVIEW2_ROOT)
    set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/webview2")
endif()

set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_ROOT}/include")
set(WEBVIEW2_STATIC_LIB  "${WEBVIEW2_ROOT}/x64/WebView2LoaderStatic.lib")

if(NOT EXISTS "${WEBVIEW2_INCLUDE_DIR}/WebView2.h")
    message(FATAL_ERROR "缺少 WebView2.h：${WEBVIEW2_INCLUDE_DIR}/WebView2.h")
endif()
if(NOT EXISTS "${WEBVIEW2_STATIC_LIB}")
    message(FATAL_ERROR "缺少 WebView2LoaderStatic.lib：${WEBVIEW2_STATIC_LIB}")
endif()

target_include_directories(ndd-mdview PRIVATE "${WEBVIEW2_INCLUDE_DIR}")

# 静态 loader 需要 /WHOLEARCHIVE，否则 CreateCoreWebView2EnvironmentWithOptions
# 所在的 obj 不会被拉进来。
target_link_libraries(ndd-mdview PRIVATE "${WEBVIEW2_STATIC_LIB}")
target_link_options(ndd-mdview PRIVATE "/WHOLEARCHIVE:${WEBVIEW2_STATIC_LIB}")

# 输出固定到 build/bin/<CONFIG>/（VS 生成器会自动追加配置子目录）
set_target_properties(ndd-mdview PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
