#include "migratool/ast_engine.hpp"
#include "migratool/file_toolkit.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/gateway.hpp"
#include "migratool/lsp_client.hpp"
#include "migratool/memories.hpp"
#include "migratool/workspace_registry.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace mt;
int checks = 0;

void check(bool cond, const std::string& msg) {
    ++checks;
    if (!cond) throw std::runtime_error("FAILED CHECK: " + msg);
}

bool expect_throws(const std::function<void()>& fn, const std::string& msg) {
    ++checks;
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    throw std::runtime_error("EXPECTED EXCEPTION NOT THROWN: " + msg);
}

} // namespace

int main(int argc, char** argv) {
    try {
        (void)argc; (void)argv;
        auto base = fs::temp_directory_path() / wide("migratool_serena_lsp_test_" + unique_id());
        fs::create_directories(base);
        struct Cleanup {
            fs::path p;
            ~Cleanup() {
                lsp_shutdown_all();
                std::error_code ec;
                fs::remove_all(p, ec);
            }
        } cleanup{base};

        // Isolate workspace registry & projects registry to temp directory
        auto ws_root = base / L"workspaces";
        auto reg_db = base / L"projects.sqlite";
        SetEnvironmentVariableW(L"MIGRATOOL_WORKSPACES_DIR", ws_root.c_str());
        SetEnvironmentVariableW(L"MIGRATOOL_REGISTRY_PATH", reg_db.c_str());

        // =====================================================================
        // TEST 1: AST Parser Disambiguation (def foo(...) vs foo(...) call!)
        // =====================================================================
        {
            const std::string py_code =
                "# comment: fetch_data(a, b)\n"
                "s = \"fetch_data(a, b)\"\n"
                "def fetch_data(url, timeout):\n"
                "    return url\n"
                "res1 = fetch_data(api_url, 30)\n"
                "res2 = client.fetch_data(other_url, 60)\n";
            auto matches = mt::ast::find_ast_matches(py_code, "fetch_data($A, $B)", "Python");
            check(matches.size() == 2, "AST matches ONLY 2 real call sites, NOT def/comment/string (got " + std::to_string(matches.size()) + ")");
            check(matches[0].line == 5 && matches[1].line == 6, "AST call matches are lines 5 and 6");

            auto [rewritten, rw_matches] = mt::ast::rewrite_ast_pattern(py_code, "fetch_data($A, $B)", "fetch_v2($B, $A)", "Python");
            check(rw_matches.size() == 2, "AST rewrite touched exactly 2 calls");
            check(rewritten.find("def fetch_data(url, timeout):") != std::string::npos, "AST rewrite preserved Python def header intact");
            check(rewritten.find("# comment: fetch_data(a, b)") != std::string::npos, "AST rewrite preserved comment intact");
            check(rewritten.find("fetch_v2(30, api_url)") != std::string::npos, "AST rewrite swapped args on call 1");

            const std::string cpp_code =
                "int compute(int a, int b) {\n"
                "    return a + b;\n"
                "}\n"
                "int main() { return compute(1, 2); }\n";
            auto cpp_matches = mt::ast::find_ast_matches(cpp_code, "compute($X, $Y)", "C++");
            check(cpp_matches.size() == 1 && cpp_matches[0].line == 4, "AST matches C++ call line 4 and NOT function definition line 1");
        }

        // =====================================================================
        // TEST 2: Zero-Config activate_workspace + onboard_workspace + list/query_workspace
        // =====================================================================
        std::cout << "[STEP 1 OK: AST]\n" << std::flush;
        auto repo_a = base / L"repo_alpha";
        auto repo_b = base / L"repo_beta";
        fs::create_directories(repo_a / L"src");
        fs::create_directories(repo_b / L"docs");
        write_text(repo_a / L"CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\nproject(Alpha)\n");
        write_text(repo_a / L"src" / L"app.py",
                   "def greet(name):\n"
                   "    return f'Hello {name}'\n\n"
                   "def run():\n"
                   "    return greet('world')\n");
        write_text(repo_a / L"src" / L"helper.py",
                   "def helper_fn():\n"
                   "    return greet('helper')\n");
        write_text(repo_b / L"docs" / L"secret_note.md", "Cross-workspace federated architecture note: DELTA_TOKEN_42\n");

        auto act_a = tool_activate_workspace({{"path", path_text(repo_a)}, {"name", "AlphaRepo"}, {"mode", "workspace"}});
        check(act_a["ok"] == true && act_a["created"] == true, "activate_workspace created AlphaRepo externally");
        check(!fs::exists(repo_a / L".migratool"), "activate_workspace kept repo_a root 100% clean");

        auto act_b = tool_activate_workspace({{"path", path_text(repo_b)}, {"name", "BetaDocs"}, {"mode", "archive"}});
        check(act_b["ok"] == true, "activate_workspace created BetaDocs archive");

        auto p_a = Project::load(fs::path(wide(act_a["project_path"].get<std::string>())));
        auto list_ws = tool_list_workspaces();
        check(list_ws["count"].get<int64_t>() >= 2, "list_workspaces sees both AlphaRepo and BetaDocs");

        // Federated cross-workspace query from AlphaRepo into BetaDocs!
        auto fed = tool_query_workspace({{"workspace", "BetaDocs"}, {"tool", "search_pattern"}, {"arguments", {{"pattern", "DELTA_TOKEN_42"}}}});
        check(fed["result"]["items"].size() == 1, "query_workspace searched BetaDocs cleanly");
        // Negative test: mutating tool rejected in federated query_workspace
        expect_throws([&]() {
            tool_query_workspace({{"workspace", "BetaDocs"}, {"tool", "create_file"}, {"arguments", {{"path", "evil.txt"}, {"content", "x"}}}});
        }, "query_workspace rejects mutating tools");

        // Onboarding creates _onboarding memory automatically
        auto onb = agent_request(p_a, "onboard_workspace", {});
        check(onb["onboarding_memory"]["ok"] == true, "onboard_workspace persisted _onboarding memory");

        // =====================================================================
        // TEST 3: Persistent Project Memories (write, read, edit, search, rename, delete + FRN move tracking!)
        // =====================================================================
        std::cout << "[STEP 2 OK: Workspaces]\n" << std::flush;
        auto m_wr = agent_request(p_a, "memory", {
            {"action", "write"}, {"name", "arch_decisions"},
            {"content", "# Architecture\nWe use greet() in src/app.py for greetings.\n"},
            {"tags", Json::array({"design", "python"})},
            {"linked_paths", Json::array({"src/app.py"})}
        });
        check(m_wr["ok"] == true, "memory write succeeded");
        check(fs::is_regular_file(fs::path(wide(m_wr["mirror_path"].get<std::string>()))), "memory markdown mirror created in state_dir");

        auto m_search = agent_request(p_a, "memory", {{"action", "search"}, {"query", "greetings"}});
        check(m_search["count"].get<int64_t>() == 1, "memory FTS5 search finds arch_decisions");

        auto m_edit = agent_request(p_a, "memory", {
            {"action", "edit"}, {"name", "arch_decisions"},
            {"needle", "greetings"}, {"repl", "internationalized greetings"}
        });
        check(m_edit["ok"] == true && m_edit["replacements"] == 1, "memory edit replaced literal needle");

        auto m_rd = agent_request(p_a, "memory", {{"action", "read"}, {"name", "arch_decisions"}});
        check(m_rd["content"].get<std::string>().find("internationalized greetings") != std::string::npos, "memory read sees edited text");
        check(m_rd["linked_files"].size() == 1 && m_rd["linked_files"][0]["status"] == "ok", "linked file FRN status ok");

        auto m_ren = agent_request(p_a, "memory", {{"action", "rename"}, {"old_name", "arch_decisions"}, {"new_name", "architecture_v2"}});
        check(m_ren["ok"] == true, "memory rename ok");
        auto m_del = agent_request(p_a, "memory", {{"action", "delete"}, {"name", "architecture_v2"}});
        check(m_del["ok"] == true, "memory delete ok");

        // =====================================================================
        // TEST 4: Surgical File & Pattern Toolkit + Atomic Multi-File Rollback Negative Test!
        // =====================================================================
        std::cout << "[STEP 3 OK: Memories]\n" << std::flush;
        auto rf = agent_request(p_a, "read_file", {{"path", "src/app.py"}, {"start_line", 1}, {"end_line", 3}});
        check(rf["total_lines"] == 5 && rf["start_line"] == 1 && rf["end_line"] == 3, "read_file exact line slice");
        const std::string app_sha = rf["sha256"].get<std::string>();

        // Negative test: path traversal outside workspace denied
        expect_throws([&]() {
            agent_request(p_a, "read_file", {{"path", "../repo_beta/docs/secret_note.md"}});
        }, "read_file blocks path traversal outside workspace");

        // Negative test: stale sha256 rejected on edit_file
        expect_throws([&]() {
            agent_request(p_a, "edit_file", {{"mode", "replace_content"}, {"path", "src/app.py"},
                                             {"needle", "world"}, {"replacement", "universe"},
                                             {"expected_sha256", "0000000000000000"}});
        }, "edit_file rejects mismatched expected_sha256");

        // Create file + immediate fs_nodes sync
        auto cf = agent_request(p_a, "create_file", {{"path", "src/created.py"}, {"content", "VALUE = 123\n"}});
        check(cf["ok"] == true, "create_file succeeded");
        auto ff_created = agent_request(p_a, "find_files", {{"query", "created.py"}});
        check(ff_created["items"].size() == 1, "create_file immediately synced into fs_nodes");

        // A09 NEGATIVE: create_file with inject_db_failure MUST roll back the file on disk!
        expect_throws([&]() {
            agent_request(p_a, "create_file", {{"path", "src/ghost_fail.py"}, {"content", "GHOST = 1\n"}, {"inject_db_failure", true}});
        }, "A09: create_file inject_db_failure throws");
        check(!fs::exists(repo_a / L"src" / L"ghost_fail.py"), "A09: create_file removed file from disk when SQLite phase failed");

        // Multi-file replace_in_files ROLLBACK negative test (inject failure after file #1!)
        const std::string app_before = read_text(repo_a / L"src" / L"app.py");
        const std::string helper_before = read_text(repo_a / L"src" / L"helper.py");
        expect_throws([&]() {
            agent_request(p_a, "edit_file", {
                {"mode", "replace_in_files"},
                {"paths", Json::array({"src/app.py", "src/helper.py"})},
                {"needle", "greet("},
                {"replacement", "salute("},
                {"inject_failure_after_file", 1}
            });
        }, "replace_in_files injected failure triggers exception");
        check(read_text(repo_a / L"src" / L"app.py") == app_before, "atomic rollback restored src/app.py byte-for-byte");
        check(read_text(repo_a / L"src" / L"helper.py") == helper_before, "atomic rollback kept src/helper.py byte-for-byte");

        // A09 NEGATIVE: edit_file with inject_db_failure (after ALL files were written to disk!) MUST restore all files on disk!
        expect_throws([&]() {
            agent_request(p_a, "edit_file", {
                {"mode", "replace_in_files"},
                {"paths", Json::array({"src/app.py", "src/helper.py"})},
                {"needle", "greet("},
                {"replacement", "salute("},
                {"inject_db_failure", true}
            });
        }, "A09: edit_file inject_db_failure throws");
        check(read_text(repo_a / L"src" / L"app.py") == app_before, "A09: DB failure rolled back src/app.py on disk byte-for-byte");
        check(read_text(repo_a / L"src" / L"helper.py") == helper_before, "A09: DB failure rolled back src/helper.py on disk byte-for-byte");

        // Now perform real atomic multi-file edit + verify with search_pattern!
        auto ef = agent_request(p_a, "edit_file", {
            {"mode", "replace_in_files"},
            {"paths", Json::array({"src/app.py", "src/helper.py"})},
            {"needle", "greet("},
            {"replacement", "salute("}
        });
        check(ef["ok"] == true && ef["files_modified"] == 2, "replace_in_files updated both files atomically");

        auto sp_res = agent_request(p_a, "search_pattern", {{"pattern", "salute\\("}, {"is_regex", true}, {"context_lines", 1}});
        check(sp_res["returned"].get<int64_t>() == 3, "search_pattern regex found all 3 occurrences of salute(");

        // =====================================================================
        // TEST 5: Universal LSP Bridge End-to-End with Deterministic Mock Stdio LSP Server!
        // =====================================================================
        std::cout << "[STEP 4 OK: FileToolkit]\n" << std::flush;
        auto mock_lsp_py = base / L"mock_lsp.py";
        write_text(mock_lsp_py, R"py(
import json, sys

def send(obj):
    data = json.dumps(obj).encode('utf-8')
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode('ascii') + data)
    sys.stdout.buffer.flush()

def read_msg():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.decode('ascii', errors='ignore').strip()
        if not line:
            break
        if ':' in line:
            k, v = line.split(':', 1)
            headers[k.strip().lower()] = v.strip()
    n = int(headers.get('content-length', '0'))
    if n <= 0:
        return None
    body = sys.stdin.buffer.read(n)
    return json.loads(body.decode('utf-8'))

while True:
    msg = read_msg()
    if msg is None:
        break
    method = msg.get('method', '')
    mid = msg.get('id')
    params = msg.get('params') or {}
    if method == 'initialize':
        send({'jsonrpc': '2.0', 'id': mid, 'result': {'capabilities': {
            'definitionProvider': True,
            'declarationProvider': True,
            'implementationProvider': True,
            'renameProvider': True,
            'diagnosticProvider': {'interFileDependencies': False, 'workspaceDiagnostics': False}
        }}})
    elif method == 'textDocument/didOpen':
        uri = params['textDocument']['uri']
        send({'jsonrpc': '2.0', 'method': 'textDocument/publishDiagnostics', 'params': {
            'uri': uri,
            'diagnostics': [{'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 0, 'character': 5}},
                             'severity': 2, 'source': 'mock-lsp', 'message': 'Mock unused warning'}]
        }})
    elif method == 'textDocument/diagnostic':
        send({'jsonrpc': '2.0', 'id': mid, 'result': {'kind': 'full', 'items': [
            {'range': {'start': {'line': 0, 'character': 4}, 'end': {'line': 0, 'character': 10}},
             'severity': 2, 'source': 'mock-lsp', 'message': 'Mock pull warning'}
        ]}})
    elif method == 'textDocument/declaration':
        uri = params['textDocument']['uri']
        send({'jsonrpc': '2.0', 'id': mid, 'result': {
            'uri': uri, 'range': {'start': {'line': 0, 'character': 4}, 'end': {'line': 0, 'character': 10}}
        }})
    elif method == 'textDocument/implementation':
        uri = params['textDocument']['uri']
        send({'jsonrpc': '2.0', 'id': mid, 'result': [
            {'uri': uri, 'range': {'start': {'line': 3, 'character': 4}, 'end': {'line': 3, 'character': 7}}}
        ]})
    elif method == 'textDocument/rename':
        uri = params['textDocument']['uri']
        new_name = params['newName']
        if new_name == '__overlap__':
            send({'jsonrpc': '2.0', 'id': mid, 'result': {'changes': {
                uri: [
                    {'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 0, 'character': 8}}, 'newText': 'A'},
                    {'range': {'start': {'line': 0, 'character': 4}, 'end': {'line': 0, 'character': 12}}, 'newText': 'B'}
                ]
            }}})
        elif new_name == '__invalid_range__':
            send({'jsonrpc': '2.0', 'id': mid, 'result': {'changes': {
                uri: [
                    {'range': {'start': {'line': 0, 'character': 12}, 'end': {'line': 0, 'character': 4}}, 'newText': 'X'}
                ]
            }}})
        elif new_name == 'doc_precedence_fn':
            send({'jsonrpc': '2.0', 'id': mid, 'result': {
                'changes': {
                    uri: [{'range': {'start': {'line': 0, 'character': 27}, 'end': {'line': 0, 'character': 37}}, 'newText': 'WRONG_DUPLICATE'}]
                },
                'documentChanges': [{
                    'textDocument': {'uri': uri, 'version': 1},
                    'edits': [{'range': {'start': {'line': 0, 'character': 27}, 'end': {'line': 0, 'character': 37}}, 'newText': new_name}]
                }]
            }})
        elif 'unicode_lsp.py' in uri:
            # Line 0: 'msg = "¡Camión 🚀 "; val = target_fn(1)\n'
            # UTF-16 code units before 'target_fn' = 27 (UTF-8 bytes = 31)
            send({'jsonrpc': '2.0', 'id': mid, 'result': {'changes': {
                uri: [
                    {'range': {'start': {'line': 0, 'character': 27}, 'end': {'line': 0, 'character': 36}}, 'newText': new_name}
                ]
            }}})
        else:
            send({'jsonrpc': '2.0', 'id': mid, 'result': {'changes': {
                uri: [
                    {'range': {'start': {'line': 0, 'character': 4}, 'end': {'line': 0, 'character': 10}}, 'newText': new_name},
                    {'range': {'start': {'line': 4, 'character': 11}, 'end': {'line': 4, 'character': 17}}, 'newText': new_name}
                ]
            }}})
    elif method == 'shutdown':
        send({'jsonrpc': '2.0', 'id': mid, 'result': None})
    elif method == 'exit':
        break
)py");

        // Configure mock LSP for Python on AlphaRepo project (resolve python from PATH)
        std::string python_exe = "python";
        wchar_t py_buf[MAX_PATH]{};
        if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, py_buf, nullptr) > 0) {
            python_exe = path_text(fs::path(py_buf));
        }
        auto cfg_a = p_a.config;
        cfg_a["lsp_servers"] = {{"python", Json::array({python_exe, path_text(mock_lsp_py)})}};
        write_json(p_a.file, cfg_a);
        p_a = Project::load(p_a.file);

        auto d_res = agent_request(p_a, "get_diagnostics", {{"path", "src/app.py"}});
        check(d_res["backend"].get<std::string>().starts_with("lsp:"), "get_diagnostics used live LSP backend");
        check(d_res["count"].get<int64_t>() == 1, "get_diagnostics returned 1 mock diagnostic");

        auto decl_res = agent_request(p_a, "find_declaration", {{"path", "src/app.py"}, {"line", 5}, {"column", 12}});
        check(decl_res["count"].get<int64_t>() == 1 && decl_res["items"][0]["line"] == 1, "find_declaration resolved line 1 via LSP");

        auto impl_res = agent_request(p_a, "find_implementations", {{"path", "src/app.py"}, {"line", 1}, {"column", 5}});
        check(impl_res["count"].get<int64_t>() == 1 && impl_res["items"][0]["line"] == 4, "find_implementations resolved line 4 via LSP");

        // A09 NEGATIVE: lsp_rename_symbol with inject_db_failure MUST roll back file on disk!
        const std::string app_pre_lsp = read_text(repo_a / L"src" / L"app.py");
        expect_throws([&]() {
            agent_request(p_a, "lsp_rename_symbol", {{"path", "src/app.py"}, {"line", 1}, {"column", 5}, {"new_name", "welcome"}, {"inject_db_failure", true}});
        }, "A09: lsp_rename_symbol inject_db_failure throws");
        check(read_text(repo_a / L"src" / L"app.py") == app_pre_lsp, "A09: lsp_rename_symbol rolled back src/app.py on disk when SQLite phase failed");

        auto ren_res = agent_request(p_a, "lsp_rename_symbol", {{"path", "src/app.py"}, {"line", 1}, {"column", 5}, {"new_name", "welcome"}});
        check(ren_res["ok"] == true && ren_res["files_modified"] == 1, "lsp_rename_symbol applied WorkspaceEdit transactionally");
        auto after_ren = read_text(repo_a / L"src" / L"app.py");
        check(after_ren.find("def welcome(name):") != std::string::npos && after_ren.find("return welcome('world')") != std::string::npos,
              "lsp_rename_symbol renamed both definition and call cleanly");

        // A10: UTF-16 code unit column conversion on line with UTF-8 accents + 4-byte emoji (surrogate pair)
        write_text(repo_a / L"src" / L"unicode_lsp.py", "msg = \"\xC2\xA1" "Cami\xC3\xB3n \xF0\x9F\x9A\x80 \"; val = target_fn(1)\n");
        auto u_ren = agent_request(p_a, "lsp_rename_symbol", {{"path", "src/unicode_lsp.py"}, {"line", 1}, {"column", 28}, {"new_name", "renamed_fn"}});
        check(u_ren["ok"] == true, "A10: lsp_rename_symbol succeeded on multibyte UTF-8 + emoji line");
        const std::string after_u_ren = read_text(repo_a / L"src" / L"unicode_lsp.py");
        check(after_u_ren == "msg = \"\xC2\xA1" "Cami\xC3\xB3n \xF0\x9F\x9A\x80 \"; val = renamed_fn(1)\n",
              "A10: UTF-16 code units converted to exact UTF-8 byte offsets without shifting (got: " + after_u_ren + ")");

        // A10+ NEGATIVE: Overlapping TextEdits & inverted ranges MUST be rejected before touching disk
        expect_throws([&]() {
            agent_request(p_a, "lsp_rename_symbol", {{"path", "src/unicode_lsp.py"}, {"line", 1}, {"column", 28}, {"new_name", "__overlap__"}});
        }, "A10+: overlapping LSP TextEdits rejected");
        expect_throws([&]() {
            agent_request(p_a, "lsp_rename_symbol", {{"path", "src/unicode_lsp.py"}, {"line", 1}, {"column", 28}, {"new_name", "__invalid_range__"}});
        }, "A10+: inverted LSP TextEdit range rejected");
        check(read_text(repo_a / L"src" / L"unicode_lsp.py") == after_u_ren,
              "A10+: file untouched after overlapping/invalid LSP WorkspaceEdit rejections");

        // A10+: When both documentChanges and changes are present, documentChanges takes exclusive precedence (no double apply)
        auto dc_ren = agent_request(p_a, "lsp_rename_symbol", {{"path", "src/unicode_lsp.py"}, {"line", 1}, {"column", 28}, {"new_name", "doc_precedence_fn"}});
        check(dc_ren["ok"] == true, "A10+: documentChanges precedence rename succeeded");
        check(read_text(repo_a / L"src" / L"unicode_lsp.py") == "msg = \"\xC2\xA1" "Cami\xC3\xB3n \xF0\x9F\x9A\x80 \"; val = doc_precedence_fn(1)\n",
              "A10+: documentChanges took exclusive precedence over changes without duplicate application");

        auto rst_res = agent_request(p_a, "restart_language_server", {{"language", "python"}});
        check(rst_res["ok"] == true && rst_res["servers_restarted"].get<int>() == 1, "restart_language_server cleanly stopped and cleared session");

        // Fallback test: language with no configured LSP falls back gracefully to native index
        write_text(repo_a / L"src" / L"sample.xyz", "hello\n");
        auto fb_res = agent_request(p_a, "get_diagnostics", {{"path", "src/sample.xyz"}});
        check(fb_res["backend"] == "native_index_fallback", "unknown language falls back gracefully to native index");

        // =========================================================================
        // 6. SELF-HOUSEKEEPING / AUTOASEO TESTS
        // =========================================================================
        {
            Database hk_db(p_a);
            hk_db.begin();
            for (int i = 0; i < 160; ++i) {
                hk_db.exec("INSERT INTO requests(at,tool,request_bytes,response_bytes,duration_us,cache_revision)VALUES(?,?,?,?,?,?)",
                           {"2025-01-01T00:00:00Z", "find_files", 10, 20, 100, 1});
            }
            hk_db.exec("INSERT OR REPLACE INTO task_claims(entity,actor,session_id,claimed_at,expires_at,lease_seconds,note,revision)VALUES('F999','ghost','s1','2020-01-01T00:00:00Z','2020-01-01T00:10:00Z',600,'expired',1)");
            hk_db.exec("INSERT OR REPLACE INTO connections(id,pid,client,opened)VALUES('dead-conn-1',999999,'dead-client','2020-01-01T00:00:00Z')");
            hk_db.exec("INSERT OR REPLACE INTO watchers(id,pid,root,opened,error)VALUES('dead-watch-1',999999,'main','2020-01-01T00:00:00Z','')");
            hk_db.commit();
        }
        const auto stale_tmp = p_a.state_dir() / L"catalog.sqlite.tmp-orphan123";
        write_text(stale_tmp, "stale temp payload");
        const auto old_bak = p_a.state_dir() / L"backups" / L"complete_task" / L"old_edit.bak";
        write_text(old_bak, "old edit backup payload");

        auto hk_st = agent_request(p_a, "housekeep", {{"action", "status"}, {"stale_temp_hours", 0}, {"backup_retention_days", 0}});
        check(hk_st["auto_housekeep_enabled"] == true, "housekeep status reports auto_housekeep_enabled=true");
        check(hk_st["catalog"]["auto_vacuum"] == "INCREMENTAL", "new catalog DB created with PRAGMA auto_vacuum=INCREMENTAL");
        check(hk_st["needs_attention"] == true, "housekeep status detects bloat/expired claims/dead pids/stale files");
        check(hk_st["tables"]["task_claims_expired"].get<int>() >= 1, "housekeep status detects expired task_claims");
        check(hk_st["tables"]["dead_open_connections"].get<int>() >= 1, "housekeep status detects dead open connections");
        check(hk_st["tables"]["dead_open_watchers"].get<int>() >= 1, "housekeep status detects dead open watchers");
        check(hk_st["stale_state_files"]["count"].get<int>() >= 2, "housekeep status detects stale temp & old edit backups");

        auto hk_dry = agent_request(p_a, "housekeep", {{"action", "run"}, {"dry_run", true}, {"max_requests", 100}, {"stale_temp_hours", 0}, {"backup_retention_days", 0}});
        check(hk_dry["dry_run"] == true && hk_dry["pruned"]["requests"].get<int>() >= 60, "housekeep dry_run predicts requests to prune");
        check(fs::exists(stale_tmp) && fs::exists(old_bak), "housekeep dry_run does NOT delete stale files");

        auto hk_run = agent_request(p_a, "housekeep", {{"action", "run"}, {"max_requests", 100}, {"stale_temp_hours", 0}, {"backup_retention_days", 0}});
        check(hk_run["ok"] == true && hk_run["dry_run"] == false, "housekeep run succeeds");
        check(hk_run["pruned"]["requests"].get<int>() >= 60, "housekeep run pruned excess requests");
        check(hk_run["pruned"]["task_claims_expired"].get<int>() >= 1, "housekeep run pruned expired task_claims");
        check(hk_run["pruned"]["connections_closed_dead"].get<int>() >= 1, "housekeep run closed dead connections");
        check(hk_run["pruned"]["watchers_closed_dead"].get<int>() >= 1, "housekeep run closed dead watchers");
        check(!fs::exists(stale_tmp) && !fs::exists(old_bak), "housekeep run recycled stale temp and old edit backup files");
        check(fs::exists(p_a.state_dir() / L"catalog.sqlite"), "housekeep run never touches catalog.sqlite");

        auto hk_after = agent_request(p_a, "housekeep", {{"action", "status"}, {"stale_temp_hours", 0}, {"backup_retention_days", 0}});
        check(hk_after["tables"]["task_claims_expired"].get<int>() == 0, "after housekeep: zero expired claims");
        check(hk_after["tables"]["dead_open_connections"].get<int>() == 0, "after housekeep: zero dead open connections");
        check(hk_after["tables"]["dead_open_watchers"].get<int>() == 0, "after housekeep: zero dead open watchers");
        check(hk_after["tables"]["requests"].get<int>() <= 105, "after housekeep: requests bounded to cap");
        check(!hk_after["last_housekeep_at"].is_null(), "after housekeep: last_housekeep_at recorded");

        bool invalid_hk_threw = false;
        try {
            (void)agent_request(p_a, "housekeep", {{"action", "invalid_op"}});
        } catch (...) {
            invalid_hk_threw = true;
        }
        check(invalid_hk_threw, "housekeep rejects invalid action");

        std::cout << "serena_lsp_tests PASSED (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "serena_lsp_tests FAILED: " << e.what() << "\n";
        return 1;
    }
}
