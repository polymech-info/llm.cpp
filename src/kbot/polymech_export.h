#pragma once

/**
 * DLL / shared-object exports for the Polymech kbot library (pipelines, LLM client).
 *
 * CMake:
 *   - Building libkbot: POLYMECH_BUILDING_LIBRARY (PRIVATE)
 *   - Linking static kbot: POLYMECH_STATIC_BUILD=1 (INTERFACE)
 *   - Linking shared kbot: default import on Windows
 */

#if defined(POLYMECH_STATIC_BUILD)
#  define POLYMECH_API
#elif defined(_WIN32)
#  if defined(POLYMECH_BUILDING_LIBRARY)
#    define POLYMECH_API __declspec(dllexport)
#  else
#    define POLYMECH_API __declspec(dllimport)
#  endif
#else
#  if defined(POLYMECH_BUILDING_LIBRARY)
#    define POLYMECH_API __attribute__((visibility("default")))
#  else
#    define POLYMECH_API
#  endif
#endif
