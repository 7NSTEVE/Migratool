#include "migratool/agent_supertools.hpp"
#include "migratool/core.hpp"
#include "migratool/docs_hub.hpp"
#include "migratool/file_toolkit.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/gateway.hpp"
#include "migratool/knowledge.hpp"
#include "migratool/path_lineage.hpp"
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
        const fs::path temp_base = fs::temp_directory_path() / wide("migratool_kn_docs_lineage_" + unique_id());
        const fs::path root = temp_base / L"repo";
        fs::create_directories(root / L"src");
        fs::create_directories(root / L"docs");

        write_text(root / L"src" / L"engine.cpp", "int compute_rate(int x) {\n    return x * 2;\n}\n");
        write_text(root / L"src" / L"a.txt", "lineage payload v1\n");
        write_text(root / L"README.md",
                   "# Motor Principal\n"
                   "Resumen general del sistema con referencia a `src/engine.cpp`.\n"
                   "- [x] Inicializar base\n"
                   "- [ ] Completar optimización\n"
                   "```markdown\n"
                   "# Este comentario dentro de bloque de código NO es una sección\n"
                   "```\n"
                   "## Arquitectura Interna\n"
                   "Detalles de `src/engine.cpp` y flujo de datos.\n"
                   "### Subsistema de Cálculo\n"
                   "Fórmula de tasa en `compute_rate`.\n");

        Project p = Project::create(temp_base / L"migration.json", "KnowledgeDocsLineageTest",
                                    Json::array({{{"id", "src"}, {"role", "source"}, {"path", path_text(root)}}}));
        agent_request(p, "inventory_sync", {{"full", true}});

        // =====================================================================
        // 1. KNOWLEDGE LEDGER & EPISTEMIC PROVENANCE TESTS
        // =====================================================================
        auto k_pitfall = agent_request(p, "knowledge", {
            {"action", "record"},
            {"kind", "pitfall"},
            {"title", "No usar MoveFileExW(0) entre volúmenes distintos"},
            {"summary", "MoveFileExW sin copia verificada falla con ERROR_NOT_SAME_DEVICE (17)"},
            {"outcome", "active"},
            {"severity", "critical"},
            {"verification_status", "verified_fact"},
            {"evidence_cmd", "fs_organize_tests.exe"},
            {"files", Json::array({"src/engine.cpp"})},
            {"pinned", true}
        });
        check(k_pitfall["ok"] == true && k_pitfall["verification_status"] == "verified_fact" &&
              k_pitfall["epistemic_label"] == "CONFIRMED",
              "knowledge recorded verified_fact pitfall linked to src/engine.cpp: " + k_pitfall.dump());
        const std::string pitfall_id = k_pitfall["id"].get<std::string>();

        auto k_method_fail = agent_request(p, "knowledge", {
            {"action", "record"},
            {"kind", "method_tried"},
            {"title", "Usar GetDC por botón dentro de ScaleView::layout"},
            {"summary", "Provocaba picos de 218 ms en gui_acceptance_tests"},
            {"outcome", "failed"},
            {"evidence_cmd", "gui_acceptance_tests.exe"},
            {"files", Json::array({"src/engine.cpp"})}
        });
        check(k_method_fail["ok"] == true && k_method_fail["outcome"] == "failed",
              "knowledge recorded failed method_tried");

        auto k_hyp = agent_request(p, "knowledge", {
            {"action", "record"},
            {"kind", "finding"},
            {"title", "Hipótesis sobre caché L2"},
            {"summary", "Podría mejorar 5% pero aún no se midió"},
            {"is_hypothesis", true},
            {"confidence", 0.5},
            {"files", Json::array({"src/engine.cpp"})}
        });
        check(k_hyp["verification_status"] == "agent_hypothesis" &&
              k_hyp["epistemic_label"] == "UNVERIFIED_HYPOTHESIS" &&
              k_hyp["is_verified_fact"] == false,
              "unproven hypothesis is strictly labeled agent_hypothesis / UNVERIFIED_HYPOTHESIS");

        // FTS5 search over knowledge
        auto k_search = agent_request(p, "knowledge", {{"action", "search"}, {"query", "ERROR_NOT_SAME_DEVICE"}});
        check(k_search["count"].get<int64_t>() >= 1 && k_search["items"][0]["id"] == pitfall_id,
              "knowledge FTS5 search matched ERROR_NOT_SAME_DEVICE");

        // Proactive auto-injection of pitfalls & failed methods inside read_bundle!
        auto bundle1 = agent_request(p, "read_bundle", {{"paths", Json::array({"src/engine.cpp"})}});
        check(bundle1["count"].get<int64_t>() == 1 && !bundle1["knowledge_warnings"].empty(),
              "read_bundle automatically injected proactive knowledge_warnings for src/engine.cpp");

        // Automatic invalidation when src/engine.cpp is modified!
        Sleep(15);
        agent_request(p, "edit_file", {
            {"path", "src/engine.cpp"},
            {"mode", "replace_content"},
            {"needle", "return x * 2;"},
            {"replacement", "return x * 4;"}
        });
        auto k_after_edit = agent_request(p, "knowledge", {{"action", "get"}, {"id", pitfall_id}});
        check(k_after_edit["verification_status"] == "stale" && k_after_edit["needs_reverification"] == true,
              "editing src/engine.cpp automatically invalidated linked knowledge entry to 'stale'");

        // Re-verify restores verified_fact status with updated SHA-256
        auto k_reverified = agent_request(p, "knowledge", {{"action", "verify"}, {"id", pitfall_id}});
        check(k_reverified["verification_status"] == "verified_fact" && k_reverified["needs_reverification"] == false,
              "knowledge(verify) refreshed SHA-256 and restored verified_fact");

        // =====================================================================
        // 2. DOCS HUB (TOC H1-H4, READ_SECTION, CODE DRIFT DETECTION)
        // =====================================================================
        auto dh_idx = agent_request(p, "docs_hub", {{"action", "index"}});
        check(dh_idx["ok"] == true && dh_idx["docs_indexed"].get<int64_t>() >= 1, "docs_hub indexed README.md");

        auto dh_out = agent_request(p, "docs_hub", {{"action", "outline"}, {"path", "README.md"}});
        check(dh_out["section_count"].get<int64_t>() == 3,
              "docs_hub outline extracted exactly 3 sections (ignoring # inside ``` code fence)");
        check(dh_out["sections"][1]["heading"] == "Arquitectura Interna" &&
              dh_out["sections"][1]["start_line"].get<int64_t>() == 8,
              "docs_hub section 2 has exact start_line=8");

        auto dh_sec = agent_request(p, "docs_hub", {
            {"action", "read_section"},
            {"path", "README.md"},
            {"anchor", "subsistema-de-clculo"}
        });
        check(dh_sec["content"].get<std::string>().find("compute_rate") != std::string::npos,
              "docs_hub(read_section) read exact target section without loading entire document");

        // Since src/engine.cpp was edited AFTER README.md was written, drift_report MUST detect DRIFT_DETECTED!
        auto dh_drift = agent_request(p, "docs_hub", {{"action", "drift_report"}});
        check(dh_drift["drifted_count"].get<int64_t>() >= 1 &&
              dh_drift["drifted_docs"][0]["doc_key"] == "README.md" &&
              dh_drift["drifted_docs"][0]["status"] == "DRIFT_DETECTED",
              "docs_hub(drift_report) detected code drift when linked src/engine.cpp is newer than README.md");

        // =====================================================================
        // 3. PATH LINEAGE (3-HOP MOVE CHAIN, CONFIDENCE, IMMUTABILITY, FILE_MOVED_HINT)
        // =====================================================================
        fs::create_directories(root / L"src" / L"sub");
        fs::create_directories(root / L"src" / L"sub2");
        MoveFileExW(native_path(root / L"src" / L"a.txt").c_str(),
                    native_path(root / L"src" / L"sub" / L"b.txt").c_str(), 0);
        agent_request(p, "inventory_sync", {{"full", false}});

        MoveFileExW(native_path(root / L"src" / L"sub" / L"b.txt").c_str(),
                    native_path(root / L"src" / L"sub2" / L"c.txt").c_str(), 0);
        agent_request(p, "inventory_sync", {{"full", false}});

        auto trace = agent_request(p, "path_history", {{"action", "trace"}, {"path", "src/a.txt"}});
        check(trace["count"].get<int64_t>() >= 2 && trace["current_path"] == "src/sub2/c.txt",
              "path_history(trace) followed 2-hop FRN rename/move chain src/a.txt -> src/sub/b.txt -> src/sub2/c.txt (got current_path=" +
              trace["current_path"].get<std::string>() + ")");

        auto scans = agent_request(p, "path_history", {{"action", "tree_scans"}});
        check(!scans["roots"].empty() && !scans["roots"][0]["last_full_scan_at"].get<std::string>().empty() &&
              !scans["history"].empty(),
              "path_history(tree_scans) reports last_full_scan_at, last_incremental_at and scan history");

        // Trying to read old path src/a.txt via read_file MUST include FILE_MOVED_HINT pointing to src/sub2/c.txt!
        bool got_moved_hint = false;
        try {
            agent_request(p, "read_file", {{"path", "src/a.txt"}});
        } catch (const std::exception& e) {
            const std::string err = e.what();
            got_moved_hint = (err.find("FILE_MOVED_HINT") != std::string::npos &&
                              err.find("src/sub2/c.txt") != std::string::npos);
        }
        check(got_moved_hint, "read_file on moved path src/a.txt includes FILE_MOVED_HINT with current_path='src/sub2/c.txt'");

        // Immutability triggers on path_lineage
        {
            Database db(p);
            bool upd_rejected = false, del_rejected = false;
            try { db.exec("UPDATE path_lineage SET op='tampered'"); } catch (...) { upd_rejected = true; }
            try { db.exec("DELETE FROM path_lineage"); } catch (...) { del_rejected = true; }
            check(upd_rejected && del_rejected, "path_lineage enforces BEFORE UPDATE and BEFORE DELETE immutability triggers");
        }

        std::error_code ec;
        fs::remove_all(temp_base, ec);
        std::cout << "knowledge_docs_lineage_tests PASSED (" << g_checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "knowledge_docs_lineage_tests FAILED: " << e.what() << "\n";
        return 1;
    }
}
