add_library(leaks_ascend_hal_hook SHARED hal_hooks.cpp)

target_include_directories(leaks_ascend_hal_hook PUBLIC
  ${FRAMEWORK_INC_DIR}
  ${EVENT_TRACE_INC_DIR}
  ${UTILITY_INC_DIR}
  ${SECUREC_INC_DIR}
  ${Python3_INCLUDE_DIRS}
  ${ANALYSIS_DIR}
  ${JSON_DIR}
  ${SQLITE3_DIR}
)
target_link_options(leaks_ascend_hal_hook PRIVATE "-Wl,--disable-new-dtags")
target_link_libraries(leaks_ascend_hal_hook stdc++fs c_sec)