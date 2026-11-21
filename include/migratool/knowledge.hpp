#pragma once
#include "migratool/gateway.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mt {

void knowledge_schema(Database& db);

void knowledge_invalidate_for_path(Database& db, uint64_t frn, const std::string& rel_path,
                                   const std::string& new_sha256);

Json knowledge_warnings_for_files(const Project& p, Database& db, const std::vector<std::string>& rel_paths,
                                  const std::vector<uint64_t>& frns = {}, int limit = 10);

Json tool_knowledge(const Project& p, Database& db, const Json& args);

} // namespace mt
