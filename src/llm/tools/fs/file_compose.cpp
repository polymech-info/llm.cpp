// Copyright PolyMech / pm-image. Internal use only.
//
// file_compose — assemble one destination from existing files, inclusive
// line ranges, and small literal fragments. Source bytes are copied by the
// filesystem. The tool result is metadata only.
#include "llm/tools/fs/FsTools.hpp"

#include "core/cli_cancel.hpp"
#include "llm/agent_memory.hpp"
#include "llm/change_store.hpp"
#include "fs/path_sanitizer.hpp"
#include "llm/llm_constants.hpp"
#include "logging/pm_log.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "string/string_utils.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace media::llm::path::detail {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t k_history_snapshot_max_bytes = 512u * 1024u;
constexpr std::size_t k_provenance_max = 32;

enum class PartKind { Literal, File };

struct Part {
    PartKind kind = PartKind::Literal;
    std::size_t index = 0; // 1-based
    std::string text;
    fs::path path;
    std::string path_display;
    bool ranged = false;
    int start_line = 0;
    int end_line = 0;
};

struct Sha256 {
    picosha2::hash256_one_by_one hasher;
    bool finished = false;

    void update(const void* data, std::size_t n) {
        if (finished || n == 0 || data == nullptr) return;
        const auto* p = static_cast<const unsigned char*>(data);
        hasher.process(p, p + n);
    }

    std::string hex() {
        if (finished) return {};
        hasher.finish();
        finished = true;
        return picosha2::get_hash_hex_string(hasher);
    }
};

struct Sink {
    std::ofstream& out;
    Sha256& hash;
    std::uintmax_t bytes = 0;
    std::uintmax_t source_bytes = 0;
    std::uintmax_t literal_bytes = 0;

    bool write(const char* data, std::size_t n, bool literal, std::string& err) {
        if (n == 0) return true;
        if (bytes > pm::llm::k_file_compose_max_output_bytes
            || static_cast<std::uintmax_t>(n)
                   > pm::llm::k_file_compose_max_output_bytes - bytes) {
            err = "output exceeds 1 GiB limit";
            return false;
        }
        out.write(data, static_cast<std::streamsize>(n));
        if (!out) {
            err = "write failed";
            return false;
        }
        hash.update(data, n);
        bytes += n;
        if (literal) literal_bytes += n;
        else source_bytes += n;
        return true;
    }
};

struct TmpFile {
    fs::path path;
    bool armed = false;

    TmpFile() = default;
    TmpFile(const TmpFile&) = delete;
    TmpFile& operator=(const TmpFile&) = delete;

    ~TmpFile() {
        if (!armed) return;
        std::error_code ec;
        fs::remove(path, ec);
    }
};

bool interrupted() {
    media::cli::yield_to_interrupt();
    return media::cli::cancel_requested();
}

bool key_present(const nlohmann::json& j, const char* key) {
    return j.contains(key) && !j[key].is_null();
}

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string strip_version_prefix(std::string_view v) {
    std::string s = trim_copy(std::string(v));
    if (s.size() > 7) {
        std::string low = s.substr(0, 7);
        low = media::strutil::lower_ascii(std::move(low));
        if (low == "sha256:") s = s.substr(7);
    }
    s = media::strutil::lower_ascii(std::move(s));
    return s;
}

std::string path_key(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec || c.empty()) {
        c = fs::absolute(p, ec);
        if (ec) c = p.lexically_normal();
    }
    std::string s = media::path::path_to_generic_utf8(c);
#if defined(_WIN32)
    s = media::strutil::lower_ascii(std::move(s));
#endif
    return s;
}

bool same_file(const fs::path& a, const fs::path& b) {
    if (path_key(a) == path_key(b)) return true;
    std::error_code ec;
    if (fs::exists(a, ec) && !ec && fs::exists(b, ec) && !ec && fs::equivalent(a, b, ec) && !ec)
        return true;
    return false;
}

