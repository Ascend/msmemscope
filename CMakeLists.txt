cmake_minimum_required(VERSION 3.16)

project(msmemscope VERSION 26.0.0)

set(CMAKE_VERBOSE_MAKEFILE ON)

# 导出编译数据库，供 clangd 语义索引（build/compile_commands.json）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 默认 Release，避免影响既有 build.py 构建。
# 注意：不能用 FORCE（否则 -DCMAKE_BUILD_TYPE=Debug 会被缓存覆盖回 Release），
# 非 FORCE 的 cache set 不会覆盖命令行传入的构建类型。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()

# 安全规范编译命令（Release/Debug 双模式均保留，勿删）
add_compile_options("-fPIE")
add_compile_options("-fPIC")
add_compile_options("-fstack-protector-all")
add_compile_options("-Wall")
add_compile_options("-D_FORTIFY_SOURCE=2")
add_compile_options("-fno-strict-aliasing")
# 编译优化与调试符号：Debug 时 -O0 -g（不叠加 -O2，不 strip）；BUILD_TESTS=ON 时保持 -O0（lcov 覆盖率）
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options("-O0")
  add_compile_options("-g")
elseif(BUILD_TESTS)
  add_compile_options("-O0") #内联扩展导致lcov覆盖率扫描不到，因此test工程不进行编译优化
else()
  add_compile_options("-O2")
endif()
add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
add_link_options("-Wl,-z,now")
add_link_options("-Wl,-z,relro")
add_link_options("-Wl,-z,noexecstack")
# Debug 构建保留调试符号，不 strip；Release 保持现状
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_link_options("-s") #strip
endif()
set(CMAKE_SKIP_RPATH TRUE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

option(BUILD_TESTS "build unit tests" OFF)

get_filename_component(ROOT_DIR ./ ABSOLUTE)

# 设置git commit ID
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  OUTPUT_VARIABLE MSLEAKS_COMMIT_ID OUTPUT_STRIP_TRAILING_WHITESPACE)
add_definitions(-D__MSLEAKS_COMMIT_ID__="${MSLEAKS_COMMIT_ID}")

# 设置版本号，优先从环境变量BUILD_VERSION读取，默认使用项目版本
if(DEFINED ENV{BUILD_VERSION})
  set(BUILD_VERSION "$ENV{BUILD_VERSION}")
else()
  set(BUILD_VERSION "${PROJECT_VERSION}")
endif()
add_definitions(-D__BUILD_VERSION__="${BUILD_VERSION}")

add_subdirectory(opensource)
add_subdirectory(platform)
add_subdirectory(csrc)

if(BUILD_TESTS)
  message(STATUS "enable unit tests build")
  enable_testing()
  add_subdirectory(test)
endif()
