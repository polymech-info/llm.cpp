// pm-llm — same `llm` command as tanit-cli.
// Registration is pm_image_register_llm. The parsed subcommand runs through
// pm_image_dispatch_llm, including `llm agent` and `llm agent --serve`.

#include "pm_image_cmd_llm_agent.hpp"
#include "pm_branding_config.hpp"
#include "commons/commons_host.hpp"
#include "core/settings_runtime.hpp"
#include "fs/path_sanitizer.hpp"
#include "logging/logger.h"
#include "plugins/plugin_manager.hpp"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

int prepare_profile(CLI::App& app, PmImageCliState& st)
{
    if (auto* opt = app.get_option_no_throw("--cwd"); opt && opt->count() > 0)
        media::runtime_settings::set_cli_cwd_explicit(true);

    if (!st.config_dir.empty()) {
        std::error_code ec;
        fs::path dir = fs::absolute(media::path::path_from_utf8(st.config_dir), ec);
        if (ec)
            dir = media::path::path_from_utf8(st.config_dir);
        fs::create_directories(dir, ec);
        if (ec) {
            std::cerr << pm::brand::k_app_id_u8 << ": --config-dir: failed to create directory: "
                      << media::path::path_to_utf8(dir) << ": " << ec.message() << "\n";
            return 1;
        }
    }
    media::runtime_settings::apply_cli_config_dir_override(st.config_dir);

    if (!st.cwd.empty() && st.cwd != ".") {
        std::error_code ec;
        const fs::path new_cwd = fs::absolute(st.cwd, ec);
        if (ec || !fs::exists(new_cwd) || !fs::is_directory(new_cwd)) {
            std::cerr << pm::brand::k_app_id_u8 << ": --cwd is not a valid directory: " << st.cwd << "\n";
            return 1;
        }
        fs::current_path(new_cwd, ec);
        if (ec) {
            std::cerr << pm::brand::k_app_id_u8 << ": --cwd failed to change directory: " << st.cwd << "\n";
            return 1;
        }
    }

    if (!st.logging_dir.empty()) {
        std::error_code ec;
        fs::path log_dir = fs::absolute(media::path::path_from_utf8(st.logging_dir), ec);
        if (ec)
            log_dir = media::path::path_from_utf8(st.logging_dir);
        fs::create_directories(log_dir, ec);
        if (ec) {
            std::cerr << pm::brand::k_app_id_u8 << ": --logging-dir: failed to create directory: "
                      << media::path::path_to_utf8(log_dir) << ": " << ec.message() << "\n";
            return 1;
        }
        media::runtime_settings::set_default_logging_dir_override(log_dir);
    }
    return 0;
}

#if defined(_WIN32)
// Same as tanit-cli. A console still on the OEM code page shows UTF-8 as
// mojibake ("ΓÇÖ" for U+2019, "ΓÇö" for an em dash). Pipes and files stay untouched.
void try_set_console_utf8()
{
    const HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD ft_out = FILE_TYPE_UNKNOWN;
    DWORD ft_err = FILE_TYPE_UNKNOWN;
    if (hout && hout != INVALID_HANDLE_VALUE)
        ft_out = GetFileType(hout);
    if (herr && herr != INVALID_HANDLE_VALUE)
        ft_err = GetFileType(herr);
    if (ft_out != FILE_TYPE_CHAR && ft_err != FILE_TYPE_CHAR)
        return;
    (void)::SetConsoleOutputCP(CP_UTF8);
    (void)::SetConsoleCP(CP_UTF8);
}
#endif

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    try_set_console_utf8();
#endif
    CLI::App app{"pm-llm"};
    app.require_subcommand(1);

    PmImageCliState st;
    st.process_args.reserve(argc > 0 ? static_cast<size_t>(argc) : 0u);
    for (int i = 0; i < argc; ++i)
        st.process_args.emplace_back(argv[i] ? argv[i] : "");

    app.add_option("--log-level", st.log_level,
                   "Log level for stderr: trace, debug, info, warn, error, critical, off.")
        ->default_val("info")
        ->check(CLI::IsMember(
            {"trace", "debug", "info", "warn", "warning", "error", "err", "critical", "off", "none"},
            CLI::ignore_case));
    app.add_option("--config-dir", st.config_dir,
                   "Profile root directory (settings, caches, skills, tokens).");
    app.add_option("--logging-dir", st.logging_dir,
                   "Session log directory. Default: <config-dir>/sessions/session-MM-DD-HH-MM-SS.");
    app.add_option("--cwd", st.cwd,
                   "Working directory for path resolution. Default: current directory.")
        ->default_val(".");

    pm_image_register_llm(app, st);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& err) {
        const int code = app.exit(err);
        return code == 0 ? 0 : 2;
    }

    logger::init_stderr(pm::brand::k_app_id_u8, st.log_level);
    logger::set_log_level(st.log_level);
    // Same bootstrap as tanit-cli. The chat add-on is a plugin beside the exe.
    // The host hooks are what the plugin scan uses to find that directory.
    pm::plugins::set_bootstrap_log_level(st.log_level.c_str());
    pm::commons::host::apply_log_level(st.log_level.c_str());
    pm::commons::host::install_process_hooks();

    if (const int prep = prepare_profile(app, st); prep != 0)
        return prep;
    if (!(st.llm_cmd && st.llm_cmd->parsed()))
        return 2;
    return pm_image_dispatch_llm(app, st, nullptr);
}
