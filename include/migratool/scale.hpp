#pragma once
#include "migratool/gateway.hpp"
#include <optional>

namespace mt {
void scale_schema(Database&);
Json build_identity();
Json scale_settings(const Project&,const Json&);
Json scale_start(const Project&,const Json&,bool launch=true);
Json scale_jobs(const Project&,const Json&);
Json scale_control(const Project&,const Json&);
int scale_coordinator(const Project&,bool once=false);
int scale_coordinator(const Project&,bool once,bool watch_changes);
Json scale_run(const Project&,const Json&,Log={},Cancel* =nullptr);
Json scale_summary(const Project&,Database&);
Json scale_snapshot(const Project&,Database&);
void scale_fingerprints(Database&);
Json scale_evidence(const Project&,Database&,bool fresh);
Json scale_progress_sql(const Json& evidence);
Json scale_export(const Project&,const Json&,Cancel* =nullptr);
void run_inventory_export(Database&,const Json&,const std::function<void()>& check_control);
Json scale_file_status(const Project&,Database&,const Json&);
Json scale_module_matrix(const Project&,Database&,const Json&);
Json scale_context(const Project&,Database&,const Json&);
Json scale_acquire(const Project&,const Json&);
Json scale_import(const Project&,const Json&,Cancel* =nullptr);
Json scale_doctor(const Project&);
std::optional<Json> scale_query(const Project&,Database&,const Json&);
bool scale_enabled(const Project&,Database&);
void scale_spawn(const Project&);
Json classify_inventory_file(const Project&,const fs::path&,const fs::path&,bool);
Json classify_inventory_name(const Project&,const fs::path&,const fs::path&,bool);
bool probe_inventory_code(const Project&,const fs::path&,std::optional<std::string_view>);
int scale_analyzer(const Project&,const fs::path&,const fs::path&);
}
