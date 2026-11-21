#pragma once
#include "migratool/gateway.hpp"
#include <string>

namespace mt {

fs::path resolve_git_executable();

Json git_repo_summary_cached(const Project& p, Database& db, const fs::path& repo_dir = {});

Json tool_git_intel(const Project& p, Database& db, const Json& args);

} // namespace mt
