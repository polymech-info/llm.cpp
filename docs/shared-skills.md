# Shared skills from session memory

Investigation only. Next steps for **cheap** and **LLM-driven** skill creation
from session memory, for local install and later network / swarm share via VFS.

Related:

- [agent-memory-next.md](./agent-memory-next.md) — hybrid recall, session ground truth, skill resource foundation
- [jcode.md](./jcode.md) — skills as retrievable capabilities; `skill_manage` is load/list/reload/read (no create)
- [skill-creator SKILL.md](../../ref/skills/skills/skill-creator/SKILL.md) — human-in-loop draft → eval → package (heavy reference, not v1)
- Harness: `tests/orchestrator/test-agent-memory.mjs` (`skill-resources` suite)

---

## Verdict

PixlWiz already has the **consume** path for shared skills (local roaming /
workspace + trusted VFS manifests → compact catalog → `info_lookup`). It does
**not** yet have a **produce** path that turns a successful session into a
shareable skill package.

**Start via `llm agent` CLI** (`pm_image_cmd_llm_agent.cpp`) — not a new C++
subsystem. Session hydrate, tools (`memory_*`, `write_file`, `info_lookup`),
`--skills`, and the memory harness already go through that entry. Prove
session → draft package there, then extract cheap helpers if needed.

Keep network publish and full skill-creator eval loops out of v1.

---

## What already exists (reuse, do not rebuild)

### Session memory as source material

| Artifact | Where | Useful for skill draft? |
|----------|-------|-------------------------|
| `turn_pair` items | `agent_memory_artifacts.cpp` — `metadata.user/assistant/tools/tool_calls/context_file_paths` | **Primary** — workflow skeleton (prompt + tool sequence) |
| Write artifacts | `locator.path` + summary + origin tool | Examples / I/O shapes |
| Session rules (`instructions`) | synthesized + `memory_write` prefs | Constraints / “when to use” hints |
| `default_memory.doc` | global prefs | Cross-session habits (later) |
| Hybrid recall | `agent_memory_recall.cpp` — BM25 + RRF + pending inject | Find similar past workflows before drafting; **not** the writer |

Ground truth stays session JSON / sidecars. Skill packages are a **derived
export**, same principle as recall: retrieval/export layer, not a second store
of file bodies in memory.

### Skills consume path (shipped)

| Layer | Behavior |
|-------|----------|
| Discover | Roaming `{config}/skills`, workspace overlay, `skill.json` / `resource.json` / `SKILL.md` |
| Catalog | `SkillsFeature` — pinned/always `name: description` only; others via `info_lookup` only |
| Rank / load | `info_lookup(kind=skill\|resource)` via shared BM25/dense/RRF; exact URI materializes capped body |
| VFS | Settings `agent_skills.vfs_roots` → manifest metadata; bodies fetched on demand; **untrusted roots stay unavailable** |
| Policy | Workspace > roaming; pinned/disabled/always; OS/bin/env gates; `allowed-tools` / `declared_tools` ignored — `request_tools` owns grants |

Evidence: `agent_skills.*`, `agent_resources.*`, harness `skill-resources` (local + live VFS stage via `service files pull`).

### What is intentionally absent

- No agent tool that **creates** or **publishes** skills (jcode `skill_manage` also does not create).
- Skill bodies are **not** stored in session/prompt memory (by design).
- No VFS **push** / signed-manifest authoring for user-created skills.
- Full skill-creator loop (subagent baselines, eval viewer, description optimizer) is too heavy for agent runtime v1.

---

## Two creation modes

```
Session memory (turn_pairs, artifacts, rules)
        │
        ├─ Cheap extract (no LLM) ──► SkillDraft JSON
        │                                  │
        └─ LLM draft (gated) ──────────────┤
                                           ▼
                              Local package under drafts/ or skills/
                              (disabled / untrusted by default)
                                           │
                                           ├─ User enable → roaming/workspace discover
                                           └─ Offline manifest row → VFS share later
```

### A. Cheap (deterministic, preferred first)

**Input:** one session id (or N recent `turn_pair` items).  
**Output:** `SkillDraft` JSON — no model call.

Suggested fields (versioned, harness-friendly):

```json
{
  "schema": "tanit.skill_draft/v1",
  "id": "meal-plan-from-session",
  "title": "…",
  "description": "…",
  "trigger_hints": ["…"],
  "tool_sequence": ["info_lookup", "write_file"],
  "declared_tools": ["write_file"],
  "example_prompts": ["…"],
  "artifact_paths": ["…"],
  "source": {
    "session_id": "…",
    "turn_pair_ids": ["…"],
    "extracted_at": "…"
  },
  "trust": "session-derived",
  "enabled": false
}
```

