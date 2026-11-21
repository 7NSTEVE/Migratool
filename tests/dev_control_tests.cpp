#include "migratool/gateway.hpp"
#include "migratool/scale.hpp"
#include <sqlite3.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace mt;
int checks = 0;

void check(bool cond, const std::string& msg) {
    ++checks;
    if (!cond) throw std::runtime_error("DEV_CONTROL CHECK FAILED: " + msg);
}

template <class Fn>
void check_throws(Fn&& fn, const std::string& expected_substr, const std::string& msg) {
    bool caught = false;
    std::string actual;
    try {
        fn();
    } catch (const std::exception& e) {
        actual = e.what();
        caught = expected_substr.empty() || actual.find(expected_substr) != std::string::npos;
    }
    check(caught, msg + " (actual='" + actual + "')");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) return 2;
        const auto repo_root = fs::path(argv[1]);
        const auto qa = repo_root / L"build/qa-scale/tests-tmp" / wide("devctl_" + unique_id());
        const auto src = qa / L"source";
        const auto dst = qa / L"target";
        fs::create_directories(src / L"marketplace");
        fs::create_directories(src / L"notebook");
        fs::create_directories(src / L"docs");
        fs::create_directories(dst / L"src/marketplace");
        fs::create_directories(dst / L"src/notebook");

        for (int i = 0; i < 200; ++i) {
            const auto mod = (i < 120) ? L"marketplace" : L"notebook";
            write_text(src / mod / wide("unit_" + std::to_string(i) + ".ts"),
                       "export function fn_" + std::to_string(i) + "(): number { return " + std::to_string(i) + "; }\n");
        }
        write_text(src / L"docs/ARCHITECTURE.md", "# Architecture Spec\nPending migration items.\n");
        write_text(dst / L"src/marketplace/score.cpp", "int score_listing() { return 100; }\n");
        write_text(dst / L"src/notebook/engine.cpp", "int render_ink() { return 1; }\n");

        auto proj = Project::create(qa / L"migration.json", "dev-control-suite",
            Json::array({{{"id", "legacy_src"}, {"role", "source"}, {"path", "source"}},
                         {{"id", "native_tgt"}, {"role", "target"}, {"path", "target"}}}));
        proj.config["inventory_engine"] = "durable";
        proj.config["analyze_cpp"] = false;
        proj.save();
        proj = Project::load(proj.file);

        const auto seeded = scale_run(proj, {{"actor", "devctl-seed"}});
        check(seeded.at("state") == "completed", "Initial durable inventory completed");

        // 1. Simulate legacy v6 DB missing task_claims & dev_control_schema -> verify self-healing & user_version==6
        {
            sqlite3* raw = nullptr;
            check(sqlite3_open(path_text(proj.state_dir() / L"catalog.sqlite").c_str(), &raw) == SQLITE_OK, "Open raw sqlite");
            sqlite3_exec(raw, "DROP TABLE IF EXISTS task_claims; DELETE FROM meta WHERE key='dev_control_schema';", nullptr, nullptr, nullptr);
            sqlite3_close(raw);
        }
        {
            Database db(proj);
            check(db.integer("PRAGMA user_version") == 6, "PRAGMA user_version preserved at 6");
            const auto fstatus = scale_file_status(proj, db, {{"limit", 5}, {"include_total", false}});
            check(fstatus.at("items").size() == 5, "scale_file_status self-healed missing task_claims table");
        }

        // 2. Default token_budget on agent status --level files returns >= 1 item without explicit token_budget
        {
            const auto default_files = agent_request(proj, "migration_status", {{"level", "files"}, {"limit", 5}});
            check(!default_files.at("items").empty(), "migration_status(level=files) returns items at default token_budget");
        }

        // 3. Negative maturity checks + fast batch_update (200 ops with shared targets & FTS5 search_rows)
        std::string first_src_id, second_src_id, doc_id, tgt_mkt_id, tgt_cua_id;
        std::vector<std::string> mkt_sources, cua_sources;
        {
            Database db(proj);
            for (const auto& r : db.query("SELECT id,path FROM files WHERE root='legacy_src' AND relevant=1 AND deleted=0 ORDER BY id")) {
                const auto fid = "F" + std::to_string(r["id"].get<int64_t>());
                if (r["path"].get<std::string>().starts_with("marketplace/")) mkt_sources.push_back(fid);
                else cua_sources.push_back(fid);
            }
            check(mkt_sources.size() == 120 && cua_sources.size() == 80, "200 source files partitioned by module");
            first_src_id = mkt_sources[0];
            second_src_id = cua_sources[0];
            doc_id = "F" + std::to_string(db.integer("SELECT id FROM files WHERE root='legacy_src' AND category='documentation' AND deleted=0 LIMIT 1"));
            tgt_mkt_id = "F" + std::to_string(db.integer("SELECT id FROM files WHERE root='native_tgt' AND path LIKE '%score.cpp' AND deleted=0 LIMIT 1"));
            tgt_cua_id = "F" + std::to_string(db.integer("SELECT id FROM files WHERE root='native_tgt' AND path LIKE '%engine.cpp' AND deleted=0 LIMIT 1"));

            check_throws([&] {
                agent_batch(proj, {{"actor", "neg-test"}, {"expected_revision", db.revision()},
                    {"operations", {{{"op", "map"}, {"source", first_src_id}, {"targets", {tgt_mkt_id}}, {"complete", true}, {"stage", "half_done"}}}}});
            }, "complete:true", "Reject complete:true combined with stage=half_done");

            check_throws([&] {
                agent_batch(proj, {{"actor", "neg-test"}, {"expected_revision", db.revision()},
                    {"operations", {{{"op", "map"}, {"source", first_src_id}, {"targets", {tgt_mkt_id}}, {"complete", true}, {"stage", "verified"}}}}});
            }, "Etapa de madurez", "Reject self-declared stage=verified without case evidence");
        }

        // Configure target-only build + target_test cases (legacy_src has NO build configured!)
        const auto worker = path_text(executable_dir() / L"test_worker.exe");
        proj.config["builds"] = Json::array({
            {{"id", "native_build"}, {"root", "native_tgt"}, {"argv", {worker, "args", "BUILD_OK"}}, {"artifacts", {worker}}}
        });
        proj.config["cases"] = Json::array({
            {{"id", "case_mkt_pass"}, {"kind", "target_test"}, {"root", "native_tgt"},
             {"argv", {worker, "args", "ALL_TESTS_PASSED"}},
             {"expected_stdout_contains", {"ALL_TESTS_PASSED"}},
             {"forbidden_stdout_contains", {"FAILED"}}},
            {{"id", "case_forbidden_hit"}, {"kind", "target_test"}, {"root", "native_tgt"},
             {"argv", {worker, "args", "1 TEST FAILED"}},
             {"forbidden_stdout_contains", {"FAILED"}}}
        });
        proj.save();
        proj = Project::load(proj.file);

        // Batch map 200 files and measure speed
        {
            Database db(proj);
            Json ops = Json::array();
            for (const auto& s : mkt_sources) {
                ops.push_back({{"op", "map"}, {"source", s}, {"targets", {tgt_mkt_id}},
                               {"complete", true}, {"stage", "integrated"}, {"cases", {"case_mkt_pass"}},
                               {"note", "Ported to native marketplace"}});
            }
            for (size_t i = 0; i < cua_sources.size(); ++i) {
                const bool half = (i < 10);
                ops.push_back({{"op", "map"}, {"source", cua_sources[i]}, {"targets", {tgt_cua_id}},
                               {"complete", !half}, {"stage", half ? "half_done" : "ported"},
                               {"note", half ? "Unwired C API" : "Ported engine"}});
            }
            const auto t0 = std::chrono::steady_clock::now();
            const auto res = agent_batch(proj, {{"actor", "fast-batch"}, {"expected_revision", db.revision()}, {"operations", ops}});
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            check(res.at("applied") == 200, "Applied 200 mapping operations atomically");
            check(elapsed_ms < 5000, "200 batch map ops completed quickly (<5000 ms, actual=" + std::to_string(elapsed_ms) + " ms)");
        }

        // 4. Anti-fabrication gate BEFORE running builds/tests: phase/task cannot claim VERIFIED
        {
            Database db(proj);
            check_throws([&] {
                agent_batch(proj, {{"actor", "gate-check"}, {"expected_revision", db.revision()},
                    {"operations", {{{"op", "upsert_phase"}, {"id", "P1"}, {"title", "Marketplace"},
                                     {"state", "VERIFIED"}, {"gate_builds", {"native_build"}}, {"gate_cases", {"case_mkt_pass"}}}}}});
            }, "PHASE_GATE_UNMET", "Phase gate blocks VERIFIED before build/case pass");
        }

        // 5. Run native_build & target_test cases -> verify promotion to VERIFIED_IN_CASES even with 0 source builds!
        const auto build_res = run_builds(proj);
        check(build_res.at("stable") == true && build_res.at("builds")[0].at("state") == "PASS", "Target build passed");
        const auto test_res = run_cases(proj);
        check(test_res.at("stable") == true, "Target test run stable");
        check(test_res.at("cases")[0].at("state") == "PASS", "target_test positive case PASS");
        check(test_res.at("cases")[1].at("state") == "DIFFERENT", "target_test forbidden_stdout_contains caught failure");

        {
            Database db(proj);
            const auto summary = scale_summary(proj, db);
            check(summary.at("files_verified_in_cases") == 120,
                  "All 120 marketplace files promoted to VERIFIED_IN_CASES without requiring source build (actual=" +
                  summary.at("files_verified_in_cases").dump() + ")");
            const auto one_file = scale_file_status(proj, db, {{"id", first_src_id}, {"limit", 1}, {"include_total", false}}).at("items")[0];
            check(one_file.at("state") == "VERIFIED_IN_CASES" && one_file.at("stage") == "verified",
                  "Verified file reports state=VERIFIED_IN_CASES and stage=verified");
            const auto half_file = scale_file_status(proj, db, {{"id", second_src_id}, {"limit", 1}, {"include_total", false}}).at("items")[0];
            check(half_file.at("stage") == "half_done" && half_file.at("declared_complete") == false,
                  "Half-done file preserves stage=half_done and declared_complete=false");

            // Module matrix check
            const auto matrix = agent_request(proj, "migration_status", {{"level", "modules"}, {"token_budget", 10000}});
            check(matrix.at("items").size() == 2, "Module matrix aggregates 2 source modules");

            // Phases, Tasks & Doc Audit now succeed with passing gate!
            const auto ctl = agent_batch(proj, {{"actor", "dev-lead"}, {"expected_revision", db.revision()},
                {"operations", {
                    {{"op", "upsert_phase"}, {"id", "P1"}, {"title", "Marketplace Native"}, {"ordinal", 1},
                     {"state", "VERIFIED"}, {"gate_builds", {"native_build"}}, {"gate_cases", {"case_mkt_pass"}}},
                    {{"op", "upsert_task"}, {"id", "T-MKT-1"}, {"phase_id", "P1"}, {"title", "Verify scoring"},
                     {"state", "VERIFIED"}, {"required_cases", {"case_mkt_pass"}}, {"source_entities", {first_src_id}}, {"target_entities", {tgt_mkt_id}}},
                    {{"op", "upsert_task"}, {"id", "T-CUA-1"}, {"phase_id", "P2"}, {"title", "Wire ink lasso"},
                     {"state", "HALF_DONE"}, {"note", "api.h unreferenced"}},
                    {{"op", "audit_doc"}, {"doc", doc_id}, {"state", "REVIEWED"}, {"disposition", "tracked_tasks"},
                     {"extracted_tasks", {"T-CUA-1"}}, {"note", "Extracted ink backlog"}}
                }}});
            check(ctl.at("applied") == 4, "Phases, tasks, and doc_audit committed");

            const auto summary2 = scale_summary(proj, db);
            check(summary2.at("development_control").at("phases_completed") == 1 &&
                  summary2.at("development_control").at("tasks_completed") == 1 &&
                  summary2.at("development_control").at("tasks_half_done") == 1 &&
                  summary2.at("development_control").at("documentation_reviewed") == 1,
                  "development_control summary reflects phases, tasks, and reviewed docs");
        }

        // 6. Tamper target_test stdout -> invalidates VERIFIED_IN_CASES; modify doc -> marks doc STALE
        write_text(fs::path(wide(test_res.at("cases")[0].at("target").at("stdout"))), "TAMPERED");
        {
            Database db(proj);
            const auto post_tamper = scale_file_status(proj, db, {{"id", first_src_id}, {"limit", 1}, {"include_total", false}}).at("items")[0];
            check(post_tamper.at("verified_in_configured_cases") == false && post_tamper.at("units_with_passing_cases") == 0,
                  "Tampered target_test stdout invalidates VERIFIED_IN_CASES");
        }
        write_text(src / L"docs/ARCHITECTURE.md", "# Architecture Spec v2\nUpdated doc.\n");
        scale_run(proj, {{"actor", "doc-edit"}, {"scope", {{"root", "legacy_src"}, {"file", "docs/ARCHITECTURE.md"}}}});
        {
            const auto docs = agent_request(proj, "migration_status", {{"level", "docs"}, {"token_budget", 8000}});
            check(!docs.at("items").empty() && docs.at("items")[0].at("state") == "STALE",
                  "Modified documentation file automatically transitions doc_audit to STALE");
        }

        // 7. Legacy v6 dev_control_schema='1' + missing trigger + stale scale_summary_cache self-healing + CLI status --no-fresh
        {
            sqlite3* raw_db = nullptr;
            check(sqlite3_open_v2(utf8((proj.state_dir() / L"catalog.sqlite").wstring()).c_str(), &raw_db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
                  "sqlite3 opened catalog for legacy v6 downgrade simulation");
            auto exec_sql = [&](const char* sql) {
                char* errmsg = nullptr;
                const int rc = sqlite3_exec(raw_db, sql, nullptr, nullptr, &errmsg);
                std::string err = errmsg ? errmsg : "";
                if (errmsg) sqlite3_free(errmsg);
                check(rc == SQLITE_OK, std::string("sqlite3 exec failed: ") + err + " sql=" + sql);
            };
            exec_sql("DROP TRIGGER IF EXISTS progress_task_update;");
            exec_sql("DROP TRIGGER IF EXISTS progress_doc_change;");
            exec_sql("INSERT OR REPLACE INTO meta(key,value) VALUES('dev_control_schema','1');");
            exec_sql("INSERT OR REPLACE INTO meta(key,value) VALUES('scale_summary_cache','{\"stale_legacy\":true}');");
            sqlite3_close(raw_db);

            Database healed_db(proj);
            const auto trig_count = healed_db.integer(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN("
                "'progress_phase_change','progress_phase_update','progress_task_change',"
                "'progress_task_update','progress_task_delete','progress_doc_change','progress_doc_update')");
            check(trig_count == 7, "Legacy v6 dev_patch restores all 7 development_control triggers");
            check(healed_db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dev_control_schema'") == 2,
                  "dev_control_schema bumped to '2'");
            check(healed_db.integer("SELECT COUNT(*) FROM meta WHERE key='scale_summary_cache'") == 0,
                  "Stale scale_summary_cache invalidated on trigger upgrade");

            const auto cli_exe = executable_dir() / L"migratool-cli-0.3.exe";
            const auto cli_res = run_process({path_text(cli_exe), "status", "--project", path_text(proj.file), "--no-fresh"},
                                             qa, "", qa / L"cli_nofresh", 15000);
            check(cli_res.at("exit_code") == 0, "CLI status --no-fresh succeeded");
            const auto cli_json = Json::parse(read_text(fs::path(wide(cli_res.at("stdout").get<std::string>()))));
            check(cli_json.contains("development_control") && cli_json["development_control"].is_object(),
                  "CLI status --no-fresh includes development_control");
        }

        // 8. Stage lifecycle (mark_file / indexed_mapping) + multi-origin (src_mkt + src_notebook) + non-source classify disposition + targetless removal
        {
            const auto mo_dir = qa / L"multi_origin";
            fs::create_directories(mo_dir / L"src_mkt/billing");
            fs::create_directories(mo_dir / L"src_notebook/ink");
            fs::create_directories(mo_dir / L"dst/cpp");
            const auto mkt_f = mo_dir / L"src_mkt/billing/pay.py";
            const auto nb_f = mo_dir / L"src_notebook/ink/stroke.py";
            const auto doc_f = mo_dir / L"src_mkt/README.md";
            const auto out_f = mo_dir / L"dst/cpp/pay.cpp";
            write_text(mkt_f, "def charge():\n    return 10\n");
            write_text(nb_f, "def stroke():\n    return 20\n");
            write_text(doc_f, "# Billing notes\n");
            write_text(out_f, "int charge() { return 10; }\n");

            auto mproj = Project::create(mo_dir / L"migration.json", "multi-origin-suite",
                Json::array({{{"id", "src_mkt"}, {"role", "source"}, {"path", "src_mkt"}},
                             {{"id", "src_notebook"}, {"role", "source"}, {"path", "src_notebook"}},
                             {{"id", "dst"}, {"role", "target"}, {"path", "dst"}}}));
            mproj.config["inventory_engine"] = "durable";
            mproj.save();
            check(scale_run(mproj, {{"actor", "mo-seed"}}).at("state") == "completed", "Multi-origin census completed");

            Database db(mproj);
            const auto mods = agent_request(mproj, "migration_status", {{"level", "modules"}, {"token_budget", 10000}});
            bool saw_mkt = false, saw_nb = false;
            for (const auto& item : mods.at("items")) {
                if (item.value("root", "") == "src_mkt") saw_mkt = true;
                if (item.value("root", "") == "src_notebook") saw_nb = true;
            }
            check(saw_mkt && saw_nb, "Module matrix includes both src_mkt and src_notebook source roots");

            const auto mods_with_ns = scale_module_matrix(mproj, db, {{"include_non_source", true}, {"limit", 50}});
            bool saw_non_src_count = false;
            for (const auto& item : mods_with_ns.at("items")) {
                if (item.value("non_source_files", int64_t(0)) > 0) saw_non_src_count = true;
            }
            check(saw_non_src_count, "include_non_source=true exposes non_source_files per module");

            const auto doc_fid = "F" + std::to_string(db.integer("SELECT id FROM files WHERE path='README.md' AND deleted=0"));
            agent_batch(mproj, {{"actor", "triage"}, {"expected_revision", db.revision()},
                {"operations", {{{"op", "classify"}, {"file", doc_fid}, {"category", "documentation"}, {"relevant", false},
                                 {"disposition", "DO_NOT_MIGRATE"}, {"note", "Historical doc"}}}}});
            const auto non_rel = scale_file_status(mproj, db, {{"include_non_relevant", true}, {"id", doc_fid}, {"limit", 1}, {"include_total", false}});
            check(!non_rel.at("items").empty() && non_rel.at("items")[0].at("disposition") == "DO_NOT_MIGRATE",
                  "Non-relevant classify disposition DO_NOT_MIGRATE persisted and queryable");

            const auto s_row = db.query("SELECT 'S'||s.id AS sid FROM symbols s JOIN files f ON s.file_id=f.id WHERE f.root='src_mkt' AND s.active=1 LIMIT 1");
            check(!s_row.empty(), "Symbol id extracted for src_mkt");
            indexed_mapping(mproj, s_row[0].at("sid").get<std::string>(), {path_text(out_f)}, {}, false, "ported symbol", "dev", "ported", "migrated", "");
            const auto q_maps = scale_query(mproj, db, {{"kind", "mappings"}, {"limit", 20}});
            check(!q_maps->at("items").empty() && q_maps->at("items")[0].at("stage") == "ported",
                  "scale_query mappings exposes stage='ported' and preserves S<id> mapping");

            check_throws([&] {
                mark_file(mproj, nb_f, {}, false, "", "dev", {}, nullptr, "planned", "intentionally_removed", "");
            }, "", "Targetless intentionally_removed rejects empty note");
            const auto rem = mark_file(mproj, nb_f, {}, true, "Obsolete ink path", "dev", {}, nullptr, "integrated", "intentionally_removed", "");
            check(rem.at("disposition") == "intentionally_removed" && rem.at("declared_complete") == true,
                  "Targetless intentionally_removed with note succeeds");
        }

        // 9. 10k+ file catalog SQLite join & module matrix CTE performance (<2000ms) + report() dev_control JSON/HTML
        {
            const auto perf_dir = qa / L"perf_10k";
            fs::create_directories(perf_dir / L"source");
            fs::create_directories(perf_dir / L"target");
            for (int b = 0; b < 5; ++b) {
                const auto bdir = perf_dir / L"source" / wide("mod_" + std::to_string(b));
                fs::create_directories(bdir);
                for (int k = 0; k < 200; ++k) {
                    write_text(bdir / wide("f_" + std::to_string(b * 200 + k) + ".py"),
                               "def p_" + std::to_string(k) + "():\n    return " + std::to_string(k) + "\n");
                }
            }
            write_text(perf_dir / L"target/t0.py", "def t0():\n    return 0\n");

            auto pproj = Project::create(perf_dir / L"migration.json", "perf-10k",
                Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                             {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
            pproj.config["inventory_engine"] = "durable";
            pproj.save();
            check(scale_run(pproj, {{"actor", "perf-sym"}, {"defer_analysis", false}}).at("state") == "completed",
                  "1k symbol extraction completed");
            for (int b = 5; b < 50; ++b) {
                const auto bdir = perf_dir / L"source" / wide("mod_" + std::to_string(b));
                fs::create_directories(bdir);
                for (int k = 0; k < 200; ++k) {
                    write_text(bdir / wide("f_" + std::to_string(b * 200 + k) + ".py"),
                               "def p_" + std::to_string(k) + "():\n    return " + std::to_string(k) + "\n");
                }
            }
            check(scale_run(pproj, {{"actor", "perf-seed"}, {"defer_analysis", true}}).at("state") == "completed",
                  "10k perf catalog census completed");

            Database db(pproj);
            agent_batch(pproj, {{"actor", "perf-ctl"}, {"expected_revision", db.revision()},
                {"operations", {
                    {{"op", "upsert_phase"}, {"id", "P-PERF"}, {"title", "Phase <script>alert(1)</script>"}, {"ordinal", 1}, {"state", "IN_PROGRESS"}},
                    {{"op", "upsert_task"}, {"id", "T-PERF"}, {"phase_id", "P-PERF"}, {"title", "Task <perf>"}, {"state", "HALF_DONE"}}
                }}});

            const auto t0 = std::chrono::steady_clock::now();
            const auto mods = scale_module_matrix(pproj, db, {{"limit", 200}});
            const auto t1 = std::chrono::steady_clock::now();
            const auto sum = scale_summary(pproj, db);
            const auto t2 = std::chrono::steady_clock::now();
            const auto mod_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            const auto sum_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
            check(!mods.at("items").empty() && mod_ms < 2000,
                  "10k+ scale_module_matrix completes <2000ms (actual=" + std::to_string(mod_ms) + "ms)");
            check(sum.at("source_files").get<int64_t>() >= 10000 && sum_ms < 2000,
                  "10k+ scale_summary completes <2000ms (actual=" + std::to_string(sum_ms) + "ms)");

            const auto rep_html = perf_dir / L"report.html";
            report(pproj, rep_html, false);
            const auto rep_json = read_json(perf_dir / L"report.json");
            const auto html_txt = read_text(rep_html);
            check(rep_json.contains("development_control") && rep_json.contains("phases") &&
                  rep_json.contains("project_tasks") && rep_json.contains("module_matrix"),
                  "report.json includes development_control, phases, project_tasks, and module_matrix");
            check(html_txt.find("<script>alert(1)</script>") == std::string::npos &&
                  html_txt.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos,
                  "report.html escapes HTML in development_control titles");
        }

        std::cout << "PASS " << checks << " dev_control checks.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
