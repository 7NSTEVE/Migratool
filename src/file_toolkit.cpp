#include "migratool/file_toolkit.hpp"
#include "migratool/fs_index.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace mt {
namespace {

std::string lower_ascii_ft(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string normalize_rel_ft(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    return p;
}

fs::path resolve_authorized_path(const Project& p, const std::string& raw_path,
                                 const std::string& req_root = "", bool must_exist = true,
                                 std::string* out_root_key = nullptr, std::string* out_rel = nullptr) {
    if (raw_path.empty()) throw std::runtime_error("Ruta vacía");
    fs::path input(wide(raw_path));
    if (input.is_absolute()) {
        fs::path canon = fs::weakly_canonical(input);
        for (const auto& r : p.config["roots"]) {
            if (!req_root.empty() && r["id"] != req_root) continue;
            fs::path rbase = p.resolve(r["path"]);
            if (within(canon, rbase)) {
                if (must_exist && !fs::is_regular_file(native_path(canon)))
                    throw std::runtime_error("Archivo no existe: " + path_text(canon));
                if (out_root_key) *out_root_key = r["id"].get<std::string>();
                if (out_rel) *out_rel = normalize_rel_ft(path_text(canon.lexically_relative(rbase)));
                return canon;
            }
        }
        throw std::runtime_error("PATH_OUTSIDE_WORKSPACE: la ruta está fuera de las raíces autorizadas: " + raw_path);
    }

    // Relative path: search matching root
    for (const auto& r : p.config["roots"]) {
        if (!req_root.empty() && r["id"] != req_root) continue;
        fs::path rbase = p.resolve(r["path"]);
        fs::path candidate = fs::weakly_canonical(rbase / input);
        if (!within(candidate, rbase)) {
            throw std::runtime_error("PATH_TRAVERSAL_DENIED: " + raw_path);
        }
        if (!must_exist || fs::is_regular_file(native_path(candidate))) {
            if (out_root_key) *out_root_key = r["id"].get<std::string>();
            if (out_rel) *out_rel = normalize_rel_ft(path_text(candidate.lexically_relative(rbase)));
            return candidate;
        }
    }
    throw std::runtime_error("FILE_NOT_FOUND: " + raw_path);
}

std::vector<std::string> split_text_lines(const std::string& text, bool* out_crlf = nullptr, bool* out_trailing_nl = nullptr) {
    if (out_crlf) *out_crlf = (text.find("\r\n") != std::string::npos);
    if (out_trailing_nl) *out_trailing_nl = (!text.empty() && text.back() == '\n');
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            std::string l = text.substr(start);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(std::move(l));
            break;
        }
        std::string l = text.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        start = nl + 1;
    }
    return lines;
}

std::string join_text_lines(const std::vector<std::string>& lines, bool crlf, bool trailing_nl) {
    const char* eol = crlf ? "\r\n" : "\n";
    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) oss << eol;
        oss << lines[i];
    }
    if (trailing_nl && !lines.empty()) oss << eol;
    return oss.str();
}

bool is_likely_binary(std::string_view content) {
    const size_t probe = std::min<size_t>(content.size(), 8192);
    size_t non_text = 0;
    for (size_t i = 0; i < probe; ++i) {
        unsigned char c = static_cast<unsigned char>(content[i]);
        if (c == 0) return true;
        if (c < 9 || (c > 13 && c < 32)) ++non_text;
    }
    return probe > 64 && (non_text * 10 > probe);
}

} // namespace