### Cheap importance vs noise (no LLM)

Do **not** invent a second ranking brain. Reuse the filters session memory
already applies for prompt inject and `turn_pair` capture, then score with a
small deterministic rubric. Every rule below should be unit-testable on fixture
JSON.

#### Layer 0 — drop before scoring (hard noise)

| Drop | Why | Existing precedent |
|------|-----|--------------------|
| Items with `kind=self_check` | Bookkeeping | `agent_context` / recall overlap skip |
| `role=security_review` / `security_decision` | Policy audit, not workflow | same |
| `kind=error_reflexion` (unless user retries and succeeds) | Failure noise | keep only if a later success turn shares tools |
| Tools: `memory_*`, `request_tools`, scheduler, `speak`, consent helpers | Utility / meta | turn_pair `else continue` (“Skip utility…”) |
| `info_lookup` except when it is the *workflow* step (skill/app resolve followed by act) | Discovery chatter | turn_pair only details `info_lookup kind=app` today |
| Turns with 0 “workflow” tool calls | Chat-only | no draft |

**Workflow tool allowlist** (start from turn_pair detail allowlist + producers):

- Producers: `write_file`, `image_*`, `create_video`, `app_screenshot`, …
- Actors: `run`, `run_sequence`, `file_read`, `file_glob`
- Optional: `info_lookup` **only** if followed in the same turn by a producer/actor

Anything else is noise for skill skeleton purposes (may still appear in
`metadata.tools` list — ignore for `tool_sequence`).

#### Layer 1 — turn strength score (0–N, integer)

Score each `turn_pair` (and linked artifacts). Suggest defaults:

| Signal | Points | Notes |
|--------|--------|-------|
| ≥1 producer tool succeeded | +2 | path/locator artifact exists |
| ≥2 distinct workflow tools | +2 | sequence, not single poke |
| Non-empty user prompt (≥12 chars) | +1 | skip empty / “ok” / “thanks” |
| Non-empty final assistant | +1 | completed turn |
| User correction pattern in next turn (“no, …”, “actually …”, redo same tools) | +2 on the *successful* turn | corrections are gold for skill body |
| Same normalized tool sequence seen ≥2× in session | +3 | reinforce → `candidate_strength=high` |
| Artifact `use_count` ≥ 2 or recalled via hybrid path | +1 | reused output |
| Only `file_glob` / only `file_read` | −2 | explore-only |
| Only failed tools / cancelled | −3 | discard |

**Gate:** emit a draft only if best turn (or merged cluster) score ≥ **5**.
Below that: optional `skill_draft_candidate` telemetry with `strength=low`, no
package write.

#### Layer 2 — what goes into the draft (important fields)

| Keep | Source | Cap |
|------|--------|-----|
| `example_prompts` | user text of high-score turns | 1–3, clamp ~200 chars |
| `tool_sequence` | ordered workflow tools, collapse consecutive dupes | ≤8 steps |
| `declared_tools` | producers/actors only (hints) | no `memory_*` |
| `artifact_paths` | locator.path from write artifacts in those turns | ≤5 |
| `trigger_hints` | tokens from user prompts + tool names (lexical) | ≤12 tokens |
| Session **hard** rules only | `instructions` with `priority=hard` | skip preference/hint unless reinforced |

**Do not keep:** full assistant essays, tool arg bodies, MCP dumps, self-check
JSON, recalled-memory inject text, raw transcripts.

#### Layer 3 — cluster before draft

1. Normalize sequence: lowercase tool names, drop utilities, collapse runs of
   the same tool (`write_file,write_file` → `write_file`).
2. Group turns with identical normalized sequence.
3. Pick the cluster with highest total score; draft `id` slug from first user
   prompt tokens + sequence fingerprint (stable for harness).
4. If two clusters both ≥5, emit **two drafts** (or one draft + `alternates[]`)
   — never merge unrelated workflows.

#### Layer 4 — human / CLI gate (still non-LLM)

Cheap extract only proposes. Guidance to the operator (and to C1 harness):

- Print score + dropped reasons (`dropped: self_check, memory_find, score=3`).
- Write package only on explicit prompt / `--save-skill-draft` / score≥5.
- Package stays `enabled: false` until user enable.

This is the non-LLM answer to “important vs noise”: **allowlist → score →
cluster → gate**, all fixture-driven. LLM fill (C3) may rewrite prose later but
must not reintroduce dropped classes.

