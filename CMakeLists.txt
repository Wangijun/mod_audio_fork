cmake_minimum_required(VERSION 3.10)
project(mod_audio_fork)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置 C 标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 查找所需的包
find_package(PkgConfig REQUIRED)

# 查找 FreeSWITCH - Ubuntu 22.04 特定路径及更好的检测
set(FREESWITCH_SEARCH_PATHS
    /usr/include/freeswitch
    /usr/local/include/freeswitch
    /opt/freeswitch/include/freeswitch
    /usr/include
    /usr/local/include
    /opt/freeswitch/include
)

find_path(FREESWITCH_INCLUDE_DIR switch.h
    PATHS ${FREESWITCH_SEARCH_PATHS}
    NO_DEFAULT_PATH
)

if(NOT FREESWITCH_INCLUDE_DIR)
    find_path(FREESWITCH_INCLUDE_DIR switch.h)
endif()

# 尝试在多个位置查找 FreeSWITCH 库
set(FREESWITCH_LIB_SEARCH_PATHS
    /usr/lib/x86_64-linux-gnu
    /usr/lib
    /usr/local/lib
    /opt/freeswitch/lib
    /usr/lib/freeswitch
)

find_library(FREESWITCH_LIBRARY
    NAMES freeswitch
    PATHS ${FREESWITCH_LIB_SEARCH_PATHS}
    NO_DEFAULT_PATH
)

if(NOT FREESWITCH_LIBRARY)
    find_library(FREESWITCH_LIBRARY NAMES freeswitch)
endif()

# 如果仍未找到 FreeSWITCH，尝试通过 pkg-config 查找
if(NOT FREESWITCH_INCLUDE_DIR OR NOT FREESWITCH_LIBRARY)
    pkg_check_modules(FREESWITCH_PKG freeswitch)
    if(FREESWITCH_PKG_FOUND)
        set(FREESWITCH_INCLUDE_DIR ${FREESWITCH_PKG_INCLUDE_DIRS})
        set(FREESWITCH_LIBRARY ${FREESWITCH_PKG_LIBRARIES})
        message(STATUS "通过 pkg-config 找到 FreeSWITCH")
    endif()
endif()

