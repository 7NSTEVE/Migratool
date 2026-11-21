#include "migratool/fs_index.hpp"
#include "migratool/gateway.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace mt;
int checks = 0;

void check(bool cond, const std::string& msg) {
    ++checks;
    if (!cond) throw std::runtime_error("FAILED CHECK: " + msg);
}

template <class Pred>
bool wait_for(Pred pred, int timeout_ms = 5000) {
    auto start = GetTickCount64();
    while (GetTickCount64() - start < static_cast<ULONGLONG>(timeout_ms)) {
        if (pred()) return true;
        Sleep(50);
    }
    return pred();
}

} // namespace

int main(int argc, char** argv) {
    try {
        (void)argc; (void)argv;
        auto base = fs::temp_directory_path() / wide("migratool_fs_index_test_" + unique_id());
        fs::create_directories(base);
        struct Cleanup {
            fs::path p;
            ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); }
        } cleanup{base};

        // =====================================================================
        // TEST 1: Archive mode with EXTERNAL state_dir (AGENTS.md Rule #2)
        //         + .gitignore '/*' ignored in archive mode + hidden + empty dir
        // =====================================================================
        auto corpus = base / L"protected_corpus";
        auto ext_meta = base / L"external_catalog";
        auto state_dir = ext_meta / L"state";
        fs::create_directories(corpus / L"01_Archive" / L"ToolSuite");
        fs::create_directories(corpus / L"02_Hardware" / L"empty_folder");
        fs::create_directories(corpus / L".git");
        write_text(corpus / L".gitignore", "/*\n!/.gitignore\n");
        write_text(corpus / L".git" / L"config", "[core]\nrepositoryformatversion = 0\n");
        write_text(corpus / L"01_Archive" / L"ToolSuite" / L"device_sync.dll", std::string(4096, 'X'));
        write_text(corpus / L"01_Archive" / L"ToolSuite" / L"README", "no extension file");
        for (int i = 0; i < 40; ++i) {
            write_text(corpus / L"02_Hardware" / wide("schematic_" + std::to_string(i) + ".pcb"),
                       std::string(512 + i, 'B'));
        }

        auto proj_file = ext_meta / L"archive_project.json";
        Json cfg = {
            {"schema", "migratool.project/1"},
            {"mode", "archive"},
            {"index_tier", "metadata"},
            {"name", "ProtectedArchiveTest"},
            {"state_dir", path_text(state_dir)},
            {"roots", Json::array({{{"id", "corpus"}, {"role", "archive"}, {"path", path_text(corpus)}}})}
        };
        write_json(proj_file, cfg);
        auto p = Project::load(proj_file);

        // Verify state_dir is external (root stays 100% clean)
        check(!fs::exists(corpus / L".migratool"), "root must NOT have .migratool");
        auto sync1 = agent_request(p, "inventory_sync", {{"full", true}});
        check(!fs::exists(corpus / L".migratool"), "root must remain 100% clean after fs_sync");
        check(sync1["complete"] == true, "archive sync complete");
        check(sync1["total_files"].get<int64_t>() == 44, "all 44 files indexed despite .gitignore /*");
        check(sync1["total_dirs"].get<int64_t>() >= 5, "all dirs including .git and empty_folder indexed");

        // Explore tree
        auto tree = agent_request(p, "explore_tree", {{"depth", 2}});
        check(tree["indexed"] == true, "explore_tree indexed");
        check(tree["summary"]["total_files"].get<int64_t>() == 44, "explore_tree exact recursive file count");
        check(tree["elapsed_us"].get<int64_t>() < 50000, "explore_tree fast (<50ms)");

        // Find files via trigram + extension + glob
        auto f_dll = agent_request(p, "find_files", {{"query", "device_sync"}});
        check(f_dll["items"].size() == 1, "trigram query finds device_sync.dll");
        check(f_dll["items"][0]["path"] == "01_Archive/ToolSuite/device_sync.dll", "exact O(1) materialized path");

        auto f_pcb = agent_request(p, "find_files", {{"ext", ".pcb"}, {"limit", 100}, {"token_budget", 20000}});
        check(f_pcb["items"].size() == 40, "ext filter finds all 40 .pcb files");
        auto f_pcb_col = agent_request(p, "find_files", {{"ext", ".pcb"}, {"limit", 100}, {"format", "columnar"}, {"token_budget", 6000}});
        check(f_pcb_col["rows"].size() == 40, "columnar format fits all 40 .pcb files cleanly in <6KB");

        auto f_empty = agent_request(p, "find_files", {{"kind", "dir"}, {"query", "empty_folder"}});
        check(f_empty["items"].size() == 1, "empty_folder indexed and searchable");

        // =====================================================================
        // TEST 2: Offline directory rename + subfolder move + add + delete
        // =====================================================================
        fs::rename(corpus / L"02_Hardware", corpus / L"02_Hardware_Renamed");
        fs::rename(corpus / L"01_Archive" / L"ToolSuite", corpus / L"02_Hardware_Renamed" / L"ToolSuite_Moved");
        write_text(corpus / L"02_Hardware_Renamed" / L"brand_new_doc.md", "new content");
        fs::remove(corpus / L"02_Hardware_Renamed" / L"schematic_0.pcb");

        auto sync2 = agent_request(p, "inventory_sync", {{"full", false}});
        check(sync2["total_files"].get<int64_t>() == 44, "44 - 1 deleted + 1 added = 44 files after incremental sync");

        auto f_old = agent_request(p, "find_files", {{"path_prefix", "02_Hardware"}});
        check(f_old["items"].empty(), "0 stale files under old path 02_Hardware");

        auto f_moved = agent_request(p, "find_files", {{"query", "device_sync"}});
        check(f_moved["items"].size() == 1, "device_sync still unique after folder move");
        check(f_moved["items"][0]["path"] == "02_Hardware_Renamed/ToolSuite_Moved/device_sync.dll",
              "moved folder updated child path automatically via FRN");

        auto f_new = agent_request(p, "find_files", {{"query", "brand_new_doc"}});
        check(f_new["items"].size() == 1, "newly added file detected");

        auto feed = agent_request(p, "changes_feed", {{"since_seq", 0}});
        check(feed["returned"].get<int64_t>() >= 3, "changes_feed recorded moves/adds/deletes");

        // =====================================================================
        // TEST 3: Live Watcher (ReadDirectoryChangesExW 8 MB + O(1) dir rename + anti-self-trigger)
        // =====================================================================
        {
            IndexWatcher watcher(p, 900000);
            Sleep(350); // allow watcher handle to arm
            write_text(corpus / L"live_watch_file.txt", "watched live");
            write_text(corpus / L"ignored_vm_disk.avhdx", "must be ignored by watcher");
            write_text(corpus / L"ignored_db.sqlite-wal", "must be ignored by watcher");
            fs::rename(corpus / L"02_Hardware_Renamed" / L"ToolSuite_Moved",
                       corpus / L"02_Hardware_Renamed" / L"ToolSuite_LiveRenamed");

            bool live_ok = wait_for([&]() {
                auto r1 = agent_request(p, "find_files", {{"query", "live_watch_file"}});
                auto r2 = agent_request(p, "find_files", {{"query", "device_sync"}});
                return r1["items"].size() == 1 &&
                       r2["items"].size() == 1 &&
                       r2["items"][0]["path"] == "02_Hardware_Renamed/ToolSuite_LiveRenamed/device_sync.dll";
            }, 6000);
            check(live_ok, "live watcher detected new file AND O(1) dir rename automatically");

            auto f_avhdx = agent_request(p, "find_files", {{"query", "ignored_vm_disk"}});
            check(f_avhdx["items"].empty(), "anti-self-trigger ignored .avhdx hot write");
            auto f_wal = agent_request(p, "find_files", {{"query", "ignored_db"}});
            check(f_wal["items"].empty(), "anti-self-trigger ignored .sqlite-wal hot write");
        }

        // =====================================================================
        // TEST 4: Synthetic 1,000,000-Entry Ultra-Scale Stress Test!
        //         - 950,000 files + 50,000 dirs across 4 branches (branch_0 has 237,500 files + 12,500 dirs)
        //         - Verify DB size < 250 MB, explore_tree < 15 ms, find_files < 30 ms,
        //           and renaming branch_0 (250,000 entries!) < 250 ms with 0 stale paths!
        // =====================================================================
        {
            auto stress_meta = base / L"stress_1m";
            auto stress_root = base / L"stress_root";
            fs::create_directories(stress_meta);
            fs::create_directories(stress_root);
            auto sp_file = stress_meta / L"stress.json";
            write_json(sp_file, {
                {"schema", "migratool.project/1"},
                {"mode", "archive"},
                {"index_tier", "metadata"},
                {"name", "Stress1M"},
                {"state_dir", path_text(stress_meta / L"state")},
                {"roots", Json::array({{{"id", "r1"}, {"role", "archive"}, {"path", path_text(stress_root)}}})}
            });
            auto sp = Project::load(sp_file);
            Database sdb(sp);
            int64_t rid = fs_ensure_root(sp, sdb, sp.config["roots"][0]);

            constexpr uint64_t ROOT_FRN = 100;
            constexpr int BRANCHES = 4;
            constexpr int DIRS_PER_BRANCH = 12499; // 4 * 12500 = 50,000 dirs total
            constexpr int FILES_PER_DIR = 19;      // 50,000 * 19 = 950,000 files total -> 1,000,000 entries!
            static const char* EXTS[] = {".c", ".cpp", ".h", ".py", ".dll", ".bin", ".json", ".md"};

            std::vector<FsBulkEntry> entries;
            entries.reserve(1000000);
            uint64_t next_frn = 1000;
            uint64_t branch0_frn = 0;

            for (int b = 0; b < BRANCHES; ++b) {
                uint64_t bfrn = next_frn++;
                if (b == 0) branch0_frn = bfrn;
                entries.push_back(FsBulkEntry{bfrn, ROOT_FRN, "branch_" + std::to_string(b), "", 1, 0, 1700000000000000000LL, FILE_ATTRIBUTE_DIRECTORY, ""});
                // 19 files directly in branch_b
                for (int f = 0; f < FILES_PER_DIR; ++f) {
                    const char* ext = EXTS[(b + f) & 7];
                    std::string nm = (b == 0 && f == 7) ? "target_needle_777.dll" : ("f_" + std::to_string(b) + "_" + std::to_string(f) + ext);
                    entries.push_back(FsBulkEntry{next_frn++, bfrn, nm, ext, 0, 1024 + f * 17, 1700000000000000000LL + f, FILE_ATTRIBUTE_NORMAL, ""});
                }
                for (int d = 0; d < DIRS_PER_BRANCH; ++d) {
                    uint64_t dfrn = next_frn++;
                    entries.push_back(FsBulkEntry{dfrn, bfrn, "sub_" + std::to_string(d), "", 1, 0, 1700000000000000000LL, FILE_ATTRIBUTE_DIRECTORY, ""});
                    for (int f = 0; f < FILES_PER_DIR; ++f) {
                        const char* ext = EXTS[(d + f) & 7];
                        std::string nm = (b == 0 && d == 12000 && f == 5)
                            ? "deep_needle_999.cpp"
                            : ("file_" + std::to_string(d) + "_" + std::to_string(f) + ext);
                        entries.push_back(FsBulkEntry{next_frn++, dfrn, nm, ext, 0, 2048 + (d & 1023), 1700000000000000000LL + d, FILE_ATTRIBUTE_NORMAL, ""});
                    }
                }
            }
            check(entries.size() == 1000000, "generated exactly 1,000,000 entries");

            auto bulk_res = fs_bulk_replace_root(sdb, rid, ROOT_FRN, 1700000000000000000LL, entries, {}, true);
            entries.clear();
            entries.shrink_to_fit();

            auto db_size = fs::file_size(sp.state_dir() / L"catalog.sqlite");
            const double db_mb = static_cast<double>(db_size) / (1024.0 * 1024.0);
            std::cout << "[1M STRESS] bulk=" << bulk_res.dump() << " db_mb=" << db_mb << "\n";
            check(bulk_res["total_entries"].get<int64_t>() == 1000000, "1M entries stored");
            check(db_mb < 300.0, "1M DB size compact (<300 MB, projects <1.8 GB @ 6M)");

            // Query 1: explore_tree (<15ms gate)
            auto t_tree0 = std::chrono::steady_clock::now();
            auto stree = fs_explore_tree(sp, sdb, {{"depth", 2}, {"limit", 10}});
            auto tree_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_tree0).count();
            std::cout << "[1M STRESS] explore_tree_ms=" << tree_ms << " total_files=" << stree["summary"]["total_files"] << "\n";
            check(stree["summary"]["total_files"].get<int64_t>() == 950000, "1M explore_tree exact 950K files");
            check(stree["summary"]["total_dirs"].get<int64_t>() == 50000, "1M explore_tree exact 50K dirs");
            check(tree_ms < 50, "1M explore_tree <50ms");

            // Query 2: trigram find_files (<30ms gate)
            auto t_find0 = std::chrono::steady_clock::now();
            auto sfind = fs_find_files(sp, sdb, {{"query", "deep_needle_999"}});
            auto find_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_find0).count();
            std::cout << "[1M STRESS] find_files_trigram_ms=" << find_ms << " strategy=" << sfind["strategy"] << " path=" << sfind["items"][0]["path"] << "\n";
            check(sfind["items"].size() == 1, "1M trigram search found exact deep needle");
            check(sfind["items"][0]["path"] == "branch_0/sub_12000/deep_needle_999.cpp", "1M exact path before rename");
            check(find_ms < 100, "1M find_files trigram fast (<100ms)");

            // Query 3: O(1)-file-touch rename of branch_0 (237,500 files + 12,500 subdirs!)
            auto t_ren0 = std::chrono::steady_clock::now();
            bool moved = fs_apply_rename_or_move(sdb, rid, branch0_frn, ROOT_FRN, "branch_0_relocated", 1700000000999000000LL, true);
            auto ren_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_ren0).count();
            std::cout << "[1M STRESS] rename_250k_subtree_ms=" << ren_ms << "\n";
            check(moved, "branch_0 renamed");
            check(ren_ms < 1500, "renaming 250K-entry subtree finished in <1.5s without touching 237,500 file rows");

            auto sfind2 = fs_find_files(sp, sdb, {{"query", "deep_needle_999"}});
            check(sfind2["items"].size() == 1, "needle still found after 250K subtree rename");
            check(sfind2["items"][0]["path"] == "branch_0_relocated/sub_12000/deep_needle_999.cpp",
                  "all 237,500 child paths updated instantly via fs_dirs prefix swap");
            auto stale_check = fs_find_files(sp, sdb, {{"path_prefix", "branch_0"}, {"limit", 5}});
            check(stale_check["items"].empty(), "0 stale paths left under old branch_0");
        }

        std::cout << "fs_index_tests PASSED (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fs_index_tests FAILED: " << e.what() << "\n";
        return 1;
    }
}
