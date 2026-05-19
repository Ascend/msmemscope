file(GLOB_RECURSE SOURCE "*.cpp")
list(SORT SOURCE)

target_sources(ascend_leaks PRIVATE ${SOURCE})
target_include_directories(ascend_leaks PUBLIC
  ${UTILITY_INC_DIR}
  ${ANALYSIS_DIR}
  ${JSON_DIR}
)
