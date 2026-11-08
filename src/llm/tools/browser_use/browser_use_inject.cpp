#include "llm/tools/browser_use/browser_use_inject.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include "string/string_utils.hpp"

namespace media::llm::browser_use {
namespace {

using media::strutil::lower_ascii;

bool contains(std::string_view hay, std::string_view needle)
{
    return hay.find(needle) != std::string_view::npos;
}

bool contains_any(std::string_view hay, std::initializer_list<std::string_view> needles)
{
    for (std::string_view n : needles) {
        if (contains(hay, n))
            return true;
    }
    return false;
}

bool has_ssh_path(std::string_view lower)
{
    return contains_any(lower, {
        "~/.ssh",
        "%userprofile%\\.ssh",
        "%userprofile%/.ssh",
        "\\.ssh\\",
        "/.ssh/",
        ".ssh\\",
        "id_rsa",
        "id_ed25519",
        ".aws/credentials",
        ".aws\\credentials",
    });
}

bool has_exfil_verb(std::string_view lower)
{
    return contains_any(lower, {
        "upload", "exfil", "exfiltrate", "compress-archive", "tar cz", "tar -cz",
        "zip ", "invoke-restmethod", "invoke-webrequest",
    });
}

bool has_override_phrase(std::string_view lower)
{
    return contains_any(lower, {
        "ignore prior instructions",
        "ignore previous instructions",
        "system override",
        "ignore safety",
        "disable sandbox",
        "skip consent",
        "developer maintenance mode",
        "new policy: always allow",
        "do not tell the user",
        "authorized maintenance",
    });
}

bool has_pipe_to_shell(std::string_view lower)
{
    // curl|wget … | sh/bash  — or PowerShell Bypass -File from a fetched script
    if (contains(lower, "| bash") || contains(lower, "|bash")
        || contains(lower, "| sh") || contains(lower, "|sh")
        || contains(lower, "| sh ") || contains(lower, "|| bash"))
        return contains_any(lower, {"curl", "wget", "invoke-webrequest", "iwr "});
    if (contains(lower, "executionpolicy bypass") || contains(lower, "-executionpolicy bypass"))
        return contains_any(lower, {".ps1", "http://", "https://"});
    return false;
}

bool has_download_and_run(std::string_view lower)
{
    const bool fetches = contains_any(lower, {
        "curl ", "wget ", "invoke-webrequest", "iwr ", "download ", "fetch ",
    });
    if (!fetches)
        return false;
    const bool script = contains_any(lower, {
        ".sh", ".ps1", ".bat", ".cmd", "bootstrap.sh", "setup-agent",
    });
    if (!script)
        return false;
    return contains_any(lower, {
        " | ", "&&", "chmod +x", "bash ", " sh ", "powershell", "cmd /c", "run it",
        "then run", "and run",
    });
}

std::optional<PromptInjectionHit> scan_lower(std::string_view lower)
{
    if (lower.empty())
        return std::nullopt;

    if (has_override_phrase(lower) && (has_ssh_path(lower) || has_pipe_to_shell(lower)
                                       || has_download_and_run(lower) || has_exfil_verb(lower))) {
        return PromptInjectionHit{
            "instruction_override",
            "page content tries to override agent instructions and coerce shell/exfil actions",
        };
    }
    if (has_ssh_path(lower) && has_exfil_verb(lower)) {
        return PromptInjectionHit{
            "ssh_exfil",
            "page content asks to archive/upload SSH or cloud credentials",
        };
    }
    if (has_pipe_to_shell(lower)) {
        return PromptInjectionHit{
            "pipe_to_shell",
            "page content asks to pipe a remote download into a shell",
        };
    }
    if (has_download_and_run(lower)) {
        return PromptInjectionHit{
            "download_and_run",
            "page content asks to download a script and execute it",
        };
    }
    if (has_override_phrase(lower) && contains_any(lower, {"curl", "wget", "powershell", "cmd /c", "run:"})) {
        return PromptInjectionHit{
            "instruction_override",
            "page content tries to override agent instructions toward shell execution",
        };
    }
    return std::nullopt;
}

void append_text(std::string& acc, std::string_view piece)
{
    if (piece.empty())
        return;
    if (!acc.empty())
        acc.push_back('\n');
    acc.append(piece.data(), piece.size());
}

void collect_strings(const nlohmann::json& node, std::string& acc, int depth)
{
    if (depth > 8 || acc.size() > 200000)
        return;
    if (node.is_string()) {
        append_text(acc, node.get_ref<const std::string&>());
        return;
    }
    if (node.is_array()) {
        for (const auto& el : node)
            collect_strings(el, acc, depth + 1);
        return;
    }
    if (!node.is_object())
        return;
    static constexpr const char* k_keys[] = {
        "pageText", "name", "text", "title", "value", "label", "ariaLabel",
        "aria-label", "placeholder", "message", "error", "note", "selector",
        "doc", "purpose", "headingPath",
    };
    for (const char* key : k_keys) {
        if (node.contains(key) && node[key].is_string())
            append_text(acc, node[key].get_ref<const std::string&>());
    }
    static constexpr const char* k_children[] = {
        "data", "items", "steps", "element", "model", "zones",
        "zoneHints", "collectionItems", "rankedZones", "tabs", "files",
    };
    for (const char* key : k_children) {
        if (node.contains(key))
            collect_strings(node[key], acc, depth + 1);
    }
}

void redact_string_field(nlohmann::json& obj, const char* key)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_string())
        return;
    const std::string lower = lower_ascii(obj[key].get_ref<const std::string&>());
    if (detect_prompt_injection(lower))
        obj[key] = "[redacted: prompt-injection content removed]";
}

