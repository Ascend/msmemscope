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

# HostMem钩子子进程专用二进制(测试工程红线的延伸):主测试二进制
# 内嵌全部csrc源码与test_kernel_hooks.cpp的dlopen/dlsym桩(内核钩子用例靠桩劫持符号解析,
# 可执行文件符号在全局查找序中先于libc,故桩对子进程同样生效);钩子so的子进程套件要求
# 无桩干净进程——桩劫持钩子so的dlsym(RTLD_NEXT)真函数解析使其永久失败,进程分配全部
# 落入有限自举竞技场,耗尽后bad_alloc→terminate→abort。子套件经本专用二进制运行,
# 与主测试二进制的符号污染隔离;构建依赖保证父用例运行前子二进制已就绪
add_executable(memscope_hostmem_child_test ./event_trace/test_host_mem_hook.cpp)
target_link_libraries(memscope_hostmem_child_test gtest_main dl pthread)
# P3变体行接收符号msmemscope_hostmem_report_stack_variant由本二进制定义:钩子闭窗经
# dlsym(RTLD_DEFAULT)解析,-rdynamic将可执行文件符号导出进动态符号表——查找序中
# 主可执行文件先于LD_PRELOAD对象与其DT_NEEDED(libascend_leaks.so,新版本自带同名
# 生产包装),测试记录器必然被选中。导出范围=全部dynamic符号,子二进制无敏感符号
set_target_properties(memscope_hostmem_child_test PROPERTIES LINK_FLAGS "-rdynamic")
add_dependencies(memscope_test memscope_hostmem_child_test)

add_test(NAME memscope_test COMMAND memscope_test)