#pragma once
#include "migratool/core.hpp"
#include <sqlite3.h>
#include <thread>
#include <memory>
#include <optional>

namespace mt {
class Database {
    sqlite3* db_{};
    HANDLE write_mutex_{};
    std::wstring mutex_name_;
    bool in_tx_{false};
    bool in_aux_write_{false};
    void acquire_write_lock();
    void release_write_lock() noexcept;
public:
    explicit Database(const Project&);
    ~Database();
    Database(const Database&)=delete;
    void exec(const std::string&,const Json& params=Json::array());
    bool try_aux_write(const std::string&,const Json& params=Json::array());
    Json query(const std::string&,const Json& params=Json::array()) const;
    // Streaming callback: false stops without materializing the remaining rows.
    void each(const std::string&,const Json&,const std::function<bool(const Json&)>&) const;
    sqlite3* native_handle() const { return db_; }
    int64_t integer(const std::string&,const Json& params=Json::array()) const;
    int64_t revision() const;
    int64_t change(const std::string& actor,const std::string& action,const Json& detail,
                   const std::string& entity="");
    void begin(int64_t expected=-1);
    void commit();
    void rollback() noexcept;
};
Json backup_catalog(const Project&,const fs::path& destination);
Json index_sync(const Project&,bool full=false,Log={},Cancel* =nullptr,const Json& scope=Json::object());
Json index_snapshot(const Project&,bool include_text=true);
Json make_compact_scan(Database& db,const Project& p);
Json indexed_status(const Project&,bool refresh=false);
Json indexed_mapping(const Project&,const std::string& source,const std::vector<std::string>& targets,
                     const std::vector<std::string>& cases,bool complete,const std::string& note,const std::string& actor,
                     const std::string& stage="",const std::string& disposition="migrated",
                     const std::string& external_replacement="");
void save_evidence(const Project&, const std::string& kind, Json value,
                   const std::function<void(Database&, Json&)>& prepare = {});
void index_record_event(const Project&,const std::string&,const Json&,const std::string&);
Json agent_request(const Project&,const std::string& tool,const Json& args=Json::object(),Log={},Cancel* =nullptr);
Json agent_tools(const std::string& profile="full");
Json agent_batch(const Project&,const Json&);
Json query_page(const Project&,Database&,const Json&);
int mcp_server(const Project&);
int mcp_server(const std::optional<Project>& initial_project, const std::string& profile="full");
class IndexWatcher {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit IndexWatcher(const Project&,unsigned reconcile_ms=900000,Log={},bool catch_up=false);
    ~IndexWatcher();
    IndexWatcher(const IndexWatcher&)=delete;
};
}
