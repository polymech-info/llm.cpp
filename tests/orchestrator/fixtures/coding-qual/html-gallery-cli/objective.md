# Objective: standalone directory gallery CLI

Build a complete, production-shaped command-line application in the current
workspace. Start by using workspace search to locate the exact marker
`GALLERY_QUAL_REFERENCE_V1`, then read the matching file before designing the
application. That local reference contains one required product detail which
is deliberately not repeated here.

Use the first-class `file_search` and `file_read` tools for that discovery and
read step. If hidden-path search requires a shell fallback, still read the
discovered reference with `file_read`; do not substitute a shell content-read.

Create a TypeScript ESM application named `pretty-gallery-cli` that:

- uses `yargs` for argument parsing;
- accepts one positional input directory and a required `--output` path;
- recursively lists directories and files in deterministic path order;
- displays relative path, file size, and a clear file/directory distinction;
- embeds previews for PNG, JPEG, GIF, and WebP images as base64 data URLs;
- writes one self-contained HTML file with no external scripts, styles, images,
  fonts, or network requests;
- escapes filesystem-derived text before inserting it into HTML;
- reports the output path and counts after successful generation.

Keep dependencies minimal: `yargs` is the only allowed runtime dependency.
TypeScript and type packages may be development dependencies. Use
`package.json` with `"type": "module"`, `src/index.ts`, `tsconfig.json`, and
Node's built-in test runner. Author test sources as TypeScript (`.ts`) so they
can be created through the workspace file tools, compile them into `dist`, and
run the compiled JavaScript tests.

Use the available file tools to create the project. Use the run tool to install
dependencies, build, run meaningful automated tests, and execute a smoke
generation against `.qualification/probe-input`, writing
`.qualification/agent-gallery.html`. Diagnose and repair any failures. Do not
modify or delete files inside `.qualification/probe-input`. Use absolute paths
rooted in the working folder shown in the system context for all writes.

Finish with a concise result that states which checks passed and where the smoke
HTML was written.
