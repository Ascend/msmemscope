add_library(ascend_mstx_hook SHARED mstx_manager.cpp mstx_inject.cpp aten_manager.cpp op_handler.cpp)

target_include_directories(ascend_mstx_hook PUBLIC
  ${FRAMEWORK_INC_DIR}
  ${EVENT_TRACE_INC_DIR}
  ${UTILITY_INC_DIR}
  ${SECUREC_INC_DIR}
  ${Python3_INCLUDE_DIRS}
  ${ANALYSIS_DIR}
  ${JSON_DIR}
  ${SQLITE3_DIR}
)

target_link_options(ascend_mstx_hook PRIVATE "-Wl,--disable-new-dtags")
target_link_libraries(ascend_mstx_hook ascend_leaks c_sec)
