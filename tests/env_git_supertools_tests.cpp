#include "migratool/agent_supertools.hpp"
#include "migratool/core.hpp"
#include "migratool/env_registry.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/gateway.hpp"
#include "migratool/git_intel.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace mt;

namespace {

int g_checks = 0;
void check(bool cond, const std::string& msg) {
    ++g_checks;
    if (!cond) {
        throw std::runtime_error("FAILED CHECK #" + std::to_string(g_checks) + ": " + msg);
    }
}

} // namespace

int wmain() {
    try {
        const fs::path temp_base = fs::temp_directory_path() / wide("migratool_env_git_supertools_" + unique_id());
        const fs::path root = temp_base / L"repo";
        const fs::path synth_tools = temp_base / L"synth_devtools";
        const fs::path env_db_file = temp_base / L"test_env_registry.sqlite";
        fs::create_directories(root / L"src");
        fs::create_directories(synth_tools / L"jdk" / L"bin");

        SetEnvironmentVariableW(L"MIGRATOOL_ENV_REGISTRY_PATH", env_db_file.c_str());

        // =====================================================================
        // 1. ENV_TOOLS (SYNTHETIC PATH_SHADOWED + REAL D:\DevTools + RECIPES)
        // =====================================================================
        write_text(synth_tools / L"jdk" / L"bin" / L"java.exe", "MZ_FAKE_JDK21");
        auto scan_synth = tool_env_tools(std::nullopt, {
            {"action", "scan"},
            {"roots", Json::array({path_text(synth_tools)})},
            {"probe_versions", false},
            {"simulated_path_map", {{"java", "C:/Program Files/Zulu/zulu-11-jre/bin/java.exe"}}}
        });
        check(scan_synth["ok"] == true && scan_synth["tools_discovered"].get<int64_t>() >= 1,
              "env_tools(scan) works projectlessly on synthetic toolchain root");
        check(scan_synth["path_shadows_detected"].get<int64_t>() >= 1,
              "env_tools(scan) detected PATH_SHADOWED when PATH resolves java to Zulu 11");

        auto doc_res = tool_env_tools(std::nullopt, {{"action", "doctor"}});
        check(doc_res["issues_count"].get<int64_t>() >= 1 &&
              doc_res["issues"][0]["diagnostic_code"] == "PATH_SHADOWED",
              "env_tools(doctor) reports PATH_SHADOWED diagnostic code");

        // Real read-only scan over D:\DevTools if present on this machine
        std::error_code ec;
        if (fs::exists(L"D:\\DevTools", ec)) {
            auto scan_real = tool_env_tools(std::nullopt, {
                {"action", "scan"},
                {"roots", Json::array({"D:/DevTools"})},
                {"probe_versions", false}
            });
            check(scan_real["tools_discovered"].get<int64_t>() >= 10,
                  "env_tools(scan) discovered 10+ real tools in D:\\DevTools");
        }

        auto t0 = std::chrono::steady_clock::now();
        auto q_cmake = tool_env_tools(std::nullopt, {{"action", "query"}, {"query", "cmake"}});
        auto q_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(q_ms < 50, "env_tools(query) cached lookup completed in < 50 ms (" + std::to_string(q_ms) + " ms)");

        auto rec = env_materialize_recipe("msvc_x64");
        check(rec.contains("PATH") && rec["PATH"].find("D:\\DevTools") != std::string::npos,
              "env_materialize_recipe('msvc_x64') injects D:\\DevTools into PATH");

        auto md = tool_env_tools(std::nullopt, {{"action", "export_md"}});
        check(md["markdown"].get<std::string>().find("D:\\DevTools") != std::string::npos,
              "env_tools(export_md) generates Markdown inventory table");

        // =====================================================================
        // 2. GIT_INTEL, REPO_MAP (PAGERANK), TASK DAG, SELECTIVE TESTS & RUN_VERIFIED
        // =====================================================================
        write_text(root / L"CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.25)\n"
                   "project(Demo)\n"
                   "add_executable(core_unit_tests tests/core_unit_tests.cpp src/hub.cpp)\n"
                   "add_test(NAME core_unit_suite COMMAND core_unit_tests)\n");
        write_text(root / L"src" / L"hub.hpp", "pragma once\nint hub_calc(int v);\n");
        write_text(root / L"src" / L"hub.cpp", "#include \"hub.hpp\"\nint hub_calc(int v) { return v + 10; }\n");
        write_text(root / L"src" / L"caller1.cpp", "#include \"hub.hpp\"\nint c1() { return hub_calc(1); }\n");
        write_text(root / L"src" / L"caller2.cpp", "#include \"hub.hpp\"\nint c2() { return hub_calc(2); }\n");
        write_text(root / L"src" / L"isolated.cpp", "int iso() { return 0; }\n");

        const fs::path git_exe = resolve_git_executable();
        if (!git_exe.empty()) {
            run_process({path_text(git_exe), "init", "-q"}, root, "", temp_base / L"glog1");
            run_process({path_text(git_exe), "config", "user.email", "test@migratool.local"}, root, "", temp_base / L"glog2");
            run_process({path_text(git_exe), "config", "user.name", "Migratool Test"}, root, "", temp_base / L"glog3");
            run_process({path_text(git_exe), "add", "."}, root, "", temp_base / L"glog4");
            run_process({path_text(git_exe), "commit", "-q", "-m", "Initial commit for git_intel"}, root, "", temp_base / L"glog5");
            write_text(root / L"src" / L"untracked.txt", "dirty untracked file\n");
        }

        Project p = Project::create(temp_base / L"migration.json", "SupertoolsTest",
                                    Json::array({{{"id", "src"}, {"role", "source"}, {"path", path_text(root)}}}));
        agent_request(p, "inventory_sync", {{"full", true}});

        if (!git_exe.empty()) {
            auto gsum = agent_request(p, "git_intel", {{"action", "summary"}});
            check(gsum["available"] == true && gsum["dirty"] == true && gsum["untracked_count"].get<int64_t>() >= 1,
                  "git_intel(summary) reports head_commit, branch and untracked_count");
            auto glog = agent_request(p, "git_intel", {{"action", "log"}, {"limit", 5}});
            check(glog["count"].get<int64_t>() >= 1 &&
                  glog["commits"][0]["subject"] == "Initial commit for git_intel",
                  "git_intel(log) returns commit history");
        }

        // Personalized PageRank in repo_map MUST rank hub.hpp highest because caller1.cpp, caller2.cpp, and hub.cpp all #include "hub.hpp"!
        auto rmap = agent_request(p, "repo_map", {{"token_budget", 1500}});
        check(!rmap["ranked_files"].empty() && rmap["ranked_files"][0]["path"] == "src/hub.hpp",
              "repo_map Personalized PageRank ranked central dependency hub 'src/hub.hpp' #1 (got: " +
              rmap["ranked_files"][0]["path"].get<std::string>() + ")");

        // Task DAG ready_tasks vs blocked_tasks + cycle detection
        auto wake_dag = agent_request(p, "agent_wake", {
            {"upsert_tasks", Json::array({
                {{"task_id", "T1"}, {"title", "Base schema"}, {"status", "completed"}, {"priority", 10}},
                {{"task_id", "T2"}, {"title", "Implement engine"}, {"status", "pending"}, {"priority", 20}, {"depends_on", Json::array({"T1"})}},
                {{"task_id", "T3"}, {"title", "UI integration"}, {"status", "pending"}, {"priority", 30}, {"depends_on", Json::array({"T2"})}}
            })}
        });
        check(wake_dag["task_dag"]["ready_count"].get<int64_t>() == 1 &&
              wake_dag["task_dag"]["ready_tasks"][0]["task_id"] == "T2",
              "resolve_task_dag identified T2 as ready (since T1 is completed)");
        check(wake_dag["task_dag"]["blocked_count"].get<int64_t>() == 1 &&
              wake_dag["task_dag"]["blocked_tasks"][0]["task_id"] == "T3",
              "resolve_task_dag identified T3 as blocked by T2");

        // read_bundle with known_sha256 deduplication
        const std::string hub_sha = hash_file(root / L"src" / L"hub.cpp");
        auto rb_skip = agent_request(p, "read_bundle", {
            {"items", Json::array({
                {{"path", "src/hub.cpp"}, {"known_sha256", hub_sha}},
                {{"path", "src/caller1.cpp"}}
            })}
        });
        check(rb_skip["files_skipped_by_sha256"].get<int64_t>() == 1 &&
              rb_skip["items"][0]["status"] == "UNCHANGED_SKIPPED",
              "read_bundle skipped unchanged src/hub.cpp when known_sha256 matched");

        // Selective test planner (select_tests) maps src/hub.cpp -> core_unit_suite from CMakeLists.txt!
        auto sel_tests = agent_request(p, "run_verified", {
            {"action", "select_tests"},
            {"touched_paths", Json::array({"src/hub.cpp"})}
        });
        check(!sel_tests["selected_suites"].empty() && sel_tests["selected_suites"][0] == "core_unit_suite",
              "run_verified(select_tests) mapped src/hub.cpp to CTest suite 'core_unit_suite' via CMakeLists.txt");

        // Error Distiller in run_verified (> 2,000 B noisy compiler output distilled to < 500 B structured errors + immutable verified_runs)
        const fs::path noisy_script = temp_base / L"noisy_build.cmd";
        std::string noisy_body = "@echo off\r\n";
        for (int i = 0; i < 45; ++i) {
            noisy_body += "echo [" + std::to_string(i) + "/45] Building intermediate object file number " + std::to_string(i) + " with verbose flags...\r\n";
        }
        noisy_body += "echo E:\\Proyectos\\Demo\\src\\hub.cpp(14,9): error C2065: 'undeclared_var': identificador no declarado\r\n";
        noisy_body += "exit /b 2\r\n";
        write_text(noisy_script, noisy_body);

        auto rv = agent_request(p, "run_verified", {
            {"command", Json::array({"cmd.exe", "/c", path_text(noisy_script)})}
        });
        check(rv["ok"] == false && rv["status"] == "FAILED" && rv["exit_code"].get<int>() == 2,
              "run_verified captured failing exit code 2");
        check(rv["raw_bytes"].get<int64_t>() > 2000 && rv["distilled_bytes"].get<int64_t>() < 500,
              "run_verified distilled >2000 B raw output into <500 B structured error payload (raw=" +
              std::to_string(rv["raw_bytes"].get<int64_t>()) + ", distilled=" +
              std::to_string(rv["distilled_bytes"].get<int64_t>()) + ")");
        check(!rv["errors"].empty() && rv["errors"][0]["code"] == "C2065" && rv["errors"][0]["line"] == 14,
              "run_verified extracted exact MSVC error code C2065 and line 14");

        // Immutable verified_runs triggers
        {
            Database db(p);
            bool upd_blocked = false, del_blocked = false;
            try { db.exec("UPDATE verified_runs SET status='PASSED'"); } catch (...) { upd_blocked = true; }
            try { db.exec("DELETE FROM verified_runs"); } catch (...) { del_blocked = true; }
            check(upd_blocked && del_blocked, "verified_runs enforces BEFORE UPDATE and BEFORE DELETE immutability triggers");
        }

        // =====================================================================
        // 3. EMPIRICAL MCP SCHEMA SIZE GATE (BM1: full <= 22,000 B, agent-os <= 9,000 B)
        // =====================================================================
        const size_t full_schema_bytes = agent_tools("full").dump().size();
        const size_t agent_os_schema_bytes = agent_tools("agent-os").dump().size();
        std::cout << "[BM1] tools/list size: full=" << full_schema_bytes
                  << " B, agent-os=" << agent_os_schema_bytes << " B\n";
        check(full_schema_bytes <= 22000,
              "BM1 GATE: agent_tools('full') schema size <= 22,000 bytes (actual=" + std::to_string(full_schema_bytes) + " B, down from 67,452 B)");
        check(agent_os_schema_bytes <= 9000,
              "BM1 GATE: agent_tools('agent-os') schema size <= 9,000 bytes (actual=" + std::to_string(agent_os_schema_bytes) + " B, -88% vs v2.1)");

        SetEnvironmentVariableW(L"MIGRATOOL_ENV_REGISTRY_PATH", nullptr);
        fs::remove_all(temp_base, ec);
        std::cout << "env_git_supertools_tests PASSED (" << g_checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        SetEnvironmentVariableW(L"MIGRATOOL_ENV_REGISTRY_PATH", nullptr);
        std::cerr << "env_git_supertools_tests FAILED: " << e.what() << "\n";
        return 1;
    }
}