### B. LLM-driven (thin, skill-creator inspired)

Use only after cheap extract exists.

| skill-creator step | PixlWiz v1 mapping |
|--------------------|--------------------|
| Capture intent from conversation | Cheap draft + session items as context |
| Write SKILL.md | One gated agent turn / offline script: fill frontmatter + short body from draft |
| Eval / baseline / viewer | **Out of runtime** — optional later harness or human review |
| Description optimizer | Defer; description comes from user prompt + tool tokens |
| Package `.skill` | Defer; directory package is enough for discover + VFS row |

Do **not** embed the full skill-creator skill into the default agent. Prefer a
small offline helper or an explicit user-invoked prompt that reads the draft
JSON and writes `SKILL.md` under a **drafts** root.

---

## Preferred first surface: `llm agent` CLI

`pm_image_cmd_llm_agent.cpp` already wires everything a v0 produce path needs:

| Piece | CLI / Turn |
|-------|------------|
| Continuity | `--session-id` + multi-turn hydrate (`session_load` → `turn.memory_state`) |
| Memory tools | `memory_read` / `memory_find` / `memory_write` (session + past chats) |
| Write package | `write_file` into `{config}/skills-drafts/<id>/SKILL.md` |
| Consume check | `--skills` + follow-up `info_lookup` after enable/copy |
| Feature mask | `--memory-features` / `PM_LLM_STRUCTURED_MEMORY_FEATURES` |
| Harness | `runAgentRound` in `test-agent-commons.mjs` already spawns `llm agent` |

### v0 flow (no new flags required)

```text
# 1) Build a real session (or reuse harness seed)
llm agent --session-id S --skills --prompt "…do a multi-step workflow…"

# 2) Same session: ask the model to export a draft skill (LLM path)
llm agent --session-id S --skills --single-turn --prompt \
  "From this session's turn_pairs/artifacts, write a disabled draft skill to
   ${CONFIG_DIR}/skills-drafts/<slug>/SKILL.md (YAML frontmatter name+description,
   short imperative body, declared tools as hints only). Do not enable it."

# 3) Optional later: enable into roaming and verify via info_lookup
```

### Optional thin CLI sugar (only after v0 works)

| Flag / mode | Purpose | Risk |
|-------------|---------|------|
| `--save-skill-draft[=slug]` | After a successful turn, run a fixed system/user prompt (or cheap extract → write) into `skills-drafts/` | Low — opt-in, disabled package |
| `--skill-draft-dir=` | Override drafts root for harness | Low |
| Subcommand `llm skill draft --session-id S` | Same as above without overloading agent prompt | Medium — more CLI surface |

Defer flags until a harness live/loopback case proves the prompt+`write_file` path.

### Why CLI-first beats pure C++ extractor first

1. **Testable tomorrow** — extend `test-agent-memory.mjs` with a `skill-draft-cli` suite; loopback can stub `write_file` / assert path.
2. **No new tools / feature bits** — reuses session inject + existing FS tools.
3. **LLM path matches skill-creator “capture from conversation”** without importing the full eval loop.
4. Cheap deterministic extract (T1) can still land **under** the CLI later as `--save-skill-draft` implementation detail, or as a no-LLM fallback when loopback/offline.

---

## Local vs VFS / swarm

| Target | v1 | Notes |
|--------|----|-------|
| **Local drafts** | yes | e.g. `{config}/skills-drafts/<id>/` or `skills/<id>/` with `disabled: true` / frontmatter flag |
| **Local enable** | yes | Move/copy into roaming or workspace; existing discover picks it up |
| **VFS consume** | already | Manifest + trusted flag; live harness stages via pull |
| **VFS publish** | later | Offline emit one manifest row + body path; human/CMS push; signed trust remains gate |
| **Swarm auto-sync** | out of scope | Needs identity, signing, conflict policy — document only |

VFS row shape already understood by `append_vfs_resources`:

- `id`, `description`, `body`/`path`, optional `declared_tools`, `sha256`/`content_hash`
- Trust: `trusted` / `signature_verified` on the root — session-derived skills must **not** default to trusted

Branded URI remains `tanit://skill/<scope>/<id>` (local) or `tanit://skill/vfs/<mount>/<id>` (remote). Drafts should use a distinct scope (e.g. `draft` / `session`) so they never collide with vendor mounts.

---

## Risk fence (keep v1 safe)

