#include "migratool/multiagent_coordinator.hpp"
#include "migratool/agent_supertools.hpp"
#include "migratool/core.hpp"
#include "migratool/gateway.hpp"
#include "migratool/git_intel.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mt;

namespace {

int g_checks = 0;

void check(bool cond, const std::string& msg) {
    ++g_checks;
    if (!cond) {
        throw std::runtime_error("FAILED CHECK #" + std::to_string(g_checks) + ": " + msg);
    }
}

void clear_readonly_recursive(const fs::path& dir) noexcept {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const DWORD attrs = GetFileAttributesW(native_path(it->path()).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(native_path(it->path()).c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        }
    }
}

struct TempDirGuard {
    fs::path path;
    ~TempDirGuard() {
        clear_readonly_recursive(path);
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string norm_slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool path_ends_with_or_matches(const std::string& candidate, const std::string& needle) {
    const std::string c = norm_slashes(candidate);
    const std::string n = norm_slashes(needle);
    if (c == n) return true;
    if (c.size() > n.size() && c.ends_with("/" + n)) return true;
    return c.find(n) != std::string::npos;
}

const Json& find_first_array(const Json& obj, std::initializer_list<const char*> keys) {
    static const Json kEmpty = Json::array();
    if (obj.is_array()) return obj;
    if (!obj.is_object()) return kEmpty;
    for (const char* k : keys) {
        if (obj.contains(k) && obj[k].is_array()) return obj[k];
    }
    return kEmpty;
}

fs::path locate_source_prd_json(int argc, wchar_t** argv) {
    std::vector<fs::path> candidates;
    if (argc > 1 && argv[1] != nullptr) {
        fs::path arg_p(argv[1]);
        if (fs::is_regular_file(arg_p)) candidates.push_back(arg_p);
        candidates.push_back(arg_p / L"docs" / L"PRODUCT_REQUIREMENTS_MASTER.json");
    }
    candidates.push_back(L"E:\\Proyectos\\Migratool\\docs\\PRODUCT_REQUIREMENTS_MASTER.json");
    candidates.push_back(fs::current_path() / L"docs" / L"PRODUCT_REQUIREMENTS_MASTER.json");
    candidates.push_back(fs::current_path().parent_path() / L"docs" / L"PRODUCT_REQUIREMENTS_MASTER.json");
    candidates.push_back(executable_dir().parent_path() / L"docs" / L"PRODUCT_REQUIREMENTS_MASTER.json");
    candidates.push_back(executable_dir().parent_path().parent_path() / L"docs" / L"PRODUCT_REQUIREMENTS_MASTER.json");

    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::is_regular_file(c, ec)) return c;
    }
    throw std::runtime_error("Could not locate docs/PRODUCT_REQUIREMENTS_MASTER.json");
}

void run_git_checked(const fs::path& git_exe, const fs::path& cwd, const std::vector<std::string>& args, const fs::path& log_dir) {
    std::vector<std::string> cmd;
    cmd.reserve(args.size() + 1);
    cmd.push_back(path_text(git_exe));
    cmd.insert(cmd.end(), args.begin(), args.end());
    const fs::path log_file = log_dir / wide("git_" + unique_id().substr(0, 8) + ".log");
    auto res = run_process(cmd, cwd, "", log_file, 30000);
    if (res.value("exit_code", -1) != 0) {
        throw std::runtime_error("Git command failed in " + path_text(cwd) + ": " + res.dump());
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const fs::path temp_base = fs::temp_directory_path() / wide("mt_multiagent_test_" + unique_id().substr(0, 8));
    TempDirGuard cleanup{temp_base};

    try {
        const fs::path git_exe = resolve_git_executable();
        check(!git_exe.empty() && fs::exists(git_exe), "Git executable resolved: " + path_text(git_exe));

        const fs::path git_logs = temp_base / L"git_logs";
        const fs::path repo_dir = temp_base / L"main_project";
        fs::create_directories(git_logs);
        fs::create_directories(repo_dir / L"src");
        fs::create_directories(repo_dir / L"include");
        fs::create_directories(repo_dir / L"tests");
        fs::create_directories(repo_dir / L"docs");

        // =====================================================================
        // 1. SETUP ISOLATED TEMPORARY GIT REPOSITORY & PROJECT (migration.json)
        // =====================================================================
        write_text(repo_dir / L"include" / L"app.hpp",
                   "#pragma once\n"
                   "namespace demo {\n"
                   "int compute_value(int x);\n"
                   "} // namespace demo\n");
        write_text(repo_dir / L"src" / L"main.cpp",
                   "#include \"../include/app.hpp\"\n"
                   "namespace demo { int compute_value(int x) { return x + 1; } }\n"
                   "int main() { return demo::compute_value(0); }\n");
        write_text(repo_dir / L"tests" / L"app_tests.cpp",
                   "#include \"../include/app.hpp\"\n"
                   "int main() { return demo::compute_value(41) == 42 ? 0 : 1; }\n");

        const fs::path src_prd = locate_source_prd_json(argc, argv);
        write_text(repo_dir / L"docs" / L"PRODUCT_REQUIREMENTS_MASTER.json", read_text(src_prd));

        run_git_checked(git_exe, repo_dir, {"init"}, git_logs);
        run_git_checked(git_exe, repo_dir, {"config", "user.email", "test@example.com"}, git_logs);
        run_git_checked(git_exe, repo_dir, {"config", "user.name", "Tester"}, git_logs);
        run_git_checked(git_exe, repo_dir, {"checkout", "-b", "main"}, git_logs);
        run_git_checked(git_exe, repo_dir, {"add", "."}, git_logs);
        run_git_checked(git_exe, repo_dir, {"commit", "-m", "Initial commit"}, git_logs);

        Project p = Project::create(
            repo_dir / L"migration.json",
            "MultiagentCoordinatorTest",
            Json::array({{{"id", "src"}, {"role", "source"}, {"path", path_text(repo_dir)}}})
        );
        agent_request(p, "inventory_sync", {{"full", true}});

        // Commit migration.json and add .migratool to .gitignore so main working tree stays clean
        write_text(repo_dir / L".gitignore", ".migratool/\nbuild_release/\nbuild_old_cache/\nold_experiments/\n");
        run_git_checked(git_exe, repo_dir, {"add", "migration.json", ".gitignore"}, git_logs);
        run_git_checked(git_exe, repo_dir, {"commit", "-m", "Add project config and gitignore"}, git_logs);

        {
            Database db(p);
            ensure_multiagent_coordinator_schema(db);
        }

        // =====================================================================
        // 2. TEST 1 — REQ-WORKTREE-001 (worktree_inventory & project_resume)
        // =====================================================================
        const fs::path wt_codex = temp_base / L"wt_agent_codex";
        run_git_checked(git_exe, repo_dir, {"worktree", "add", "-b", "feat/codex-module", path_text(wt_codex)}, git_logs);
        write_text(wt_codex / L"src" / L"codex_feature.cpp",
                   "#include \"../include/app.hpp\"\n"
                   "int codex_multi_worktree_engine() { return 101; }\n");
        run_git_checked(git_exe, wt_codex, {"add", "src/codex_feature.cpp"}, git_logs);
        run_git_checked(git_exe, wt_codex, {"commit", "-m", "Add codex_feature.cpp in feat/codex-module"}, git_logs);

        auto wt_scan = agent_request(p, "worktree_inventory", {{"action", "scan"}});
        const Json& wt_list = find_first_array(wt_scan, {"worktrees", "items"});
        check(wt_list.size() >= 2,
              "worktree_inventory(scan) discovered at least 2 Git worktrees: " + wt_scan.dump());

        bool found_main_wt = false;
        bool found_codex_wt = false;
        int64_t codex_ahead = 0;
        for (const auto& w : wt_list) {
            const std::string wpath = w.value("worktree_path", w.value("path", ""));
            const std::string wbranch = w.value("branch", "");
            if (wbranch == "main" || path_ends_with_or_matches(wpath, path_text(repo_dir))) {
                found_main_wt = true;
            }
            if (wbranch == "feat/codex-module" || path_ends_with_or_matches(wpath, "wt_agent_codex")) {
                found_codex_wt = true;
                codex_ahead = w.value("ahead_count", int64_t(0));
            }
        }
        check(found_main_wt && found_codex_wt,
              "worktree_inventory(scan) discovered both main worktree and wt_agent_codex");
        check(codex_ahead >= 1,
              "worktree_inventory(scan) detected ahead_count >= 1 on feat/codex-module (got " + std::to_string(codex_ahead) + ")");

        auto reg_codex = agent_request(p, "worktree_inventory", {
            {"action", "register"},
            {"worktree_path", path_text(wt_codex)},
            {"owner_agent", "Codex"},
            {"task_id", "TASK-101"},
            {"goal", "Implementar motor multi-worktree"},
            {"lifecycle_state", "CONFIRMED"},
            {"is_mandatory", true}
        });
        check(reg_codex.value("ok", true) == true,
              "worktree_inventory(register) registered wt_agent_codex in CONFIRMED state: " + reg_codex.dump());

        // Negative test: invalid lifecycle_state throws INVALID_LIFECYCLE_STATE
        bool threw_invalid_lifecycle = false;
        try {
            agent_request(p, "worktree_inventory", {
                {"action", "register"},
                {"worktree_path", path_text(wt_codex)},
                {"lifecycle_state", "DONE_MAYBE"}
            });
        } catch (const std::exception& ex) {
            threw_invalid_lifecycle = (std::string(ex.what()).find("INVALID_LIFECYCLE_STATE") != std::string::npos);
        }
        check(threw_invalid_lifecycle,
              "worktree_inventory(register) rejects invalid lifecycle_state 'DONE_MAYBE' with INVALID_LIFECYCLE_STATE");

        // Negative test: exclude_from_canonical=true without excluded_by and exclusion_reason throws EXCLUSION_REQUIRES_ACTOR_AND_REASON
        bool threw_exclusion_missing = false;
        try {
            agent_request(p, "worktree_inventory", {
                {"action", "register"},
                {"worktree_path", path_text(wt_codex)},
                {"exclude_from_canonical", true}
            });
        } catch (const std::exception& ex) {
            threw_exclusion_missing = (std::string(ex.what()).find("EXCLUSION_REQUIRES_ACTOR_AND_REASON") != std::string::npos);
        }
        check(threw_exclusion_missing,
              "worktree_inventory(register) rejects exclude_from_canonical=true without excluded_by and exclusion_reason with EXCLUSION_REQUIRES_ACTOR_AND_REASON");

        auto resume_pkt = agent_request(p, "project_resume", {{"task_query", "WORKTREE"}});
        check(resume_pkt.value("handover_ready", false) == true,
              "project_resume returns handover_ready == true: " + resume_pkt.dump());
        check(resume_pkt.contains("active_worktrees") && resume_pkt["active_worktrees"].is_array() &&
              !resume_pkt["active_worktrees"].empty(),
              "project_resume returns non-empty active_worktrees");
        check(resume_pkt.contains("unintegrated_mandatory_worktrees") &&
              resume_pkt["unintegrated_mandatory_worktrees"].is_array() &&
              !resume_pkt["unintegrated_mandatory_worktrees"].empty(),
              "project_resume returns non-empty unintegrated_mandatory_worktrees");
        check(resume_pkt.dump().find("REQ-WORKTREE-001") != std::string::npos,
              "project_resume includes task-relevant REQ-WORKTREE-001 requirement context");

        // =====================================================================
        // 3. TEST 2 — REQ-INTEGRATE-001 & REQ-BUILD-001
        //    (integrate_ready, Non-Destructive Conflict Check, Canonical Build Gate & trace_binary)
        // =====================================================================
        auto eval1 = agent_request(p, "integrate_ready", {{"action", "evaluate"}, {"target_branch", "main"}});
        check(eval1.value("canonical_build_ready", true) == false &&
              eval1.value("unintegrated_mandatory_count", int64_t(0)) >= 1,
              "integrate_ready(evaluate) sets canonical_build_ready=false while mandatory wt_agent_codex is unintegrated: " + eval1.dump());

        const Json& q_items1 = find_first_array(eval1, {"queue", "worktrees", "items", "evaluations"});
        bool codex_clean_ready = false;
        for (const auto& item : q_items1) {
            const std::string br = item.value("branch", "");
            const std::string wp = item.value("worktree_path", item.value("path", ""));
            if (br == "feat/codex-module" || path_ends_with_or_matches(wp, "wt_agent_codex")) {
                if (item.value("conflict_status", "") == "CLEAN" && item.value("status", "") == "READY") {
                    codex_clean_ready = true;
                }
            }
        }
        check(codex_clean_ready,
              "integrate_ready(evaluate) reports conflict_status=='CLEAN' and status=='READY' for wt_agent_codex: " + eval1.dump());

        // Hard Canonical Build Gate Test
        bool threw_canonical_gate = false;
        try {
            agent_request(p, "build_canonical", {
                {"action", "record"},
                {"lifecycle_state", "INTEGRATED"},
                {"requesting_agent", "Antigravity"}
            });
        } catch (const std::exception& ex) {
            threw_canonical_gate = (std::string(ex.what()).find("CANONICAL_BUILD_BLOCKED_UNINTEGRATED_WORKTREES") != std::string::npos);
        }
        check(threw_canonical_gate,
              "build_canonical(record, INTEGRATED) throws CANONICAL_BUILD_BLOCKED_UNINTEGRATED_WORKTREES when mandatory worktree is unintegrated");

        // Create second worktree wt_agent_conflict on branch feat/conflict modifying the same line of src/main.cpp
        const fs::path wt_conflict = temp_base / L"wt_agent_conflict";
        run_git_checked(git_exe, repo_dir, {"worktree", "add", "-b", "feat/conflict", path_text(wt_conflict)}, git_logs);

        // Commit modification to line 2-3 of src/main.cpp on main
        const std::string main_v2 =
            "#include \"../include/app.hpp\"\n"
            "namespace demo { int compute_value(int x) { return x + 100; /* main change */ } }\n"
            "int main() { return demo::compute_value(0); }\n";
        write_text(repo_dir / L"src" / L"main.cpp", main_v2);
        run_git_checked(git_exe, repo_dir, {"add", "src/main.cpp"}, git_logs);
        run_git_checked(git_exe, repo_dir, {"commit", "-m", "Update compute_value on main"}, git_logs);

        // Commit conflicting modification to line 2-3 of src/main.cpp on feat/conflict
        const std::string conflict_v2 =
            "#include \"../include/app.hpp\"\n"
            "namespace demo { int compute_value(int x) { return x * 999; /* conflicting change */ } }\n"
            "int main() { return demo::compute_value(0); }\n";
        write_text(wt_conflict / L"src" / L"main.cpp", conflict_v2);
        run_git_checked(git_exe, wt_conflict, {"add", "src/main.cpp"}, git_logs);
        run_git_checked(git_exe, wt_conflict, {"commit", "-m", "Conflicting compute_value on feat/conflict"}, git_logs);

        agent_request(p, "worktree_inventory", {
            {"action", "register"},
            {"worktree_path", path_text(wt_conflict)},
            {"owner_agent", "Claude"},
            {"task_id", "TASK-102"},
            {"goal", "Experimento con conflicto en main.cpp"},
            {"lifecycle_state", "CONFIRMED"},
            {"is_mandatory", true}
        });

        auto eval2 = agent_request(p, "integrate_ready", {{"action", "evaluate"}, {"target_branch", "main"}});
        const Json& q_items2 = find_first_array(eval2, {"queue", "worktrees", "items", "evaluations"});
        bool conflict_detected = false;
        for (const auto& item : q_items2) {
            const std::string br = item.value("branch", "");
            const std::string wp = item.value("worktree_path", item.value("path", ""));
            if (br == "feat/conflict" || path_ends_with_or_matches(wp, "wt_agent_conflict")) {
                if (item.value("conflict_status", "") == "CONFLICT" &&
                    item.value("status", "") == "BLOCKED_CONFLICT") {
                    conflict_detected = true;
                }
            }
        }
        check(conflict_detected,
              "integrate_ready(evaluate) non-destructively detected conflict_status=='CONFLICT' and status=='BLOCKED_CONFLICT' via git merge-tree: " + eval2.dump());

        // Verify main working tree is 100% clean and unaltered after merge-tree conflict check
        const std::string main_after_eval = read_text(repo_dir / L"src" / L"main.cpp");
        check(main_after_eval == main_v2 && main_after_eval.find("<<<<<<<") == std::string::npos,
              "Non-destructive git merge-tree left main working tree src/main.cpp 100% clean");

        // Exclude wt_agent_conflict with explicit audit trail
        auto excl_res = agent_request(p, "worktree_inventory", {
            {"action", "register"},
            {"worktree_path", path_text(wt_conflict)},
            {"exclude_from_canonical", true},
            {"excluded_by", "user_supervisor"},
            {"exclusion_reason", "Experimento descartado"}
        });
        check(excl_res.value("ok", true) == true,
              "Excluded wt_agent_conflict with explicit excluded_by and exclusion_reason");

        // Adversarial AUD-MIG-P0-001: mark_integrated MUST refuse unmerged branch & unknown worktree
        bool threw_unmerged_mark = false;
        try {
            agent_request(p, "integrate_ready", {
                {"action", "mark_integrated"},
                {"worktree_path", path_text(wt_codex)},
                {"branch", "feat/codex-module"},
                {"target_branch", "main"},
                {"integrated_by", "Antigravity"}
            });
        } catch (const std::exception& ex) {
            threw_unmerged_mark = (std::string(ex.what()).find("MARK_INTEGRATED_REFUSED_UNMERGED") != std::string::npos);
        }
        check(threw_unmerged_mark,
              "AUD-MIG-P0-001: integrate_ready(mark_integrated) refuses unmerged branch with MARK_INTEGRATED_REFUSED_UNMERGED");

        bool threw_unknown_mark = false;
        try {
            agent_request(p, "integrate_ready", {
                {"action", "mark_integrated"},
                {"worktree_path", path_text(temp_base / L"wt_does_not_exist")},
                {"target_branch", "main"}
            });
        } catch (const std::exception& ex) {
            threw_unknown_mark = (std::string(ex.what()).find("WORKTREE_NOT_FOUND") != std::string::npos);
        }
        check(threw_unknown_mark,
              "AUD-MIG-P0-001: integrate_ready(mark_integrated) refuses unknown worktree with WORKTREE_NOT_FOUND");

        // Merge feat/codex-module into main and mark wt_agent_codex as INTEGRATED (now verified via git merge-base --is-ancestor)
        run_git_checked(git_exe, repo_dir, {"merge", "--no-edit", "feat/codex-module"}, git_logs);
        auto mark_int = agent_request(p, "integrate_ready", {
            {"action", "mark_integrated"},
            {"worktree_path", path_text(wt_codex)},
            {"branch", "feat/codex-module"},
            {"target_branch", "main"},
            {"integrated_by", "Antigravity"}
        });
        check(mark_int.value("ok", true) == true &&
              mark_int.value("verification_mode", "") == "VERIFIED_GIT_ANCESTOR",
              "integrate_ready(mark_integrated) verified ancestry via Git and marked wt_agent_codex INTEGRATED: " + mark_int.dump());

        // Adversarial AUD-MIG-P1-003: Missing artifact rejected (zero synthetic hashes)
        bool threw_missing_art = false;
        try {
            agent_request(p, "build_canonical", {
                {"action", "record"},
                {"build_id", "bld-missing-artifact-adv"},
                {"lifecycle_state", "CONFIRMED"},
                {"requesting_agent", "Antigravity"},
                {"artifacts", Json::array({
                    {{"name", "ghost.exe"}, {"path", path_text(repo_dir / L"build_release" / L"ghost.exe")}, {"kind", "executable"}}
                })}
            });
        } catch (const std::exception& ex) {
            threw_missing_art = (std::string(ex.what()).find("BUILD_ARTIFACT_MISSING") != std::string::npos);
        }
        check(threw_missing_art,
              "AUD-MIG-P1-003: build_canonical refuses missing artifact with BUILD_ARTIFACT_MISSING (zero fake SHA-256)");

        // Create mock binary artifact build_release/Migratool-final.exe
        fs::create_directories(repo_dir / L"build_release");
        const fs::path final_exe = repo_dir / L"build_release" / L"Migratool-final.exe";
        write_text(final_exe, "MZ_CANONICAL_RELEASE_BINARY_V3_1_0_PAYLOAD");
        const std::string artifact_sha = hash_file(final_exe);

        // Adversarial AUD-MIG-P1-008: action="record" cannot claim DELIVERED without observed build execution
        bool threw_rec_deliv = false;
        try {
            agent_request(p, "build_canonical", {
                {"action", "record"},
                {"build_id", "bld-unobserved-deliv-adv"},
                {"lifecycle_state", "DELIVERED"},
                {"requesting_agent", "Antigravity"},
                {"artifacts", Json::array({
                    {{"name", "Migratool-final.exe"}, {"path", path_text(final_exe)}, {"kind", "executable"}, {"sha256", artifact_sha}}
                })}
            });
        } catch (const std::exception& ex) {
            threw_rec_deliv = (std::string(ex.what()).find("DELIVERED_REQUIRES_OBSERVED_BUILD") != std::string::npos);
        }
        check(threw_rec_deliv,
              "AUD-MIG-P1-008: build_canonical(record, DELIVERED) rejected with DELIVERED_REQUIRES_OBSERVED_BUILD");

        // Record genuine OBSERVED_BUILD DELIVERED canonical build via build_and_record(execute=true)
        auto build_rec = agent_request(p, "build_canonical", {
            {"action", "build_and_record"},
            {"execute", true},
            {"enforce_audit_gates", false},
            {"command", Json::array({"cmd.exe", "/c", "echo canonical-build-ok"})},
            {"lifecycle_state", "DELIVERED"},
            {"requesting_agent", "Antigravity"},
            {"build_dir", path_text(repo_dir / L"build_release")},
            {"compiler_id", "MSVC 14.44.35207"},
            {"build_config", "Release"},
            {"artifacts", Json::array({
                {
                    {"name", "Migratool-final.exe"},
                    {"path", path_text(final_exe)},
                    {"artifact_path", path_text(final_exe)},
                    {"kind", "executable"},
                    {"sha256", artifact_sha}
                }
            })}
        });
        const std::string build_id = build_rec.value("build_id", "");
        check(!build_id.empty(), "build_canonical(build_and_record) returned non-empty build_id: " + build_rec.dump());
        check(build_rec.value("provenance_mode", "") == "OBSERVED_BUILD",
              "AUD-MIG-P1-008: build_canonical(build_and_record, execute=true) recorded provenance_mode == 'OBSERVED_BUILD': " + build_rec.dump());
        check(build_rec.value("conservation_tier", "") == "PROTECTED",
              "build_canonical(DELIVERED) assigned conservation_tier == 'PROTECTED': " + build_rec.dump());
        check(!build_rec.value("manifest_sha256", "").empty(),
              "AUD-MIG-P1-004: build_canonical returned non-empty manifest_sha256");

        const fs::path manifest_path = p.state_dir() / L"builds" / wide(build_id) / L"manifest.json";
        check(fs::is_regular_file(manifest_path),
              "build_canonical wrote immutable manifest on disk at " + path_text(manifest_path));
        const std::string original_manifest_bytes = read_text(manifest_path);

        // Adversarial AUD-MIG-P1-004: Re-using an existing build_id MUST fail with BUILD_MANIFEST_IMMUTABLE_COLLISION
        bool threw_immutable_collision = false;
        try {
            agent_request(p, "build_canonical", {
                {"action", "record"},
                {"build_id", build_id},
                {"lifecycle_state", "CONFIRMED"},
                {"requesting_agent", "TamperAgent"},
                {"artifacts", Json::array({
                    {{"name", "Migratool-final.exe"}, {"path", path_text(final_exe)}, {"kind", "executable"}}
                })}
            });
        } catch (const std::exception& ex) {
            threw_immutable_collision = (std::string(ex.what()).find("BUILD_MANIFEST_IMMUTABLE_COLLISION") != std::string::npos);
        }
        check(threw_immutable_collision && read_text(manifest_path) == original_manifest_bytes,
              "AUD-MIG-P1-004: duplicate build_id rejected with BUILD_MANIFEST_IMMUTABLE_COLLISION and manifest bytes untouched");

        // Reverse Binary Traceability Test (trace_binary)
        auto trace_res = agent_request(p, "build_canonical", {
            {"action", "trace_binary"},
            {"sha256", artifact_sha}
        });
        const Json& trace_build = (trace_res.contains("build") && trace_res["build"].is_object())
            ? trace_res["build"]
            : ((trace_res.contains("manifest") && trace_res["manifest"].is_object()) ? trace_res["manifest"] : trace_res);
        const std::string traced_build_id = trace_res.value("build_id", trace_build.value("build_id", ""));
        const std::string traced_commit = trace_res.value("head_commit", trace_build.value("head_commit", ""));
        const std::string traced_agent = trace_res.value("requesting_agent", trace_build.value("requesting_agent", ""));
        const std::string traced_state = trace_res.value("lifecycle_state", trace_build.value("lifecycle_state", ""));

        check(traced_build_id == build_id,
              "trace_binary resolved exact build_id (" + traced_build_id + " == " + build_id + ")");
        check(!traced_commit.empty(),
              "trace_binary returned non-empty head_commit (" + traced_commit + ")");
        check(traced_agent == "Antigravity",
              "trace_binary returned requesting_agent == 'Antigravity' (got '" + traced_agent + "')");
        check(traced_state == "DELIVERED",
              "trace_binary returned lifecycle_state == 'DELIVERED' (got '" + traced_state + "')");

        // =====================================================================
        // 4. TEST 3 — REQ-STORAGE-001
        //    (storage_audit, Configurable Directory Policies & 4 Conservation Tiers)
        // =====================================================================
        fs::create_directories(repo_dir / L"old_experiments");
        write_text(repo_dir / L"old_experiments" / L"draft_notes.txt",
                   "Unclassified experiment that must NEVER be auto-cleaned without review.");

        fs::create_directories(repo_dir / L"build_old_cache");
        write_text(repo_dir / L"build_old_cache" / L"stale_artifact.obj",
                   std::string(4096, 'X'));

        auto audit_res = agent_request(p, "storage_audit", {{"action", "audit"}});
        const Json& dir_items = find_first_array(audit_res, {"directories", "items", "entries"});
        check(!dir_items.empty(), "storage_audit(audit) returned classified directories: " + audit_res.dump());

        auto find_dir_entry = [&](const std::string& name) -> Json {
            for (const auto& d : dir_items) {
                const std::string rel = d.value("rel_path", d.value("path", d.value("name", "")));
                if (rel == name || path_ends_with_or_matches(rel, name)) return d;
            }
            return Json::object();
        };

        const Json d_src = find_dir_entry("src");
        const Json d_inc = find_dir_entry("include");
        const Json d_tst = find_dir_entry("tests");
        const Json d_old = find_dir_entry("old_experiments");
        const Json d_bld = find_dir_entry("build_old_cache");

        check(!d_src.empty() && d_src.value("category", "") == "SOURCE" &&
              d_src.value("conservation_tier", "") == "PROTECTED",
              "storage_audit classified 'src' as SOURCE + PROTECTED: " + d_src.dump());
        check(!d_inc.empty() && d_inc.value("category", "") == "SOURCE" &&
              d_inc.value("conservation_tier", "") == "PROTECTED",
              "storage_audit classified 'include' as SOURCE + PROTECTED: " + d_inc.dump());
        check(!d_tst.empty() && d_tst.value("category", "") == "TEST_SOURCE" &&
              d_tst.value("conservation_tier", "") == "PROTECTED",
              "storage_audit classified 'tests' as TEST_SOURCE + PROTECTED: " + d_tst.dump());
        check(!d_old.empty() && d_old.value("category", "") == "UNKNOWN" &&
              d_old.value("conservation_tier", "") == "NEEDS_REVIEW",
              "storage_audit classified ambiguous 'old_experiments' as UNKNOWN + NEEDS_REVIEW (never VERIFIED_SAFE_CLEAN): " + d_old.dump());
        check(!d_bld.empty() && d_bld.value("category", "") == "BUILD_OUTPUT" &&
              d_bld.value("conservation_tier", "") == "VERIFIED_SAFE_CLEAN",
              "storage_audit classified 'build_old_cache' as BUILD_OUTPUT + VERIFIED_SAFE_CLEAN: " + d_bld.dump());

        const int64_t reclaimable = audit_res.value("reclaimable_bytes",
                                                    audit_res.value("reclaimable_storage_bytes", int64_t(0)));
        check(reclaimable > 0,
              "storage_audit reported reclaimable_bytes > 0 (" + std::to_string(reclaimable) + ")");

        // Negative test: clean_verified on NEEDS_REVIEW ('old_experiments') throws STORAGE_CLEAN_REFUSED_NON_SAFE_TIER
        bool threw_clean_old = false;
        try {
            agent_request(p, "storage_audit", {
                {"action", "clean_verified"},
                {"target_paths", Json::array({"old_experiments"})},
                {"execute", true}
            });
        } catch (const std::exception& ex) {
            threw_clean_old = (std::string(ex.what()).find("STORAGE_CLEAN_REFUSED_NON_SAFE_TIER") != std::string::npos);
        }
        check(threw_clean_old,
              "storage_audit(clean_verified) refuses NEEDS_REVIEW directory 'old_experiments' with STORAGE_CLEAN_REFUSED_NON_SAFE_TIER");
        check(fs::is_regular_file(repo_dir / L"old_experiments" / L"draft_notes.txt"),
              "'old_experiments/draft_notes.txt' remains 100% intact on disk after refused clean");

        // Negative test: clean_verified on PROTECTED ('tests') throws STORAGE_CLEAN_REFUSED_NON_SAFE_TIER
        bool threw_clean_tests = false;
        try {
            agent_request(p, "storage_audit", {
                {"action", "clean_verified"},
                {"target_paths", Json::array({"tests"})},
                {"execute", true}
            });
        } catch (const std::exception& ex) {
            threw_clean_tests = (std::string(ex.what()).find("STORAGE_CLEAN_REFUSED_NON_SAFE_TIER") != std::string::npos);
        }
        check(threw_clean_tests,
              "storage_audit(clean_verified) refuses PROTECTED directory 'tests' with STORAGE_CLEAN_REFUSED_NON_SAFE_TIER");
        check(fs::is_regular_file(repo_dir / L"tests" / L"app_tests.cpp"),
              "'tests/app_tests.cpp' remains 100% intact on disk after refused clean");

        // =====================================================================
        // 5. TEST 4 — REQ-DISCOVERY-001 (discover_replicas)
        // =====================================================================
        const fs::path orphan_dir = temp_base / L"orphan_copy";
        fs::create_directories(orphan_dir / L"src");
        fs::create_directories(orphan_dir / L"include");
        write_text(orphan_dir / L"src" / L"main.cpp", read_text(repo_dir / L"src" / L"main.cpp"));
        write_text(orphan_dir / L"include" / L"app.hpp", read_text(repo_dir / L"include" / L"app.hpp"));
        write_text(orphan_dir / L"src" / L"orphan_secret_feature.cpp",
                   "int recovered_orphan_work() { return 777; }\n");

        auto disc_res = agent_request(p, "discover_replicas", {
            {"action", "discover"},
            {"candidate_paths", Json::array({
                path_text(p.directory()),
                path_text(wt_codex),
                path_text(orphan_dir)
            })}
        });
        const Json& replicas = find_first_array(disc_res, {"replicas", "items", "candidates", "discovered"});
        check(replicas.size() >= 3,
              "discover_replicas evaluated all 3 candidate paths: " + disc_res.dump());

        std::string cls_main, cls_wt, cls_orphan;
        int64_t orphan_unique_cnt = 0;
        const std::string main_canon_str = path_text(canonical_win32_path(p.directory()));
        for (const auto& r : replicas) {
            const std::string rp = r.value("replica_path", r.value("path", ""));
            const std::string cls = r.value("classification", "");
            if (path_ends_with_or_matches(rp, main_canon_str) || path_ends_with_or_matches(rp, path_text(p.directory()))) {
                cls_main = cls;
            } else if (path_ends_with_or_matches(rp, "wt_agent_codex")) {
                cls_wt = cls;
            } else if (path_ends_with_or_matches(rp, "orphan_copy")) {
                cls_orphan = cls;
                orphan_unique_cnt = r.value("unique_or_modified_files_count", int64_t(0));
            }
        }
        check(cls_main == "MAIN_PROJECT",
              "discover_replicas classified p.directory() as MAIN_PROJECT (got '" + cls_main + "')");
        check(cls_wt == "RELATED_WORKTREE",
              "discover_replicas classified wt_codex as RELATED_WORKTREE (got '" + cls_wt + "')");
        check(cls_orphan == "ORPHAN_WORK" && orphan_unique_cnt >= 1,
              "discover_replicas classified orphan_copy as ORPHAN_WORK with unique_or_modified_files_count >= 1 (cls='" +
              cls_orphan + "', count=" + std::to_string(orphan_unique_cnt) + ")");

        // =====================================================================
        // 6. TEST 5 — REQ-TRACE-001 & REQ-GUI-001
        //    (project_health & Surgical Requirements Traceability)
        // =====================================================================
        auto sync_prd_res = agent_request(p, "project_health", {{"action", "sync_prd"}});
        check(sync_prd_res.value("ok", true) == true &&
              sync_prd_res.value("requirements_indexed", sync_prd_res.value("count", int64_t(1))) >= 10,
              "project_health(sync_prd) indexed PRODUCT_REQUIREMENTS_MASTER.json into SQLite: " + sync_prd_res.dump());

        auto q_req = agent_request(p, "project_health", {
            {"action", "query_requirements"},
            {"req_id", "REQ-BUILD-001"}
        });
        const Json& req_items = find_first_array(q_req, {"requirements", "items"});
        check(req_items.size() == 1 && req_items[0].value("req_id", "") == "REQ-BUILD-001",
              "project_health(query_requirements, req_id='REQ-BUILD-001') surgically returned only REQ-BUILD-001: " + q_req.dump());
        check(req_items[0].contains("invariants") && !req_items[0]["invariants"].empty() &&
              req_items[0].contains("acceptance_criteria") && !req_items[0]["acceptance_criteria"].empty() &&
              req_items[0].contains("trace_links") && !req_items[0]["trace_links"].empty(),
              "REQ-BUILD-001 query result includes invariants, acceptance_criteria, and trace_links: " + req_items[0].dump());

        auto health_sum = agent_request(p, "project_health", {{"action", "summary"}});
        check(health_sum.contains("pending_integrations_count") &&
              health_sum.contains("worktrees_with_pending_changes_count") &&
              health_sum.contains("old_builds_count") &&
              health_sum.contains("reclaimable_storage_bytes") &&
              health_sum.contains("last_canonical_build") &&
              health_sum.contains("requirements_coverage"),
              "project_health(summary) provides all low-cognitive-load executive supervision metrics: " + health_sum.dump());
        check(health_sum["reclaimable_storage_bytes"].get<int64_t>() > 0,
              "project_health(summary) reports accurate reclaimable_storage_bytes > 0");
        check(health_sum["last_canonical_build"].is_object() &&
              health_sum["last_canonical_build"].value("build_id", "") == build_id &&
              health_sum["last_canonical_build"].value("lifecycle_state", "") == "DELIVERED",
              "project_health(summary) reports accurate last_canonical_build (build_id=" + build_id + ")");

        // =====================================================================
        // 7. TEST 6 — DEEPENED MULTI-AGENT COORDINATOR CAPABILITIES
        //    (merge_execute, remove_worktree, prune_stale_admin, set_quota,
        //     OFFLINE_DRIVE replica discovery, session_pendings & 6-question trace_links)
        // =====================================================================
        // 6.1 integrate_ready(action="merge_execute", execute=true) on wt_auto_merge (feat/auto-merge)
        const fs::path wt_auto_merge = temp_base / L"wt_auto_merge";
        run_git_checked(git_exe, repo_dir, {"worktree", "add", "-b", "feat/auto-merge", path_text(wt_auto_merge)}, git_logs);
        write_text(wt_auto_merge / L"src" / L"auto_merge_feature.cpp",
                   "#include \"../include/app.hpp\"\n"
                   "int auto_merged_feature() { return 2026; }\n");
        run_git_checked(git_exe, wt_auto_merge, {"add", "src/auto_merge_feature.cpp"}, git_logs);
        run_git_checked(git_exe, wt_auto_merge, {"commit", "-m", "Add auto_merge_feature.cpp in feat/auto-merge"}, git_logs);

        agent_request(p, "worktree_inventory", {
            {"action", "register"},
            {"worktree_path", path_text(wt_auto_merge)},
            {"owner_agent", "Antigravity"},
            {"task_id", "TASK-206"},
            {"goal", "Auto-merge execution test"},
            {"lifecycle_state", "CONFIRMED"},
            {"is_mandatory", true}
        });

        auto merge_exec_res = agent_request(p, "integrate_ready", {
            {"action", "merge_execute"},
            {"worktree_path", path_text(wt_auto_merge)},
            {"target_branch", "main"},
            {"execute", true}
        });
        check(merge_exec_res.value("ok", false) == true &&
              merge_exec_res.value("executed", false) == true &&
              merge_exec_res.value("lifecycle_state", "") == "INTEGRATED" &&
              fs::is_regular_file(repo_dir / L"src" / L"auto_merge_feature.cpp"),
              "integrate_ready(merge_execute, execute=true) cleanly merged feat/auto-merge into main and promoted wt_auto_merge to INTEGRATED: " + merge_exec_res.dump());

        // 6.2 worktree_inventory(action="remove_worktree", execute=true) & prune_stale_admin
        auto rm_wt_res = agent_request(p, "worktree_inventory", {
            {"action", "remove_worktree"},
            {"worktree_path", path_text(wt_auto_merge)},
            {"execute", true}
        });
        check(rm_wt_res.value("ok", false) == true && rm_wt_res.value("executed", false) == true,
              "worktree_inventory(remove_worktree, execute=true) safely removed integrated wt_auto_merge: " + rm_wt_res.dump());

        auto prune_res = agent_request(p, "worktree_inventory", {{"action", "prune_stale_admin"}});
        check(prune_res.value("ok", false) == true && prune_res.value("action", "") == "prune_stale_admin",
              "worktree_inventory(prune_stale_admin) executed git worktree prune cleanly: " + prune_res.dump());

        // 6.3 storage_audit(action="set_quota", max_build_storage_gb=1, keep_last_n_builds=2) & audit verification
        auto set_quota_res = agent_request(p, "storage_audit", {
            {"action", "set_quota"},
            {"max_build_storage_gb", 1},
            {"keep_last_n_builds", 2}
        });
        check(set_quota_res.value("ok", false) == true &&
              set_quota_res.contains("quota_policy") &&
              set_quota_res["quota_policy"].value("keep_last_n_builds", int64_t(0)) == 2,
              "storage_audit(set_quota) persisted max_build_storage_gb=1 and keep_last_n_builds=2: " + set_quota_res.dump());

        auto audit_quota_res = agent_request(p, "storage_audit", {{"action", "audit"}});
        check(audit_quota_res.contains("quota_policy") &&
              audit_quota_res["quota_policy"].value("max_build_storage_bytes", int64_t(0)) == 1073741824LL &&
              audit_quota_res["quota_policy"].value("keep_last_n_builds", int64_t(0)) == 2,
              "storage_audit(audit) reflects configured quota_policy: " + audit_quota_res.dump());

        // 6.4 discover_replicas with non-existent path "Z:/OfflineDrive/Nortitech_Backup" -> OFFLINE_DRIVE
        auto offline_disc_res = agent_request(p, "discover_replicas", {
            {"action", "discover"},
            {"candidate_paths", Json::array({"Z:/OfflineDrive/Nortitech_Backup"})}
        });
        const Json& offline_replicas = find_first_array(offline_disc_res, {"replicas", "items"});
        bool found_offline_drive = false;
        for (const auto& r : offline_replicas) {
            const std::string rp = r.value("replica_path", r.value("path", ""));
            if (path_ends_with_or_matches(rp, "OfflineDrive/Nortitech_Backup")) {
                if (r.value("classification", "") == "OFFLINE_DRIVE") {
                    found_offline_drive = true;
                }
            }
        }
        check(found_offline_drive,
              "discover_replicas classified non-existent 'Z:/OfflineDrive/Nortitech_Backup' as OFFLINE_DRIVE without crashing: " + offline_disc_res.dump());

        // 6.5 project_health pendings_upsert -> pendings_close & 6-question trace_links post_change_status
        const int64_t reqs_before_pending = health_sum.value("requirements_indexed", int64_t(0));
        auto pend_up_res = agent_request(p, "project_health", {
            {"action", "pendings_upsert"},
            {"pending_id", "PEND-SES-001"},
            {"session_name", "PENDIENTES_SESION_2026_11"},
            {"owner_agent", "Codex"},
            {"title", "Verificar limpieza de artefactos duplicados"},
            {"linked_req_id", "REQ-BUILD-001"}
        });
        check(pend_up_res.value("ok", false) == true,
              "project_health(pendings_upsert) created session pending PEND-SES-001: " + pend_up_res.dump());

        auto pend_close_res = agent_request(p, "project_health", {
            {"action", "pendings_close"},
            {"pending_id", "PEND-SES-001"}
        });
        const auto health_after_pend = agent_request(p, "project_health", {{"action", "summary"}});
        const int64_t reqs_after_pending = health_after_pend.value("requirements_indexed", int64_t(0));
        check(pend_close_res.value("ok", false) == true &&
              reqs_after_pending == reqs_before_pending &&
              reqs_after_pending >= 10 &&
              health_after_pend.value("session_pendings_open_count", int64_t(-1)) == 0,
              "project_health(pendings_close) closed PEND-SES-001 while keeping product_requirements count 100% unchanged (" +
              std::to_string(reqs_after_pending) + ")");

        auto q_req_trace = agent_request(p, "project_health", {
            {"action", "query_requirements"},
            {"req_id", "REQ-BUILD-001"}
        });
        const Json& req_trace_items = find_first_array(q_req_trace, {"requirements", "items"});
        bool has_post_change_status = false;
        if (!req_trace_items.empty() && req_trace_items[0].contains("trace_links") && req_trace_items[0]["trace_links"].is_array()) {
            for (const auto& tl : req_trace_items[0]["trace_links"]) {
                if (tl.contains("post_change_status") && !tl.value("post_change_status", "").empty() &&
                    tl.contains("included_builds_json")) {
                    has_post_change_status = true;
                    break;
                }
            }
        }
        check(has_post_change_status,
              "project_health(query_requirements) trace_links includes post_change_status and included_builds_json: " + q_req_trace.dump());

        // =====================================================================
        // 8. TEST 7 — ADVERSARIAL PRODUCTION HARDENING REGRESSIONS
        //    (AUD-MIG-P0-019, P0-002, P1-007, P1-010, P1-011, P2-013, P2-014)
        // =====================================================================
        // 7.1 AUD-MIG-P0-019: merge_execute rejects dirty source worktree & wrong checked-out target branch
        const fs::path wt_adv_merge = temp_base / L"wt_adv_merge";
        run_git_checked(git_exe, repo_dir, {"worktree", "add", "-b", "feat/adv-merge", path_text(wt_adv_merge)}, git_logs);
        write_text(wt_adv_merge / L"src" / L"adv_feat.cpp", "int adv_feat() { return 11; }\n");
        run_git_checked(git_exe, wt_adv_merge, {"add", "src/adv_feat.cpp"}, git_logs);
        run_git_checked(git_exe, wt_adv_merge, {"commit", "-m", "Add adv_feat.cpp"}, git_logs);
        agent_request(p, "worktree_inventory", {
            {"action", "register"},
            {"worktree_path", path_text(wt_adv_merge)},
            {"owner_agent", "Codex"},
            {"task_id", "TASK-ADV-019"},
            {"lifecycle_state", "CONFIRMED"},
            {"is_mandatory", false}
        });
        // 7.1a Dirty source worktree -> MERGE_BLOCKED_DIRTY_SOURCE_WORKTREE
        const fs::path dirty_untracked = wt_adv_merge / L"src" / L"uncommitted_draft.cpp";
        write_text(dirty_untracked, "int draft() { return 0; }\n");
        bool threw_dirty_src = false;
        try {
            agent_request(p, "integrate_ready", {
                {"action", "merge_execute"},
                {"worktree_path", path_text(wt_adv_merge)},
                {"target_branch", "main"},
                {"execute", true}
            });
        } catch (const std::exception& ex) {
            threw_dirty_src = (std::string(ex.what()).find("MERGE_BLOCKED_DIRTY_SOURCE_WORKTREE") != std::string::npos);
        }
        DeleteFileW(native_path(dirty_untracked).c_str());
        check(threw_dirty_src,
              "AUD-MIG-P0-019: merge_execute rejects dirty source worktree with MERGE_BLOCKED_DIRTY_SOURCE_WORKTREE");

        // 7.1b Wrong checked-out branch in repo_dir ('wrong-branch' != 'main') -> MERGE_BLOCKED_WRONG_TARGET_BRANCH
        run_git_checked(git_exe, repo_dir, {"checkout", "-b", "wrong-branch"}, git_logs);
        bool threw_wrong_branch = false;
        try {
            agent_request(p, "integrate_ready", {
                {"action", "merge_execute"},
                {"worktree_path", path_text(wt_adv_merge)},
                {"target_branch", "main"},
                {"execute", true}
            });
        } catch (const std::exception& ex) {
            threw_wrong_branch = (std::string(ex.what()).find("MERGE_BLOCKED_WRONG_TARGET_BRANCH") != std::string::npos);
        }
        run_git_checked(git_exe, repo_dir, {"checkout", "main"}, git_logs);
        run_git_checked(git_exe, repo_dir, {"branch", "-D", "wrong-branch"}, git_logs);
        check(threw_wrong_branch,
              "AUD-MIG-P0-019: merge_execute rejects wrong checked-out target branch with MERGE_BLOCKED_WRONG_TARGET_BRANCH");

        // 7.2 AUD-MIG-P0-002: remove_worktree preserves catalog & directory when file handle is locked (no remove_all!)
        const fs::path wt_locked = temp_base / L"wt_adv_locked";
        run_git_checked(git_exe, repo_dir, {"worktree", "add", "-b", "feat/adv-locked", path_text(wt_locked)}, git_logs);
        agent_request(p, "worktree_inventory", {
            {"action", "register"},
            {"worktree_path", path_text(wt_locked)},
            {"owner_agent", "Codex"},
            {"task_id", "TASK-ADV-002"},
            {"lifecycle_state", "INTEGRATED"},
            {"is_mandatory", false}
        });
        const fs::path locked_file = wt_locked / L"src" / L"main.cpp";
        // Share READ so git status can hash the racy-git file cleanly (uncommitted == 0),
        // while omitting FILE_SHARE_DELETE so neither git worktree remove, Recycle Bin, nor MoveFileExW can delete/move it!
        HANDLE h_lock = CreateFileW(native_path(locked_file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(h_lock != INVALID_HANDLE_VALUE, "Opened non-deletable Win32 lock on wt_adv_locked/src/main.cpp");
        bool threw_rm_locked = false;
        std::string rm_locked_err;
        try {
            agent_request(p, "worktree_inventory", {
                {"action", "remove_worktree"},
                {"worktree_path", path_text(wt_locked)},
                {"execute", true}
            });
        } catch (const std::exception& ex) {
            rm_locked_err = ex.what();
            threw_rm_locked = (rm_locked_err.find("WORKTREE_REMOVE_FAILED") != std::string::npos);
        }
        const bool locked_dir_intact = fs::is_directory(native_path(wt_locked)) && fs::is_regular_file(native_path(locked_file));
        const Json inv_after_lock = agent_request(p, "worktree_inventory", {{"action", "list"}});
        bool locked_row_preserved = false;
        for (const auto& w : find_first_array(inv_after_lock, {"worktrees", "items"})) {
            if (path_ends_with_or_matches(w.value("worktree_path", ""), "wt_adv_locked")) {
                locked_row_preserved = true;
                break;
            }
        }
        CloseHandle(h_lock);
        check(threw_rm_locked && locked_dir_intact && locked_row_preserved,
              "AUD-MIG-P0-002: remove_worktree failed closed with WORKTREE_REMOVE_FAILED, keeping directory & catalog intact (threw=" +
              std::to_string(threw_rm_locked) + ", intact=" + std::to_string(locked_dir_intact) +
              ", row=" + std::to_string(locked_row_preserved) + ", err='" + rm_locked_err + "')");

        // 7.3 AUD-MIG-P1-007 & P2-014: >1500 generated files + hidden .cpp at end -> NEEDS_REVIEW (never VERIFIED_SAFE_CLEAN)
        const fs::path big_build = repo_dir / L"build_overflow_adv";
        fs::create_directories(big_build);
        for (int idx = 0; idx < 1550; ++idx) {
            char fname[32];
            snprintf(fname, sizeof(fname), "a_%04d.obj", idx);
            write_text(big_build / wide(fname), "obj");
        }
        write_text(big_build / L"zzzz_hidden_source.cpp", "int must_never_be_deleted() { return 999; }\n");
        const Json big_audit = agent_request(p, "storage_audit", {{"action", "audit"}});
        Json big_entry = Json::object();
        for (const auto& d : find_first_array(big_audit, {"directories", "items", "entries"})) {
            if (path_ends_with_or_matches(d.value("rel_path", d.value("path", "")), "build_overflow_adv")) {
                big_entry = d;
                break;
            }
        }
        check(!big_entry.empty() &&
              big_entry.value("conservation_tier", "") == "NEEDS_REVIEW" &&
              big_entry.value("auto_clean_allowed", true) == false &&
              big_entry.value("source_scan_complete", true) == false,
              "AUD-MIG-P1-007/P2-014: >1500-entry build dir with truncated source scan forced to NEEDS_REVIEW: " + big_entry.dump());

        // 7.4 AUD-MIG-P2-013: discover_replicas depth-2 recursion in search_roots + sample_complete field
        const fs::path search_pool = temp_base / L"search_pool_adv";
        const fs::path nested_clone = search_pool / L"org_sub" / L"nested_replica_adv";
        fs::create_directories(nested_clone / L"src");
        write_text(nested_clone / L"src" / L"main.cpp", read_text(repo_dir / L"src" / L"main.cpp"));
        write_text(nested_clone / L"src" / L"nested_unique.cpp", "int nested_unique() { return 1; }\n");
        const Json depth_disc = agent_request(p, "discover_replicas", {
            {"action", "discover"},
            {"search_roots", Json::array({path_text(search_pool)})},
            {"max_depth", 2}
        });
        bool found_nested = false;
        bool has_sample_complete = false;
        for (const auto& r : find_first_array(depth_disc, {"replicas", "items"})) {
            if (path_ends_with_or_matches(r.value("replica_path", r.value("path", "")), "nested_replica_adv")) {
                found_nested = true;
                has_sample_complete = r.contains("sample_complete") && r.contains("visited_entries");
                break;
            }
        }
        check(found_nested && has_sample_complete,
              "AUD-MIG-P2-013: discover_replicas discovered depth-2 nested replica and reported sample_complete/visited_entries: " + depth_disc.dump());

        // 7.5 AUD-MIG-P1-010 & P1-011: Honest verified_coverage_pct vs file_presence_pct + decoy cwd rejection
        {
            Database db(p);
            runs_telemetry_schema(db);
            // Decoy run whose cwd mentions 'multiagent_coordinator' while targeting an unrelated suite!
            db.exec(
                "INSERT INTO verified_runs("
                "id,at_iso,actor,workstream_id,selection_mode,selected_targets_json,command_json,env_profile,cwd,"
                "exit_code,duration_ms,status,passed_count,failed_count,errors_json,raw_bytes,distilled_bytes,log_path,git_head)"
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                {"run-decoy-substr", now(), "DecoyAgent", "", "explicit",
                 "[\"totally_unrelated_suite\"]", "[\"ctest\",\"-R\",\"totally_unrelated_suite\"]",
                 "", path_text(repo_dir) + "/multiagent_coordinator/decoy",
                 0, 50, "PASSED", 5, 0, "[]", 64, 16, "", "deadbeef"});
        }
        agent_request(p, "project_health", {{"action", "sync_prd"}});
        const Json honest_health = agent_request(p, "project_health", {{"action", "summary"}});
        const Json& cov = honest_health.at("requirements_coverage");
        check(cov.contains("file_presence_pct") &&
              cov.contains("verified_coverage_pct") &&
              cov.contains("test_verified_links_count") &&
              cov.value("test_verified_links_count", int64_t(-1)) == 0 &&
              cov.value("verified_coverage_pct", -1.0) == 0.0,
              "AUD-MIG-P1-010/P1-011: decoy cwd substring did not fabricate verified_coverage_pct (must be 0.0): " + cov.dump());

        // Cleanly detach Git worktrees before TempDirGuard removes the temp directory
        {
            std::vector<std::string> rm1 = {path_text(git_exe), "worktree", "remove", "--force", path_text(wt_codex)};
            std::vector<std::string> rm2 = {path_text(git_exe), "worktree", "remove", "--force", path_text(wt_conflict)};
            std::vector<std::string> rm3 = {path_text(git_exe), "worktree", "remove", "--force", path_text(wt_adv_merge)};
            std::vector<std::string> rm4 = {path_text(git_exe), "worktree", "remove", "--force", path_text(wt_locked)};
            run_process(rm1, repo_dir, "", git_logs / L"rm1.log", 15000);
            run_process(rm2, repo_dir, "", git_logs / L"rm2.log", 15000);
            run_process(rm3, repo_dir, "", git_logs / L"rm3.log", 15000);
            run_process(rm4, repo_dir, "", git_logs / L"rm4.log", 15000);
        }

        std::cout << "multiagent_coordinator_tests PASSED (" << g_checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "multiagent_coordinator_tests FAILED: " << e.what() << "\n";
        return 1;
    }
}
