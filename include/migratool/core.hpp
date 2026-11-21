#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <windows.h>

namespace mt {
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;
using Log = std::function<void(const std::string&)>;
struct Cancel { std::atomic_bool requested{false}; };
std::string utf8(const std::wstring&);
std::wstring wide(const std::string&);
std::string path_text(const fs::path&);
fs::path native_path(const fs::path&);
std::string read_text(const fs::path&, size_t limit = 256 * 1024 * 1024);
void write_text(const fs::path&, const std::string&);
Json read_json(const fs::path&);
void write_json(const fs::path&, const Json&);
std::string sha256(const std::string&);
std::string hash_file(const fs::path&);
std::string hash_file(const fs::path&, Cancel*);
std::string now();
std::string unique_id();
fs::path executable_dir();
bool within(const fs::path&, const fs::path&);
std::wstring quote_arg(const std::wstring&);
Json run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                 const std::string& input, const fs::path& logs, unsigned timeout_ms = 30000,
                 Cancel* cancel = nullptr, const Json& env = Json::object());

struct Project {
    fs::path file;
    Json config;
    mutable std::string revision;
    fs::path directory() const { return file.parent_path(); }
    fs::path state_dir() const;
    fs::path resolve(const std::string&) const;
    void save() const;
    static Project load(const fs::path&);
    static Project create(const fs::path&, const std::string&, const Json& roots);
};
void validate(const Json&);
Json inventory(const Project&, Log = {}, Cancel* = nullptr);
Json analyze(const Project&, Json inventory, Log = {}, Cancel* = nullptr);
Json scan(const Project&, Log = {}, Cancel* = nullptr);
Json load_scan(const Project&);
Json freshness(const Project&, const Json&, Cancel* = nullptr);
Json suggestions(const Project&, const Json&);
Json run_builds(const Project&, Log = {}, Cancel* = nullptr);
Json run_cases(const Project&, Log = {}, Cancel* = nullptr,
               const std::vector<std::string>& case_ids = {});
bool case_evidence_fresh(const Json& cfg, const Json& previous, bool tests_current);
Json import_coverage(const Project&, const fs::path&, const std::string& root,
                     const std::string& format, Log = {});
Json status(const Project&, bool check_fresh = true);
void report(const Project&, const fs::path&, bool check_fresh = true);
Json sqlite_snapshot(const fs::path&);
Json compare_outputs(const Json& test, const Json& left, const Json& right,
                     const fs::path& left_work, const fs::path& right_work);
std::string language(const fs::path&);
Json language_capabilities(const Project&);
Json syntactic_ast_scan_file(const fs::path& file_path, const std::string& lang);
void add_mapping(Project&, const std::string& source, const std::vector<std::string>& targets,
                 const std::vector<std::string>& cases, const std::string& note,
                 bool complete = false, const std::string& actor = "");
void event(const Project&, const std::string& action, const Json& detail, const std::string& actor = "");
Json mark_file(Project&, const fs::path& source, const std::vector<fs::path>& targets,
               bool complete, const std::string& note, const std::string& actor,
               const std::vector<std::string>& cases = {}, Cancel* = nullptr,
               const std::string& stage = "", const std::string& disposition = "migrated",
               const std::string& external_replacement = "");
void add_note(const Project&, const std::string& text, const std::string& actor, const std::string& source = "");
Json pending(const Project&, bool check_fresh = true);
}
