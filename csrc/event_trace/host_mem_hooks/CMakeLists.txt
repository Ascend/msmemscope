# host堆内存钩子so:LD_PRELOAD劫持malloc族,经纯C ABI bind桥接
# 与libascend_leaks交互(不解析任何C++符号)
add_library(msmemscope_host_mem_hook SHARED host_mem_hooks.cpp py_stack_capture.cpp)

# Python头(py栈采集): py_stack_capture.cpp include <Python.h>/<frameobject.h>,
# 弱符号解析到宿主解释器,本so不链接libpython
find_package(Python3 REQUIRED COMPONENTS Development)

target_include_directories(msmemscope_host_mem_hook PUBLIC
  ${FRAMEWORK_INC_DIR}
  ${EVENT_TRACE_INC_DIR}
  ${SECUREC_INC_DIR}
  ${Python3_INCLUDE_DIRS}
)

# 帧指针快速走栈(决策⑩)前提: 本so全部函数保留帧指针,fp链自劫持入口无断点直达
# 宿主调用链(锚点帧=malloc劫持入口,参与去重键与报告)。宿主代码不受控:
# omit-fp帧对链透明(寄存器透传),毁链代码由校验层截断并回退backtrace
# (见WalkFramePointers)
target_compile_options(msmemscope_host_mem_hook PRIVATE "-fno-omit-frame-pointer")

# DT_NEEDED=libascend_leaks:CLI模式下钩子so经LD_PRELOAD进入目标进程时,依
# LD_LIBRARY_PATH(指向output/lib64)解析并加载libascend_leaks。钩子不引用
# libascend_leaks的任何符号(bind经dlsym解析),--no-as-needed防止链接器因无符号引用而
# 丢弃DT_NEEDED条目。安全规范禁止RPATH/RUNPATH(检查ELF dynamic section的
# RPATH/RUNPATH tag),故此处不设置rpath,依赖改由LD_LIBRARY_PATH解析。
target_link_options(msmemscope_host_mem_hook PRIVATE
  "-Wl,--no-as-needed"
)
target_link_libraries(msmemscope_host_mem_hook ascend_leaks dl pthread)