bool commit_temp(const fs::path& from, const fs::path& to, std::string& err) {
#if defined(_WIN32)
    if (MoveFileExW(from.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    err = "rename failed (" + std::to_string(GetLastError()) + ")";
    return false;
#else
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    err = "rename failed: " + ec.message();
    return false;
#endif
}

bool read_exact(const fs::path& p, std::uintmax_t sz, std::string& out) {
    if (sz > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    out.assign(static_cast<std::size_t>(sz), '\0');
    if (sz == 0) return true;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.read(out.data(), static_cast<std::streamsize>(sz));
    return static_cast<std::uintmax_t>(in.gcount()) == sz;
}

std::string hash_file(const fs::path& p, bool& cancelled, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        err = "could not read existing output";
        return {};
    }
    Sha256 hash;
    std::array<char, pm::llm::k_file_compose_copy_buffer_bytes> buf{};
    while (true) {
        if (interrupted()) {
            cancelled = true;
            return {};
        }
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0) hash.update(buf.data(), static_cast<std::size_t>(n));
        if (in.eof()) break;
        if (!in) {
            err = "could not read existing output";
            return {};
        }
    }
    std::string hex = hash.hex();
    if (hex.empty()) err = "could not hash existing output";
    return hex;
}

bool take_line(const nlohmann::json& j, const char* key, bool& present, int& out, std::string& err) {
    if (!key_present(j, key)) {
        present = false;
        return true;
    }
    present = true;
    if (!j[key].is_number_integer()) {
        err = std::string(key) + " must be an integer";
        return false;
    }
    const auto v = j[key].get<std::int64_t>();
    if (v < 1 || v > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        err = std::string(key) + " must be >= 1";
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

ExecuteResult fail(const std::string& msg) {
    return fatal("file_compose: " + msg);
}

ExecuteResult fail_part(std::size_t index, const std::string& msg) {
    return fatal("file_compose: part " + std::to_string(index) + ": " + msg);
}

bool copy_whole_file(Sink& sink, const fs::path& src, std::string& err, bool& cancelled) {
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        err = "could not open source";
        return false;
    }
    std::array<char, pm::llm::k_file_compose_copy_buffer_bytes> buf{};
    while (true) {
        if (interrupted()) {
            cancelled = true;
            err = "cancelled";
            return false;
        }
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0 && !sink.write(buf.data(), static_cast<std::size_t>(n), false, err))
            return false;
        if (in.eof()) return true;
        if (!in) {
            err = "could not read source";
            return false;
        }
    }
}

// Sequential scan. Line breaks match file_read (LF, CR, CRLF). The copied
// range keeps the original line endings. A held CR waits for the next chunk
// so a CRLF split across reads stays one line.
bool copy_line_range(Sink& sink, const fs::path& src, int start_line, int end_line,
                     std::string& err, bool& cancelled) {
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        err = "could not open source";
        return false;
    }

    std::array<char, pm::llm::k_file_compose_copy_buffer_bytes> buf{};
    int line = 1;
    bool saw_any = false;
    bool line_open = false;
    bool hold_cr = false;
    bool done = false;

    auto emit = [&](const char* data, std::size_t n) -> bool {
        if (line < start_line || line > end_line) return true;
        return sink.write(data, n, false, err);
    };

    auto finish_line = [&](const char* ending, std::size_t n) -> bool {
        if (!emit(ending, n)) return false;
        if (line == end_line) {
            done = true;
            return true;
        }
        ++line;
        line_open = false;
        return true;
    };

    auto consume = [&](const char* data, std::size_t n) -> bool {
        std::size_t i = 0;
        while (i < n && !done) {
            if (hold_cr) {
                hold_cr = false;
                if (data[i] == '\n') {
                    if (!finish_line("\r\n", 2)) return false;
                    ++i;
                    continue;
                }
                if (!finish_line("\r", 1)) return false;
                continue;
            }
            const char c = data[i];
            if (c == '\r') {
                hold_cr = true;
                saw_any = true;
                ++i;
                continue;
            }
            if (c == '\n') {
                saw_any = true;
                if (!finish_line("\n", 1)) return false;
                ++i;
                continue;
            }
            std::size_t j = i + 1;
            while (j < n && data[j] != '\n' && data[j] != '\r') ++j;
            if (!emit(data + i, j - i)) return false;
            line_open = true;
            saw_any = true;
            i = j;
        }
        return true;
    };

    while (!done) {
        if (interrupted()) {
            cancelled = true;
            err = "cancelled";
            return false;
        }
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0 && !consume(buf.data(), static_cast<std::size_t>(n)))
            return false;
        if (done) return true;
        if (in.eof()) break;
        if (!in) {
            err = "could not read source";
            return false;
        }
    }
    if (done) return true;

    if (hold_cr) {
        hold_cr = false;
        saw_any = true;
        if (!finish_line("\r", 1)) return false;
        if (done) return true;
    }

    // A final line with no terminator still counts, matching file_read.
    if (line_open && line == end_line)
        return true;

    int lines = 0;
    if (line_open) lines = line;
    else if (saw_any) lines = line - 1;
    if (start_line > lines)
        err = "start_line past end (" + std::to_string(lines) + " lines)";
    else
        err = "end_line past end (" + std::to_string(lines) + " lines)";
    return false;
}

