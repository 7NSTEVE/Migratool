#include "migratool/gateway.hpp"
#include "migratool/scale.hpp"
#include "migratool/live.hpp"
#include "migratool/workspace_registry.hpp"
#include "migratool/lsp_client.hpp"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace mt {
namespace {

bool is_read_only_mcp_tool(const std::string& name, const Json& args) {
    static const std::set<std::string> ro_tools = {
        "explore_tree", "find_files", "changes_feed", "search_pattern", "read_file",
        "verify_archive_vs_dir", "inspect_workspace_topology", "organize_verify",
        "query", "session_bootstrap", "migration_status", "project_context",
        "project_diagnostics", "next_tasks", "evidence_report", "history",
        "changes_since", "statistics", "connections", "get_task_context",
        "query_workspace", "job_status",
        "get_diagnostics", "find_implementations", "find_declaration"
    };
    if (ro_tools.contains(name)) return true;
    if (name == "find_duplicates") {
        // Read-only only when persist_cache is disabled; default writes to fs_hashes
        return !args.value("persist_cache", true);
    }
    if (name == "list_workspaces") {
        return args.value("action", "list") != "prune" || args.value("dry_run", false);
    }
    if (name == "memory") {
        std::string act = args.value("action", args.value("op", "list"));
        return act == "list" || act == "read" || act == "search";
    }
    if (name == "project_settings") {
        return args.value("action", "get") == "get";
    }
    if (name == "housekeep") {
        std::string act = args.value("action", "status");
        return act == "status" || act == "inspect" || act == "health" || args.value("dry_run", false);
    }
    return false;
}

} // namespace

int mcp_server(const Project& initial_project) {
    return mcp_server(std::optional<Project>(initial_project));
}

