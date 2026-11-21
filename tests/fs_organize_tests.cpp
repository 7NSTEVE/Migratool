#include "migratool/fs_index.hpp"
#include "migratool/fs_organize.hpp"
#include "migratool/gateway.hpp"
#include "migratool/workspace_registry.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <zip.h>

namespace {
using namespace mt;
int checks = 0;

void check(bool cond, const std::string& msg) {
    ++checks;
    if (!cond) throw std::runtime_error("FAILED CHECK #" + std::to_string(checks) + ": " + msg);
}

void write_bytes_ext(const fs::path& p, const std::string& data) {
    const auto np = native_path(p);
    auto parent = p.parent_path();
    if (!parent.empty()) {
        // Create directories segment-by-segment using CreateDirectoryW with \\?\ so trailing spaces work!
        std::wstring wp = native_path(parent).wstring();
        std::wstring prefix;
        std::wstring body = wp;
        if (body.starts_with(L"\\\\?\\")) { prefix = L"\\\\?\\"; body = body.substr(4); }
        std::wstring cur;
        size_t pos = 0;
        while (pos <= body.size()) {
            size_t sep = body.find(L'\\', pos);
            std::wstring seg = (sep == std::wstring::npos) ? body.substr(pos) : body.substr(pos, sep - pos);
            if (!seg.empty()) {
                cur = cur.empty() ? seg : (cur + L"\\" + seg);
                if (cur.size() > 2) CreateDirectoryW((prefix + cur).c_str(), nullptr);
            }
            if (sep == std::wstring::npos) break;
            pos = sep + 1;
        }
    }
    HANDLE h = CreateFileW(np.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("write_bytes_ext failed: " + path_text(p) + " win32=" + std::to_string(GetLastError()));
    DWORD written = 0;
    if (!data.empty()) WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
}

void remove_tree_ext(const fs::path& p) {
    const auto np = native_path(p);
    DWORD a = GetFileAttributesW(np.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        std::wstring pat = np.wstring();
        if (!pat.ends_with(L"\\")) pat.push_back(L'\\');
        pat.push_back(L'*');
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring_view n(fd.cFileName);
                if (n == L"." || n == L"..") continue;
                remove_tree_ext(p / std::wstring(n));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        SetFileAttributesW(np.c_str(), FILE_ATTRIBUTE_NORMAL);
        RemoveDirectoryW(np.c_str());
    } else {
        SetFileAttributesW(np.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(np.c_str());
    }
}

std::string run_cli_capture(const fs::path& exe, const std::wstring& args, const std::string& stdin_payload, int* out_rc) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_r = nullptr, out_w = nullptr, in_r = nullptr, in_w = nullptr;
    CreatePipe(&out_r, &out_w, &sa, 0);
    CreatePipe(&in_r, &in_w, &sa, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = quote_arg(exe.wstring()) + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    si.hStdInput = in_r;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(out_r); CloseHandle(out_w); CloseHandle(in_r); CloseHandle(in_w);
        throw std::runtime_error("CreateProcessW failed");
    }
    CloseHandle(out_w);
    CloseHandle(in_r);
    if (!stdin_payload.empty()) {
        DWORD wr = 0;
        WriteFile(in_w, stdin_payload.data(), static_cast<DWORD>(stdin_payload.size()), &wr, nullptr);
    }
    CloseHandle(in_w);
    std::string out;
    char rbuf[4096];
    DWORD got = 0;
    while (ReadFile(out_r, rbuf, sizeof(rbuf), &got, nullptr) && got > 0) out.append(rbuf, got);
    CloseHandle(out_r);
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (out_rc) *out_rc = static_cast<int>(rc);
    return out;
}

fs::path find_git_for_test() {
    std::error_code ec;
    for (const wchar_t* p : {L"D:\\DevTools\\Tools\\Git\\cmd\\git.exe", L"C:\\Program Files\\Git\\cmd\\git.exe"}) {
        if (fs::is_regular_file(p, ec)) return fs::path(p);
    }
    wchar_t buf[32768]{};
    if (SearchPathW(nullptr, L"git.exe", nullptr, 32768, buf, nullptr)) return fs::path(buf);
    return {};
}

// Create a small real ZIP file using libzip dynamically
void create_zip_from_map(const fs::path& zip_path, const std::vector<std::pair<std::string, std::string>>& entries) {
    auto dll_path = fs::path(MIGRATOOL_ZIP_DLL).make_preferred();
    HMODULE h = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(h != nullptr, "libzip loaded for test");
    auto z_open = reinterpret_cast<decltype(&::zip_open)>(GetProcAddress(h, "zip_open"));
    auto z_sbuf = reinterpret_cast<decltype(&::zip_source_buffer)>(GetProcAddress(h, "zip_source_buffer"));
    auto z_add = reinterpret_cast<decltype(&::zip_file_add)>(GetProcAddress(h, "zip_file_add"));
    auto z_close = reinterpret_cast<decltype(&::zip_close)>(GetProcAddress(h, "zip_close"));
    int errp = 0;
    zip_t* za = z_open(path_text(zip_path).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errp);
    check(za != nullptr, "zip_open create ok");
    for (const auto& [name, content] : entries) {
        zip_source_t* s = z_sbuf(za, content.data(), content.size(), 0);
        check(s != nullptr, "zip_source_buffer ok");
        check(z_add(za, name.c_str(), s, ZIP_FL_ENC_UTF_8) >= 0, "zip_file_add ok");
    }
    check(z_close(za) == 0, "zip_close ok");
    FreeLibrary(h);
}

} // namespace

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    try {
        auto base = fs::temp_directory_path() / wide("migratool_v21_test_" + unique_id());
        fs::create_directories(base);
        struct Cleanup {
            fs::path p;
            ~Cleanup() { remove_tree_ext(p); }
        } cleanup{base};

        // =====================================================================
        // TEST 1: Regression D9 — classify_win32_node ReparseTag unit checks
        // =====================================================================
        {
            const DWORD onedrive_cloud_tag = 0x9000001Au; // IO_REPARSE_TAG_CLOUD
            const DWORD wof_tag = 0x80000017u;            // IO_REPARSE_TAG_WOF
            check(!is_link_reparse_tag(onedrive_cloud_tag), "D9: OneDrive cloud tag is NOT a link");
            check(!is_link_reparse_tag(wof_tag), "D9: WOF tag is NOT a link");
            check(is_link_reparse_tag(IO_REPARSE_TAG_MOUNT_POINT), "D9: mount point IS a link");
            check(is_link_reparse_tag(IO_REPARSE_TAG_SYMLINK), "D9: symlink IS a link");
            check(classify_win32_node(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_REPARSE_POINT, onedrive_cloud_tag) == FsNodeKind::File,
                  "D9: OneDrive file classified as File (never SymlinkFile)");
            check(classify_win32_node(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT, onedrive_cloud_tag) == FsNodeKind::Directory,
                  "D9: OneDrive dir classified as Directory (never SymlinkDir)");
            check(classify_win32_node(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_MOUNT_POINT) == FsNodeKind::Junction,
                  "D9: NTFS junction classified as Junction");
            check(classify_win32_node(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_SYMLINK) == FsNodeKind::SymlinkFile,
                  "D9: real file symlink classified as SymlinkFile");
        }

        // =====================================================================
        // TEST 2: Regression D1 (12-thread rollup race) + D7 (trailing space dir)
        //         + D2 (explore_tree budget & flat/columnar) + D3 (returned count)
        // =====================================================================
        auto root = base / L"corpus_v21";
        auto state = base / L"state_v21";
        fs::create_directories(root);
        // 3 direct files in root
        write_bytes_ext(root / L"root_a.txt", "AAA");
        write_bytes_ext(root / L"root_b.txt", "BBBB");
        write_bytes_ext(root / L"stray.obj", std::string(2048, 'O'));

        // D7: directory with trailing space in segment name!
        write_bytes_ext(root / L"bad_wsl " / L"trailing_seg " / L"probe.json", "{\"ok\":true}");

        // D1 stress: 60 subdirectories x 25 files = 1,500 files across 12 threads!
        int64_t expected_files = 4; // 3 root + 1 trailing-space
        int64_t expected_bytes = 3 + 4 + 2048 + 11;
        for (int d = 0; d < 60; ++d) {
            auto sub = root / wide("grp_" + std::to_string(d / 10)) / wide("sub_" + std::to_string(d));
            for (int f = 0; f < 25; ++f) {
                std::string body(1200 + (d % 5) * 100 + f, static_cast<char>('A' + (f % 26)));
                write_bytes_ext(sub / wide("item_" + std::to_string(f) + ".dat"), body);
                ++expected_files;
                expected_bytes += static_cast<int64_t>(body.size());
            }
        }

        auto proj_json = base / L"workspace.json";
        write_json(proj_json, {
            {"schema", "migratool.project/1"},
            {"mode", "archive"},
            {"index_tier", "metadata"},
            {"name", "V21RegressionTest"},
            {"state_dir", path_text(state)},
            {"roots", Json::array({{{"id", "main"}, {"role", "archive"}, {"path", path_text(root)}}})}
        });
        auto p = Project::load(proj_json);

        // Full 12-thread sync
        auto sync1 = agent_request(p, "inventory_sync", {{"full", true}, {"threads", 12}});
        check(sync1["complete"] == true, "D1/D7: full sync complete");
        check(sync1["scan_errors"].get<int64_t>() == 0, "D7: zero scan errors on trailing-space folder");
        check(sync1["total_files"].get<int64_t>() == expected_files, "D1: exact total_files=" + std::to_string(expected_files));
        check(sync1["total_bytes"].get<int64_t>() == expected_bytes, "D1: exact total_bytes");

        // D1 invariant check: root.direct_files == 3 AND root.direct_files + sum(depth=1 subdirs) == root.total_files!
        auto tree1 = agent_request(p, "explore_tree", {{"depth", 1}, {"limit", 100}, {"include_samples", false}});
        check(tree1["items"].size() == 1, "D2: explore_tree returns root with default budget");
        const auto& rnode = tree1["items"][0];
        check(rnode["direct_files"].get<int64_t>() == 3, "D1: root direct_files MUST be exactly 3 (got " + std::to_string(rnode["direct_files"].get<int64_t>()) + ")");
        int64_t sum_sub_files = 0, sum_sub_bytes = 0;
        for (const auto& sd : rnode["subdirs"]) {
            sum_sub_files += sd["total_files"].get<int64_t>();
            sum_sub_bytes += sd["total_bytes"].get<int64_t>();
        }
        check(rnode["direct_files"].get<int64_t>() + sum_sub_files == expected_files, "D1: direct_files + sum(subdirs.total_files) == total_files");
        check(rnode["total_bytes"].get<int64_t>() == expected_bytes, "D1: root total_bytes exact");
        check(sum_sub_bytes + (3 + 4 + 2048) == expected_bytes, "D1: subdirs byte rollup exact");

        // D2: tiny budget (900 bytes) on nested explore_tree MUST still return root node (trimmed subdirs), NEVER items:[]
        auto tree_tight = agent_request(p, "explore_tree", {{"depth", 1}, {"limit", 100}, {"token_budget", 900}});
        check(tree_tight["items"].size() == 1, "D2: explore_tree under tight budget never empties items");
        check(tree_tight["response_bytes"].get<int64_t>() <= 900, "D2: explore_tree respects token_budget");

        // D2: flat + columnar explore_tree
        auto tree_flat = agent_request(p, "explore_tree", {{"depth", 1}, {"flat", true}, {"format", "columnar"}});
        check(tree_flat["format"] == "columnar", "D2: flat explore_tree supports columnar");
        check(tree_flat["rows"].size() == tree_flat["returned"].get<size_t>(), "D3: explore_tree columnar returned == rows.size()");
        check(tree_flat["rows"].size() >= 7, "D2: flat explore_tree returns root + subdirs");

        // D3: find_files columnar returned == rows.size() even under tight truncation
        auto ff_col = agent_request(p, "find_files", {{"ext", ".dat"}, {"limit", 50}, {"format", "columnar"}, {"token_budget", 1200}});
        check(ff_col["format"] == "columnar", "D3: find_files columnar");
        check(!ff_col["rows"].empty(), "D3: find_files columnar has rows");
        check(ff_col["returned"].get<size_t>() == ff_col["rows"].size(), "D3: find_files returned matches truncated rows.size()");

        // =====================================================================
        // TEST 3: Regression D8 + D10 — Move dir OUT of root + create dir + move file INTO new dir
        //         Verify zero ghost files, exact byte parity, and full new_path in changes_feed!
        // =====================================================================
        {
            auto outside_q = base / L"outside_moved_grp_5";
            MoveFileExW(native_path(root / L"grp_5").c_str(), native_path(outside_q).c_str(), 0);
            fs::create_directories(root / L"new_bucket" / L"nested");
            MoveFileExW(native_path(root / L"root_a.txt").c_str(),
                        native_path(root / L"new_bucket" / L"nested" / L"root_a.txt").c_str(), 0);

            // Recompute expected exact files/bytes on disk
            int64_t moved_out_files = 10 * 25; // grp_5 had 10 subdirs * 25 files = 250 files
            auto sync2 = agent_request(p, "inventory_sync", {{"full", false}});
            check(sync2["total_files"].get<int64_t>() == expected_files - moved_out_files,
                  "D8: incremental/USN sync exact total_files after moving 250-file subtree out (got " +
                  std::to_string(sync2["total_files"].get<int64_t>()) + ", exp " + std::to_string(expected_files - moved_out_files) + ")");

            auto tree2 = agent_request(p, "explore_tree", {{"depth", 1}, {"include_samples", false}});
            check(tree2["summary"]["total_files"].get<int64_t>() == sync2["total_files"].get<int64_t>(),
                  "D8: explore_tree summary == fs_roots total_files (zero self-contradiction)");
            check(tree2["summary"]["total_bytes"].get<int64_t>() == sync2["total_bytes"].get<int64_t>(),
                  "D8: explore_tree summary == fs_roots total_bytes (zero 1GB divergence)");
            check(tree2["items"][0]["direct_files"].get<int64_t>() == 2, "D8: root direct_files updated from 3 -> 2");

            // Check changes_feed logged full destination path for root_a.txt (D10)
            auto feed = agent_request(p, "changes_feed", {{"since_seq", 0}, {"limit", 500}, {"token_budget", 30000}});
            bool saw_full_new_path = false;
            for (const auto& it : feed["items"]) {
                const std::string np = it.value("new_path", "");
                if (np == "new_bucket/nested/root_a.txt") saw_full_new_path = true;
                if (it.value("op", "") == "moved" && it.value("old_path", "") == "root_a.txt") {
                    check(np == "new_bucket/nested/root_a.txt", "D10: moved new_path includes new_bucket/nested/, got: " + np);
                }
            }
            check(saw_full_new_path, "D10: changes_feed recorded full path new_bucket/nested/root_a.txt");

            // Move grp_5 & root_a.txt back so workspace is clean for next tests
            MoveFileExW(native_path(root / L"new_bucket" / L"nested" / L"root_a.txt").c_str(),
                        native_path(root / L"root_a.txt").c_str(), 0);
            remove_tree_ext(root / L"new_bucket");
            MoveFileExW(native_path(outside_q).c_str(), native_path(root / L"grp_5").c_str(), 0);
        }

        // =====================================================================
        // TEST 4: Pillar 3 — find_duplicates (cached fs_hashes + tree_overlap)
        //         & verify_archive_vs_dir (positive + negative 1-byte tamper)
        // =====================================================================
        {
            std::string big_payload(80000, 'Z'); // > 64 KiB exercises Stage 2 + Stage 3
            write_bytes_ext(root / L"dup_a" / L"shared.dll", big_payload);
            write_bytes_ext(root / L"dup_b" / L"shared_copy1.dll", big_payload);
            write_bytes_ext(root / L"dup_c" / L"shared_copy2.dll", big_payload);

            // Real ZIP + extracted copy
            write_bytes_ext(root / L"extracted_pkg" / L"alpha.bin", "HELLO_ARCHIVE_ALPHA");
            write_bytes_ext(root / L"extracted_pkg" / L"sub" / L"beta.bin", "HELLO_ARCHIVE_BETA_12345");
            write_bytes_ext(root / L"extracted_pkg" / L".git" / L"index", "DIFFERING_LOCAL_INDEX");
            create_zip_from_map(root / L"pkg.zip", {
                {"alpha.bin", "HELLO_ARCHIVE_ALPHA"},
                {"sub/beta.bin", "HELLO_ARCHIVE_BETA_12345"},
                {".git/index", "ORIG_ZIP_INDEX"}
            });

            agent_request(p, "inventory_sync", {{"full", true}});

            // First duplicates run (computes + caches hashes)
            auto d1 = agent_request(p, "find_duplicates", {{"min_bytes", 10000}, {"token_budget", 20000}});
            check(d1["dup_groups"].get<int64_t>() >= 1, "P3: find_duplicates finds >=1 big group");
            check(d1["redundant_bytes"].get<int64_t>() >= 160000, "P3: redundant_bytes >= 2 * 80000");
            check(d1["hashed_full"].get<int64_t>() >= 3, "P3: hashed_full >= 3 on cold cache");
            check(!d1["tree_overlap"].empty(), "P3: tree_overlap matrix populated");

            // Second duplicates run (must hit fs_hashes SQLite cache!)
            auto d2 = agent_request(p, "find_duplicates", {{"min_bytes", 10000}, {"token_budget", 20000}});
            check(d2["cache_hits"].get<int64_t>() >= 3, "P3: second find_duplicates uses fs_hashes cache");
            check(d2["hashed_full"].get<int64_t>() == 0, "P3: zero re-hashing on warm cache");

            // verify_archive_vs_dir: positive (ignoring .git/index)
            auto vz_ok = agent_request(p, "verify_archive_vs_dir", {
                {"zip", "pkg.zip"}, {"target_dir", "extracted_pkg"}
            });
            check(vz_ok["exact_match"] == true, "P3: verify_archive_vs_dir positive match");
            check(vz_ok["matched_crc"].get<int64_t>() == 2, "P3: 2 matched entries");
            check(vz_ok["ignored"].get<int64_t>() == 1, "P3: 1 ignored .git/index");

            // verify_archive_vs_dir: negative (tamper 1 byte -> MUST detect crc_mismatch == 1)
            write_bytes_ext(root / L"extracted_pkg" / L"alpha.bin", "HELLO_ARCHIVE_ALPHX");
            auto vz_bad = agent_request(p, "verify_archive_vs_dir", {
                {"zip", "pkg.zip"}, {"target_dir", "extracted_pkg"}
            });
            check(vz_bad["exact_match"] == false, "P3 NEGATIVE: 1-byte tamper breaks exact_match");
            check(vz_bad["crc_mismatch"].get<int64_t>() == 1, "P3 NEGATIVE: crc_mismatch == 1");
            write_bytes_ext(root / L"extracted_pkg" / L"alpha.bin", "HELLO_ARCHIVE_ALPHA");
        }

        // =====================================================================
        // TEST 5: Pillar 3 — inspect_workspace_topology + organize_plan/apply/verify/undo
        //         (including negative safety test rejecting git-tracked quarantine!)
        // =====================================================================
        {
            const auto git = find_git_for_test();
            auto repo = root / L"my_repo";
            write_bytes_ext(repo / L".gitignore", "build/\n");
            write_bytes_ext(repo / L"src" / L"main.cpp", "int main(){return 0;}\n");
            write_bytes_ext(repo / L"build" / L"artifact.obj", std::string(4096, 'X'));
            write_bytes_ext(root / L"scripts_old" / L"run.py", "P = r'" + path_text(root) + "/scripts_old/data.txt'\n");

            if (!git.empty()) {
                int rc = 0;
                run_cli_capture(git, L"-C " + quote_arg(repo.wstring()) + L" init -q", "", &rc);
                run_cli_capture(git, L"-C " + quote_arg(repo.wstring()) + L" config user.email t@t.local", "", &rc);
                run_cli_capture(git, L"-C " + quote_arg(repo.wstring()) + L" config user.name T", "", &rc);
                run_cli_capture(git, L"-C " + quote_arg(repo.wstring()) + L" add .gitignore src/main.cpp", "", &rc);
                run_cli_capture(git, L"-C " + quote_arg(repo.wstring()) + L" commit -q -m init", "", &rc);
            }
            agent_request(p, "inventory_sync", {{"full", true}});

            auto topo = agent_request(p, "inspect_workspace_topology", {{"token_budget", 30000}});
            check(!topo["roots"].empty(), "P3: topology returned root");
            const auto& rt = topo["roots"][0];
            if (!git.empty()) {
                check(rt["git_repos_count"].get<int64_t>() >= 1, "P3: topology detected git repo");
                bool saw_safe_build = false;
                for (const auto& rd : rt["regenerable_dirs"]) {
                    if (rd["path"] == "my_repo/build") {
                        check(rd["git_tracked_files"].get<int64_t>() == 0, "P3: my_repo/build has 0 tracked files");
                        check(rd["git_ignored"] == true, "P3: my_repo/build is git_ignored");
                        check(rd["safe_to_quarantine"] == true, "P3: my_repo/build marked safe_to_quarantine");
                        saw_safe_build = true;
                    }
                }
                check(saw_safe_build, "P3: topology classified my_repo/build");

                // NEGATIVE SAFETY TEST: organize_plan MUST reject quarantining git-tracked file!
                bool rejected_tracked = false;
                try {
                    agent_request(p, "organize_plan", {
                        {"plan_id", "neg_tracked"},
                        {"operations", Json::array({{{"op", "quarantine_file"}, {"src", "my_repo/src/main.cpp"}}})}
                    });
                } catch (const std::exception& e) {
                    rejected_tracked = std::string(e.what()).find("SAFETY_VIOLATION") != std::string::npos;
                }
                check(rejected_tracked, "P3 NEGATIVE: organize_plan rejects quarantining git-tracked file");
            }

            // POSITIVE FULL CYCLE: plan -> dry-run apply -> real apply -> verify -> undo -> bit-for-bit check!
            const std::string orig_py = read_text(root / L"scripts_old" / L"run.py");
            auto plan = agent_request(p, "organize_plan", {
                {"plan_id", "full_cycle_1"},
                {"quarantine_dir", path_text(base / L"q_test")},
                {"operations", Json::array({
                    {{"op", "quarantine_file"}, {"category", "Q1"}, {"src", "stray.obj"}, {"reason", "obj suelto"}},
                    {{"op", "quarantine_dir"}, {"category", "Q2"}, {"src", "my_repo/build"}, {"reason", "build gitignored"}},
                    {{"op", "quarantine_file"}, {"category", "Q3"}, {"src", "bad_wsl /trailing_seg /probe.json"}, {"dst_rel", "probe.json"}},
                    {{"op", "rmdir_empty_tree"}, {"category", "Q3"}, {"src", "bad_wsl "}},
                    {{"op", "move_dir"}, {"category", "B"}, {"src", "scripts_old"}, {"dst", "Tools/scripts_new"}},
                    {{"op", "edit_text_paths"}, {"category", "D"}, {"target", "Tools/scripts_new/run.py"},
                     {"replacements", Json::array({Json::array({"/scripts_old/", "/Tools/scripts_new/"})})}}
                })}
            });
            check(plan["safety_checks_passed"] == true, "P3: organize_plan passed safety checks");

            // Dry-run apply without execute:true MUST NOT touch disk
            auto dry = agent_request(p, "organize_apply", {{"plan_id", "full_cycle_1"}});
            check(dry["dry_run"] == true, "P3 NEGATIVE: organize_apply without execute:true stays dry_run");
            check(fs::exists(native_path(root / L"stray.obj")), "P3: dry_run left stray.obj untouched");

            // Real transactional apply
            auto app = agent_request(p, "organize_apply", {{"plan_id", "full_cycle_1"}, {"execute", true}});
            check(app["quarantined_files"].get<int64_t>() == 3, "P3: 3 files quarantined");
            check(!fs::exists(native_path(root / L"stray.obj")), "P3: stray.obj moved to quarantine");
            check(fs::exists(native_path(root / L"Tools" / L"scripts_new" / L"run.py")), "P3: scripts_old moved to Tools/scripts_new");
            check(read_text(root / L"Tools" / L"scripts_new" / L"run.py").find("/Tools/scripts_new/") != std::string::npos,
                  "P3: edit_text_paths updated path");

            // Independent verify
            auto ver = agent_request(p, "organize_verify", {{"plan_id", "full_cycle_1"}});
            check(ver["all_passed"] == true, "P3: organize_verify all gates passed");
            check(ver["gates"]["conservation"]["missing_baseline_files"].get<int64_t>() == 0, "P3: 0 missing files");
            check(ver["gates"]["integrity"]["quarantine_sha256_verified"].get<int64_t>() == 3, "P3: 3/3 SHA-256 verified");
            check(ver["gates"]["git"]["diffs"].empty(), "P3: 0 git diffs");

            // Full reversible undo
            auto und = agent_request(p, "organize_undo", {{"plan_id", "full_cycle_1"}, {"execute", true}});
            check(und["undone"] == true, "P3: organize_undo completed");
            check(fs::exists(native_path(root / L"stray.obj")), "P3 UNDO: stray.obj restored");
            check(fs::exists(native_path(root / L"my_repo" / L"build" / L"artifact.obj")), "P3 UNDO: build restored");
            check(fs::exists(native_path(root / L"bad_wsl " / L"trailing_seg " / L"probe.json")), "P3 UNDO: trailing-space file restored");
            check(read_text(root / L"scripts_old" / L"run.py") == orig_py, "P3 UNDO: exact pre-edit bytes restored");
        }

        // =====================================================================
        // TEST 6: Pillar 2 — CLI --json-stdin (D6), --help (D4), Projectless MCP (D5)
        // =====================================================================
        {
            const fs::path cli = executable_dir() / L"migratool-cli-0.3.exe";
            check(fs::is_regular_file(cli), "D5: migratool-cli-0.3.exe exists");

            int rc = 0;
            // D4: --help lists workspace_commands, organize_commands, powershell_note
            auto help_out = run_cli_capture(cli, L"--help", "", &rc);
            check(rc == 0, "D4: --help exit 0");
            auto hj = Json::parse(help_out);
            check(hj.contains("workspace_commands") && hj.contains("organize_commands") && hj.contains("powershell_note"),
                  "D4/D6: --help exposes workspace_commands, organize_commands, powershell_note");

            // D6: --json-stdin works seamlessly (PowerShell-safe)
            auto ex_out = run_cli_capture(cli, L"agent explore --project " + quote_arg(proj_json.wstring()) + L" --json-stdin",
                                          "{\"depth\":1,\"flat\":true,\"format\":\"columnar\"}", &rc);
            check(rc == 0, "D6: --json-stdin exit 0");
            auto exj = Json::parse(ex_out);
            check(exj["format"] == "columnar" && !exj["rows"].empty(), "D6: --json-stdin parsed & returned columnar rows");

            // D5: Projectless MCP startup (no --project!) + initialize + tools/list + compact schemas + NO_ACTIVE_WORKSPACE check
            {
                SECURITY_ATTRIBUTES sa{};
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;
                HANDLE out_r = nullptr, out_w = nullptr, in_r = nullptr, in_w = nullptr;
                CreatePipe(&out_r, &out_w, &sa, 0);
                CreatePipe(&in_r, &in_w, &sa, 0);
                SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
                SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
                HANDLE err_null = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
                std::wstring cmd = quote_arg(cli.wstring()) + L" mcp";
                std::vector<wchar_t> cbuf(cmd.begin(), cmd.end());
                cbuf.push_back(L'\0');
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                si.hStdOutput = out_w;
                si.hStdError = err_null;
                si.hStdInput = in_r;
                PROCESS_INFORMATION pi{};
                check(CreateProcessW(nullptr, cbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE,
                      "D5: projectless MCP process started");
                CloseHandle(out_w);
                CloseHandle(in_r);
                CloseHandle(err_null);

                std::string pending;
                auto send_msg = [&](const std::string& line) {
                    std::string s = line + "\n";
                    DWORD wr = 0;
                    WriteFile(in_w, s.data(), static_cast<DWORD>(s.size()), &wr, nullptr);
                };
                auto read_msg = [&]() -> Json {
                    auto t0 = GetTickCount64();
                    while (GetTickCount64() - t0 < 15000) {
                        auto nl = pending.find('\n');
                        if (nl != std::string::npos) {
                            std::string l = pending.substr(0, nl);
                            pending.erase(0, nl + 1);
                            if (!l.empty() && l.back() == '\r') l.pop_back();
                            if (!l.empty() && l.front() == '{') return Json::parse(l);
                            continue;
                        }
                        DWORD avail = 0;
                        if (!PeekNamedPipe(out_r, nullptr, 0, nullptr, &avail, nullptr)) break;
                        if (avail > 0) {
                            char rbuf[4096];
                            DWORD got = 0;
                            if (ReadFile(out_r, rbuf, std::min<DWORD>(avail, sizeof(rbuf)), &got, nullptr) && got > 0)
                                pending.append(rbuf, got);
                        } else {
                            Sleep(10);
                        }
                    }
                    throw std::runtime_error("D5: MCP response timeout");
                };

                send_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"clientInfo\":{\"name\":\"v21-test\"}}}");
                auto r1 = read_msg();
                check(r1["id"] == 1 && r1["result"]["protocolVersion"] == "2025-11-25", "D5: projectless MCP initialize ok");
                send_msg("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

                send_msg("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
                auto r2 = read_msg();
                const auto& tools = r2["result"]["tools"];
                const size_t found_tools = tools.size();
                bool saw_find_dups = false, saw_org_plan = false;
                for (const auto& t : tools) {
                    if (t["name"] == "find_duplicates") {
                        saw_find_dups = true;
                        check(t["inputSchema"]["properties"].size() <= 15, "P2: compact per-tool schema for find_duplicates");
                    }
                    if (t["name"] == "organize_plan") saw_org_plan = true;
                }
                check(found_tools >= 45, "D5: projectless MCP exposes >=45 tools (got " + std::to_string(found_tools) + ")");
                check(saw_find_dups && saw_org_plan, "D5/P3: MCP exposes find_duplicates and organize_plan");

                send_msg("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"list_workspaces\",\"arguments\":{\"available_only\":true}}}");
                auto r3 = read_msg();
                check(r3["id"] == 3 && r3["result"]["isError"] == false, "D5: list_workspaces works in projectless MCP mode");

                send_msg("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"explore_tree\",\"arguments\":{}}}");
                auto r4 = read_msg();
                check(r4["id"] == 4 && r4["result"]["isError"] == true &&
                      r4["result"]["content"][0]["text"].get<std::string>().find("NO_ACTIVE_WORKSPACE") != std::string::npos,
                      "D5 NEGATIVE: calling explore_tree before activate_workspace returns NO_ACTIVE_WORKSPACE");

                CloseHandle(in_w);
                CloseHandle(out_r);
                WaitForSingleObject(pi.hProcess, 10000);
                DWORD exit_rc = 1;
                GetExitCodeProcess(pi.hProcess, &exit_rc);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                check(exit_rc == 0, "D5: projectless MCP exited cleanly with 0 on EOF");
            }
        }

        std::cout << "fs_organize_tests PASSED (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fs_organize_tests FAILED: " << e.what() << "\n";
        return 1;
    }
}