void note_source(nlohmann::json& sources, int& omitted, const Part& part) {
    if (sources.size() >= k_provenance_max) {
        ++omitted;
        return;
    }
    nlohmann::json row = {{"path", part.path_display}};
    if (part.ranged) {
        row["start_line"] = part.start_line;
        row["end_line"] = part.end_line;
    }
    sources.push_back(std::move(row));
}

} // namespace

ExecuteResult do_file_compose(const nlohmann::json& args) {
    try {
        if (!args.contains("output") || !args["output"].is_string() || args["output"].get<std::string>().empty())
            return fail("'output' (string) is required");
        if (!args.contains("parts") || !args["parts"].is_array())
            return fail("'parts' (array) is required");
        const auto& raw_parts = args["parts"];
        if (raw_parts.empty())
            return fail("'parts' must contain at least one part");
        if (raw_parts.size() > pm::llm::k_file_compose_max_parts)
            return fail("'parts' exceeds " + std::to_string(pm::llm::k_file_compose_max_parts) + " parts");

        bool overwrite = false;
        if (key_present(args, "overwrite")) {
            if (!args["overwrite"].is_boolean())
                return fail("overwrite must be a boolean");
            overwrite = args["overwrite"].get<bool>();
        }

        std::string expected;
        const bool check_version = key_present(args, "expected_version");
        if (check_version) {
            if (!args["expected_version"].is_string())
                return fail("expected_version must be a string");
            expected = strip_version_prefix(args["expected_version"].get<std::string>());
        }

        std::vector<Part> parts;
        parts.reserve(raw_parts.size());
        std::size_t literal_total = 0;
        int literal_parts = 0;
        int whole_file_parts = 0;
        int line_range_parts = 0;

        for (std::size_t i = 0; i < raw_parts.size(); ++i) {
            const auto& item = raw_parts[i];
            const std::size_t index = i + 1;
            if (!item.is_object())
                return fail_part(index, "must be an object");
            if (!item.contains("type") || !item["type"].is_string())
                return fail_part(index, "'type' is required");
            const std::string type = item["type"].get<std::string>();
            if (key_present(item, "offset") || key_present(item, "length"))
                return fail_part(index, "byte ranges are not supported");

            Part part;
            part.index = index;
            if (type == "literal") {
                if (key_present(item, "path") || key_present(item, "start_line") || key_present(item, "end_line"))
                    return fail_part(index, "literal parts only accept text");
                if (!item.contains("text") || !item["text"].is_string())
                    return fail_part(index, "literal requires text");
                part.kind = PartKind::Literal;
                part.text = item["text"].get<std::string>();
                if (part.text.size() > pm::llm::k_file_compose_literal_max_bytes)
                    return fail_part(index, "literal exceeds 64 KiB");
                if (literal_total > pm::llm::k_file_compose_literal_total_max_bytes
                    || part.text.size() > pm::llm::k_file_compose_literal_total_max_bytes - literal_total)
                    return fail("literal text exceeds 256 KiB total");
                literal_total += part.text.size();
                ++literal_parts;
            } else if (type == "file") {
                if (key_present(item, "text"))
                    return fail_part(index, "file parts do not accept text");
                if (!item.contains("path") || !item["path"].is_string() || item["path"].get<std::string>().empty())
                    return fail_part(index, "file requires path");
                bool has_start = false;
                bool has_end = false;
                if (!take_line(item, "start_line", has_start, part.start_line, part.text))
                    return fail_part(index, part.text);
                if (!take_line(item, "end_line", has_end, part.end_line, part.text))
                    return fail_part(index, part.text);
                part.text.clear();
                if (has_start != has_end)
                    return fail_part(index, "line range requires both start_line and end_line");
                if (has_start && part.end_line < part.start_line)
                    return fail_part(index, "end_line is before start_line");
                part.kind = PartKind::File;
                part.ranged = has_start;
                const std::string raw = item["path"].get<std::string>();
                part.path_display = resolve_tool_path_string(raw);
                if (part.path_display.empty())
                    return fail_part(index, "could not resolve path");
                part.path = part.path_display;
                if (const std::string deny = llm_path_fs_deny(part.path_display); !deny.empty())
                    return fail_part(index, deny);
                std::error_code ec;
                if (!fs::is_regular_file(part.path, ec) || ec)
                    return fail_part(index, "source is not a file");
                if (part.ranged) ++line_range_parts;
                else ++whole_file_parts;
            } else {
                return fail_part(index, "type must be \"literal\" or \"file\"");
            }
            parts.push_back(std::move(part));
        }

        const std::string out_raw = args["output"].get<std::string>();
        const std::string out_abs = resolve_tool_path_string(out_raw);
        if (out_abs.empty())
            return fail("could not resolve output");
        if (const std::string deny = llm_path_fs_deny(out_abs); !deny.empty())
            return fail(deny);
        if (const std::string deny = llm_path_write_deny(out_abs); !deny.empty())
            return fail(deny);

        const fs::path dest(out_abs);
        const fs::path tmp_path(dest.native() + media::path::path_from_utf8(".tmp~").native());
        for (const auto& part : parts) {
            if (part.kind != PartKind::File) continue;
            if (same_file(part.path, dest) || same_file(part.path, tmp_path))
                return fail_part(part.index, "source path is the output path");
        }

        std::error_code ec;
        if (fs::is_directory(dest, ec) && !ec)
            return fail("output is a directory");
        const bool dest_exists = fs::exists(dest, ec) && !ec;
        if (dest_exists && !fs::is_regular_file(dest, ec))
            return fail("output is not a regular file");
        if (dest_exists && !overwrite)
            return fail("output already exists");

        std::string before_body;
        bool before_ready = !dest_exists;
        std::uintmax_t before_size = 0;
        if (dest_exists) {
            before_size = fs::file_size(dest, ec);
            if (ec) return fail("could not read existing output");
            if (before_size <= k_history_snapshot_max_bytes
                && read_exact(dest, before_size, before_body))
                before_ready = true;
        }

        if (check_version && !expected.empty()) {
            if (interrupted()) return fail("cancelled");
            std::string have;
            bool cancelled = false;
            std::string hash_err;
            if (!dest_exists)
                have = media::llm::change::content_sha256_hex("");
            else if (before_ready && before_body.size() == static_cast<std::size_t>(before_size))
                have = media::llm::change::content_sha256_hex(before_body);
            else
                have = hash_file(dest, cancelled, hash_err);
            if (cancelled) return fail("cancelled");
            if (have.empty())
                return fail(hash_err.empty() ? "could not hash existing output" : hash_err);
            if (have != expected) {
                return fail("stale version (expected sha256:" + expected
                            + ", current sha256:" + have + "); re-read the file");
            }
        }

        const fs::path parent = dest.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) return fail("cannot create output directory: " + ec.message());
        }

        pm::log::info_lazy("llm_path_tools", [&] {
            return "file_compose output=" + out_abs
                + " parts=" + std::to_string(parts.size())
                + " overwrite=" + (overwrite ? "1" : "0");
        });

        Sha256 hash;
        std::uintmax_t bytes_written = 0;
        std::uintmax_t source_bytes = 0;
        std::uintmax_t literal_bytes = 0;
        nlohmann::json sources = nlohmann::json::array();
        int sources_omitted = 0;
        std::string after_body;
        bool after_ready = false;
        std::string version_hex;

        {
            TmpFile tmp;
            tmp.path = tmp_path;
            tmp.armed = true;
            std::ofstream out(tmp.path, std::ios::binary | std::ios::trunc);
            if (!out) return fail("cannot open temporary output");
            Sink sink{out, hash};

            for (const auto& part : parts) {
                if (interrupted()) return fail("cancelled");
                std::string err;
                bool cancelled = false;
                bool ok = false;
                if (part.kind == PartKind::Literal) {
                    ok = sink.write(part.text.data(), part.text.size(), true, err);
                } else if (part.ranged) {
                    ok = copy_line_range(sink, part.path, part.start_line, part.end_line, err, cancelled);
                    if (ok) note_source(sources, sources_omitted, part);
                } else {
                    ok = copy_whole_file(sink, part.path, err, cancelled);
                    if (ok) note_source(sources, sources_omitted, part);
                }
                if (cancelled) return fail("cancelled");
                if (!ok) return fail_part(part.index, err.empty() ? "failed" : err);
            }

            out.flush();
            if (!out) return fail("write failed");
            out.close();
            bytes_written = sink.bytes;
            source_bytes = sink.source_bytes;
            literal_bytes = sink.literal_bytes;
            version_hex = hash.hex();

            if (bytes_written <= k_history_snapshot_max_bytes && before_ready)
                after_ready = read_exact(tmp.path, bytes_written, after_body);

            std::string rename_err;
            if (!commit_temp(tmp.path, dest, rename_err))
                return fail(rename_err);
            tmp.armed = false;
        }

        nlohmann::json meta = {
            {"parts", static_cast<int>(parts.size())},
            {"bytes_written", bytes_written},
            {"source_bytes_copied", source_bytes},
            {"literal_bytes_written", literal_bytes},
            {"whole_file_parts", whole_file_parts},
            {"line_range_parts", line_range_parts},
            {"literal_parts", literal_parts},
            {"atomic", true},
            {"sources", std::move(sources)},
        };
        if (sources_omitted > 0)
            meta["sources_omitted"] = sources_omitted;
        if (!version_hex.empty())
            meta["version"] = media::llm::change::version_token(version_hex);

        const bool snapshot = before_ready && after_ready
            && before_body.size() <= k_history_snapshot_max_bytes
            && after_body.size() <= k_history_snapshot_max_bytes
            && static_cast<std::uintmax_t>(after_body.size()) == bytes_written;
        if (snapshot) {
            media::llm::change::RecordInput in;
            in.session_id = media::llm::agent::get_current_task_id();
            in.tool = "file_compose";
            in.path = media::path::path_to_utf8(dest);
            in.before = std::move(before_body);
            in.after = std::move(after_body);
            in.meta = {
                {"source", "agent"},
                {"tool", "file_compose"},
                {"parts", static_cast<int>(parts.size())},
                {"bytes_written", bytes_written},
            };
            const auto rec = media::llm::change::record_mutation(in);
            media::llm::change::attach_record_fields(meta, rec);
        } else {
            meta["history_ok"] = false;
            const bool too_large = bytes_written > k_history_snapshot_max_bytes
                || (dest_exists && before_size > k_history_snapshot_max_bytes);
            meta["history_note"] = too_large
                ? "snapshot skipped; file exceeds 512 KiB"
                : "snapshot skipped";
        }

        PerFileResult row;
        row.path = out_abs;
        row.ok = true;
        row.output_path = out_abs;
        row.result = std::move(meta);
        std::vector<PerFileResult> one;
        one.push_back(std::move(row));
        return build_envelope("file_compose", std::move(one));
    } catch (const std::exception& e) {
        return fail(e.what());
    } catch (...) {
        return fail("failed");
    }
}

