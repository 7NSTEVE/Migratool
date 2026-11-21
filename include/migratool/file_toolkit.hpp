#pragma once
#include "migratool/gateway.hpp"

namespace mt {

// Surgical file reading with line ranges, symbol windowing, sha256, and token_budget
Json tool_read_file(const Project& p, Database& db, const Json& args);

// Safe file creation inside authorized workspace roots + immediate fs_nodes sync
Json tool_create_file(const Project& p, Database& db, const Json& args);

// Transactional file editing with rollback journal:
// mode: "replace_content" | "replace_lines" | "insert_at_line" | "delete_lines" | "replace_in_files"
Json tool_edit_file(const Project& p, Database& db, const Json& args);

// Native multi-threaded content search (literal or regex) over indexed files with context lines and binary skipping
Json tool_search_pattern(const Project& p, Database& db, const Json& args, Cancel* cancel = nullptr);

} // namespace mt
