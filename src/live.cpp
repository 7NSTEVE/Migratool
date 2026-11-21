#include "migratool/live.hpp"

#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <set>
#include <map>
#include <stdexcept>

namespace mt {
namespace {
std::string project_key(const fs::path& path) {
    auto canonical = fs::weakly_canonical(path);
    canonical.make_preferred();
    auto value = canonical.wstring();
    std::wstring expanded(32768, L'\0');
    const auto length = GetLongPathNameW(value.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (length && length < expanded.size()) { expanded.resize(length); value = std::move(expanded); }
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return utf8(value);
}

std::string process_created(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return "";
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return std::to_string(value.QuadPart);
}

bool session_alive(const Json& session) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, session.at("pid").get<DWORD>());
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
                       process_created(process) == session.at("process_created").get<std::string>();
    CloseHandle(process);
    return alive;
}

class Registry {
    sqlite3* handle_{};
public:
    Registry() {
        const auto path = project_registry_path();
        fs::create_directories(path.parent_path());
        if (sqlite3_open_v2(path_text(path).c_str(), &handle_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const auto message = handle_ ? sqlite3_errmsg(handle_) : "Registro no disponible";
            const auto copy = std::string(message);
            if (handle_) sqlite3_close(handle_);
            throw std::runtime_error(copy);
        }
        sqlite3_busy_timeout(handle_, 2000);
        try {
            sqlite3_exec(handle_, "PRAGMA journal_size_limit=16777216;", nullptr, nullptr, nullptr);
            const auto version = query("PRAGMA user_version")[0]["user_version"].get<int>();
            if (version > 1) throw std::runtime_error("Registro de proyectos de una versión posterior");
            if (version == 0) {
                const char* schema = R"sql(
PRAGMA journal_mode=WAL;
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS projects(
 project_key TEXT PRIMARY KEY,path TEXT NOT NULL,name TEXT NOT NULL,last_opened TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS sessions(
 id TEXT PRIMARY KEY,project_key TEXT NOT NULL,pid INTEGER NOT NULL,process_created TEXT NOT NULL,
 interface TEXT NOT NULL,actor TEXT NOT NULL,operation TEXT NOT NULL,state TEXT NOT NULL,
 started TEXT NOT NULL,updated TEXT NOT NULL,finished TEXT,exit_code INTEGER,message TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS sessions_project ON sessions(project_key,started);
CREATE TABLE IF NOT EXISTS activity(
 id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,project_key TEXT NOT NULL,
 at TEXT NOT NULL,actor TEXT NOT NULL,operation TEXT NOT NULL,state TEXT NOT NULL,message TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS activity_project ON activity(project_key,id);
PRAGMA user_version=1;
COMMIT;
)sql";
                if (sqlite3_exec(handle_, schema, nullptr, nullptr, nullptr) != SQLITE_OK)
                    throw std::runtime_error(sqlite3_errmsg(handle_));
            }
            // Conservar sesiones de la primera edición que guardaba las barras del argumento original.
            const auto legacy = query("SELECT project_key,path,name,last_opened FROM projects WHERE instr(project_key,'/')>0");
            if (!legacy.empty()) {
                begin();
                for (const auto& row : legacy) {
                    const auto key = project_key(fs::path(wide(row.at("path"))));
                    query("INSERT INTO projects(project_key,path,name,last_opened)VALUES(?,?,?,?) ON CONFLICT(project_key)DO UPDATE SET path=CASE WHEN excluded.last_opened>projects.last_opened THEN excluded.path ELSE projects.path END,name=CASE WHEN excluded.last_opened>projects.last_opened THEN excluded.name ELSE projects.name END,last_opened=MAX(projects.last_opened,excluded.last_opened)",
                          {key,row["path"],row["name"],row["last_opened"]});
                    query("UPDATE sessions SET project_key=? WHERE project_key=?", {key,row["project_key"]});
                    query("UPDATE activity SET project_key=? WHERE project_key=?", {key,row["project_key"]});
                    query("DELETE FROM projects WHERE project_key=?", {row["project_key"]});
                }
                commit();
            }
        } catch (...) {
            sqlite3_close(handle_);
            throw;
        }
    }
    ~Registry() { sqlite3_close(handle_); }
    Registry(const Registry&) = delete;

    Json query(const std::string& sql, const Json& parameters = Json::array()) {
        sqlite3_stmt* raw{};
        if (sqlite3_prepare_v2(handle_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(handle_));
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        if (sqlite3_bind_parameter_count(raw) != static_cast<int>(parameters.size()))
            throw std::runtime_error("Parámetros de registro inválidos");
        for (size_t i = 0; i < parameters.size(); ++i) {
            const auto& value = parameters[i];
            int rc = SQLITE_OK;
            if (value.is_number_integer() || value.is_number_unsigned())
                rc = sqlite3_bind_int64(raw, static_cast<int>(i) + 1, value.get<int64_t>());
            else if (value.is_null())
                rc = sqlite3_bind_null(raw, static_cast<int>(i) + 1);
            else {
                const auto bytes = value.get<std::string>();
                rc = sqlite3_bind_text(raw, static_cast<int>(i) + 1, bytes.c_str(),
                                       static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(handle_));
        }
        Json rows = Json::array();
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
            Json row = Json::object();
            for (int i = 0; i < sqlite3_column_count(raw); ++i) {
                const auto name = sqlite3_column_name(raw, i);
                switch (sqlite3_column_type(raw, i)) {
                case SQLITE_NULL: row[name] = nullptr; break;
                case SQLITE_INTEGER: row[name] = sqlite3_column_int64(raw, i); break;
                default:
                    row[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw, i)),
                                            sqlite3_column_bytes(raw, i));
                }
            }
            rows.push_back(std::move(row));
        }
        if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(handle_));
        return rows;
    }
    void begin() { query("BEGIN IMMEDIATE"); }
    void commit() { query("COMMIT"); }
};

std::string current_user() {
    wchar_t buffer[256]{};
    DWORD length = static_cast<DWORD>(std::size(buffer));
    return GetUserNameW(buffer, &length) ? utf8(buffer) : "usuario local";
}

std::string result_state(int exit_code, const Json& response) {
    if (exit_code != 0) return exit_code == 4 ? "INCOMPLETE" : "FAILED";
    const auto& result = response.contains("result") ? response["result"] : response;
    if (result.value("cancelled", false)) return "CANCELLED";
    for (const auto* key : {"builds", "cases"}) {
        if (!result.contains("stable")) continue;
        if (!result.contains(key) || !result[key].is_array()) continue;
        if (result[key].empty()) return "INCOMPLETE";
        if (!result.value("stable", false)) return "FAILED";
        for (const auto& item : result[key])
            if (item.value("state", "") != "PASS") return "FAILED";
    }
    if (result.contains("complete") && result["complete"] == false) return "INCOMPLETE";
    return "SUCCEEDED";
}
}

fs::path project_registry_path() {
    std::wstring override_path(32768, L'\0');
    const auto size = GetEnvironmentVariableW(L"MIGRATOOL_REGISTRY_PATH", override_path.data(),
                                               static_cast<DWORD>(override_path.size()));
    if (size && size < override_path.size()) {
        override_path.resize(size);
        return fs::absolute(override_path);
    }
    return fs::path(MIGRATOOL_REGISTRY_DEFAULT);
}

Json registered_projects() {
    Registry registry;
    auto projects = registry.query("SELECT project_key,path,name,last_opened FROM projects ORDER BY last_opened DESC,path");
    auto sessions = registry.query("SELECT s.*,p.path AS project_path,p.name AS project_name FROM sessions s JOIN projects p USING(project_key) WHERE s.finished IS NULL OR s.id IN(SELECT id FROM sessions ORDER BY started DESC LIMIT 500) ORDER BY s.started DESC");
    for (auto& session : sessions) {
        const bool live = session["finished"].is_null() && session_alive(session);
        session["live"] = live;
        if (!live && session["finished"].is_null()) session["state"] = "INTERRUPTED";
    }
    for (auto& project : projects) {
        project["available"] = fs::is_regular_file(fs::path(wide(project["path"])));
        project["active_sessions"] = 0;
        project["operation"] = "";
        project["actor"] = "";
        for (const auto& session : sessions) {
            if (session["project_key"] != project["project_key"]) continue;
            if (project["operation"] == "") {
                project["operation"] = session["operation"];
                project["actor"] = session["actor"];
            }
            if (session["live"] == true)
                project["active_sessions"] = project["active_sessions"].get<int>() + 1;
        }
    }
    const auto revision = registry.query("SELECT COALESCE(MAX(id),0) AS revision FROM activity")[0]["revision"];
    return {{"revision", revision}, {"projects", projects}, {"sessions", sessions},
            {"registry_path", path_text(project_registry_path())}};
}

Json project_activity(const fs::path& project) {
    Registry registry;
    auto rows = registry.query("SELECT * FROM (SELECT id,session_id,at,actor,operation,state,message FROM activity WHERE project_key=? ORDER BY id DESC LIMIT 200) ORDER BY id", {project_key(project)});
    return {{"items", rows}, {"registry_path", path_text(project_registry_path())}};
}

Json project_activity_delta(const fs::path& project,int64_t after){
    Registry registry;
    constexpr auto columns="id,session_id,at,actor,operation,state,message";
    const auto key=project_key(project);
    if(after<=0)return registry.query(std::string("SELECT * FROM (SELECT ")+columns+" FROM activity WHERE project_key=? ORDER BY id DESC LIMIT 15) ORDER BY id",{key});
    return registry.query(std::string("SELECT ")+columns+" FROM activity WHERE project_key=? AND id>? ORDER BY id LIMIT 200",{key,after});
}

Json housekeep_registry(const Json& args, bool dry_run) {
    const int64_t max_sessions = std::clamp<int64_t>(args.value("max_sessions", int64_t(2000)), 50, 200000);
    const int64_t max_activity = std::clamp<int64_t>(args.value("max_activity", int64_t(10000)), 100, 500000);
    const bool prune_missing = args.value("prune_missing_projects", false);

    const fs::path reg_path = project_registry_path();
    std::error_code ec;
    const int64_t bytes_before = fs::is_regular_file(reg_path, ec) ? static_cast<int64_t>(fs::file_size(reg_path, ec)) : 0;

    Registry registry;
    const int64_t total_projects = registry.query("SELECT COUNT(*) AS c FROM projects")[0]["c"].get<int64_t>();
    const int64_t total_sessions = registry.query("SELECT COUNT(*) AS c FROM sessions")[0]["c"].get<int64_t>();
    const int64_t total_activity = registry.query("SELECT COUNT(*) AS c FROM activity")[0]["c"].get<int64_t>();

    // Find dead-PID sessions still marked unfinished
    auto open_sessions = registry.query("SELECT id, pid, process_created FROM sessions WHERE finished IS NULL");
    std::vector<std::string> dead_session_ids;
    for (const auto& s : open_sessions) {
        if (!session_alive(s)) dead_session_ids.push_back(s["id"].get<std::string>());
    }

    // Find missing projects
    std::vector<std::string> missing_project_keys;
    auto all_projects = registry.query("SELECT project_key, path FROM projects");
    for (const auto& pr : all_projects) {
        if (!fs::is_regular_file(fs::path(wide(pr["path"].get<std::string>())), ec)) {
            missing_project_keys.push_back(pr["project_key"].get<std::string>());
        }
    }

    const int64_t finished_sessions = registry.query("SELECT COUNT(*) AS c FROM sessions WHERE finished IS NOT NULL")[0]["c"].get<int64_t>();
    const int64_t excess_sessions = std::max<int64_t>(0, finished_sessions + static_cast<int64_t>(dead_session_ids.size()) - max_sessions);
    const int64_t excess_activity = std::max<int64_t>(0, total_activity - max_activity);

    int64_t closed_dead = 0;
    int64_t pruned_sessions = 0;
    int64_t pruned_activity = 0;
    int64_t pruned_projects = 0;

    if (!dry_run) {
        registry.begin();
        const auto ts = now();
        for (const auto& sid : dead_session_ids) {
            registry.query("UPDATE sessions SET state='INTERRUPTED', finished=?, updated=?, message='Proceso finalizado inesperadamente (limpiado por housekeep)' WHERE id=? AND finished IS NULL",
                           {ts, ts, sid});
            ++closed_dead;
        }
        if (prune_missing) {
            for (const auto& pk : missing_project_keys) {
                registry.query("DELETE FROM activity WHERE project_key=?", {pk});
                registry.query("DELETE FROM sessions WHERE project_key=?", {pk});
                registry.query("DELETE FROM projects WHERE project_key=?", {pk});
                ++pruned_projects;
            }
        }
        const int64_t cur_fin = registry.query("SELECT COUNT(*) AS c FROM sessions WHERE finished IS NOT NULL")[0]["c"].get<int64_t>();
        if (cur_fin > max_sessions) {
            const int64_t to_del = cur_fin - max_sessions;
            registry.query("DELETE FROM sessions WHERE id IN (SELECT id FROM sessions WHERE finished IS NOT NULL ORDER BY started ASC LIMIT ?)", {to_del});
            pruned_sessions = to_del;
        }
        const int64_t max_act_id = registry.query("SELECT COALESCE(MAX(id),0) AS m FROM activity")[0]["m"].get<int64_t>();
        const int64_t cur_act = registry.query("SELECT COUNT(*) AS c FROM activity")[0]["c"].get<int64_t>();
        if (cur_act > max_activity && max_act_id > max_activity) {
            auto cutoff_rows = registry.query("SELECT id FROM activity ORDER BY id DESC LIMIT 1 OFFSET ?", {max_activity});
            if (!cutoff_rows.empty()) {
                const int64_t cut_id = cutoff_rows[0]["id"].get<int64_t>();
                const int64_t before_cnt = cur_act;
                registry.query("DELETE FROM activity WHERE id <= ?", {cut_id});
                const int64_t after_cnt = registry.query("SELECT COUNT(*) AS c FROM activity")[0]["c"].get<int64_t>();
                pruned_activity = std::max<int64_t>(0, before_cnt - after_cnt);
            }
        }
        registry.commit();
        try { registry.query("PRAGMA optimize"); } catch (...) {}
        try { registry.query("PRAGMA wal_checkpoint(PASSIVE)"); } catch (...) {}
    }

    const int64_t bytes_after = fs::is_regular_file(reg_path, ec) ? static_cast<int64_t>(fs::file_size(reg_path, ec)) : 0;
    return {
        {"registry_path", path_text(reg_path)},
        {"dry_run", dry_run},
        {"bytes_before", bytes_before},
        {"bytes_after", bytes_after},
        {"projects_total", total_projects},
        {"missing_projects", static_cast<int64_t>(missing_project_keys.size())},
        {"sessions_total", total_sessions},
        {"activity_total", total_activity},
        {"dead_sessions_detected", static_cast<int64_t>(dead_session_ids.size())},
        {"dead_sessions_closed", dry_run ? static_cast<int64_t>(dead_session_ids.size()) : closed_dead},
        {"sessions_pruned", dry_run ? excess_sessions : pruned_sessions},
        {"activity_pruned", dry_run ? excess_activity : pruned_activity},
        {"projects_pruned", dry_run ? (prune_missing ? static_cast<int64_t>(missing_project_keys.size()) : int64_t(0)) : pruned_projects}
    };
}

struct LiveSession::Impl {
    std::string id = unique_id();
    std::string key;
    std::string author;
    std::string operation;
    bool finished = false;

