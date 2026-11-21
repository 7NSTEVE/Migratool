#pragma once
#include "migratool/gateway.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace mt {
using CodeProbe = std::function<bool(const fs::path&, std::optional<std::string_view>)>;
using ZipEntrySink = std::function<void(const Json&, bool)>;
struct ZipScanOptions {
    int64_t start_index = 0;
    int64_t start_nested_index = -10;
    int max_depth = 0;
    uint64_t max_nested_bytes = 64ULL * 1024 * 1024;
    size_t max_nested_entries = 10000;
    std::function<void(int64_t next_index, int64_t next_nested_index)> on_progress{};
};
Json zip_code_entries(const fs::path&, const CodeProbe&, Cancel*, const ZipEntrySink& = {}, const ZipScanOptions& = {});
Json duplicate_code_report(const Project&, Database&);
Json group_code_copies(const Json& files, const Json& archive_cache);
}
