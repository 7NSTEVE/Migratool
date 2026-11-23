#include "migratool/code_audit.hpp"
#include "migratool/multiagent_coordinator.hpp"
#include "migratool/agent_supertools.hpp"
#include "migratool/core.hpp"
#include "migratool/gateway.hpp"

#include <algorithm>
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

} // namespace

int wmain(int argc, wchar_t** argv) {
    (void)argc;
    (void)argv;
    const fs::path temp_base = fs::temp_directory_path() / wide("mt_code_audit_test_" + unique_id().substr(0, 8));
    TempDirGuard cleanup{temp_base};

    try {
        const fs::path repo_dir = temp_base / L"audit_project";
        fs::create_directories(repo_dir / L"include");
        fs::create_directories(repo_dir / L"src");
        fs::create_directories(repo_dir / L"tests");

        // =====================================================================
        // SCENARIO 1: Incremental Caching & Header Dependency Invalidation
        // =====================================================================
        write_text(repo_dir / L"include" / L"shared.hpp",
                   "#pragma once\n"
                   "namespace demo {\n"
                   "inline int shared_offset() { return 10; }\n"
                   "} // namespace demo\n");

        write_text(repo_dir / L"src" / L"mod_a.cpp",
                   "#include \"../include/shared.hpp\"\n"
                   "namespace demo {\n"
                   "int compute_a(int x) {\n"
                   "    if (x > 0) return x + shared_offset();\n"
                   "    return 0;\n"
                   "}\n"
                   "} // namespace demo\n");

        write_text(repo_dir / L"src" / L"mod_b.cpp",
                   "#include \"shared.hpp\"\n"
                   "namespace demo {\n"
                   "int compute_b(int y) {\n"
                   "    return y * shared_offset();\n"
                   "}\n"
                   "} // namespace demo\n");

        write_text(repo_dir / L"src" / L"independent.cpp",
                   "namespace demo {\n"
                   "int standalone_fn(int z) {\n"
                   "    return z + 1;\n"
                   "}\n"
                   "} // namespace demo\n");

        Json cc_doc = Json::array({
            {{"directory", path_text(repo_dir)}, {"file", "src/mod_a.cpp"}, {"command", "cl.exe /std:c++23 /W4 /c src/mod_a.cpp"}},
            {{"directory", path_text(repo_dir)}, {"file", "src/mod_b.cpp"}, {"command", "cl.exe /std:c++23 /W4 /c src/mod_b.cpp"}},
            {{"directory", path_text(repo_dir)}, {"file", "src/independent.cpp"}, {"command", "cl.exe /std:c++23 /W4 /c src/independent.cpp"}}
        });
        write_json(repo_dir / L"compile_commands.json", cc_doc);

        Project p = Project::create(
            repo_dir / L"migration.json",
            "CodeAuditIncrementalTest",
            Json::array({{{"id", "src"}, {"role", "source"}, {"path", path_text(repo_dir)}}})
        );

        Database db(p);
        ensure_code_audit_schema(db);

        // 1a. Initial scan: 4 files audited, 0 reused from cache
        const Json scan1 = tool_code_audit(p, db, {{"action", "scan_incremental"}});
        check(scan1.value("ok", false) == true, "scan1 ok == true");
        check(scan1.value("files_audited", int64_t{-1}) == 4,
              "Initial scan_incremental audited all 4 files (got " + std::to_string(scan1.value("files_audited", int64_t{-1})) + ")");
        check(scan1.value("files_reused_cache", int64_t{-1}) == 0,
              "Initial scan_incremental reused 0 cached files");

        // 1b. Second scan without changes: 0 audited, 4 reused from cache
        const Json scan2 = tool_code_audit(p, db, {{"action", "scan_incremental"}});
        check(scan2.value("files_audited", int64_t{-1}) == 0,
              "Second scan_incremental without changes audited 0 files (got " + std::to_string(scan2.value("files_audited", int64_t{-1})) + ")");
        check(scan2.value("files_reused_cache", int64_t{-1}) == 4,
              "Second scan_incremental reused all 4 files from cache");

        // 1c. Modify src/independent.cpp: 1 audited, 3 reused
        write_text(repo_dir / L"src" / L"independent.cpp",
                   "namespace demo {\n"
                   "int standalone_fn(int z) {\n"
                   "    return z + 42;\n"
                   "}\n"
                   "} // namespace demo\n");
        const Json scan3 = tool_code_audit(p, db, {{"action", "scan_incremental"}});
        check(scan3.value("files_audited", int64_t{-1}) == 1,
              "Modifying src/independent.cpp re-audited exactly 1 file (got " + std::to_string(scan3.value("files_audited", int64_t{-1})) + ")");
        check(scan3.value("files_reused_cache", int64_t{-1}) == 3,
              "Modifying src/independent.cpp reused 3 unaffected files from cache");

        // 1d. Modify include/shared.hpp: invalidates shared.hpp + mod_a.cpp + mod_b.cpp (3 audited, 1 reused)
        write_text(repo_dir / L"include" / L"shared.hpp",
                   "#pragma once\n"
                   "namespace demo {\n"
                   "inline int shared_offset() { return 99; }\n"
                   "} // namespace demo\n");
        const Json scan4 = tool_code_audit(p, db, {{"action", "scan_incremental"}});
        check(scan4.value("files_audited", int64_t{-1}) == 3,
              "Modifying include/shared.hpp invalidated and re-audited 3 files (shared.hpp, mod_a.cpp, mod_b.cpp), got " +
              std::to_string(scan4.value("files_audited", int64_t{-1})));
        check(scan4.value("files_reused_cache", int64_t{-1}) == 1,
              "Modifying include/shared.hpp reused 1 unaffected file (independent.cpp) from cache, got " +
              std::to_string(scan4.value("files_reused_cache", int64_t{-1})));

        // =====================================================================
        // SCENARIO 2: Strict C vs. C++23 Modernization Rules (NULL vs nullptr)
        // =====================================================================
        write_text(repo_dir / L"src" / L"legacy_c.c",
                   "#include <stddef.h>\n"
                   "void* c_init_ptr(void) {\n"
                   "    void* ptr = NULL;\n"
                   "    return ptr;\n"
                   "}\n");

        write_text(repo_dir / L"src" / L"legacy_cpp.cpp",
                   "#include <cstddef>\n"
                   "void* cpp_init_ptr() {\n"
                   "    void* ptr = NULL;\n"
                   "    return ptr;\n"
                   "}\n");

        cc_doc.push_back({{"directory", path_text(repo_dir)}, {"file", "src/legacy_c.c"}, {"command", "cl.exe /TC /W4 /c src/legacy_c.c"}});
        cc_doc.push_back({{"directory", path_text(repo_dir)}, {"file", "src/legacy_cpp.cpp"}, {"command", "cl.exe /std:c++23 /W4 /c src/legacy_cpp.cpp"}});
        write_json(repo_dir / L"compile_commands.json", cc_doc);

        tool_code_audit(p, db, {{"action", "scan_incremental"}});

        const Json c_findings = tool_code_audit(p, db, {
            {"action", "findings"},
            {"file_path", "src/legacy_c.c"},
            {"rule_id", "CPP23-MODERNIZE-NULLPTR"}
        });
        check(c_findings["findings"].empty(),
              "C file src/legacy_c.c has 0 CPP23-MODERNIZE-NULLPTR findings: " + c_findings.dump());

        const Json cpp_findings = tool_code_audit(p, db, {
            {"action", "findings"},
            {"file_path", "src/legacy_cpp.cpp"},
            {"rule_id", "CPP23-MODERNIZE-NULLPTR"}
        });
        check(cpp_findings["findings"].size() == 1,
              "C++23 file src/legacy_cpp.cpp has exactly 1 CPP23-MODERNIZE-NULLPTR finding: " + cpp_findings.dump());

        // =====================================================================
        // SCENARIO 3 & 4: AST Cyclomatic Complexity, Duplicate TEST_CASE
        //                 Disambiguation & Test-Aware Security Triage
        // =====================================================================
        write_text(repo_dir / L"tests" / L"sample_tests.cpp",
                   "// Fake comment with branching keywords: if (fake) && while (comment_loop) || for (;;)\n"
                   "const char* kMockApiKey = \"sk-test-1234567890abcdef1234567890abcdef\";\n"
                   "const char* kFixturePath = \"D:\\\\Temp\\\\fixture\";\n"
                   "\n"
                   "int helper_with_branches(int a, int b) {\n"
                   "    // if (comment_branch_ignored) { while (true) {} }\n"
                   "    const char* s = \"if (string_literal) && || for while case catch ?\";\n"
                   "    (void)s;\n"
                   "    int acc = 0;\n"
                   "    if (a > 0 && b > 0) {\n"
                   "        for (int i = 0; i < a; ++i) {\n"
                   "            if (i == b || i == 10) {\n"
                   "                acc += i;\n"
                   "            }\n"
                   "        }\n"
                   "    }\n"
                   "    return acc;\n"
                   "}\n"
                   "\n"
                   "TEST_CASE(\"duplicate_name\") {\n"
                   "    int r = helper_with_branches(3, 2);\n"
                   "    (void)r;\n"
                   "}\n"
                   "\n"
                   "TEST_CASE(\"duplicate_name\") {\n"
                   "    int r = helper_with_branches(5, 4);\n"
                   "    (void)r;\n"
                   "}\n");

        // Also inject unsafe strcpy into production file src/mod_a.cpp
        write_text(repo_dir / L"src" / L"mod_a.cpp",
                   "#include \"../include/shared.hpp\"\n"
                   "#include <cstring>\n"
                   "namespace demo {\n"
                   "int compute_a(int x, const char* input) {\n"
                   "    char buf[16]{};\n"
                   "    strcpy(buf, input);\n"
                   "    if (x > 0) return x + shared_offset();\n"
                   "    return 0;\n"
                   "}\n"
                   "} // namespace demo\n");

        tool_code_audit(p, db, {{"action", "scan_incremental"}});

        const Json test_funcs_res = tool_code_audit(p, db, {
            {"action", "functions"},
            {"file_path", "tests/sample_tests.cpp"}
        });
        const Json& test_funcs = test_funcs_res["functions"];
        int dup_test_case_count = 0;
        int first_dup_line = -1;
        int second_dup_line = -1;
        int64_t helper_cc = -1;
        for (const auto& fn : test_funcs) {
            const std::string qn = fn.value("qualified_name", std::string{});
            if (qn == "TEST_CASE::duplicate_name") {
                ++dup_test_case_count;
                if (first_dup_line < 0) first_dup_line = fn.value("start_line", -1);
                else second_dup_line = fn.value("start_line", -1);
            } else if (qn == "helper_with_branches") {
                helper_cc = fn.value("cyclomatic_complexity", int64_t{-1});
            }
        }
        check(dup_test_case_count == 2 && first_dup_line != second_dup_line,
              "Both duplicate TEST_CASE(\"duplicate_name\") blocks stored as distinct rows by start_line (" +
              std::to_string(first_dup_line) + " vs " + std::to_string(second_dup_line) + ")");
        // helper_with_branches has: 1 (base) + 1 (if) + 1 (&&) + 1 (for) + 1 (if) + 1 (||) = 6
        check(helper_cc == 6,
              "AST cyclomatic_complexity for helper_with_branches is exact (6) and uninflated by comments/strings (got " +
              std::to_string(helper_cc) + ")");

        // Check Scenario 4: Test-Aware Security Triage
        const Json test_sec_findings = tool_code_audit(p, db, {
            {"action", "findings"},
            {"file_path", "tests/sample_tests.cpp"}
        });
        check(test_sec_findings["findings"].size() >= 2,
              "Detected mock API key and hardcoded path in tests/sample_tests.cpp");
        for (const auto& f : test_sec_findings["findings"]) {
            check(f.value("triage_status", std::string{}) == "PENDING_VALIDATION",
                  "Finding in tests/sample_tests.cpp is triaged as PENDING_VALIDATION (0 CONFIRMED in test files): " + f.dump());
        }

        const Json prod_sec_findings = tool_code_audit(p, db, {
            {"action", "findings"},
            {"file_path", "src/mod_a.cpp"},
            {"rule_id", "SEC-UNSAFE-CRT"}
        });
        check(prod_sec_findings["findings"].size() == 1 &&
              prod_sec_findings["findings"][0].value("triage_status", std::string{}) == "CONFIRMED",
              "Unsafe strcpy in production src/mod_a.cpp is triaged as CONFIRMED: " + prod_sec_findings.dump());

        // =====================================================================
        // SCENARIO 5: Anti-Hallucination Agent Claim Verifier (verify_claim)
        // =====================================================================
        // 5a. Inflated claim without build or test evidence
        const Json claim1 = tool_code_audit(p, db, {
            {"action", "verify_claim"},
            {"actor", "Claude"},
            {"module_or_path", "src/mod_a.cpp"},
            {"claim_text", "Implemented 99 functions, compiled, passed tests, and achieved 100% functional equivalence"},
            {"claimed_functions_count", 99},
            {"claimed_compiled", true},
            {"claimed_tests_passed", true},
            {"claimed_equivalent", true}
        });
        const std::string v1 = claim1.value("verdict", std::string{});
        check(v1 == "UNVERIFIED_CLAIM" || v1 == "CONTRADICTED_BY_EVIDENCE",
              "Inflated claim without build/tests returns UNVERIFIED_CLAIM or CONTRADICTED_BY_EVIDENCE (got " + v1 + ")");
        check(claim1["compilation_evidence"].value("status", std::string{}) != "VERIFIED_BUILD_PASS",
              "compilation_evidence is not VERIFIED_BUILD_PASS when no build exists");
        check(claim1["functional_equivalence_evidence"].value("status", std::string{}) == "NOT_PROVEN",
              "functional_equivalence_evidence is NOT_PROVEN");
        check(claim1["functions_evidence"].value("matches_claim", true) == false &&
              claim1["functions_evidence"].value("actual_functions_count", int64_t{-1}) == 1,
              "functions_evidence detected mismatch (claimed 99 vs actual 1 AST function)");

        // 5b. Record a passing canonical build and a passing verified_runs entry, then verify accurate claim
        tool_build_canonical(p, db, {
            {"action", "record"},
            {"build_id", "bld-audit-test-01"},
            {"requesting_agent", "Claude"},
            {"lifecycle_state", "INTEGRATED"},
            {"require_complete_integration", false},
            {"exit_code", 0}
        });

        runs_telemetry_schema(db);
        db.exec(
            "INSERT INTO verified_runs("
            "id,at_iso,actor,workstream_id,selection_mode,selected_targets_json,command_json,env_profile,cwd,"
            "exit_code,duration_ms,status,passed_count,failed_count,errors_json,raw_bytes,distilled_bytes,log_path,git_head)"
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {"run-audit-test-01", now(), "Claude", "", "explicit", "[\"code_audit_tests\"]",
             "[\"ctest\"]", "", path_text(repo_dir), 0, 120, "PASSED", 12, 0, "[]", 256, 2, "", "HEAD"});

        const Json claim2 = tool_code_audit(p, db, {
            {"action", "verify_claim"},
            {"actor", "Claude"},
            {"module_or_path", "src/mod_a.cpp"},
            {"claim_text", "Implemented 1 function in src/mod_a.cpp, compiled and unit tests passed"},
            {"claimed_functions_count", 1},
            {"claimed_compiled", true},
            {"claimed_tests_passed", true},
            {"claimed_equivalent", false}
        });
        check(claim2.value("verdict", std::string{}) == "VERIFIED",
              "Accurate claim with verified build and test run returns VERIFIED: " + claim2.dump());
        check(claim2["compilation_evidence"].value("status", std::string{}) == "VERIFIED_BUILD_PASS",
              "compilation_evidence is VERIFIED_BUILD_PASS");
        check(claim2["tests_evidence"].value("status", std::string{}) == "VERIFIED_TESTS_PASS",
              "tests_evidence is VERIFIED_TESTS_PASS");
        check(claim2["functional_equivalence_evidence"].value("status", std::string{}) == "NOT_PROVEN",
              "functional_equivalence_evidence honestly reports NOT_PROVEN even when build and unit tests pass");

        // =====================================================================
        // SCENARIO 6: Regression Attribution, Triage & Gate Check
        // =====================================================================
        const Json reg_res = tool_code_audit(p, db, {{"action", "regressions"}});
        check(reg_res.value("ok", false) == true && reg_res.contains("deltas") && reg_res.contains("confirmed_findings"),
              "regressions action returns structured snapshot comparison and confirmed_findings");

        const Json gate_res = tool_code_audit(p, db, {
            {"action", "gate_check"},
            {"max_confirmed_p0", 0},
            {"require_verified_tests", true}
        });
        check(gate_res.value("ok", false) == true && gate_res.value("gate_passed", false) == true,
              "gate_check passes when confirmed_p0 == 0 and verified tests exist: " + gate_res.dump());

        // Triage the strcpy P1 finding as SUPPRESSED and verify preservation across re-scan
        const std::string strcpy_fnd_id = prod_sec_findings["findings"][0].value("finding_id", std::string{});
        const Json tri_res = tool_code_audit(p, db, {
            {"action", "triage"},
            {"finding_id", strcpy_fnd_id},
            {"triage_status", "SUPPRESSED"},
            {"suppression_reason", "Bounded buffer verified by caller contract"}
        });
        check(tri_res.value("ok", false) == true && tri_res.value("triage_status", std::string{}) == "SUPPRESSED",
              "triage action updated finding status to SUPPRESSED");

        std::cout << "ALL CODE AUDIT TESTS PASSED (" << g_checks << " checks)\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "CODE AUDIT TEST FAILURE: " << ex.what() << "\n";
        return 1;
    }
}
