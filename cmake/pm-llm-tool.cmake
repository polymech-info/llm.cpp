# pm-llm executable. The package owns the target. The host may add CLI
# adapters with target_sources after this function returns.
function(pm_add_llm_tool)
  if(TARGET pm-llm OR NOT TARGET pm-commons-lib OR NOT TARGET CLI11::CLI11)
    return()
  endif()
  add_executable(pm-llm
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../cli/main.cpp"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/llm/server_options_cli.cpp")
  target_compile_features(pm-llm PRIVATE cxx_std_20)
  target_include_directories(pm-llm PRIVATE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/core"
    "${CMAKE_BINARY_DIR}/include")
  target_link_libraries(pm-llm PRIVATE pm-commons-lib CLI11::CLI11)
  if(WIN32)
    target_link_libraries(pm-llm PRIVATE shell32 ole32 crypt32 bcrypt ncrypt)
  endif()
  if(MSVC)
    target_compile_definitions(pm-llm PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(pm-llm PRIVATE /W3 /utf-8 /EHsc)
  endif()
  if(PM_DIST_DIR)
    set_target_properties(pm-llm PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PM_DIST_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PM_DIST_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PM_DIST_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PM_DIST_DIR}")
  endif()
endfunction()