| Risk | Mitigation |
|------|------------|
| Junk skills pollute catalog | Drafts disabled; not in prompt until enabled |
| Prompt injection via “skill from chat” | Sanitize like `materialize`; cap body; no secrets from tool args |
| Declared tools escalate privileges | Keep hints-only; never auto-grant schemas |
| Accidental VFS publish | Export manifest file only; no network write in v1 |
| Overfitting one session | Cheap draft keeps example prompts short; LLM pass must generalize (skill-creator principle) |
| Duplicating skill-creator complexity | No eval viewer / subagent baselines in agent runtime |

---

## Next todos (low risk, testable)

Ordered for small diffs. **CLI agent first**; pure extractors after the path works.

### P0 — CLI produce path (`pm_image_cmd_llm_agent.cpp` / harness)

- [ ] **C0 — Draft root convention**  
      Pick `{config}/skills-drafts/<id>/` (recommended) vs disabled-in-`skills/`.  
      Document in this file; no discover until copied/enabled into roaming/workspace.

- [ ] **C1 — Harness `skill-draft-cli` (loopback)**  
      Round A: seed session with a known multi-tool workflow (existing memory fixtures).  
      Round B: same `--session-id`, prompt “write draft SKILL.md under skills-drafts/…”, loopback script calls `write_file` with a sentinel body.  
      Assert: file exists under config dir; frontmatter parseable; **not** in skill catalog until enable.

- [ ] **C2 — Harness enable + consume**  
      Copy/enable draft into temp roaming `skills/` → `--skills` round → catalog URI + `info_lookup` body (mirror `skill-resources`).  
      Assert: consume path unchanged for non-draft skills.

- [ ] **C3 — Live optional**  
      Same as C1 with real model (no loopback script).  
      Assert: `write_file` to drafts/; body capped; reply ack. Skip without key.

- [ ] **C4 — Optional `--save-skill-draft` sugar** (only if C1–C3 useful)  
      Thin wrapper in `pm_image_cmd_llm_agent.cpp`: fixed prompt template + drafts path; still uses normal agent turn.  
      Assert: flag writes package; default agent path unchanged.

### P1 — cheap extract (fallback / flag impl detail)

- [ ] **T1 — `SkillDraft` schema + pure extractor**  
      From `memory_state` turn_pairs → `tanit.skill_draft/v1` (no LLM).  
      Can power `--save-skill-draft` offline or validate loopback drafts.

- [ ] **T2 — Harness `skill-draft-extract`**  
      Fixture → golden JSON; no agent turn.

- [ ] **T3 — Shadow candidate signal**  
      Telemetry / `skill_draft_candidate` item; not catalog-injected.

### P1 — share prep (VFS-shaped, no push)

- [ ] **T7 — Manifest row exporter**  
      Local skill dir → VFS manifest row + sha256; `PM_LLM_TEST_VFS_*` discover when trusted only.

- [ ] **T8 — Doc: human export → CMS/VFS mount**  
      Link from agent-memory-next; no new feature bits.

### P2 — defer (not next)

- Auto-publish / swarm sync / signed manifest authoring
- Runtime `skill_manage`-style create/update tool
- Full skill-creator eval viewer / description optimizer in-process
- Indexing draft bodies into agent-memory VectorStore before user enable
- Cross-session reinforce → auto-promote to roaming
- Dedicated `llm skill draft` subcommand (until C4 proves need)

---

## Suggested test matrix

| Test | Mode | Assert |
|------|------|--------|
| `skill-draft-cli` | loopback | Session B writes drafts/ SKILL.md via `llm agent` |
| `skill-draft-cli` enable | loopback | Enable → catalog + `info_lookup` |
| `skill-draft-cli` live | live optional | Model writes draft from real session memory |
| `skill-draft-extract` | unit | Draft JSON from fixture turn_pairs |
| `skill-manifest-export` | loopback + `PM_LLM_TEST_VFS_*` | Row discovered only when trusted |
| existing `skill-resources` | loopback + live | Regression: consume path unchanged |

---

## Open questions

1. **Draft root** — separate `skills-drafts/` vs write into `skills/` with forced `disabled`?
2. **Scope id** — `session/<session_id>/<slug>` vs user-picked slug only?
3. **Who triggers** — user prompt on same `--session-id` (v0), `--save-skill-draft` (C4), or UI later?
4. **CMS push** — reuse `service files` push to a skills mount, or keep export file + external upload?
5. **Swarm identity** — per-user vs per-org signing before any auto-share?

Track decisions here. Prefer **C0–C2** before extractors or any network surface.
