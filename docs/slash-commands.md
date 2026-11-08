# Slash commands — typeahead (handoff)

Composer **`/`** typeahead. v1 searches **skills** only; later sources can share
the same trigger.

Related: [shared-skills.md](./shared-skills.md), [jcode.md](./jcode.md)
(jcode `/skill-name` gap), agentskills client guide (user-explicit activation).

---

## Goal

| Trigger | Role |
|---------|------|
| `@query` | File/folder typeahead → collect pills in prompt |
| `/query` | Slash typeahead — **v1: skills** (name + description); later other sources |
| Agent prompt | Stable catalog stays **pinned/always** `name: description` only; bodies via `info_lookup` |

User can pick **multiple** skills; each pick inserts a skill reference pill
(`@[skill:name](path/to/SKILL.md)`). Clicking the pill opens the skill file
(same as file pills). Pin-on-select / turn-local pin apply can follow.

---

## UX

- Trigger character is **`/`** (parallel to `@`), not a `/skill ` prefix.
- Query after `/` filters the current slash catalog client-side.
- v1 catalog = skills from enabled/configured roots (roaming / workspace).
- Later: same `/` trigger can mix other source kinds without a second trigger.
- Does **not** route slash search through `searchStart` / `rg`.

---

## Architecture

```
Chat composer "/"
  → chatSkillsList (ChatWebPanel_Agent.cpp)     // thin RPC only
  → media::llm::skills::* (agent_skills.cpp)    // policy + discover + list JSON
  → SearchExtraItem[] + filterExtraItems(name/description)
  → skill collect pill
```

### Retrieval: `agent_skills.cpp`

- `load_policy_from_settings()`
- `discover_skills(policy, cwd_hint)`
- `list_skills_data_json(cwd)` / `skills_list_data_json(snap)` / `skill_entry_json`
- Shared by `chatSkillsList` and `settingsSkillsGet`

### Chat RPC: `ChatWebPanel_Agent.cpp`

`chat_skills_list_rpc` → dispatched from `provider_rpc_dispatch` as
`method: "chatSkillsList"` with optional `cwd` (chat folder; else fallback work folder).

### Web

- Lexical: `extractAtTriggerInSegment` prefers nearest `@` or `/` (slash needs token boundary).
- `ChatComposerDock`: `/` → skill extras only; `@` → file scopes + MRU as before.

`InfoTool` / `resources::discover` remain the agent **rank + materialize** path
(including VFS). Slash v1 does not call them.

---

## Involved files (checklist)

### Skills core

- [x] `src/llm/agent_skills.hpp/.cpp` — policy load + list JSON
- [x] `CSettingsWebView.cpp` `settingsSkillsGet` — shared serializer

### Web (chat-next + shared)

- [x] `apps/shared/search/searchCollectDocument.ts` — `/` beside `@`
- [x] `apps/chat-next/src/pm-chat/ChatComposerDock.tsx` — `/` → skill extras
- [x] collect `kind: "skill"` pill via existing extra-item path
- [ ] Optional: pin-on-select / `pmChatStore` pin helpers
- [ ] Turn apply: parse skill pills → turn-local pins / `info_lookup`

### Host (C++)

- [x] `ChatWebPanel_Agent.cpp` — `chat_skills_list_rpc`
- [x] `ChatWebPanel_Provider.cpp` — dispatch `chatSkillsList`
- [ ] Prompt apply: `SkillsFeature` honor skill pills
- [x] Body load unchanged: `info_lookup` → `agent_resources.cpp`

---

## Out of scope (v1)

- Other slash sources beyond skills
- VFS skills in the slash list
- Full agentskills “catalog all skills in system prompt”
- jcode-style `/skill-name trailing prompt` without typeahead UI
- Routing slash search through `rg` or `resources::rank`
- Honoring skill `allowed-tools` (ignored by policy)
