# Vendored Codex

Upstream is [openai/codex](https://github.com/openai/codex), cloned at `packages/codex` (blobless `main`, not a pixlwiz submodule). Tanit does not link or spawn this tree. The installed CLI at `%LOCALAPPDATA%\Programs\OpenAI\Codex\bin\codex.exe` is a separate binary.

Rust is pinned by `packages/codex/codex-rs/rust-toolchain.toml` (currently 1.99.1). Windows release builds set `LIBSQLITE3_FLAGS=SQLITE_DISABLE_INTRINSIC`.

## Update

```powershell
git -C packages/codex pull --ff-only
```

Then rebuild. `packages/` is gitignored, so the clone stays local and can move independently of pixlwiz.

## Build

From `packages/codex/codex-rs`:

```powershell
$env:LIBSQLITE3_FLAGS = "SQLITE_DISABLE_INTRINSIC"
cargo build -p codex-cli --bin codex --locked
```

Debug binary: `packages/codex/codex-rs/target/debug/codex.exe`.

## Tests that do not call the API

```powershell
cargo test -p codex-login --lib --locked
cargo test -p codex-protocol --lib --locked
```

`codex-login` tests use a temp `CODEX_HOME` and fake keys. They do not read or write `%USERPROFILE%\.codex\auth.json`.

Upstream's full suite is `just test` (`cargo nextest run`) from the repo root. That builds the workspace and is much larger than the two crates above.

## API key

Codex does not pick up `OPENAI_API_KEY` from the environment. `codex login status` on this machine reports **Logged in using ChatGPT**. A live `codex exec` from either the installed CLI or this build uses that cached ChatGPT session.

An API key is a different login, stored in `%USERPROFILE%\.codex\auth.json` (or the OS credential store when `cli_auth_credentials_store` is `keyring` or `auto`). It replaces the ChatGPT session and bills the OpenAI Platform account:

```powershell
$env:OPENAI_API_KEY | .\target\debug\codex.exe login --with-api-key
.\target\debug\codex.exe login status
.\target\debug\codex.exe logout
```

`logout` clears whichever credential is stored. Do not run the API-key login if the ChatGPT session should stay.