ToolDef make_file_compose() {
    return ToolDef{
        "file_compose",
        "Compose one file from existing files, inclusive 1-based line ranges, and small "
        "literal fragments. The filesystem copies source bytes directly; do not paste large "
        "sources through write_file. Parts concatenate in order with no added newlines. "
        "overwrite defaults to false: an existing output is left untouched unless overwrite "
        "is true. When expected_version is set it must match the current output, or the call "
        "fails without writing. A line range needs both start_line and end_line. "
        "Whole files are copied verbatim. Use file_str_replace for a small edit of an existing file.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"output", {
                    {"type", "string"},
                    {"description", "Destination file path. Relative paths resolve like file_read and write_file."},
                }},
                {"parts", {
                    {"type", "array"},
                    {"minItems", 1},
                    {"maxItems", static_cast<int>(pm::llm::k_file_compose_max_parts)},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"type", {
                                {"type", "string"},
                                {"enum", nlohmann::json::array({"literal", "file"})},
                            }},
                            {"text", {
                                {"type", "string"},
                                {"description",
                                 "UTF-8 fragment for type=literal. Max 64 KiB per part and 256 KiB total per call."},
                            }},
                            {"path", {
                                {"type", "string"},
                                {"description",
                                 "Source file for type=file. Copied verbatim, or the inclusive line range when both start_line and end_line are set."},
                            }},
                            {"start_line", {
                                {"type", "integer"},
                                {"minimum", 1},
                                {"description", "First line to copy, 1-based. Requires end_line."},
                            }},
                            {"end_line", {
                                {"type", "integer"},
                                {"minimum", 1},
                                {"description", "Last line to copy, inclusive. Requires start_line. Past end of file is an error."},
                            }},
                        }},
                        {"required", nlohmann::json::array({"type"})},
                    }},
                }},
                {"overwrite", {
                    {"type", "boolean"},
                    {"description", "Replace an existing output. Default false; an existing file is left unchanged."},
                }},
                {"expected_version", {
                    {"type", "string"},
                    {"description",
                     "Optional. Whole-file version from file_read (sha256:...). "
                     "If the output changed since that read, composition fails and the file is left unchanged."},
                }},
            }},
            {"required", nlohmann::json::array({"output", "parts"})},
        }};
}

} // namespace media::llm::path::detail
