#include "migratool/workspace_registry.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/live.hpp"
#include "migratool/memories.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mt {
namespace {

std::string lower_ws(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string slugify(std::string_view name) {
    std::string out;
    out.reserve(std::min<size_t>(name.size(), 40));
    for (char c : name) {
        auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_') out.push_back(static_cast<char>(std::tolower(uc)));
        else if (!out.empty() && out.back() != '_') out.push_back('_');
        if (out.size() >= 40) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "workspace" : out;
}

std::string get_env_str(const wchar_t* name) {
    DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (!len) return {};
    std::wstring buf(len, L'\0');
    DWORD got = GetEnvironmentVariableW(name, buf.data(), len);
    if (!got || got >= len) return {};
    buf.resize(got);
    return utf8(buf);
}

fs::path find_registered_workspace_path(const std::string& selector) {
    if (selector.empty()) throw std::runtime_error("Se requiere workspace o path");
    fs::path maybe(wide(selector));
    std::error_code ec;
    if (maybe.is_absolute() && fs::is_regular_file(native_path(maybe), ec)) {
        return fs::weakly_canonical(maybe);
    }

    auto reg = registered_projects();
    const std::string low_sel = lower_ws(selector);
    std::string canon_dir_low;
    if (maybe.is_absolute() && fs::is_directory(native_path(maybe), ec)) {
        canon_dir_low = lower_ws(path_text(fs::weakly_canonical(maybe)));
    }

    for (const auto& proj : reg.value("projects", Json::array())) {
        if (!proj.value("available", false)) continue;
        std::string pname = lower_ws(proj.value("name", ""));
        std::string ppath = proj.value("path", "");
        if (pname == low_sel || lower_ws(ppath) == low_sel) {
            return fs::path(wide(ppath));
        }
        if (!canon_dir_low.empty()) {
            try {
                auto loaded = Project::load(fs::path(wide(ppath)));
                for (const auto& r : loaded.config.value("roots", Json::array())) {
                    if (lower_ws(path_text(loaded.resolve(r.value("path", "")))) == canon_dir_low) {
                        return fs::path(wide(ppath));
                    }
                }
            } catch (...) {}
        }
    }
    throw std::runtime_error("WORKSPACE_NOT_FOUND: " + selector);
}

Json detect_markers(const fs::path& root_dir) {
    static const std::vector<std::pair<std::string, std::string>> checks = {
        {"CMakeLists.txt", "cmake"},
        {"package.json", "node"},
        {"pyproject.toml", "python"},
        {"requirements.txt", "python"},
        {"Cargo.toml", "rust"},
        {"go.mod", "go"},
        {"pom.xml", "maven"},
        {"build.gradle", "gradle"},
        {"Makefile", "make"},
        {"AGENTS.md", "agents_instructions"},
        {"README.md", "readme"},
        {".gitignore", "gitignore"}
    };
    Json found = Json::array();
    std::error_code ec;
    for (const auto& [fname, kind] : checks) {
        if (fs::is_regular_file(native_path(root_dir / wide(fname)), ec)) {
            found.push_back({{"file", fname}, {"kind", kind}});
        }
    }
    return found;
}

} // namespace

fs::path default_workspaces_root() {
    if (auto ov = get_env_str(L"MIGRATOOL_WORKSPACES_DIR"); !ov.empty()) {
        return fs::absolute(fs::path(wide(ov)));
    }
    if (auto la = get_env_str(L"LOCALAPPDATA"); !la.empty()) {
        return fs::path(wide(la)) / L"Migratool" / L"workspaces";
    }
    return fs::temp_directory_path() / L"Migratool" / L"workspaces";
}

Json tool_activate_workspace(const Json& args, fs::path* out_project_file, Log log, Cancel* cancel) {
    const std::string raw = args.at("path").get<std::string>();
    if (raw.empty()) throw std::runtime_error("activate_workspace requiere path");

    fs::path target(wide(raw));
    std::error_code ec;
    fs::path project_file;
    bool created = false;

    if (target.is_absolute() && fs::is_regular_file(native_path(target), ec)) {
        project_file = fs::weakly_canonical(target);
    } else if (target.is_absolute() && fs::is_directory(native_path(target), ec)) {
        fs::path canon_dir = fs::weakly_canonical(target);
        const std::string requested_mode = lower_ws(args.value("mode", "workspace"));
        const bool prefer_external = args.value("external_state", requested_mode == "archive" || true);

        // Check if an existing project file is already registered for this root (matching mode if specified)
        bool found_existing = false;
        if (!args.value("force_new", false)) {
            if (!prefer_external && fs::is_regular_file(native_path(canon_dir / L"migration.json"), ec)) {
                project_file = canon_dir / L"migration.json";
                found_existing = true;
            } else {
                const std::string slug = slugify(utf8(canon_dir.filename().wstring()));
                const std::string hash8 = sha256(lower_ws(path_text(canon_dir))).substr(0, 10);
                fs::path ext_dir = default_workspaces_root() / wide(slug + "-" + hash8);
                fs::path ext_json = ext_dir / L"workspace.json";
                if (fs::is_regular_file(native_path(ext_json), ec)) {
                    project_file = ext_json;
                    found_existing = true;
                }
            }
        }

        if (!found_existing) {
            const std::string mode = (requested_mode == "archive" || requested_mode == "migration") ? requested_mode : "workspace";
            const std::string name = args.value("name", utf8(canon_dir.filename().wstring()));
            const std::string slug = slugify(name);
            const std::string hash8 = sha256(lower_ws(path_text(canon_dir))).substr(0, 10);

            fs::path ws_dir;
            if (args.contains("state_dir") && args["state_dir"].is_string() && !args["state_dir"].get<std::string>().empty()) {
                ws_dir = fs::absolute(fs::path(wide(args["state_dir"].get<std::string>())));
            } else {
                ws_dir = default_workspaces_root() / wide(slug + "-" + hash8);
            }
            fs::create_directories(ws_dir);
            project_file = ws_dir / L"workspace.json";
            fs::path state_dir = ws_dir / L"state";

            const std::string role = (mode == "archive") ? "archive" : "workspace";
            const std::string tier = args.value("index_tier", mode == "archive" ? "metadata" : "code");
            Json exclude = (mode == "archive")
                ? Json::array()
                : Json::array({".git", ".svn", ".hg", ".vs", ".migratool", "bin", "obj", "build", "out",
                               "node_modules", "__pycache__", ".venv", "venv", "target", "dist"});
            if (args.contains("exclude_dirs") && args["exclude_dirs"].is_array()) exclude = args["exclude_dirs"];

            Json cfg = {
                {"schema", "migratool.project/1"},
                {"mode", mode},
                {"index_tier", tier},
                {"name", name.empty() ? "workspace" : name},
                {"state_dir", path_text(state_dir)},
                {"roots", Json::array({{{"id", "main"}, {"role", role}, {"path", path_text(canon_dir)}}})},
                {"exclude_dirs", std::move(exclude)},
                {"defines", Json::array()},
                {"clang_args", Json::array()},
                {"analyzers", Json::array()},
                {"builds", Json::array()},
                {"cases", Json::array()},
                {"mappings", Json::array()}
            };
            if (args.contains("watch_ignore_globs") && args["watch_ignore_globs"].is_array()) {
                cfg["watch_ignore_globs"] = args["watch_ignore_globs"];
            }
            validate(cfg);
            write_json(project_file, cfg);
            created = true;
        }
    } else {
        // Resolve by registered workspace name
        project_file = find_registered_workspace_path(raw);
    }

    auto loaded = Project::load(project_file);
    LiveSession session(loaded, "MCP", "activate_workspace", args.value("actor", "agent"));

    Json sync_info = nullptr;
    if (args.value("sync", true)) {
        Database db(loaded);
        sync_info = fs_sync(loaded, db, {{"full", args.value("full", false)},
                                         {"threads", args.value("threads", 8)},
                                         {"build_trigram_fts", args.value("build_trigram_fts", true)}},
                            log, cancel);
    }
    if (out_project_file) *out_project_file = loaded.file;

    Json markers = Json::array();
    if (!loaded.config["roots"].empty()) {
        markers = detect_markers(loaded.resolve(loaded.config["roots"][0]["path"]));
    }
    Json result = {
        {"ok", true},
        {"created", created},
        {"name", loaded.config.value("name", "")},
        {"mode", project_mode(loaded)},
        {"project_path", path_text(loaded.file)},
        {"state_dir", path_text(loaded.state_dir())},
        {"roots", loaded.config["roots"]},
        {"markers", std::move(markers)},
        {"sync", std::move(sync_info)}
    };
    session.finish(0, result);
    return result;
}

Json tool_list_workspaces(const Json& args) {
    const std::string action = lower_ws(args.value("action", "list"));
    const bool available_only = args.value("available_only", false);
    Json prune_report = nullptr;
    if (action == "prune") {
        const bool dry_run = args.value("dry_run", false);
        prune_report = housekeep_registry({{"prune_missing_projects", true}}, dry_run);
    }

    auto reg = registered_projects();
    Json items = Json::array();
    int64_t unavailable_skipped = 0;
    for (const auto& proj : reg.value("projects", Json::array())) {
        const bool avail = proj.value("available", false);
        if (available_only && !avail) {
            ++unavailable_skipped;
            continue;
        }
        Json item = proj;
        if (avail) {
            try {
                auto loaded = Project::load(fs::path(wide(proj["path"].get<std::string>())));
                item["mode"] = project_mode(loaded);
                item["state_dir"] = path_text(loaded.state_dir());
                item["roots_count"] = loaded.config["roots"].size();
                std::error_code ec;
                auto dbp = loaded.state_dir() / L"catalog.sqlite";
                if (fs::is_regular_file(native_path(dbp), ec)) {
                    item["catalog_bytes"] = static_cast<int64_t>(fs::file_size(native_path(dbp), ec));
                    sqlite3* rdb = nullptr;
                    if (sqlite3_open_v2(path_text(dbp).c_str(), &rdb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
                        sqlite3_busy_timeout(rdb, 200);
                        sqlite3_stmt* st = nullptr;
                        if (sqlite3_prepare_v2(rdb, "SELECT COALESCE(SUM(total_files),0), COALESCE(SUM(total_dirs),0), COALESCE(SUM(total_bytes),0) FROM fs_roots", -1, &st, nullptr) == SQLITE_OK) {
                            if (sqlite3_step(st) == SQLITE_ROW) {
                                item["fs_files"] = sqlite3_column_int64(st, 0);
                                item["fs_dirs"] = sqlite3_column_int64(st, 1);
                                item["fs_bytes"] = sqlite3_column_int64(st, 2);
                            }
                            sqlite3_finalize(st);
                        }
                        sqlite3_close(rdb);
                    }
                }
            } catch (...) {
                item["mode"] = "unknown";
            }
        }
        items.push_back(std::move(item));
    }
    Json out = {
        {"schema", "migratool.workspaces/1"},
        {"workspaces_root", path_text(default_workspaces_root())},
        {"registry_path", reg.value("registry_path", "")},
        {"available_only", available_only},
        {"unavailable_skipped", unavailable_skipped},
        {"count", items.size()},
        {"items", std::move(items)}
    };
    if (!prune_report.is_null()) out["prune"] = std::move(prune_report);
    return out;
}

Json tool_query_workspace(const Json& args, Log log, Cancel* cancel) {
    const std::string ws_sel = args.at("workspace").get<std::string>();
    const std::string target_tool = args.at("tool").get<std::string>();
    Json target_args = args.value("arguments", Json::object());
    if (!target_args.is_object()) throw std::runtime_error("arguments debe ser un objeto JSON");

    static const std::set<std::string> allowed_ro = {
        "explore_tree", "find_files", "changes_feed", "search_pattern", "read_file",
        "find_duplicates", "verify_archive_vs_dir", "inspect_workspace_topology", "organize_verify",
        "query", "session_bootstrap", "migration_status", "project_context",
        "project_diagnostics", "next_tasks", "evidence_report", "history",
        "changes_since", "statistics", "get_task_context", "get_diagnostics",
        "find_implementations", "find_declaration"
    };
    bool ok_tool = allowed_ro.contains(target_tool);
    if (target_tool == "memory") {
        const std::string act = target_args.value("action", target_args.value("op", "list"));
        ok_tool = (act == "list" || act == "read" || act == "search");
    }
    if (!ok_tool) {
        throw std::runtime_error("QUERY_WORKSPACE_READONLY: herramienta no permitida en consulta federada: " + target_tool);
    }
    if (target_tool == "find_duplicates") {
        target_args["persist_cache"] = false;
    }

    fs::path pfile = find_registered_workspace_path(ws_sel);
    auto target_project = Project::load(pfile);
    auto res = agent_request(target_project, target_tool, target_args, log, cancel);
    return {
        {"schema", "migratool.query_workspace/1"},
        {"workspace", target_project.config.value("name", ws_sel)},
        {"project_path", path_text(target_project.file)},
        {"tool", target_tool},
        {"result", std::move(res)}
    };
}

Json tool_onboard_workspace(const Project& p, Database& db, const Json& args) {
    fs_schema(db);
    memories_schema(db);
    if (!args.value("skip_sync", false) && db.integer("SELECT EXISTS(SELECT 1 FROM fs_roots WHERE complete=1)") == 0) {
        fs_sync(p, db, {{"full", true}});
    }
    auto stats = fs_stats_summary(p, db);
    auto tree = fs_explore_tree(p, db, {{"depth", 2}, {"limit", 25}, {"include_samples", true}, {"sample_limit", 5}});

    Json markers = Json::array();
    if (!p.config["roots"].empty()) {
        markers = detect_markers(p.resolve(p.config["roots"][0]["path"]));
    }

    // Build concise Markdown onboarding summary
    std::ostringstream md;
    md << "# Onboarding: " << p.config.value("name", "workspace") << "\n\n";
    md << "- **Mode**: `" << project_mode(p) << "`\n";
    md << "- **Project file**: `" << path_text(p.file) << "`\n";
    md << "- **Indexed entries**: " << stats["total_files"] << " files, "
       << stats["total_dirs"] << " dirs, " << stats["total_links"] << " links ("
       << (stats["total_bytes"].get<int64_t>() / (1024 * 1024)) << " MiB)\n\n";

    if (!markers.empty()) {
        md << "## Detected Project Markers\n";
        for (const auto& m : markers) {
            md << "- `" << m["file"].get<std::string>() << "` (" << m["kind"].get<std::string>() << ")\n";
        }
        md << "\n";
    }

    md << "## Top-Level Structure\n";
    for (const auto& root_node : tree.value("items", Json::array())) {
        for (const auto& sd : root_node.value("subdirs", Json::array())) {
            md << "- `" << sd["path"].get<std::string>() << "/`: "
               << sd["total_files"] << " files, " << sd["total_dirs"] << " dirs, "
               << (sd["total_bytes"].get<int64_t>() / (1024 * 1024)) << " MiB";
            if (sd.contains("top_extensions") && sd["top_extensions"].is_object() && !sd["top_extensions"].empty()) {
                md << " | top exts: ";
                int n = 0;
                for (auto it = sd["top_extensions"].begin(); it != sd["top_extensions"].end() && n < 4; ++it, ++n) {
                    if (n) md << ", ";
                    md << it.key() << "(" << it.value() << ")";
                }
            }
            md << "\n";
        }
    }

    const std::string md_text = md.str();
    auto mem_res = memory_request(p, db, {
        {"action", "write"},
        {"name", "_onboarding"},
        {"content", md_text},
        {"tags", Json::array({"onboarding", "architecture"})},
        {"actor", args.value("actor", "agent")}
    });
    auto existing_mems = memory_request(p, db, {{"action", "list"}});

    return {
        {"schema", "migratool.onboarding/1"},
        {"name", p.config.value("name", "")},
        {"mode", project_mode(p)},
        {"stats", std::move(stats)},
        {"markers", std::move(markers)},
        {"tree", std::move(tree)},
        {"onboarding_memory", std::move(mem_res)},
        {"existing_memories", existing_mems["items"]}
    };
}

} // namespace mt
