# Agent translation units. The host, the chat plugin, the CLI, and Android
# compile these. kbot stays the HTTP client. Call pm_llm_sources(<group> <out>)
# and append the result to the target that already owns that group.
# Paths are absolute so a caller outside the repo root resolves the same files.

set(PM_LLM_SRC "${CMAKE_CURRENT_LIST_DIR}/../src/llm")

function(pm_llm_sources group out_var)
  set(_s "${PM_LLM_SRC}")
  if(group STREQUAL "runner-mcp")
    set(_files "${_s}/agent_runner_mcp.cpp")
  elseif(group STREQUAL "appfinder")
    set(_files
      "${_s}/tools/info/AppFinder.cpp"
      "${_s}/tools/info/AppFinder_Win.cpp")
  elseif(group STREQUAL "media-tools")
    set(_files
      "${_s}/agent_tools.cpp"
      "${_s}/provider_tools.cpp"
      "${_s}/agent_retrieval.cpp"
      "${_s}/llm_image_tool_defaults.cpp"
      "${_s}/tools/commons/PathToolCommons.cpp"
      "${_s}/tools/pictures/PictureTools.cpp"
      "${_s}/tools/run/RunTool.cpp"
      "${_s}/tools/run/run_capture.cpp"
      "${_s}/tools/run/RunValidators.cpp"
      "${_s}/tools/browser_use/Tool_BrowserUse.cpp"
      "${_s}/tools/browser_use/browser_use_compact.cpp"
      "${_s}/tools/browser_use/browser_use_inject.cpp"
      "${_s}/tools/browser_use/browser_use_page_model.cpp"
      "${_s}/tools/computer_use/Tool_ComputerUse.cpp")
  elseif(group STREQUAL "hf")
    set(_files
      "${_s}/huggingface/hf_catalog.cpp"
      "${_s}/huggingface/hf_ref.cpp"
      "${_s}/huggingface/hf_hub_client.cpp"
      "${_s}/huggingface/hf_gguf.cpp"
      "${_s}/huggingface/hf_gguf_header.cpp"
      "${_s}/huggingface/hf_quant.cpp"
      "${_s}/huggingface/hf_mmproj.cpp"
      "${_s}/huggingface/hf_vram.cpp"
      "${_s}/huggingface/hf_meta.cpp"
      "${_s}/huggingface/hf_sidecar.cpp"
      "${_s}/huggingface/hf_stream_download.cpp"
      "${_s}/huggingface/hf_manifest.cpp"
      "${_s}/huggingface/hf_download.cpp"
      "${_s}/huggingface/hf_jobs.cpp")
  elseif(group STREQUAL "hf-xet")
    set(_files
      "${_s}/huggingface/hf_xet_xorb.cpp"
      "${_s}/huggingface/hf_xet_download.cpp")
  elseif(group STREQUAL "hf-xet-stub")
    set(_files "${_s}/huggingface/hf_xet_download_stub.cpp")
  elseif(group STREQUAL "host")
    # change_store and loopback stay on the host image. The exe must not
    # import tanit-chat-plugin.dll to reach them.
    set(_files
      "${_s}/agent_json_storage.cpp"
      "${_s}/agent_launch.cpp"
      "${_s}/agent_turn_options.cpp"
      "${_s}/agent_skills.cpp"
      "${_s}/skills/skillshub.cpp"
      "${_s}/agent_memory.cpp"
      "${_s}/agent_memory_artifacts.cpp"
      "${_s}/agent_scheduler.cpp"
      "${_s}/chat_session_store.cpp"
      "${_s}/change_store.cpp"
      "${_s}/security_review_host.cpp"
      "${_s}/chat_host_events.cpp"
      "${_s}/tools/inquire/InquireTools.cpp"
      "${_s}/agent_workflow.cpp"
      "${_s}/loopback_agent_client.cpp"
      "${_s}/decisions.cpp"
      "${_s}/agent_each.cpp"
      "${_s}/agent_decide.cpp")
  elseif(group STREQUAL "chat")
    set(_files
      "${_s}/agent_child_launch.cpp"
      "${_s}/agent_commons.cpp"
      "${_s}/agent_context.cpp"
      "${_s}/agent_feature.cpp"
      "${_s}/agent_feature_BrowserUse.cpp"
      "${_s}/agent_feature_Workflow.cpp"
      "${_s}/agent_factory.cpp"
      "${_s}/agent_tool_provider.cpp"
      "${_s}/agent_tool_provider_http.cpp"
      "${_s}/agent_runner.cpp"
      "${_s}/agent_runner_context.cpp"
      "${_s}/agent_runner_log.cpp"
      "${_s}/agent_runner_session.cpp"
      "${_s}/agent_resources.cpp"
      "${_s}/agent.cpp"
      "${_s}/agent_memory_recall.cpp"
      "${_s}/realtime_agent_bridge.cpp"
      "${_s}/realtime_session_config.cpp"
      "${_s}/realtime_voice_agent.cpp"
      "${_s}/security_review.cpp"
      "${_s}/llm_info_compact.cpp"
      "${_s}/tool_catalog.cpp"
      "${_s}/tool_executor.cpp"
      "${_s}/path_tool_catalog.cpp"
      "${_s}/path_tool_executor.cpp"
      "${_s}/tools/fs/FsTools.cpp"
      "${_s}/tools/fs/file_compose.cpp"
      "${_s}/tools/fs/search_tools.cpp"
      "${_s}/tools/video/VideoTools.cpp"
      "${_s}/tools/audio/AudioTools.cpp"
      "${_s}/tools/audio/ReadTool.cpp"
      "${_s}/tools/memory/MemoryTools.cpp"
      "${_s}/tools/app_command/Tool_AppCommand.cpp"
      "${_s}/tools/info/InfoTool.cpp"
      "${_s}/tools/network/NetworkTools.cpp")
  elseif(group STREQUAL "codex")
    set(_files "${_s}/agent_runner_codex.cpp")
  elseif(group STREQUAL "claude")
    set(_files
      "${_s}/agent_runner_claude_code.cpp"
      "${_s}/agent_runner_claude_defaults.cpp")
  elseif(group STREQUAL "cursor")
    set(_files "${_s}/agent_runner_cursor.cpp")
  elseif(group STREQUAL "service")
    set(_files "${_s}/tools/service/ServiceTools.cpp")
  elseif(group STREQUAL "service-android")
    set(_files "${_s}/tools/service/ServiceTools_Android.cpp")
  elseif(group STREQUAL "scheduler")
    set(_files "${_s}/tools/scheduler/SchedulerTools.cpp")
  elseif(group STREQUAL "command-creation")
    set(_files "${_s}/tools/command_creation/CreateCommandTool.cpp")
  elseif(group STREQUAL "sequencer")
    set(_files "${_s}/tools/sequencer/SequencerTool.cpp")
  elseif(group STREQUAL "cancel-hook")
    set(_files "${_s}/liboai_cancel_hook.cpp")
  elseif(group STREQUAL "llama-clients")
    set(_files
      "${_s}/llama_agent_client.cpp"
      "${_s}/llama_vlm_client.cpp"
      "${_s}/ocr_grounding.cpp")
  elseif(group STREQUAL "compression-reducer")
    set(_files "${_s}/compressors/compress_reducer.cpp")
  elseif(group STREQUAL "compression-host")
    set(_files
      "${_s}/compressors/structure_mask.cpp"
      "${_s}/compressors/content_detector.cpp"
      "${_s}/compressors/json_handler.cpp"
      "${_s}/compressors/text_handler.cpp"
      "${_s}/compressors/universal_compressor.cpp"
      "${_s}/reduce/reduce.cpp")
  elseif(group STREQUAL "mcp")
    set(_files
      "${_s}/mcp_stream_client.cpp"
      "${_s}/mcp_stdio_client.cpp"
      "${_s}/mcp_config.cpp"
      "${_s}/mcp_tools_cache.cpp"
      "${_s}/mcp_probe.cpp")
  elseif(group STREQUAL "mcp-bridge")
    set(_files "${_s}/tools/mcp/McpBridge.cpp")
  elseif(group STREQUAL "speak")
    set(_files "${_s}/active_speak_output.cpp")
  elseif(group STREQUAL "loopback")
    set(_files "${_s}/loopback_agent_client.cpp")
  elseif(group STREQUAL "cli")
    # server_options_cli.cpp is not here. pm_add_llm_tool already compiles it
    # into the pm-llm executable.
    set(_files
      "${_s}/cli/agent_cli_util.cpp"
      "${_s}/cli/agent_cli_voice.cpp"
      "${_s}/cli/agent_cli_batch.cpp"
      "${_s}/cli/agent_cli_run.cpp"
      "${_s}/cli/agent_cli_register.cpp"
      "${_s}/agent_openai_server.cpp")
  elseif(group STREQUAL "cli-server-options")
    set(_files "${_s}/server_options_cli.cpp")
  elseif(group STREQUAL "run")
    set(_files
      "${_s}/tools/run/RunTool.cpp"
      "${_s}/tools/run/run_capture.cpp"
      "${_s}/tools/run/RunValidators.cpp")
  elseif(group STREQUAL "chrome")
    set(_files
      "${_s}/tools/browser_use/browser_use_compact.cpp"
      "${_s}/tools/browser_use/browser_use_inject.cpp"
      "${_s}/tools/browser_use/browser_use_page_model.cpp"
      "${_s}/agent_retrieval.cpp")
  elseif(group STREQUAL "security")
    set(_files
      "${_s}/provider_tools.cpp"
      "${_s}/agent_tools.cpp"
      "${_s}/agent_json_storage.cpp"
      "${_s}/mcp_tools_cache.cpp"
      "${_s}/chat_session_store.cpp"
      "${_s}/agent_memory.cpp"
      "${_s}/agent_memory_artifacts.cpp")
  elseif(group STREQUAL "android")
    set(_files
      "${_s}/agent.cpp"
      "${_s}/agent_launch.cpp"
      "${_s}/agent_factory.cpp"
      "${_s}/agent_feature.cpp"
      "${_s}/agent_feature_Workflow.cpp"
      "${_s}/agent_commons.cpp"
      "${_s}/agent_context.cpp"
      "${_s}/agent_json_storage.cpp"
      "${_s}/agent_runner_context.cpp"
      "${_s}/agent_runner.cpp"
      "${_s}/agent_runner_session.cpp"
      "${_s}/agent_runner_log.cpp"
      "${_s}/agent_runner_mcp.cpp"
      "${_s}/realtime_agent_bridge.cpp"
      "${_s}/agent_memory.cpp"
      "${_s}/agent_scheduler.cpp"
      "${_s}/tools/scheduler/SchedulerTools.cpp"
      "${_s}/agent_memory_artifacts.cpp"
      "${_s}/agent_memory_recall.cpp"
      "${_s}/agent_retrieval.cpp"
      "${_s}/agent_tool_provider.cpp"
      "${_s}/chat_session_store.cpp"
      "${_s}/loopback_agent_client.cpp"
      "${_s}/llama_agent_client.cpp"
      "${_s}/agent_child_launch.cpp"
      "${_s}/agent_tools.cpp"
      "${_s}/change_store.cpp"
      "${_s}/llm_image_tool_defaults.cpp"
      "${_s}/llama_vlm_client.cpp"
      "${_s}/path_tool_catalog.cpp"
      "${_s}/path_tool_executor.cpp"
      "${_s}/tools/commons/PathToolCommons.cpp"
      "${_s}/tools/pictures/PictureTools.cpp"
      "${_s}/tools/fs/FsTools.cpp"
      "${_s}/tools/fs/file_compose.cpp"
      "${_s}/tools/memory/MemoryTools.cpp"
      "${_s}/tools/video/VideoTools.cpp"
      "${_s}/tools/audio/AudioTools.cpp"
      "${_s}/tools/audio/ReadTool.cpp"
      "${_s}/tools/run/RunTool.cpp"
      "${_s}/tools/run/run_capture.cpp"
      "${_s}/tools/run/RunValidators.cpp"
      "${_s}/tools/computer_use/Tool_ComputerUse.cpp"
      "${_s}/tools/browser_use/Tool_BrowserUse.cpp"
      "${_s}/tools/app_command/Tool_AppCommand.cpp"
      "${_s}/tools/mcp/McpBridge.cpp"
      "${_s}/tools/network/NetworkTools.cpp"
      "${_s}/tools/service/ServiceTools.cpp"
      "${_s}/tools/service/ServiceTools_Android.cpp"
      "${_s}/mcp_config.cpp"
      "${_s}/mcp_probe.cpp"
      "${_s}/mcp_tools_cache.cpp"
      "${_s}/mcp_stdio_client.cpp"
      "${_s}/mcp_stream_client.cpp")
  else()
    message(FATAL_ERROR "pm_llm_sources: unknown group '${group}'")
  endif()
  set(${out_var} ${_files} PARENT_SCOPE)
endfunction()