Json tool_read_file(const Project& p, Database& db, const Json& args) {
    const std::string raw_path = args.value("path", args.value("file", ""));
    const std::string req_root = args.value("root", "");
    std::string rkey, rel;
    fs::path abs_path = resolve_authorized_path(p, raw_path, req_root, true, &rkey, &rel);

    const size_t max_read = std::clamp<size_t>(args.value("max_bytes", size_t(4 * 1024 * 1024)), 1024, 32 * 1024 * 1024);
    const std::string content = read_text(abs_path, max_read);
    const std::string digest = sha256(content);
    const bool binary = is_likely_binary(content);
    if (binary && !args.value("allow_binary", false)) {
        return {
            {"schema", "migratool.read_file/1"},
            {"root", rkey},
            {"path", rel},
            {"absolute_path", path_text(abs_path)},
            {"binary", true},
            {"bytes", content.size()},
            {"sha256", digest},
            {"frn", static_cast<int64_t>(query_path_frn(abs_path))}
        };
    }

    auto lines = split_text_lines(content);
    const int total_lines = static_cast<int>(lines.size());
    int start_line = args.value("start_line", 1);
    int end_line = args.value("end_line", total_lines);

    // Optional symbol windowing: jump directly to symbol's line range!
    if (args.contains("symbol") && args["symbol"].is_string() && !args["symbol"].get<std::string>().empty()) {
        const std::string sym = args["symbol"].get<std::string>();
        auto srows = db.query(
            "SELECT s.line, s.data FROM symbols s JOIN files f ON f.id=s.file_id "
            "WHERE f.absolute_path=? COLLATE NOCASE AND s.active=1 AND (s.name=? OR s.qualified_name=?) LIMIT 1",
            {path_text(abs_path), sym, sym});
        if (!srows.empty()) {
            int sl = std::max(1, srows[0]["line"].get<int>());
            int el = sl + 60;
            auto sdata = Json::parse(srows[0]["data"].get<std::string>(), nullptr, false);
            if (sdata.is_object() && sdata.contains("end_line") && sdata["end_line"].is_number_integer()) {
                el = sdata["end_line"].get<int>();
            }
            int ctx = std::clamp(args.value("context_lines", 2), 0, 50);
            start_line = std::max(1, sl - ctx);
            end_line = std::min(total_lines, el + ctx);
        }
    }

    start_line = std::clamp(start_line, 1, std::max(1, total_lines));
    end_line = std::clamp(end_line, start_line, std::max(start_line, total_lines));

    const bool numbered = args.value("with_line_numbers", true);
    const size_t max_chars = std::clamp<size_t>(args.value("max_chars", size_t(60000)), 256, 256000);
    std::ostringstream out;
    bool truncated = false;
    int actual_end = start_line - 1;
    for (int ln = start_line; ln <= end_line && ln <= total_lines; ++ln) {
        std::string row_str = numbered ? (std::to_string(ln) + ": " + lines[static_cast<size_t>(ln - 1)])
                                       : lines[static_cast<size_t>(ln - 1)];
        if (ln > start_line && static_cast<size_t>(out.tellp()) + row_str.size() + 1 > max_chars) {
            truncated = true;
            break;
        }
        if (ln > start_line) out << '\n';
        out << row_str;
        actual_end = ln;
    }

    return {
        {"schema", "migratool.read_file/1"},
        {"root", rkey},
        {"path", rel},
        {"absolute_path", path_text(abs_path)},
        {"sha256", digest},
        {"bytes", content.size()},
        {"total_lines", total_lines},
        {"start_line", start_line},
        {"end_line", actual_end},
        {"truncated", truncated || actual_end < total_lines},
        {"frn", static_cast<int64_t>(query_path_frn(abs_path))},
        {"content", out.str()}
    };
}

Json tool_create_file(const Project& p, Database& db, const Json& args) {
    const std::string raw_path = args.at("path").get<std::string>();
    const std::string content = args.at("content").get<std::string>();
    const std::string req_root = args.value("root", "");
    const bool overwrite = args.value("overwrite", false);
    const std::string actor = args.value("actor", "agent");

    std::string rkey, rel;
    fs::path abs_path = resolve_authorized_path(p, raw_path, req_root, false, &rkey, &rel);
    std::error_code ec;
    if (!overwrite && fs::exists(native_path(abs_path), ec)) {
        throw std::runtime_error("FILE_ALREADY_EXISTS (use overwrite:true): " + path_text(abs_path));
    }

    write_text(abs_path, content);
    const std::string digest = sha256(content);
    fs_sync_single_path(p, db, abs_path, true);
    int64_t rev = 0;
    db.begin();
    try {
        rev = db.change(actor, "create_file", {{"root", rkey}, {"path", rel}, {"bytes", content.size()}, {"sha256", digest}}, rel);
        db.commit();
    } catch (...) { db.rollback(); throw; }

    return {
        {"ok", true},
        {"root", rkey},
        {"path", rel},
        {"absolute_path", path_text(abs_path)},
        {"bytes", content.size()},
        {"sha256", digest},
        {"frn", static_cast<int64_t>(query_path_frn(abs_path))},
        {"revision", rev}
    };
}

