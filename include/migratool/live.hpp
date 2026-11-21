#pragma once
#include "migratool/core.hpp"

#include <memory>

namespace mt {
fs::path project_registry_path();
Json registered_projects();
Json project_activity(const fs::path& project);
Json project_activity_delta(const fs::path& project,int64_t after);
Json housekeep_registry(const Json& args = Json::object(), bool dry_run = false);

class LiveSession {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    LiveSession(const Project& project, const std::string& interface_name,
                const std::string& operation, const std::string& actor = "");
    ~LiveSession();
    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;
    void actor(const std::string& name);
    void log(const std::string& message);
    void finish(int exit_code, const Json& result = Json::object());
};

Json migration_comparison(const Json& snapshot, const Json& status);
}