    void update(const std::string& message, const std::string& state, const Json& exit_code = nullptr) {
        Registry registry;
        registry.begin();
        const auto at = now();
        registry.query("UPDATE sessions SET actor=?,updated=?,state=?,message=?,finished=?,exit_code=? WHERE id=?",
                       {author, at, state, message.substr(0, 4096), finished ? Json(at) : Json(), exit_code, id});
        registry.query("INSERT INTO activity(session_id,project_key,at,actor,operation,state,message)VALUES(?,?,?,?,?,?,?)",
                       {id, key, at, author, operation, state, message.substr(0, 4096)});
        registry.commit();
    }
};

LiveSession::LiveSession(const Project& project, const std::string& interface_name,
                         const std::string& operation, const std::string& actor)
    : impl_(std::make_unique<Impl>()) {
    impl_->key = project_key(project.file);
    impl_->author = actor.empty() ? current_user() : actor;
    impl_->operation = operation;
    Registry registry;
    registry.begin();
    const auto at = now();
    registry.query("INSERT INTO projects(project_key,path,name,last_opened)VALUES(?,?,?,?) ON CONFLICT(project_key)DO UPDATE SET path=excluded.path,name=excluded.name,last_opened=excluded.last_opened",
                   {impl_->key, path_text(fs::weakly_canonical(project.file)), project.config.at("name"), at});
    registry.query("INSERT INTO sessions(id,project_key,pid,process_created,interface,actor,operation,state,started,updated,message)VALUES(?,?,?,?,?,?,?,'RUNNING',?,?,?)",
                   {impl_->id, impl_->key, GetCurrentProcessId(), process_created(GetCurrentProcess()),
                    interface_name, impl_->author, operation, at, at, "Operación iniciada"});
    registry.commit();
    impl_->update("Operación iniciada", "RUNNING");
}

LiveSession::~LiveSession() {
    if (!impl_ || impl_->finished) return;
    try { finish(1); } catch (...) {}
}

void LiveSession::actor(const std::string& name) {
    if (name.empty() || name == impl_->author) return;
    impl_->author = name;
    impl_->update("Actor identificado", "RUNNING");
}

void LiveSession::log(const std::string& message) {
    if (!impl_->finished) impl_->update(message, "RUNNING");
}

void LiveSession::finish(int exit_code, const Json& result) {
    if (impl_->finished) return;
    impl_->finished = true;
    const auto state = result_state(exit_code, result);
    const auto message = result.contains("error") ? result["error"].get<std::string>() :
                         state == "SUCCEEDED" ? "Operación terminada" : "Operación terminada con pendientes o errores";
    impl_->update(message, state, exit_code);
    static std::atomic<uint32_t> finish_counter{0};
    if ((finish_counter.fetch_add(1, std::memory_order_relaxed) & 255u) == 255u) {
        try { (void)housekeep_registry(Json::object(), false); } catch (...) {}
    }
}

Json migration_comparison(const Json& snapshot, const Json& status) {
    Json result = {{"origins", Json::array()}, {"targets", Json::array()}};
    if (!snapshot.is_object() || !status.is_object()) return result;
    result["inventory_complete"] = status.value("inventory_complete", false);
    std::set<std::string> linked_paths;
    std::set<std::string> stale_sources;
    std::map<std::string, Json> target_origins;
    std::map<std::string, Json> target_sources;
    for (const auto& mapping : status.at("mappings"))
        if (mapping.at("state") == "STALE") stale_sources.insert(mapping.value("source_file_id", ""));
    for (const auto& source : status.at("files")) {
        auto item = source;
        std::string state = "PENDING";
        const auto percentage = source.at("migration_percent");
        if (percentage.is_null() || source.at("state") == "STALE") state = "STALE";
        else if (percentage == 100) state = "COMPLETE";
        else if (percentage.get<double>() > 0 || !source.at("target_paths").empty()) state = "IN_PROGRESS";
        if (stale_sources.contains(source.at("id").get<std::string>())) state = "STALE";
        item["comparison_state"] = state;
        for (const auto& target : source.at("target_paths")) {
            const auto path = target.get<std::string>();
            linked_paths.insert(path);
            if (!target_origins.contains(path)) {
                target_origins[path] = Json::array();
                target_sources[path] = Json::array();
            }
            target_origins[path].push_back(source.at("id"));
            target_sources[path].push_back(source.at("source_path"));
        }
        result["origins"].push_back(std::move(item));
    }
    for (const auto& file : snapshot.at("files")) {
        if (file.at("role") != "target") continue;
        if (!file.value("relevant", false) && !linked_paths.contains(file.at("absolute_path"))) continue;
        const auto path = file.at("absolute_path").get<std::string>();
        const auto origins = target_origins.contains(path) ? target_origins.at(path) : Json::array();
        const auto paths = target_sources.contains(path) ? target_sources.at(path) : Json::array();
        result["targets"].push_back({{"id", file.at("id")}, {"absolute_path", file.at("absolute_path")},
                                      {"language", file.at("language")}, {"origins", origins},
                                      {"source_paths", paths}, {"linked", !origins.empty()}});
    }
    return result;
}
}
