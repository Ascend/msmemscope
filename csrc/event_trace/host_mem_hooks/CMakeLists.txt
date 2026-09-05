# host堆内存钩子so:LD_PRELOAD劫持malloc族,经纯C ABI bind桥接
# 与libascend_leaks交互(不解析任何C++符号)
add_library(msmemscope_host_mem_hook SHARED host_mem_hooks.cpp)

target_include_directories(msmemscope_host_mem_hook PUBLIC
  ${FRAMEWORK_INC_DIR}
  ${EVENT_TRACE_INC_DIR}
  ${SECUREC_INC_DIR}
)

# 帧指针快速走栈(决策⑩)前提: 本so全部函数保留帧指针,fp链自劫持入口无断点直达
# 宿主调用链(锚点帧=malloc劫持入口,参与去重键与报告)。宿主代码不受控:
# omit-fp帧对链透明(寄存器透传),毁链代码由校验层截断并回退backtrace
# (见WalkFramePointers)
target_compile_options(msmemscope_host_mem_hook PRIVATE "-fno-omit-frame-pointer")

# DT_NEEDED=libascend_leaks + $ORIGIN RPATH:CLI模式下钩子so经LD_PRELOAD进入目标
# 进程时,依同目录(output/lib64)解析并加载libascend_leaks。钩子不引用libascend_leaks的任何
# 符号(bind经dlsym解析),--no-as-needed防止链接器因无符号引用而丢弃DT_NEEDED条目;
# --disable-new-dtags生成DT_RPATH(RUNPATH对LD_PRELOAD场景下本so的依赖解析不生效)
target_link_options(msmemscope_host_mem_hook PRIVATE
  "-Wl,--disable-new-dtags"
  "-Wl,-rpath,$ORIGIN"
  "-Wl,--no-as-needed"
)
target_link_libraries(msmemscope_host_mem_hook ascend_leaks dl pthread)