Json tool_edit_file(const Project& p, Database& db, const Json& args) {
    const std::string mode = args.value("mode", args.value("action", "replace_content"));
    const bool dry_run = args.value("dry_run", false);
    const std::string actor = args.value("actor", "agent");
    const std::string expected_sha = args.value("expected_sha256", "");
    const int inject_fail_after = args.value("inject_failure_after_file", -1);

    struct FileEditPlan {
        std::string root_key;
        std::string rel_path;
        fs::path abs_path;
        std::string original;
        std::string updated;
        std::string old_sha;
        std::string new_sha;
        size_t replacements = 0;
    };

    std::vector<FileEditPlan> plans;

    auto apply_single_transform = [&](const std::string& orig, const Json& spec, size_t& out_reps) -> std::string {
        if (mode == "replace_content" || mode == "replace_in_files") {
            const std::string needle = spec.at("needle").get<std::string>();
            const std::string repl = spec.at("replacement").get<std::string>();
            const bool is_regex = spec.value("is_regex", false);
            const bool allow_multi = spec.value("allow_multiple_occurrences", mode == "replace_in_files");
            if (needle.empty()) throw std::runtime_error("needle vacío");
            if (is_regex) {
                std::regex re(needle, std::regex::ECMAScript);
                auto b = std::sregex_iterator(orig.begin(), orig.end(), re);
                auto e = std::sregex_iterator();
                out_reps = static_cast<size_t>(std::distance(b, e));
                if (out_reps == 0) {
                    if (mode == "replace_in_files") return orig;
                    throw std::runtime_error("NEEDLE_NOT_FOUND");
                }
                if (out_reps > 1 && !allow_multi) throw std::runtime_error("MULTIPLE_MATCHES: use allow_multiple_occurrences:true");
                return std::regex_replace(orig, re, repl);
            } else {
                size_t pos = 0;
                out_reps = 0;
                while ((pos = orig.find(needle, pos)) != std::string::npos) {
                    ++out_reps;
                    pos += needle.size();
                }
                if (out_reps == 0) {
                    if (mode == "replace_in_files") return orig;
                    throw std::runtime_error("NEEDLE_NOT_FOUND");
                }
                if (out_reps > 1 && !allow_multi) throw std::runtime_error("MULTIPLE_MATCHES: use allow_multiple_occurrences:true");
                std::string res = orig;
                pos = 0;
                while ((pos = res.find(needle, pos)) != std::string::npos) {
                    res.replace(pos, needle.size(), repl);
                    pos += repl.size();
                }
                return res;
            }
        } else if (mode == "replace_lines" || mode == "delete_lines" || mode == "insert_at_line") {
            bool crlf = false, trailing_nl = true;
            auto lines = split_text_lines(orig, &crlf, &trailing_nl);
            const int n = static_cast<int>(lines.size());
            if (mode == "insert_at_line") {
                int line_no = spec.at("line").get<int>();
                if (line_no < 1 || line_no > n + 1) throw std::runtime_error("LINE_OUT_OF_RANGE");
                auto ins_lines = split_text_lines(spec.at("content").get<std::string>());
                lines.insert(lines.begin() + (line_no - 1), ins_lines.begin(), ins_lines.end());
                out_reps = 1;
            } else {
                int s = spec.at("start_line").get<int>();
                int e = spec.at("end_line").get<int>();
                if (s < 1 || e < s || e > n) throw std::runtime_error("LINE_RANGE_INVALID");
                if (mode == "delete_lines") {
                    lines.erase(lines.begin() + (s - 1), lines.begin() + e);
                } else {
                    auto rep_lines = split_text_lines(spec.at("replacement").get<std::string>());
                    lines.erase(lines.begin() + (s - 1), lines.begin() + e);
                    lines.insert(lines.begin() + (s - 1), rep_lines.begin(), rep_lines.end());
                }
                out_reps = 1;
            }
            return join_text_lines(lines, crlf, trailing_nl);
        }
        throw std::runtime_error("edit_file.mode inválido: " + mode);
    };

    if (mode == "replace_in_files") {
        std::vector<fs::path> target_files;
        if (args.contains("paths") && args["paths"].is_array() && !args["paths"].empty()) {
            for (const auto& entry : args["paths"]) {
                target_files.push_back(resolve_authorized_path(p, entry.get<std::string>(), args.value("root", ""), true));
            }
        } else {
            // Find candidate files via ultra-scale fs_find_files!
            Json fq = {{"kind", "file"}, {"limit", std::clamp(args.value("max_files", 200), 1, 1000)}};
            for (const auto* k : {"root", "path_prefix", "glob", "ext"}) if (args.contains(k)) fq[k] = args[k];
            fq["include_absolute_path"] = true;
            auto found = fs_find_files(p, db, fq);
            for (const auto& it : found["items"]) {
                if (it.contains("absolute_path")) target_files.emplace_back(wide(it["absolute_path"].get<std::string>()));
            }
        }
        for (const auto& abs_p : target_files) {
            std::string rkey, rel;
            resolve_authorized_path(p, path_text(abs_p), "", true, &rkey, &rel);
            std::string orig = read_text(abs_p, 16 * 1024 * 1024);
            if (is_likely_binary(orig)) continue;
            size_t reps = 0;
            std::string upd = apply_single_transform(orig, args, reps);
            if (reps > 0 && upd != orig) {
                plans.push_back(FileEditPlan{rkey, rel, abs_p, orig, upd, sha256(orig), sha256(upd), reps});
            }
        }
    } else {
        std::string rkey, rel;
        fs::path abs_p = resolve_authorized_path(p, args.at("path").get<std::string>(), args.value("root", ""), true, &rkey, &rel);
        std::string orig = read_text(abs_p, 32 * 1024 * 1024);
        std::string old_sha = sha256(orig);
        if (!expected_sha.empty() && old_sha != expected_sha) {
            throw std::runtime_error("SHA256_MISMATCH: expected=" + expected_sha + " actual=" + old_sha);
        }
        size_t reps = 0;
        std::string upd = apply_single_transform(orig, args, reps);
        plans.push_back(FileEditPlan{rkey, rel, abs_p, orig, upd, old_sha, sha256(upd), reps});
    }

    if (dry_run) {
        Json preview = Json::array();
        for (const auto& pl : plans) {
            preview.push_back({{"root", pl.root_key}, {"path", pl.rel_path}, {"replacements", pl.replacements},
                               {"old_sha256", pl.old_sha}, {"new_sha256", pl.new_sha},
                               {"old_bytes", pl.original.size()}, {"new_bytes", pl.updated.size()}});
        }
        return {{"ok", true}, {"dry_run", true}, {"mode", mode}, {"files_matched", preview.size()}, {"files", std::move(preview)}};
    }

    // Transactional multi-file write with atomic rollback on ANY error!
    size_t written_count = 0;
    try {
        for (size_t i = 0; i < plans.size(); ++i) {
            write_text(plans[i].abs_path, plans[i].updated);
            written_count = i + 1;
            if (inject_fail_after >= 0 && static_cast<int>(written_count) >= inject_fail_after) {
                throw std::runtime_error("INJECTED_FAILURE_FOR_ROLLBACK_TEST");
            }
        }
    } catch (...) {
        // Atomic rollback of all previously written files
        for (size_t i = 0; i < written_count; ++i) {
            try { write_text(plans[i].abs_path, plans[i].original); } catch (...) {}
        }
        throw;
    }

    // Sync touched files into fs_nodes immediately + record change in history
    int64_t rev = 0;
    Json applied = Json::array();
    db.begin();
    try {
        for (const auto& pl : plans) {
            fs_sync_single_path(p, db, pl.abs_path, true);
            applied.push_back({{"root", pl.root_key}, {"path", pl.rel_path}, {"replacements", pl.replacements},
                               {"old_sha256", pl.old_sha}, {"new_sha256", pl.new_sha}});
        }
        rev = db.change(actor, "edit_file", {{"mode", mode}, {"files_modified", plans.size()}},
                        plans.size() == 1 ? plans[0].rel_path : "multi");
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }

    return {
        {"ok", true},
        {"mode", mode},
        {"files_modified", plans.size()},
        {"revision", rev},
        {"files", std::move(applied)}
    };
}

