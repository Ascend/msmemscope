find_package(Python3 REQUIRED COMPONENTS Development)
if (NOT Python3_FOUND)
    message(FATAL_ERROR "Python3 not found.")
else()
    message(STATUS "Found Python3: ${Python3_INCLUDE_DIRS}")
endif()

add_executable(memscope_test)
add_subdirectory(framework)
add_subdirectory(utility)
add_subdirectory(analysis)
add_subdirectory(event_trace)

set(memscope_header ../csrc
                 ../csrc/analysis
                 ../csrc/framework
                 ../csrc/event_trace
                 ../csrc/utility
                 ../platform/securec/include)
set(CMAKE_CXX_FLAGS "-fprofile-arcs -ftest-coverage -lgcov")
set(gtest_header ../opensource/googletest/googletest/include/)
set(json_header ../opensource/json/single_include)

file(GLOB_RECURSE TEST_ORIGINAL_SOURCE ../csrc/framework/*.cpp
                                       ../csrc/analysis/*.cpp
                                       ../csrc/event_trace/*.cpp
                                       ../csrc/utility/*.cpp
                                       ../platform/securec/src/*.c)
list(FILTER TEST_ORIGINAL_SOURCE EXCLUDE REGEX ".*\/atb_hooks\/.*")
# 测试工程红线:钩子源文件禁止glob进测试二进制——否则钩子的malloc族
# 符号在测试进程自身内生效,gtest的每次分配都过钩子、real_malloc解析指向测试二进制自身。
# 钩子so测试(test/event_trace/test_host_mem_hook.cpp)经独立子进程LD_PRELOAD驱动
list(FILTER TEST_ORIGINAL_SOURCE EXCLUDE REGEX ".*\/host_mem_hooks\/.*")

list(SORT TEST_ORIGINAL_SOURCE)
file(GLOB_RECURSE TEST_SOURCE ./test.cpp)
list(SORT TEST_SOURCE)
target_sources(memscope_test PRIVATE ${TEST_SOURCE})
target_sources(memscope_test PRIVATE ${TEST_ORIGINAL_SOURCE})

include_directories(memscope_test ${memscope_header} ${gtest_header} ${Python3_INCLUDE_DIRS} ${json_header})

target_link_libraries(memscope_test gtest_main dl pthread stdc++fs ${Python3_LIBRARIES} rt sqlite3)
 
add_test(NAME memscope_test COMMAND memscope_test)