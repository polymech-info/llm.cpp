cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-llm
    VERSION 0.1.0
    DESCRIPTION "Tanit agent, kbot client, and liboai"
    LANGUAGES CXX)
endif()

# One static library for the OpenAI client and the kbot wrapper.
# Agent translation units stay on the host targets (pm-media / pm-llm-host /
# android / security) so the chat-plugin export split still holds.
# The file list is packages/llm.cpp/cmake/host-sources.cmake.
# The target keeps the name kbot: the host already links that.
add_library(kbot STATIC
  src/kbot/kbot.cpp
  src/kbot/llm_client.cpp
  src/kbot/llm_client_realtime.cpp
  src/kbot/source_files.cpp
  src/liboai/liboai/components/audio.cpp
  src/liboai/liboai/components/azure.cpp
  src/liboai/liboai/components/chat.cpp
  src/liboai/liboai/components/completions.cpp
  src/liboai/liboai/components/edits.cpp
  src/liboai/liboai/components/embeddings.cpp
  src/liboai/liboai/components/files.cpp
  src/liboai/liboai/components/fine_tunes.cpp
  src/liboai/liboai/components/images.cpp
  src/liboai/liboai/components/models.cpp
  src/liboai/liboai/components/moderations.cpp
  src/liboai/liboai/components/responses.cpp
  src/liboai/liboai/core/authorization.cpp
  src/liboai/liboai/core/netimpl.cpp
  src/liboai/liboai/core/response.cpp
)

target_compile_features(kbot PUBLIC cxx_std_20)
target_compile_definitions(kbot PRIVATE POLYMECH_STATIC_BUILD=1)
target_compile_definitions(kbot INTERFACE POLYMECH_STATIC_BUILD=1)

target_include_directories(kbot PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kbot"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/liboai/liboai/include"
)
if(taskflow_SOURCE_DIR)
  target_include_directories(kbot PUBLIC "${taskflow_SOURCE_DIR}")
endif()

target_link_libraries(kbot PUBLIC
  logger
  json
  nlohmann_json::nlohmann_json
  CURL::libcurl
  pranav_glob
)