Json tool_search_pattern(const Project& p, Database& db, const Json& args, Cancel* cancel) {
    auto t0 = std::chrono::steady_clock::now();
    const std::string pattern = args.at("pattern").get<std::string>();
    if (pattern.empty()) throw std::runtime_error("search_pattern requiere pattern");
    const bool is_regex = args.value("is_regex", false);
    const bool case_sensitive = args.value("case_sensitive", true);
    const int ctx_default = std::clamp(args.value("context_lines", 0), 0, 20);
    const int ctx_before = std::clamp(args.value("context_before", ctx_default), 0, 20);
    const int ctx_after = std::clamp(args.value("context_after", ctx_default), 0, 20);
    const int max_per_file = std::clamp(args.value("max_matches_per_file", 20), 1, 200);
    const int max_total = std::clamp(args.value("limit", 100), 1, 1000);
    const size_t max_file_bytes = std::clamp<size_t>(args.value("max_file_bytes", size_t(4 * 1024 * 1024)), 4096, 32 * 1024 * 1024);
    const std::string include_glob = args.value("include_glob", args.value("glob", ""));
    const std::string exclude_glob = args.value("exclude_glob", "");
    const std::string path_prefix = normalize_rel_ft(args.value("path_prefix", ""));
    const std::string req_root = args.value("root", "");

    // Gather candidate files from fs_nodes (fast indexed list!) or fallback to files table / disk
    struct CandFile { std::string root_key; std::string rel; fs::path abs; };
    std::vector<CandFile> candidates;

    fs_schema(db);
    const bool has_fs = db.integer("SELECT EXISTS(SELECT 1 FROM fs_roots WHERE complete=1)") != 0;
    if (has_fs) {
        Json fq = {{"kind", "file"}, {"limit", 1000}, {"max_bytes", static_cast<int64_t>(max_file_bytes)}, {"include_absolute_path", true}};
        if (!req_root.empty()) fq["root"] = req_root;
        if (!path_prefix.empty()) fq["path_prefix"] = path_prefix;
        if (!include_glob.empty()) fq["glob"] = include_glob;
        if (args.contains("ext")) fq["ext"] = args["ext"];
        // Page up to 25,000 indexed candidate files cleanly
        std::string cursor;
        for (int page = 0; page < 25; ++page) {
            if (!cursor.empty()) fq["cursor"] = cursor;
            auto res = fs_find_files(p, db, fq);
            for (const auto& it : res["items"]) {
                std::string rel = it["path"].get<std::string>();
                if (!exclude_glob.empty() && fs_glob_match(exclude_glob, rel)) continue;
                candidates.push_back(CandFile{it["root"].get<std::string>(), rel, fs::path(wide(it["absolute_path"].get<std::string>()))});
            }
            if (!res.value("has_more", false) || res["next_cursor"].is_null()) break;
            cursor = res["next_cursor"].get<std::string>();
        }
    } else {
        // Fallback: use `files` table if populated, else direct recursive walk on roots
        auto frows = db.query("SELECT root, path, absolute_path FROM files WHERE deleted=0 LIMIT 25000");
        if (!frows.empty()) {
            for (const auto& r : frows) {
                std::string rk = r["root"].get<std::string>();
                std::string rel = normalize_rel_ft(r["path"].get<std::string>());
                if (!req_root.empty() && rk != req_root) continue;
                if (!path_prefix.empty() && !(rel == path_prefix || rel.starts_with(path_prefix + "/"))) continue;
                if (!include_glob.empty() && !fs_glob_match(include_glob, rel)) continue;
                if (!exclude_glob.empty() && fs_glob_match(exclude_glob, rel)) continue;
                candidates.push_back(CandFile{rk, rel, fs::path(wide(r["absolute_path"].get<std::string>()))});
            }
        } else {
            for (const auto& r : p.config["roots"]) {
                std::string rk = r["id"].get<std::string>();
                if (!req_root.empty() && rk != req_root) continue;
                fs::path rbase = p.resolve(r["path"]);
                std::error_code ec;
                for (fs::recursive_directory_iterator it(rbase, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                    if (ec) continue;
                    if (!it->is_regular_file(ec)) continue;
                    std::string rel = normalize_rel_ft(path_text(it->path().lexically_relative(rbase)));
                    if (rel.starts_with(".migratool/") || rel.starts_with(".git/")) continue;
                    if (!path_prefix.empty() && !(rel == path_prefix || rel.starts_with(path_prefix + "/"))) continue;
                    if (!include_glob.empty() && !fs_glob_match(include_glob, rel)) continue;
                    if (!exclude_glob.empty() && fs_glob_match(exclude_glob, rel)) continue;
                    candidates.push_back(CandFile{rk, rel, it->path()});
                    if (candidates.size() >= 25000) break;
                }
            }
        }
    }

    // Pre-compile regex or lowercase needle once
    std::regex compiled_re;
    if (is_regex) {
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (!case_sensitive) flags |= std::regex::icase;
        compiled_re = std::regex(pattern, flags);
    }
    const std::string needle_low = case_sensitive ? pattern : lower_ascii_ft(pattern);

    std::atomic<size_t> next_idx{0};
    std::atomic<int> total_matches{0};
    std::atomic<int> files_scanned{0};
    std::atomic<int> files_matched{0};
    std::mutex out_mu;
    Json matches = Json::array();

    const int n_threads = std::clamp<int>(static_cast<int>(std::thread::hardware_concurrency()), 2, 8);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(n_threads));

    for (int t = 0; t < n_threads; ++t) {
        workers.emplace_back([&]() {
            for (;;) {
                if ((cancel && cancel->requested) || total_matches.load() >= max_total) break;
                size_t idx = next_idx.fetch_add(1);
                if (idx >= candidates.size()) break;
                const auto& cand = candidates[idx];
                std::string text;
                try { text = read_text(cand.abs, max_file_bytes); } catch (...) { continue; }
                if (is_likely_binary(text)) continue;
                files_scanned.fetch_add(1);

                // Fast file-level pre-check for literal patterns before splitting lines!
                if (!is_regex) {
                    if (case_sensitive) {
                        if (text.find(pattern) == std::string::npos) continue;
                    } else {
                        if (lower_ascii_ft(text).find(needle_low) == std::string::npos) continue;
                    }
                }

                auto lines = split_text_lines(text);
                Json file_hits = Json::array();
                for (size_t li = 0; li < lines.size(); ++li) {
                    bool hit = false;
                    int col = 1;
                    if (is_regex) {
                        std::smatch m;
                        if (std::regex_search(lines[li], m, compiled_re)) {
                            hit = true;
                            col = static_cast<int>(m.position()) + 1;
                        }
                    } else {
                        size_t pos = case_sensitive ? lines[li].find(pattern) : lower_ascii_ft(lines[li]).find(needle_low);
                        if (pos != std::string::npos) {
                            hit = true;
                            col = static_cast<int>(pos) + 1;
                        }
                    }
                    if (!hit) continue;
                    Json item = {
                        {"root", cand.root_key},
                        {"path", cand.rel},
                        {"line", static_cast<int>(li + 1)},
                        {"column", col},
                        {"text", lines[li].size() > 300 ? lines[li].substr(0, 300) + "..." : lines[li]}
                    };
                    if (ctx_before > 0 || ctx_after > 0) {
                        Json ctx = Json::array();
                        size_t s = (li > static_cast<size_t>(ctx_before)) ? (li - static_cast<size_t>(ctx_before)) : 0;
                        size_t e = std::min(lines.size() - 1, li + static_cast<size_t>(ctx_after));
                        for (size_t k = s; k <= e; ++k) {
                            ctx.push_back(std::to_string(k + 1) + ": " + (lines[k].size() > 240 ? lines[k].substr(0, 240) : lines[k]));
                        }
                        item["context"] = std::move(ctx);
                    }
                    file_hits.push_back(std::move(item));
                    if (static_cast<int>(file_hits.size()) >= max_per_file) break;
                }
                if (!file_hits.empty()) {
                    files_matched.fetch_add(1);
                    std::lock_guard lk(out_mu);
                    for (auto& h : file_hits) {
                        if (static_cast<int>(matches.size()) >= max_total) break;
                        matches.push_back(std::move(h));
                        total_matches.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto& w : workers) if (w.joinable()) w.join();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return {
        {"schema", "migratool.search_pattern/1"},
        {"pattern", pattern},
        {"is_regex", is_regex},
        {"files_candidates", candidates.size()},
        {"files_scanned", files_scanned.load()},
        {"files_matched", files_matched.load()},
        {"returned", matches.size()},
        {"truncated", total_matches.load() >= max_total},
        {"elapsed_ms", elapsed_ms},
        {"items", std::move(matches)}
    };
}

} // namespace mt
