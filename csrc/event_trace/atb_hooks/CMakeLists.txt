add_library(atb_abi_0_hook SHARED atb_hooks.cpp)

target_sources(atb_abi_0_hook PRIVATE
  atb_hooks.cpp
  enum2string.cpp
)

target_compile_definitions(atb_abi_0_hook PRIVATE -D_GLIBCXX_USE_CXX11_ABI=0)

target_include_directories(atb_abi_0_hook PUBLIC
  ${FRAMEWORK_INC_DIR}
  ${EVENT_TRACE_INC_DIR}
  ${UTILITY_INC_DIR}
  ${SECUREC_INC_DIR}
  ${ANALYSIS_DIR}
  ${JSON_DIR}
)

target_link_libraries(atb_abi_0_hook ascend_leaks dl)


add_library(atb_abi_1_hook SHARED atb_hooks.cpp)

target_compile_options(atb_abi_1_hook PRIVATE
  -U_GLIBCXX_USE_CXX11_ABI
  -D_GLIBCXX_USE_CXX11_ABI=1
)

target_sources(atb_abi_1_hook PRIVATE
  atb_hooks.cpp
  enum2string.cpp
)

target_include_directories(atb_abi_1_hook PUBLIC
  ${FRAMEWORK_INC_DIR}
  ${EVENT_TRACE_INC_DIR}
  ${UTILITY_INC_DIR}
  ${SECUREC_INC_DIR}
  ${ANALYSIS_DIR}
  ${JSON_DIR}
)

target_link_libraries(atb_abi_1_hook ascend_leaks dl)