void redact_tree(nlohmann::json& node, int depth)
{
    if (depth > 8)
        return;
    if (node.is_array()) {
        for (auto& el : node)
            redact_tree(el, depth + 1);
        return;
    }
    if (!node.is_object())
        return;
    static constexpr const char* k_keys[] = {
        "pageText", "name", "text", "title", "value", "placeholder", "note",
        "doc", "purpose", "headingPath",
    };
    for (const char* key : k_keys)
        redact_string_field(node, key);
    static constexpr const char* k_children[] = {
        "data", "items", "steps", "element", "model", "zones",
        "zoneHints", "collectionItems", "rankedZones", "tabs", "files",
    };
    for (const char* key : k_children) {
        if (node.contains(key))
            redact_tree(node[key], depth + 1);
    }
}

} // namespace

std::optional<PromptInjectionHit> detect_prompt_injection(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    return scan_lower(lower_ascii(std::string(text)));
}

std::optional<PromptInjectionHit> detect_prompt_injection_in_envelope(
    const nlohmann::json& envelope)
{
    if (envelope_flags_prompt_injection(envelope)) {
        PromptInjectionHit hit;
        hit.kind = envelope.value("prompt_injection_kind", std::string{"detected"});
        hit.reason = envelope.value("prompt_injection_reason",
                                    std::string{"prompt injection previously flagged"});
        return hit;
    }
    std::string acc;
    collect_strings(envelope, acc, 0);
    return detect_prompt_injection(acc);
}

void apply_prompt_injection_guard(nlohmann::json& envelope, const PromptInjectionHit& hit)
{
    if (!envelope.is_object())
        envelope = nlohmann::json::object();
    redact_tree(envelope, 0);
    envelope["ok"] = false;
    envelope["prompt_injection_detected"] = true;
    envelope["prompt_injection_kind"] = hit.kind;
    envelope["prompt_injection_reason"] = hit.reason;
    envelope["error"] =
        "Prompt injection detected in page content (" + hit.kind + "). "
        "Page text is untrusted — refusing to continue this turn. "
        + hit.reason;
}

bool envelope_flags_prompt_injection(const nlohmann::json& envelope)
{
    return envelope.is_object() && envelope.value("prompt_injection_detected", false);
}

std::optional<PromptInjectionHit> detect_injection_followthrough_command(
    std::string_view command)
{
    if (command.empty())
        return std::nullopt;
    const std::string lower = lower_ascii(std::string(command));
    // Standalone legit builds/downloads should not trip; require exfil / pipe /
    // download-and-run shape.
    if (auto hit = scan_lower(lower))
        return hit;
    return std::nullopt;
}

} // namespace media::llm::browser_use
