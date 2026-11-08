# Follow-up: add Markdown directory listings

Extend the application you just completed. Before editing, recover the exact
prior `npm test` tool result with exactly one `memory_find` call. Retrieve that
tool payload, not the prior assistant-response body and not a smoke-validation
result. Use it as continuity evidence; do not rerun discovery, dependency
installation, or tests merely to rediscover the previous state.

Add a boolean `--md` option. When present, the CLI must write a deterministic
Markdown directory listing to the required `--output` path instead of HTML. The
Markdown output must:

- begin with `# Atlas Local Gallery`;
- list every file and directory recursively in the same deterministic order;
- identify each entry as a file or directory and include byte sizes for files;
- preserve readable relative paths, including spaces and punctuation;
- keep filesystem punctuation literal in Markdown (for example, `&` remains
  `&` rather than becoming an HTML entity such as `&amp;`);
- remain a single self-contained Markdown file.

The default mode must remain the existing standalone HTML gallery. Add
meaningful TypeScript tests for Markdown mode. After making the changes, run the
test suite once as final verification, then generate
`.qualification/agent-listing.md` from `.qualification/probe-input`.

Do not reinstall dependencies and do not repeat the original workspace search.
Use absolute paths rooted in the working folder shown in the system context for
all writes.
