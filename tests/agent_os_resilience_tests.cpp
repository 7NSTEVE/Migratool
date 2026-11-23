#include "migratool/agent_supertools.hpp"
#include "migratool/core.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/gateway.hpp"
#include "migratool/housekeep.hpp"
#include "migratool/path_lineage.hpp"
#include "migratool/workstream.hpp"
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

int run_child_crash_mode(const fs::path& proj_file, const std::string& stage) {
    Project p = Project::load(proj_file);
    Database db(p);
    tool_workstream(p, db, {
        {"action", "start"},
        {"workstream_id", "ws-crash-1"},
        {"title", "Implementar motor v3"},
        {"goal", "Sobrevivir corte de energía con 7 etapas"},
        {"actor", "agent-alpha"},
        {"planned_files", Json::array({"src/a.cpp", "src/b.cpp", "src/c.cpp"})},
        {"current_step", "editando a.cpp y b.cpp"},
        {"resume_instructions", "Retomar en c.cpp tras verificar a.cpp y b.cpp"}
    });

    tool_batch_edit(p, db, {
        {"workstream_id", "ws-crash-1"},
        {"actor", "agent-alpha"},
        {"idempotency_key", "idem-child-1"},
        {"edits", Json::array({
            {{"path", "src/a.cpp"}, {"op", "replace_text"}, {"needle", "int a = 1;"}, {"replacement", "int a = 100;"}},
            {{"path", "src/b.cpp"}, {"op", "replace_text"}, {"needle", "int b = 2;"}, {"replacement", "int b = 200;"}}
        })}
    });

    tool_workstream(p, db, {
        {"action", "checkpoint"},
        {"workstream_id", "ws-crash-1"},
        {"actor", "agent-alpha"},
        {"summary", "a.cpp y b.cpp actualizados antes del corte"}
    });

    // Now start a second batch_edit that crashes mid-operation via TerminateProcess!
    tool_batch_edit(p, db, {
        {"workstream_id", "ws-crash-1"},
        {"actor", "agent-alpha"},
        {"simulate_crash_at_stage", stage},
        {"edits", Json::array({
            {{"path", "src/b.cpp"}, {"op", "replace_text"}, {"needle", "int b = 200;"}, {"replacement", "int b = 999;"}},
            {{"path", "src/c.cpp"}, {"op", "replace_text"}, {"needle", "int c = 3;"}, {"replacement", "int c = 300;"}}
        })}
    });
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    mt::set_failpoints_enabled_for_tests(true);
    try {
        if (argc >= 4 && std::wstring_view(argv[1]) == L"--child-crash") {
            return run_child_crash_mode(fs::path(argv[2]), utf8(argv[3]));
        }

        const fs::path temp_base = fs::temp_directory_path() / wide("migratool_agent_os_resilience_" + unique_id());
        const fs::path root = temp_base / L"repo";
        fs::create_directories(root / L"src");

        write_text(root / L"src" / L"a.cpp", "int a = 1;\n");
        write_text(root / L"src" / L"b.cpp", "int b = 2;\n");
        write_text(root / L"src" / L"c.cpp", "int c = 3;\n");

        Project p = Project::create(temp_base / L"migration.json", "ResilienceTest",
                                    Json::array({{{"id", "src"}, {"role", "source"}, {"path", path_text(root)}}}));
        agent_request(p, "inventory_sync", {{"full", true}});

        // 1. Launch real child process that crashes via TerminateProcess mid-batch_edit (APPLYING_AFTER_1)
        wchar_t self_exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self_exe, MAX_PATH);
        std::wstring cmd = quote_arg(self_exe) + L" --child-crash " + quote_arg(p.file.wstring()) + L" APPLYING_AFTER_1";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(L'\0');
        check(CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE,
              "spawned child process for TerminateProcess resilience test");
        WaitForSingleObject(pi.hProcess, 15000);
        DWORD child_rc = 0;
        GetExitCodeProcess(pi.hProcess, &child_rc);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        check(child_rc == 137, "child process terminated abruptly via TerminateProcess(137)");

        // 2. Call agent_wake and verify it detects INTERRUPTED_RECOVERABLE and reconciles the 7-stage operation
        auto wake = agent_request(p, "agent_wake", {{"actor", "agent-beta"}});
        check(wake["ok"] == true, "agent_wake succeeded after child TerminateProcess");
        const auto& ws_info = wake["workstreams"];
        check(!ws_info["recovered_workstreams"].empty() && ws_info["recovered_workstreams"][0] == "ws-crash-1",
              "agent_wake detected dead PID and transitioned ws-crash-1 to interrupted_recoverable");
        check(!ws_info["recovered_operations"].empty() &&
              ws_info["recovered_operations"][0]["interrupted_stage"] == "APPLYING",
              "agent_wake transitioned interrupted 7-stage operation from APPLYING to RECOVERY");

        const auto& open_ws = ws_info["open_workstreams"][0];
        check(open_ws["status"] == "interrupted_recoverable", "open workstream status is interrupted_recoverable");
        check(!open_ws["completed_before_crash"].empty() && open_ws["completed_before_crash"][0] == "src/a.cpp",
              "src/a.cpp classified as completed_before_crash");
        check(!open_ws["partially_modified_on_disk"].empty() && open_ws["partially_modified_on_disk"][0] == "src/b.cpp",
              "src/b.cpp classified as partially_modified_on_disk after mid-batch crash");
        check(!open_ws["planned_not_yet_touched"].empty() && open_ws["planned_not_yet_touched"][0] == "src/c.cpp",
              "src/c.cpp classified as planned_not_yet_touched");

        // 3. Because src/b.cpp was modified on disk after the last committed checkpoint, takeover WITHOUT resolution MUST block with CONFLICT_EXTERNAL_EDIT!
        bool blocked_conflict = false;
        try {
            agent_request(p, "workstream", {{"action", "takeover"}, {"workstream_id", "ws-crash-1"}, {"actor", "agent-beta"}});
        } catch (const std::exception& e) {
            blocked_conflict = std::string(e.what()).find("CONFLICT_EXTERNAL_EDIT") != std::string::npos;
        }
        check(blocked_conflict, "workstream(takeover) blocks with CONFLICT_EXTERNAL_EDIT when uncheckpointed/external disk changes exist");

        // Takeover WITH explicit resolution='accept_disk' succeeds
        auto tk = agent_request(p, "workstream", {
            {"action", "takeover"},
            {"workstream_id", "ws-crash-1"},
            {"actor", "agent-beta"},
            {"resolution", "accept_disk"}
        });
        check(tk["ok"] == true && tk["status"] == "active" && tk["new_owner_actor"] == "agent-beta",
              "workstream(takeover) with resolution='accept_disk' resumed workstream under agent-beta");

        // 4. Multi-Agent File Lock Conflict (WORKSTREAM_FILE_CONFLICT)
        bool lock_conflict = false;
        try {
            agent_request(p, "workstream", {
                {"action", "start"},
                {"workstream_id", "ws-competing"},
                {"actor", "agent-gamma"},
                {"planned_files", Json::array({"src/a.cpp"})}
            });
        } catch (const std::exception& e) {
            lock_conflict = std::string(e.what()).find("WORKSTREAM_FILE_CONFLICT") != std::string::npos;
        }
        check(lock_conflict, "competing workstream is blocked with WORKSTREAM_FILE_CONFLICT on locked file src/a.cpp");

        // 5. Idempotency Ledger (replay returns idempotent_replay=true; different args with same key throws IDEMPOTENCY_CONFLICT)
        auto be1 = agent_request(p, "batch_edit", {
            {"workstream_id", "ws-crash-1"},
            {"actor", "agent-beta"},
            {"idempotency_key", "idem-op-42"},
            {"edits", Json::array({
                {{"path", "src/a.cpp"}, {"op", "replace_text"}, {"needle", "int a = 100;"}, {"replacement", "int a = 555;"}}
            })}
        });
        check(be1["ok"] == true && read_text(root / L"src" / L"a.cpp") == "int a = 555;\n", "first batch_edit with idempotency_key applied");

        auto be_replay = agent_request(p, "batch_edit", {
            {"workstream_id", "ws-crash-1"},
            {"actor", "agent-beta"},
            {"idempotency_key", "idem-op-42"},
            {"edits", Json::array({
                {{"path", "src/a.cpp"}, {"op", "replace_text"}, {"needle", "int a = 100;"}, {"replacement", "int a = 555;"}}
            })}
        });
        check(be_replay.value("idempotent_replay", false) == true, "retry with identical idempotency_key returned cached response without failing");

        bool idemp_mismatch = false;
        try {
            agent_request(p, "batch_edit", {
                {"workstream_id", "ws-crash-1"},
                {"actor", "agent-beta"},
                {"idempotency_key", "idem-op-42"},
                {"edits", Json::array({
                    {{"path", "src/a.cpp"}, {"op", "replace_text"}, {"needle", "int a = 555;"}, {"replacement", "int a = 777;"}}
                })}
            });
        } catch (const std::exception& e) {
            idemp_mismatch = std::string(e.what()).find("IDEMPOTENCY_CONFLICT") != std::string::npos;
        }
        check(idemp_mismatch, "reusing idempotency_key with different arguments throws IDEMPOTENCY_CONFLICT");

        // 6. P4 Snapshot Retention Policy: active/interrupted workstreams keep pinned snapshots even when housekeep prunes ephemeral ones
        {
            for (int i = 0; i < 4; ++i) {
                agent_request(p, "workstream", {
                    {"action", "snapshot_create"},
                    {"files", Json::array({"src/a.cpp"})},
                    {"label", "eph_" + std::to_string(i)},
                    {"pinned", false}
                });
            }
            auto hk = agent_request(p, "housekeep", {{"action", "run"}, {"keep_ephemeral_snapshots", 2}});
            check(hk["workstream_snapshots"]["protected_pinned_snapshots"].get<int64_t>() >= 1,
                  "P4: housekeep protected pinned snapshots of active workstream");
            check(hk["workstream_snapshots"]["pruned_snapshots_count"].get<int64_t>() >= 2,
                  "P4: housekeep pruned excess ephemeral snapshots");
        }

        // 7. External user edit preservation during workstream(rollback)
        write_text(root / L"src" / L"c.cpp", "int c = 3333; // EXTERNAL USER EDIT\n");
        auto rb = agent_request(p, "workstream", {
            {"action", "rollback"},
            {"workstream_id", "ws-crash-1"},
            {"actor", "agent-beta"},
            {"preserve_external_edits", true}
        });
        check(rb["ok"] == true && rb["status"] == "rolled_back", "workstream(rollback) succeeded");
        check(read_text(root / L"src" / L"a.cpp") == "int a = 1;\n", "rollback restored src/a.cpp to original baseline");
        check(read_text(root / L"src" / L"b.cpp") == "int b = 2;\n", "rollback restored src/b.cpp to original baseline");
        check(read_text(root / L"src" / L"c.cpp") == "int c = 3333; // EXTERNAL USER EDIT\n",
              "rollback preserved external user edit in src/c.cpp without overwriting");

        // 8. Immutable workstream_checkpoints trigger verification
        {
            Database db(p);
            bool update_blocked = false, delete_blocked = false;
            try { db.exec("UPDATE workstream_checkpoints SET summary='tampered'"); } catch (...) { update_blocked = true; }
            try { db.exec("DELETE FROM workstream_checkpoints"); } catch (...) { delete_blocked = true; }
            check(update_blocked && delete_blocked, "workstream_checkpoints enforces BEFORE UPDATE and BEFORE DELETE immutability triggers");
        }

        std::error_code ec;
        fs::remove_all(temp_base, ec);
        std::cout << "agent_os_resilience_tests PASSED (" << g_checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "agent_os_resilience_tests FAILED: " << e.what() << "\n";
        return 1;
    }
}
