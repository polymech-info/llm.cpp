#include "RunValidators.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include "string/string_utils.hpp"

namespace media::llm::run {

namespace {

using VR = ValidationResult;
using VT = ValidationTier;

bool icontains(const std::string& haystack, const std::string& needle_lower) {
    const auto it = std::search(
        haystack.begin(), haystack.end(),
        needle_lower.begin(), needle_lower.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

bool imatches(const std::string& text, const std::regex& re) {
    return std::regex_search(text, re, std::regex_constants::match_any);
}

// ── Deny-tier ─────────────────────────────────────────────────────────────────

VR check_download_cradle(const std::string& cmd) {
    static const std::regex re(
        R"((?:curl|wget|Invoke-WebRequest|iwr|Invoke-RestMethod|irm)\b.*\|\s*(?:ba)?sh\b)"
        R"(|(?:iwr|irm|Invoke-WebRequest|Invoke-RestMethod)\b.*\|\s*(?:iex|Invoke-Expression)\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "download-cradle: pipe-to-exec pattern detected"};
    return {};
}

VR check_encoded_command(const std::string& cmd) {
    static const std::regex re(
        R"((?:pwsh|powershell)(?:\.exe)?\b.*\s+-(?:EncodedCommand|Enc)\b)"
        R"(|base64\s.*\|\s*(?:ba)?sh\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "encoded-command: obfuscated shell execution"};
    return {};
}

VR check_nested_shell(const std::string& cmd) {
    static const std::regex re(
        R"(\beval\s+[\"'])"
        R"(|\bbash\s+-c\s+[\"']\$\()"
        R"(|\bsh\s+-c\s+[\"'])"
        R"(|\bcmd\s+/c\s+[\"'])"
        R"(|\b(?:pwsh|powershell)(?:\.exe)?\s+-(?:Command|c)\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "nested-shell: shell-in-shell execution"};
    return {};
}

VR check_privilege_escalation(const std::string& cmd) {
    static const std::regex re(
        R"(\bsudo\b)"
        R"(|\bsu\s+-\b)"
        R"(|\bStart-Process\b.*-Verb\s+RunAs)"
        R"(|\brunas\s+/user\b)"
        R"(|\bdoas\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "privilege-escalation: attempt to gain elevated privileges"};
    return {};
}

VR check_recursive_system_delete(const std::string& cmd) {
    static const std::regex re(
        R"(\brm\s+-r[f]*\s+[/~](?:\s|$))"
        R"(|\brm\s+-[f]*r[f]*\s+[/~](?:\s|$))"
        R"(|\bRemove-Item\s.*-Recurse.*(?:C:\\Windows|C:\\Program|/etc|/usr|/var|/bin)\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "recursive-system-delete: refusing to delete system paths"};
    return {};
}

VR check_registry_write(const std::string& cmd) {
    static const std::regex re(
        R"(\breg\s+(?:add|delete)\b)"
        R"(|\bregedit\s+/s\b)"
        R"(|\bSet-ItemProperty\s+HK[A-Z]{1,3}:)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "registry-write: Windows registry modification"};
    return {};
}

VR check_com_object_exec(const std::string& cmd) {
    static const std::regex re(
        R"(\bNew-Object\s+-ComObject\b)"
        R"(|\bWScript\.Shell\b)"
        R"(|\bShell\.Application\b)"
        R"(|\bMMC20\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "com-object: COM-based code execution"};
    return {};
}

VR check_wmi_spawn(const std::string& cmd) {
    static const std::regex re(
        R"(\bInvoke-WmiMethod\b)"
        R"(|\bInvoke-CimMethod\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "wmi-spawn: WMI/CIM process creation"};
    return {};
}

VR check_add_type(const std::string& cmd) {
    if (icontains(cmd, "add-type"))
        return {VT::Deny, "add-type: runtime .NET compilation"};
    return {};
}

VR check_scheduled_task_create(const std::string& cmd) {
    static const std::regex re(
        R"(\bRegister-ScheduledTask\b)"
        R"(|\bschtasks\s+/create\b)"
        R"(|\bcrontab\s+-)"
        R"(|\bat\s+\d)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "scheduled-task: task/cron creation"};
    return {};
}

VR check_module_install(const std::string& cmd) {
    static const std::regex re(
        R"(\bInstall-Module\b)"
        R"(|\bSave-Module\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "module-install: PowerShell module installation"};
    return {};
}

VR check_dynamic_invoke(const std::string& cmd) {
    static const std::regex re(
        R"(\bInvoke-Expression\b)"
        R"(|\biex\s)"
        R"(|\biex$)"
        R"(|&\s*\(\s*\$)"
        R"(|&\s*\(\s*['"])",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "dynamic-invoke: dynamic code execution"};
    return {};
}

VR check_download_utility(const std::string& cmd) {
    static const std::regex re(
        R"(\bStart-BitsTransfer\b)"
        R"(|\bcertutil\s+-urlcache\b)"
        R"(|\bbitsadmin\s+/transfer\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "download-utility: file download via system utility"};
    return {};
}

VR check_persistence_hooks(const std::string& cmd) {
    static const std::regex re(
        R"(\.git[\\/]+hooks[\\/]+)"
        R"(|\b(?:npm|pnpm|yarn)\s+(?:set|pkg\s+set)\s+(?:script\.|scripts\.)?(?:postinstall|preinstall|prepare)\b)"
        R"(|\b(?:Set-Content|Add-Content)\b[^\r\n;|&]*(?:package\.json|\$PROFILE|Microsoft\.PowerShell_profile\.ps1))"
        R"(|\bpostinstall\b[^\r\n;|&]*(?:curl|wget|iwr|irm|node|powershell|pwsh|bash|sh)\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "persistence-hook: startup/package/git-hook modification"};
    return {};
}

VR check_env_or_identity_exfil(const std::string& cmd) {
    static const std::regex re(
        R"(process\.env)"
        R"(|\bGet-ChildItem\s+Env:)"
        R"(|\bwhoami\s+/all\b)"
        R"(|\bnet\s+user\b)"
        R"(|\bipconfig\s+/all\b)"
        R"(|\bssh-add\s+-L\b)"
        R"(|\bcertutil\s+-user\s+-store\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "identity-env-exfil: environment, identity, or certificate inventory"};
    return {};
}

VR check_secret_copy_or_clipboard(const std::string& cmd) {
    static const std::regex re(
        R"(\bCopy-Item\b[^\r\n;|&]*(?:\.env\b|Cookies\b|Login Data\b|Local State\b))"
        R"(|\bGet-Content\b[^\r\n;|&]*(?:\.env\b|id_rsa\b|id_ed25519\b)[^\r\n;|&]*\|\s*Set-Clipboard\b)"
        R"(|\bSet-Clipboard\b[^\r\n;|&]*(?:\.env\b|API_KEY|TOKEN|SECRET))",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "secret-copy: copying or clipboarding secrets/profile data"};
    return {};
}

VR check_remote_or_url_mutation(const std::string& cmd) {
    static const std::regex re(
        R"(\bgit\s+remote\s+set-url\b)"
        R"(|\bStart-Process\b\s+["']?https?://[^\s"']*(?:token=|\$env:|API_KEY|SECRET|TOKEN))"
        R"(|\bcurl\b[^\r\n;|&]*(?:\$env:|API_KEY|SECRET|TOKEN))"
        R"(|\bpython(?:\.exe)?\s+-c\b[^\r\n]*(?:urllib|requests|http\.client)[^\r\n]*https?://)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny, "remote-url-mutation: remote endpoint/token exposure"};
    return {};
}

// ── WarnStrict-tier (promoted to Deny under FEATURE_LLM_RUN_STRICT) ─────────────

VR check_shell_script_execution(const std::string& cmd) {
    static const std::regex re(
        R"(\./[^\s]+\.(?:sh|bash|ps1)\b)"
        R"(|\bbash\s+[^\s-])"
        R"(|\bInvoke-Command\s+-FilePath\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::WarnStrict, "shell-script: executing an external script file"};
    return {};
}

// ── Warn-tier (informational; never blocks, even under strict mode) ─────────────

VR check_network_access(const std::string& cmd) {
    static const std::regex re(
        R"(\bcurl\b)"
        R"(|\bwget\b)"
        R"(|\bInvoke-WebRequest\b)"
        R"(|\biwr\b)"
        R"(|\bInvoke-RestMethod\b)"
        R"(|\birm\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Warn, "network-access: command may access the network"};
    return {};
}

VR check_env_var_write(const std::string& cmd) {
    static const std::regex re(
        R"(\bexport\s+\w+=)"
        R"(|\$env:\w+\s*=)"
        R"(|\bSet-Item\s+env:)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Warn, "env-var-write: modifying environment variables"};
    return {};
}

VR check_member_invocation(const std::string& cmd) {
    static const std::regex re(
        R"(\.\w+\()"
        R"(|\[\w+\]::\w+\()",
        std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Warn, "member-invocation: .NET method or member call"};
    return {};
}

VR check_sub_expression(const std::string& cmd) {
    static const std::regex re(R"(\$\()", std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Warn, "sub-expression: $(…) may execute arbitrary code"};
    return {};
}

VR check_splatting(const std::string& cmd) {
    static const std::regex re(R"(@\w+\b)", std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Warn, "splatting: @variable splatting may hide arguments"};
    return {};
}

// ── Sensitive-path access (Deny) ─────────────────────────────────────────────
// Commands that reference credential directories / key files are denied
// unconditionally.  The check operates on the raw command string and covers:
//
//  1. Separator-anchored fragments  (/home/u/.ssh/, C:\Users\u\.ssh\, …)
//  2. Quote-anchored fragments      ('\.ssh', ".ssh", Join-Path $HOME '.ssh')
//     — catches the variable-indirection pattern where the model does
//       $src = Join-Path $HOME '.ssh'; … Compress-Archive -LiteralPath $src
//  3. SSH key filenames             (id_rsa, id_ed25519, known_hosts, …)
//  4. Home-dir archiving            (Compress-Archive / zip / tar against
//     $HOME/$env:USERPROFILE — the archiving tool need not mention .ssh
//     explicitly if the variable indirection already hides it)
VR check_sensitive_path_access(const std::string& cmd) {
    // ── 1. Directory fragments (separator- or quote-anchored) ───────────────
    static const char* const k_dir_fragments[] = {
        // SSH — classic separator-anchored forms
        "/.ssh/",  "\\.ssh\\",  "/.ssh\\",  "\\.ssh/",
        "/.ssh*",  "\\.ssh*", "~/.ssh",
        // SSH — quote-anchored (catches Join-Path $HOME '.ssh' and "/.ssh" args)
        // The leading quote is enough; we don't require a trailing separator.
        "/.ssh\"", "\\.ssh\"",  "/.ssh'",  "\\.ssh'",
        "'.ssh",   "\".ssh",                              // standalone quoted arg: '.ssh' / ".ssh"
        // GPG keyring
        "/.gnupg/", "/.gnupg\\", "\\.gnupg\\",
        "'.gnupg",  "\".gnupg",
        // AWS credentials
        "/.aws/",  "/.aws\\",   "\\.aws\\",
        "'.aws",   "\".aws",
        // Kubernetes config
        "/.kube/", "/.kube\\",  "\\.kube\\",
        "'.kube",  "\".kube",
        // Azure / Docker credentials
        "/.azure/","\\.azure\\", "'.azure", "\".azure",
        "/.docker/","\\.docker\\","'.docker","\".docker",
        // Windows Credential Manager / user hive
        "microsoft/credentials", "microsoft\\credentials",
        "microsoft/protect",     "microsoft\\protect",
        "microsoft/vault",       "microsoft\\vault",
        "ntuser.dat",
        nullptr,
    };
    const std::string lower_cmd = [&] {
        std::string s = cmd;
        s = media::strutil::lower_ascii(std::move(s));
        return s;
    }();
    for (const char* const* p = k_dir_fragments; *p; ++p) {
        if (lower_cmd.find(*p) != std::string::npos)
            return {VT::Deny,
                    std::string("sensitive-path: command references protected directory: ") + *p};
    }

    // ── 2. SSH private-key filenames ─────────────────────────────────────────
    static const std::regex re_keyfile(
        R"(\bid_rsa\b|\bid_dsa\b|\bid_ecdsa\b|\bid_ed25519\b)"
        R"(|\bauthorized_keys\b|\bknown_hosts\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re_keyfile))
        return {VT::Deny, "sensitive-path: command references SSH private key file"};

    // ── 3. Join-Path with .ssh argument ──────────────────────────────────────
    // Catches:  Join-Path $HOME '.ssh'
    //           join-path $env:USERPROFILE ".ssh"
    // This is the first half of the variable-indirection pattern; blocking it
    // prevents $src = Join-Path … '.ssh' from hiding the path later.
    static const std::regex re_joinpath_ssh(
        R"(\bJoin-Path\b[^;|&]*['"]\.ssh['"]?)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re_joinpath_ssh))
        return {VT::Deny, "sensitive-path: Join-Path references .ssh directory"};

    // ── 4. Compress-Archive / archiving targeting the user home tree ─────────
    // Catches the full exfiltration idiom even when the actual path is hidden
    // in a variable:
    //   Compress-Archive -LiteralPath $src -DestinationPath $dst
    // where $src was set to the .ssh dir earlier in the same command string.
    // We deny any Compress-Archive that references $HOME / $env:USERPROFILE /
    // $env:APPDATA or their %…% counterparts as source operands.
    static const std::regex re_archive_home(
        R"(\bCompress-Archive\b)"
        R"(.*(?:\$HOME|\$env:USERPROFILE|\$env:APPDATA|\$USERPROFILE)"
        R"(|%USERPROFILE%|%APPDATA%|%HOMEDRIVE%%HOMEPATH%)\b)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re_archive_home))
        return {VT::Deny, "sensitive-path: Compress-Archive targeting user home directory tree"};

    return {};
}

VR check_converted_part_dump(const std::string& cmd) {
    static const std::regex re(
        R"(\b(?:Get-Content|gc|type|cat|more)\b[^|\r\n]*_md[\\/][^|\r\n]*_\d+\.md)",
        std::regex::icase | std::regex::optimize);
    if (imatches(cmd, re))
        return {VT::Deny,
                "converted-part: do not dump pdf-to-md parts. Call search_query, then write_file the summary."};
    return {};
}

using ValidatorFn = VR (*)(const std::string&);

static const ValidatorFn k_validators[] = {
    // Hard-deny tier runs first — order matters.
    check_converted_part_dump,
    check_sensitive_path_access,  // NEW: block SSH/cred exfiltration
    check_download_cradle,
    check_encoded_command,
    check_nested_shell,
    check_privilege_escalation,
    check_recursive_system_delete,
    check_registry_write,
    check_com_object_exec,
    check_wmi_spawn,
    check_add_type,
    check_scheduled_task_create,
    check_module_install,
    check_dynamic_invoke,
    check_download_utility,
    check_persistence_hooks,
    check_env_or_identity_exfil,
    check_secret_copy_or_clipboard,
    check_remote_or_url_mutation,
    check_network_access,
    check_shell_script_execution,
    check_env_var_write,
    check_member_invocation,
    check_sub_expression,
    check_splatting,
};

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Public API — validate_command
// ═══════════════════════════════════════════════════════════════════════════════

ValidationResult validate_command(const std::string& command,
                                  const std::string& /*shell_hint*/) {
    if (command.empty())
        return {ValidationTier::Deny, "run: command is required and non-empty"};

    for (const auto& fn : k_validators) {
        auto r = fn(command);
        if (r.tier != ValidationTier::Allow)
            return r;
    }
    return {ValidationTier::Allow, {}};
}

} // namespace media::llm::run
