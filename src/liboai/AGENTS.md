# AGENTS.md

This repo is a maintained fork of liboai. Our goal is to make it more reliable and feature-complete without breaking existing api.

## Core Principles
- Preserve backward compatibility; add features without breaking existing APIs.
- Favor small, composable changes over rewrites.
- Keep the codebase clean and maintainable; document anything user-facing.
- Prioritize stability, correctness, and clear error handling.

## Current Priorities
- Add OpenAI Responses API support for GPT-5.2 and gpt-5.2-pro.
- Keep Chat Completions and other existing components intact.
- Add documentation and examples for new features.

## Workflow
- Update docs whenever you add or change public APIs.
- Use existing patterns and naming conventions in liboai.
- Avoid introducing new dependencies unless justified.

## Notes
- The initial Responses API implementation should accept raw JSON payloads.
- A ResponseInput helper is planned, but not part of the initial implementation.
- Azure Responses support is out of scope for now.
