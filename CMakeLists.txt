cmake_minimum_required(VERSION 3.16)
 
project(msmemscope VERSION 26.0.0)
 
set(CMAKE_VERBOSE_MAKEFILE ON)
 
# 安全规范编译命令
add_compile_options("-fPIE")
add_compile_options("-fPIC")
add_compile_options("-fstack-protector-all")
add_compile_options("-Wall")
add_compile_options("-D_FORTIFY_SOURCE=2")
add_compile_options("-fno-strict-aliasing")
if (${BUILD_TESTS})
  add_compile_options("-O0") #内联扩展导致lcov覆盖率扫描不到，因此test工程不进行编译优化
else()
  add_compile_options("-O2")
endif()
add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
add_link_options("-Wl,-z,now")
add_link_options("-Wl,-z,relro")
add_link_options("-Wl,-z,noexecstack")
add_link_options("-s") #strip
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

if (${BUILD_TESTS})
  message(STATUS "enable unit tests build")
  enable_testing()
  add_subdirectory(test)
endif()