int mcp_server(const std::optional<Project>& initial_project) {
    // MCP stdio: negociación explícita + modo projectless opcional + pool concurrente de lecturas WAL (hasta 4 simultáneas).
    const std::set<std::string> supported = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
    std::mutex output_mutex, state_mutex;
    std::condition_variable state_cv;
    bool initialized = false, ready = false;
    std::string client = "unknown", protocol = "2025-11-25", connection = unique_id();
    fs::path active_project_file = initial_project ? initial_project->file : fs::path{};
    std::unique_ptr<IndexWatcher> watcher;

    struct ActiveOp {
        std::shared_ptr<Cancel> cancel;
        bool is_write = false;
        std::thread worker;
        ~ActiveOp() {
            if (worker.joinable()) {
                if (worker.get_id() == std::this_thread::get_id()) worker.detach();
                else worker.join();
            }
        }
    };
    std::map<std::string, std::shared_ptr<ActiveOp>> in_flight;
    int active_reads = 0;
    bool active_write = false;
    std::vector<std::thread> finished_threads;

    auto id_key = [](const Json& id) { return id.dump(); };
    auto join_finished = [&]() {
        std::vector<std::thread> to_join;
        {
            std::lock_guard lk(state_mutex);
            to_join.swap(finished_threads);
        }
        for (auto& t : to_join) if (t.joinable()) t.join();
    };

    std::unique_ptr<Database> db;
    if (initial_project) {
        db = std::make_unique<Database>(*initial_project);
        db->exec("INSERT INTO connections(id,pid,client,opened)VALUES(?,?,?,?)",
                 {connection, GetCurrentProcessId(), client, now()});
    }
    auto send = [&](const Json& value) {
        std::lock_guard lock(output_mutex);
        std::cout << value.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
    };
    auto error = [&](const Json& id, int code, const std::string& message) {
        send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
    };
    auto response = [&](const Json& id, const Json& value) {
        send({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        join_finished();
        if (line.empty()) continue;
        if (line.size() > 16 * 1024 * 1024) { error(nullptr, -32700, "Mensaje excede 16 MiB"); continue; }
        Json request;
        try { request = Json::parse(line); } catch (...) { error(nullptr, -32700, "JSON inválido"); continue; }
        if (!request.is_object() || request.value("jsonrpc", "") != "2.0" || !request.contains("method")) {
            error(request.value("id", Json()), -32600, "Solicitud inválida");
            continue;
        }
        auto method = request.at("method").get<std::string>();
        auto id = request.value("id", Json());
        auto params = request.value("params", Json::object());
        bool notification = !request.contains("id");
        try {
            if (method == "notifications/cancelled") {
                std::lock_guard lock(state_mutex);
                auto req_id = params.value("requestId", Json());
                if (auto it = in_flight.find(id_key(req_id)); it != in_flight.end()) {
                    it->second->cancel->requested = true;
                }
                continue;
            }
            if (method == "notifications/initialized") {
                if (initialized && !ready) {
                    ready = true;
                    if (!active_project_file.empty()) {
                        watcher = std::make_unique<IndexWatcher>(
                            Project::load(active_project_file), 900000,
                            [](const std::string& text) { std::cerr << text << '\n'; }, true);
                    }
                }
                continue;
            }
            if (notification) continue;
            if (method == "initialize") {
                if (initialized) { error(id, -32600, "Ya inicializado"); continue; }
                auto version = params.value("protocolVersion", "2025-11-25");
                if (!supported.contains(version)) version = "2025-11-25";
                initialized = true;
                protocol = version;
                client = params.value("clientInfo", Json::object()).value("name", "unknown");
                if (db) db->exec("UPDATE connections SET client=? WHERE id=?", {client, connection});
                response(id, {
                    {"protocolVersion", version},
                    {"capabilities", {{"tools", {{"listChanged", false}}}}},
                    {"serverInfo", {{"name", "migratool"}, {"version", build_identity()["version"]}}},
                    {"instructions",
                     "Migratool v2.1 (Ultra-Escala 6M+ archivos + Organización/Deduplicación Transaccional + Workspace/LSP + Migraciones). "
                     "Para explorar millones de archivos: activate_workspace (modo archive/workspace sin ensuciar la raíz), explore_tree (flat/columnar), find_files y changes_feed. "
                     "Para ordenar y deduplicar directorios masivos: inspect_workspace_topology, find_duplicates, verify_archive_vs_dir, organize_plan, organize_apply (execute:true, SHA-256 + journal), organize_verify y organize_undo. "
                     "Para código y migraciones: onboard_workspace, memory, read_file, edit_file, search_pattern, LSP, session_bootstrap, get_task_context y complete_task. "
                     "Lecturas concurrentes habilitadas (hasta 4 en paralelo sobre SQLite WAL)."}
                });
                continue;
            }
            if (method == "ping") { response(id, Json::object()); continue; }
            if (!initialized) { error(id, -32000, "Initialize requerido"); continue; }
            if (!ready) { error(id, -32000, "Falta notifications/initialized"); continue; }
            if (method == "tools/list") { response(id, {{"tools", agent_tools()}}); continue; }
            if (method != "tools/call") { error(id, -32601, "Método no implementado: " + method); continue; }

            auto name = params.at("name").get<std::string>();
            bool known = false;
            for (const auto& tool : agent_tools()) if (tool["name"] == name) { known = true; break; }
            if (!known) { error(id, -32602, "Herramienta desconocida: " + name); continue; }

            auto arguments = params.value("arguments", Json::object());
            const bool is_ro = is_read_only_mcp_tool(name, arguments);

            auto op = std::make_shared<ActiveOp>();
            op->cancel = std::make_shared<Cancel>();
            op->is_write = !is_ro;
            const std::string key = id_key(id);
            fs::path default_project_snapshot;

            {
                std::unique_lock lock(state_mutex);
                if (in_flight.contains(key)) {
                    error(id, -32600, "DUPLICATE_REQUEST_ID: ya existe una operación activa con ese id");
                    continue;
                }
                if (!is_ro) {
                    if (active_write || active_reads > 0) {
                        state_cv.wait_for(lock, std::chrono::milliseconds(200),
                                          [&]() { return !active_write && active_reads == 0 && !in_flight.contains(key); });
                    }
                    if (in_flight.contains(key)) {
                        error(id, -32600, "DUPLICATE_REQUEST_ID: ya existe una operación activa con ese id");
                        continue;
                    }
                    if (active_write || active_reads > 0) {
                        error(id, -32000, "Otra operación de lectura o escritura está en curso; espere o cancele su requestId");
                        continue;
                    }
                } else {
                    if (active_write || active_reads >= 4) {
                        state_cv.wait_for(lock, std::chrono::milliseconds(200),
                                          [&]() { return !active_write && active_reads < 4 && !in_flight.contains(key); });
                    }
                    if (in_flight.contains(key)) {
                        error(id, -32600, "DUPLICATE_REQUEST_ID: ya existe una operación activa con ese id");
                        continue;
                    }
                    if (active_write) {
                        error(id, -32000, "Operación de escritura en curso; espere o cancele su requestId");
                        continue;
                    }
                    if (active_reads >= 4) {
                        error(id, -32000, "Máximo de 4 consultas de lectura concurrentes alcanzado; reintente");
                        continue;
                    }
                }
                if (is_ro) ++active_reads;
                else active_write = true;
                in_flight[key] = op;
                default_project_snapshot = active_project_file;
            }

            op->worker = std::thread([&, name, arguments, id, op, key, is_ro, default_project_snapshot]() {
                std::unique_ptr<LiveSession> live;
                auto token = op->cancel;
                try {
                    Json result;
                    if (name == "activate_workspace") {
                        fs::path new_proj;
                        result = tool_activate_workspace(arguments, &new_proj,
                                                         [](const std::string& m) { std::cerr << m << '\n'; }, token.get());
                        if (!new_proj.empty() && arguments.value("switch_active", true)) {
                            std::lock_guard lk(state_mutex);
                            active_project_file = new_proj;
                            try {
                                watcher = std::make_unique<IndexWatcher>(
                                    Project::load(active_project_file), 900000,
                                    [](const std::string& text) { std::cerr << text << '\n'; }, false);
                            } catch (...) {}
                        }
                    } else if (name == "list_workspaces") {
                        result = tool_list_workspaces(arguments);
                    } else if (name == "query_workspace") {
                        result = tool_query_workspace(arguments, [](const std::string& m) { std::cerr << m << '\n'; }, token.get());
                    } else {
                        fs::path selected = default_project_snapshot;
                        if (arguments.contains("project")) {
                            if (!arguments["project"].is_string()) throw std::runtime_error("project debe ser ruta absoluta");
                            selected = fs::path(wide(arguments["project"].get<std::string>()));
                            if (!selected.is_absolute()) throw std::runtime_error("project debe ser ruta absoluta");
                        }
                        if (selected.empty()) {
                            throw std::runtime_error("NO_ACTIVE_WORKSPACE: llame primero a activate_workspace o pase project absoluto.");
                        }
                        auto current = Project::load(selected);
                        auto request_args = arguments;
                        request_args.erase("project");
                        live = std::make_unique<LiveSession>(current, "MCP", name, request_args.value("actor", client));
                        result = agent_request(current, name, request_args,
                                               [&live](const std::string& message) { std::cerr << message << '\n'; live->log(message); },
                                               token.get());
                        live->finish(token->requested ? 1 : 0, result);
                    }

                    if (!token->requested) {
                        const auto payload = result.dump(-1, ' ', false, Json::error_handler_t::replace);
                        Json answer = {{"content", Json::array({{{"type", "text"}, {"text", payload}}})}, {"isError", false}};
                        if (protocol == "2025-06-18" || protocol == "2025-11-25") answer["structuredContent"] = result;
                        response(id, answer);
                    }
                } catch (const std::exception& e) {
                    try { if (live) live->finish(1, {{"error", e.what()}}); } catch (...) {}
                    if (!token->requested) {
                        response(id, {{"content", Json::array({{{"type", "text"}, {"text", Json{{"error", e.what()}}.dump()}}})}, {"isError", true}});
                    }
                }
                std::lock_guard lock(state_mutex);
                if (is_ro) --active_reads;
                else active_write = false;
                if (op->worker.joinable()) {
                    finished_threads.push_back(std::move(op->worker));
                }
                if (auto it = in_flight.find(key); it != in_flight.end() && it->second == op) {
                    in_flight.erase(it);
                }
                state_cv.notify_all();
            });
        } catch (const std::exception& e) {
            if (!notification) error(id, -32602, e.what());
        }
    }

    // Clean shutdown: cancel all in-flight tasks, join workers, stop watcher, terminate LSP child processes!
    std::vector<std::thread> remaining;
    {
        std::lock_guard lock(state_mutex);
        for (auto& [_, op] : in_flight) {
            op->cancel->requested = true;
            if (op->worker.joinable()) remaining.push_back(std::move(op->worker));
        }
        in_flight.clear();
        for (auto& t : finished_threads) remaining.push_back(std::move(t));
        finished_threads.clear();
    }
    for (auto& t : remaining) if (t.joinable()) t.join();
    watcher.reset();
    lsp_shutdown_all();
    if (db) db->exec("UPDATE connections SET closed=? WHERE id=?", {now(), connection});
    return 0;
}

} // namespace mt