# 检查是否找到 FreeSWITCH
if(NOT FREESWITCH_INCLUDE_DIR)
    message(FATAL_ERROR "未找到 FreeSWITCH 头文件。请安装 FreeSWITCH 开发包:
    sudo apt-get install freeswitch-dev
    或手动设置 FREESWITCH_INCLUDE_DIR。")
endif()

if(NOT FREESWITCH_LIBRARY)
    message(FATAL_ERROR "未找到 FreeSWITCH 库。请安装 FreeSWITCH:
    sudo apt-get install freeswitch
    或手动设置 FREESWITCH_LIBRARY。")
endif()

# 查找 libwebsockets
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)

# 查找 speex
pkg_check_modules(SPEEX REQUIRED speex)

# 同时尝试查找包含重采样函数的 speexdsp
find_library(SPEEXDSP_LIBRARY
    NAMES speexdsp
    PATHS
    /usr/lib/x86_64-linux-gnu
    /usr/lib
    /usr/local/lib
    NO_DEFAULT_PATH
)

if(NOT SPEEXDSP_LIBRARY)
    find_library(SPEEXDSP_LIBRARY NAMES speexdsp)
endif()

# 如果未找到 speexdsp，尝试通过 pkg-config 查找
if(NOT SPEEXDSP_LIBRARY)
    pkg_check_modules(SPEEXDSP speexdsp)
    if(SPEEXDSP_FOUND)
        set(SPEEXDSP_LIBRARY ${SPEEXDSP_LIBRARIES})
    endif()
endif()

# 查找 boost - Ubuntu 22.04 特定
find_package(Boost REQUIRED COMPONENTS system thread)

# 查找 boost circular_buffer（仅头文件）- Ubuntu 22.04 路径
find_path(BOOST_INCLUDE_DIR boost/circular_buffer.hpp
    PATHS
    /usr/include
    /usr/local/include
    /opt/homebrew/include
    NO_DEFAULT_PATH
)

if(NOT BOOST_INCLUDE_DIR)
    find_path(BOOST_INCLUDE_DIR boost/circular_buffer.hpp)
endif()

# 设置源文件
set(MOD_AUDIO_FORK_SOURCES
    mod_audio_fork.c
    lws_glue.cpp
    parser.cpp
    audio_pipe.cpp
    vector_math.cpp
)

# 创建共享库
add_library(mod_audio_fork SHARED ${MOD_AUDIO_FORK_SOURCES})

# 设置包含目录
target_include_directories(mod_audio_fork PRIVATE
    ${FREESWITCH_INCLUDE_DIR}
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
    ${SPEEX_INCLUDE_DIRS}
    ${BOOST_INCLUDE_DIRS}
    ${BOOST_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 设置编译定义
target_compile_definitions(mod_audio_fork PRIVATE
    MOD_AUDIO_FORK_BUILD
    SWITCH_PACKAGE_VERSION="1.0.0"
    _GNU_SOURCE
)

# 设置编译标志 - Ubuntu 22.04 优化
target_compile_options(mod_audio_fork PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -fPIC -O2>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -fPIC -O2>
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -fPIC -O2>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -fPIC -O2>
)

# 检查 AVX2 支持 - Ubuntu 22.04 通常支持此特性
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
if(COMPILER_SUPPORTS_AVX2)
    target_compile_options(mod_audio_fork PRIVATE -mavx2)
    target_compile_definitions(mod_audio_fork PRIVATE USE_AVX2)
    message(STATUS "使用 AVX2 优化")
else()
    # 检查 SSE2 支持
    check_cxx_compiler_flag("-msse2" COMPILER_SUPPORTS_SSE2)
    if(COMPILER_SUPPORTS_SSE2)
        target_compile_options(mod_audio_fork PRIVATE -msse2)
        target_compile_definitions(mod_audio_fork PRIVATE USE_SSE2)
        message(STATUS "使用 SSE2 优化")
    endif()
endif()

# 链接库
target_link_libraries(mod_audio_fork
    ${FREESWITCH_LIBRARY}
    ${LIBWEBSOCKETS_LIBRARIES}
    ${SPEEX_LIBRARIES}
    ${SPEEXDSP_LIBRARY}
    ${Boost_LIBRARIES}
    stdc++
    pthread
    dl
)

# 设置链接标志 - Ubuntu 22.04 特定
set_target_properties(mod_audio_fork PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "/usr/lib/freeswitch/mod"
)

# 为 FreeSWITCH 模块正确设置链接器标志
target_link_options(mod_audio_fork PRIVATE
    -Wl,-no-undefined
    -Wl,-shared
)

# 安装目标 - Ubuntu 22.04 路径
install(TARGETS mod_audio_fork
    LIBRARY DESTINATION lib/freeswitch/mod
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

# 打印配置信息
message(STATUS "=== mod_audio_fork 配置 ===")
message(STATUS "FreeSWITCH 头文件目录: ${FREESWITCH_INCLUDE_DIR}")
message(STATUS "FreeSWITCH 库: ${FREESWITCH_LIBRARY}")
message(STATUS "libwebsockets: ${LIBWEBSOCKETS_LIBRARIES}")
message(STATUS "speex: ${SPEEX_LIBRARIES}")
message(STATUS "speexdsp: ${SPEEXDSP_LIBRARY}")
message(STATUS "boost: ${Boost_LIBRARIES}")
message(STATUS "boost 头文件目录: ${BOOST_INCLUDE_DIR}")
message(STATUS "构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ 标准: ${CMAKE_CXX_STANDARD}")
message(STATUS "C 标准: ${CMAKE_C_STANDARD}")
