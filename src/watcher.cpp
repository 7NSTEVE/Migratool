#include "migratool/scale.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/housekeep.hpp"
#include "usn.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <vector>
#include <winioctl.h>

namespace mt {
namespace {

using FnReadDirectoryChangesExW = BOOL(WINAPI*)(
    HANDLE hDirectory,
    LPVOID lpBuffer,
    DWORD nBufferLength,
    BOOL bWatchSubtree,
    DWORD dwNotifyFilter,
    LPDWORD lpBytesReturned,
    LPOVERLAPPED lpOverlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
    READ_DIRECTORY_NOTIFY_INFORMATION_CLASS ReadDirectoryNotifyInformationClass);

FnReadDirectoryChangesExW resolve_rdcew() {
    static FnReadDirectoryChangesExW fn = []() -> FnReadDirectoryChangesExW {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (!k32) return nullptr;
        return reinterpret_cast<FnReadDirectoryChangesExW>(GetProcAddress(k32, "ReadDirectoryChangesExW"));
    }();
    return fn;
}

int64_t li_to_unix_ns(LARGE_INTEGER ft) {
    constexpr int64_t DIFF = 116444736000000000LL;
    if (ft.QuadPart <= DIFF) return 0;
    return (ft.QuadPart - DIFF) * 100LL;
}

std::string basename_utf8(const std::wstring& rel) {
    auto pos = rel.find_last_of(L"\\/");
    std::wstring base = (pos == std::wstring::npos) ? rel : rel.substr(pos + 1);
    return utf8(base);
}

} // namespace

struct IndexWatcher::Impl {
    struct Watch {
        HANDLE directory = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED overlap{};
        std::unique_ptr<UsnReplay> journal;
        // 8 MB overlapped buffer (128x larger than 64 KB) prevents kernel overflows on burst writes/moves!
        std::vector<unsigned char> buffer;
        std::string root, id;
        int64_t fs_root_id = 0;
        fs::path base;
        bool pending = false;
        bool use_extended = false;

        Watch() : buffer(8 * 1024 * 1024) {}
        ~Watch() {
            if (directory != INVALID_HANDLE_VALUE) {
                CancelIoEx(directory, &overlap);
                CloseHandle(directory);
            }
            if (event) CloseHandle(event);
        }
        bool arm() {
            ResetEvent(event);
            overlap = {};
            overlap.hEvent = event;
            constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                     FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                     FILE_NOTIFY_CHANGE_CREATION;
            if (auto rdcew = resolve_rdcew(); rdcew != nullptr) {
                pending = rdcew(directory, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
                                filter, nullptr, &overlap, nullptr,
                                ReadDirectoryNotifyExtendedInformation) != FALSE;
                if (pending) {
                    use_extended = true;
                    return true;
                }
            }
            // Fallback to standard ReadDirectoryChangesW if ExW unsupported on target volume
            use_extended = false;
            DWORD fallback_sz = static_cast<DWORD>(std::min<size_t>(buffer.size(), 1024 * 1024));
            pending = ReadDirectoryChangesW(directory, buffer.data(), fallback_sz, TRUE,
                                            filter, nullptr, &overlap, nullptr) != FALSE;
            return pending;
        }
    };

    Project project;
    std::vector<std::unique_ptr<Watch>> watches;
    std::jthread thread;
    unsigned period;
    Log log;
    Cancel cancel;

    Impl(const Project& p, unsigned interval, Log logger, bool catch_up)
        : project(p), period(std::max(interval, 500u)), log(logger) {
        thread = std::jthread([this, catch_up](std::stop_token stop) { own(stop, catch_up); });
    }
    ~Impl() {
        cancel.requested = true;
        thread.request_stop();
        if (thread.joinable()) thread.join();
    }

    static bool relevant(const Project& p, std::wstring name, bool all_metadata) {
        if (is_watch_ignored_path(name, p)) return false;
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        const auto contains = [&](const std::wstring& folder) {
            return name == folder || name.starts_with(folder + L"\\") ||
                   name.find(L"\\" + folder + L"\\") != std::wstring::npos;
        };
        if (contains(L".migratool")) return false;
        return all_metadata || (!contains(L".git") && !contains(L".svn") && !contains(L".hg"));
    }

    void open() {
        Database db(project);
        fs_schema(db);
        const bool is_migration = (project_mode(project) == "migration");
        for (const auto& root : project.config["roots"]) {
            auto w = std::make_unique<Watch>();
            w->root = root["id"];
            w->id = unique_id();
            auto path = project.resolve(root["path"]);
            w->base = path;
            auto fsr = db.query("SELECT root_id, complete FROM fs_roots WHERE root_key=?", {w->root});
            if (!fsr.empty() && fsr[0]["complete"].get<int64_t>() != 0) {
                w->fs_root_id = fsr[0]["root_id"].get<int64_t>();
            }
            w->directory = CreateFileW(native_path(path).c_str(), FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            w->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::string error;
            if (w->directory == INVALID_HANDLE_VALUE || !w->event || !w->arm()) {
                error = "RDCW: " + std::to_string(GetLastError());
            }
            if (is_migration && scale_enabled(project, db)) {
                w->journal = std::make_unique<UsnReplay>(project, root, db);
            }
            db.exec("INSERT INTO watchers(id,pid,root,opened,error)VALUES(?,?,?,?,?)",
                    {w->id, GetCurrentProcessId(), w->root, now(), error});
            watches.push_back(std::move(w));
        }
    }

    void close() {
        try {
            Database db(project);
            for (const auto& w : watches) db.try_aux_write("UPDATE watchers SET closed=? WHERE id=?", {now(), w->id});
        } catch (...) {}
        watches.clear();
    }

    void own(std::stop_token stop, bool catch_up) {
        auto name = L"Local\\MigratoolWatchOwner_" + wide(sha256(path_text(fs::weakly_canonical(project.state_dir()))).substr(0, 32));
        HANDLE owner = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!owner) return;
        while (!stop.stop_requested()) {
            auto acquired = WaitForSingleObject(owner, 200);
            if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) continue;
            try {
                open();
                loop(stop, catch_up);
            } catch (const std::exception& e) {
                if (log) log(std::string("Watcher: ") + e.what());
            }
            close();
            ReleaseMutex(owner);
            if (!stop.stop_requested()) Sleep(200);
        }
        CloseHandle(owner);
    }

    void loop(std::stop_token stop, bool catch_up) {
        auto last_reconcile = GetTickCount64(), last_config = last_reconcile, last_usn_poll = last_reconcile;
        auto last_housekeep = last_reconcile;
        Database initial(project);
        const std::string mode = project_mode(project);
        const bool is_ultra_only = (mode == "archive" || mode == "workspace");
        const bool initial_dirty = initial.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'") != 0;
        const bool initial_paths = initial.integer("SELECT EXISTS(SELECT 1 FROM dirty_paths)") != 0;
        ULONGLONG due = (!is_ultra_only && (catch_up || initial_dirty || initial_paths)) ? last_reconcile + 500 : 0;
        bool reconcile = !is_ultra_only && (catch_up || (initial_dirty && !initial_paths));

        while (!stop.stop_requested()) {
            try {
                Database db(project);
                bool changed = false;
                const bool all_metadata = is_ultra_only || scale_enabled(project, db);

                // Legacy USN poll for migration mode
                for (auto& w : watches) {
                    if (w->journal) w->journal->poll(db, reconcile, changed);
                }

                // Refresh fs_root_id in case fs_sync ran after watcher opened
                for (auto& w : watches) {
                    if (w->fs_root_id == 0) {
                        auto fsr = db.query("SELECT root_id, complete FROM fs_roots WHERE root_key=?", {w->root});
                        if (!fsr.empty() && fsr[0]["complete"].get<int64_t>() != 0) {
                            w->fs_root_id = fsr[0]["root_id"].get<int64_t>();
                        }
                    }
                }

                // Ultra-scale USN delta poll every 2s if available (no full recrawl!)
                auto now_tick = GetTickCount64();
                if (is_ultra_only && now_tick - last_usn_poll >= 2000) {
                    last_usn_poll = now_tick;
                    for (const auto& rcfg : project.config["roots"]) {
                        try { fs_usn_replay_root(project, db, rcfg, &cancel); } catch (...) {}
                    }
                }

                // Automatic non-blocking background self-housekeeping (default every 30 minutes)
                const ULONGLONG hk_interval = static_cast<ULONGLONG>(project.config.value("housekeep_interval_ms", 1800000));
                if (hk_interval > 0 && now_tick - last_housekeep >= std::max<ULONGLONG>(hk_interval, 60000ULL)) {
                    last_housekeep = now_tick;
                    try { (void)housekeep_run(project, db, Json::object(), /*background=*/true, log); } catch (...) {}
                }

                for (auto& w : watches) {
                    if (!w->pending) continue;
                    if (WaitForSingleObject(w->event, 0) != WAIT_OBJECT_0) continue;
                    DWORD size{};
                    auto ok = GetOverlappedResult(w->directory, &w->overlap, &size, FALSE);
                    w->pending = false;

                    std::vector<std::pair<std::string, DWORD>> legacy_events;
                    bool fs_overflow = false;

                    if (!ok || !size) {
                        if (is_ultra_only) fs_overflow = true;
                        else reconcile = changed = true;
                    } else if (w->use_extended) {
                        // Zero-privilege ReadDirectoryChangesExW with FileId (FRN) + ParentFileId!
                        struct ExtEvent {
                            DWORD action;
                            uint64_t frn;
                            uint64_t parent_frn;
                            int64_t size_bytes;
                            int64_t mtime_ns;
                            uint32_t attrs;
                            DWORD reparse_tag;
                            std::wstring rel_w;
                        };
                        std::vector<ExtEvent> ext_events;
                        size_t at = 0;
                        while (at + offsetof(FILE_NOTIFY_EXTENDED_INFORMATION, FileName) <= size) {
                            const auto* e = reinterpret_cast<const FILE_NOTIFY_EXTENDED_INFORMATION*>(w->buffer.data() + at);
                            std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
                            if (relevant(project, name, all_metadata)) {
                                const bool is_dir = (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                                const bool dir_mod_noise = (e->Action == FILE_ACTION_MODIFIED && is_dir);
                                if (!dir_mod_noise) {
                                    ext_events.push_back(ExtEvent{
                                        e->Action,
                                        static_cast<uint64_t>(e->FileId.QuadPart),
                                        static_cast<uint64_t>(e->ParentFileId.QuadPart),
                                        is_dir ? 0 : std::max<int64_t>(0, e->FileSize.QuadPart),
                                        li_to_unix_ns(e->LastModificationTime),
                                        e->FileAttributes,
                                        e->ReparsePointTag,
                                        name
                                    });
                                    if (!is_ultra_only) {
                                        legacy_events.emplace_back(path_text(fs::path(name)), e->Action);
                                    }
                                }
                            }
                            if (!e->NextEntryOffset) break;
                            at += e->NextEntryOffset;
                        }

                        // Apply O(1) FRN updates directly into fs_nodes/fs_dirs inside a single batched transaction!
                        if (!ext_events.empty() && w->fs_root_id != 0) {
                            db.begin();
                            try {
                                for (const auto& ev : ext_events) {
                                    if (ev.action == FILE_ACTION_RENAMED_OLD_NAME) continue; // handled atomically by RENAMED_NEW_NAME via FRN!
                                    const bool is_rep = (ev.attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                                    const bool is_dir = (ev.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                                    int kind = is_rep
                                        ? (ev.reparse_tag == IO_REPARSE_TAG_MOUNT_POINT ? static_cast<int>(FsNodeKind::Junction)
                                            : (is_dir ? static_cast<int>(FsNodeKind::SymlinkDir) : static_cast<int>(FsNodeKind::SymlinkFile)))
                                        : (is_dir ? static_cast<int>(FsNodeKind::Directory) : static_cast<int>(FsNodeKind::File));

                                    if (ev.action == FILE_ACTION_REMOVED) {
                                        if (ev.frn != 0) fs_apply_delete(db, w->fs_root_id, ev.frn, true);
                                        else fs_sync_single_path(project, db, w->base / ev.rel_w, true);
                                    } else if (ev.action == FILE_ACTION_RENAMED_NEW_NAME) {
                                        if (ev.frn != 0 && ev.parent_frn != 0) {
                                            if (!fs_apply_rename_or_move(db, w->fs_root_id, ev.frn, ev.parent_frn,
                                                                         basename_utf8(ev.rel_w), ev.mtime_ns, true)) {
                                                // If item wasn't in DB yet, upsert it cleanly
                                                fs_apply_upsert(db, w->fs_root_id, ev.frn, ev.parent_frn,
                                                                basename_utf8(ev.rel_w), kind, ev.size_bytes, ev.mtime_ns, ev.attrs, "", true);
                                            }
                                        } else {
                                            fs_sync_single_path(project, db, w->base / ev.rel_w, true);
                                        }
                                    } else {
                                        // ADDED or MODIFIED
                                        if (ev.frn != 0 && ev.parent_frn != 0) {
                                            fs_apply_upsert(db, w->fs_root_id, ev.frn, ev.parent_frn,
                                                            basename_utf8(ev.rel_w), kind, ev.size_bytes, ev.mtime_ns, ev.attrs, "", true);
                                        } else {
                                            fs_sync_single_path(project, db, w->base / ev.rel_w, true);
                                        }
                                    }
                                }
                                db.commit();
                            } catch (...) {
                                db.rollback();
                                throw;
                            }
                        }
                    } else {
                        // Standard ReadDirectoryChangesW fallback
                        size_t at = 0;
                        std::vector<std::wstring> touched_paths;
                        while (at < size) {
                            auto* e = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(w->buffer.data() + at);
                            std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
                            if (relevant(project, name, all_metadata)) {
                                auto attrs = GetFileAttributesW(native_path(w->base / name).c_str());
                                const bool directory_metadata = e->Action == FILE_ACTION_MODIFIED &&
                                                                attrs != INVALID_FILE_ATTRIBUTES &&
                                                                (attrs & FILE_ATTRIBUTE_DIRECTORY);
                                if (!directory_metadata) {
                                    if (!is_ultra_only) legacy_events.emplace_back(path_text(fs::path(name)), e->Action);
                                    touched_paths.push_back(std::move(name));
                                }
                            }
                            if (!e->NextEntryOffset) break;
                            at += e->NextEntryOffset;
                        }
                        if (!touched_paths.empty() && w->fs_root_id != 0) {
                            db.begin();
                            try {
                                for (const auto& rel_w : touched_paths) {
                                    fs_sync_single_path(project, db, w->base / rel_w, true);
                                }
                                db.commit();
                            } catch (...) { db.rollback(); throw; }
                        }
                    }

                    if (!w->arm()) {
                        if (is_ultra_only) fs_overflow = true;
                        else reconcile = changed = true;
                    }

                    // On true 8 MB buffer overflow in archive/workspace mode: incremental mtime+FRN reconcile (NOT full 6M recrawl!)
                    if (fs_overflow && w->fs_root_id != 0) {
                        for (const auto& rcfg : project.config["roots"]) {
                            if (rcfg["id"] == w->root) {
                                fs_reconcile_incremental(project, db, rcfg, log, &cancel);
                                break;
                            }
                        }
                    }

                    if (!legacy_events.empty()) {
                        db.begin();
                        try {
                            for (const auto& [path, action] : legacy_events) {
                                db.exec("INSERT INTO dirty_paths(root,path,action,at)VALUES(?,?,?,?)ON CONFLICT(root,path)DO UPDATE SET action=excluded.action,at=excluded.at",
                                        {w->root, path, action, now()});
                            }
                            db.exec("INSERT INTO meta VALUES('dirty','1')ON CONFLICT(key)DO UPDATE SET value='1'");
                            db.change("watcher", "filesystem_dirty", {{"root", w->root}, {"events", legacy_events.size()}});
                            db.commit();
                        } catch (...) {
                            db.rollback();
                            throw;
                        }
                        changed = true;
                    }
                    db.try_aux_write("UPDATE watchers SET last_event=? WHERE id=?", {now(), w->id});
                }

                auto tick = GetTickCount64();
                if (!is_ultra_only) {
                    if (changed) due = tick + 200;
                    // Only run periodic reconciliation in legacy small/migration projects (NEVER force 15-min full recrawl on 6M archives!)
                    if (tick - last_reconcile >= period) {
                        reconcile = true;
                        due = tick;
                    }
                    if (!due && all_metadata &&
                        db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'") != 0 &&
                        db.integer("SELECT COUNT(*) FROM inventory_jobs WHERE state IN('queued','running')") == 0) {
                        if (!db.integer("SELECT EXISTS(SELECT 1 FROM dirty_paths)")) reconcile = true;
                        due = tick + 250;
                    }
                }

                if (tick - last_config > 1000) {
                    auto current = Project::load(project.file);
                    last_config = tick;
                    if (current.revision != project.revision) {
                        const bool roots = current.config["roots"] != project.config["roots"];
                        const bool engine = current.config.value("inventory_engine", "auto") != project.config.value("inventory_engine", "auto");
                        auto inv_cfg = [](Json cfg) { cfg.erase("builds"); cfg.erase("cases"); cfg.erase("name"); return cfg; };
                        const bool inv_changed = inv_cfg(current.config) != inv_cfg(project.config);
                        project = std::move(current);
                        if (roots || engine) { close(); open(); }
                        if (inv_changed && !is_ultra_only) { reconcile = true; due = tick; }
                    }
                }

                if (!is_ultra_only && due && tick >= due) {
                    if (!scale_enabled(project, db)) {
                        index_sync(project, false, log, &cancel);
                        db.exec("DELETE FROM dirty_paths");
                    } else if (db.integer("SELECT COUNT(*) FROM inventory_jobs WHERE state IN('queued','running')") == 0) {
                        if (reconcile || db.integer("SELECT EXISTS(SELECT 1 FROM inventory_generations WHERE state='published')") == 0) {
                            auto receipts = db.query("SELECT id,at FROM dirty_paths ORDER BY id LIMIT 1000");
                            scale_start(project, {{"actor", "watcher"}, {"event_receipts", receipts}});
                            last_reconcile = tick;
                            reconcile = false;
                        } else {
                            auto events = db.query("SELECT id,root,path,action,at FROM dirty_paths ORDER BY id LIMIT 1000");
                            std::map<std::string, Json> file_paths, file_receipts;
                            bool launched = false;
                            for (const auto& e : events) {
                                Json root;
                                for (const auto& r : project.config["roots"]) if (r["id"] == e["root"]) root = r;
                                auto discard = [&]() { db.exec("DELETE FROM dirty_paths WHERE id=? AND at=?", {e["id"], e["at"]}); };
                                if (root.is_null()) { discard(); continue; }
                                auto path = project.resolve(root["path"]) / wide(e["path"]);
                                auto attrs = GetFileAttributesW(native_path(path).c_str());
                                if (attrs == INVALID_FILE_ATTRIBUTES &&
                                    db.integer("SELECT EXISTS(SELECT 1 FROM files WHERE root=? AND path=? AND deleted=0) OR EXISTS(SELECT 1 FROM census_entries WHERE root=? AND path=?)",
                                               {e["root"], e["path"], e["root"], e["path"]}) == 0) {
                                    discard();
                                    continue;
                                }
                                const bool directory = attrs != INVALID_FILE_ATTRIBUTES
                                    ? (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0
                                    : db.integer("SELECT EXISTS(SELECT 1 FROM census_entries WHERE root=? AND path=? AND kind='directory')",
                                                 {e["root"], e["path"]}) != 0;
                                if (directory && e["action"] == FILE_ACTION_MODIFIED) { discard(); continue; }
                                const auto rid = e["root"].get<std::string>();
                                if (!directory) {
                                    if (!file_paths.contains(rid)) {
                                        file_paths[rid] = Json::array();
                                        file_receipts[rid] = Json::array();
                                    }
                                    file_paths[rid].push_back(e["path"]);
                                    file_receipts[rid].push_back(Json{{"id", e["id"]}, {"at", e["at"]}});
                                } else {
                                    scale_start(project, {{"scope", {{"root", e["root"]}, {"subdirectory", e["path"]}}},
                                                          {"actor", "watcher"},
                                                          {"event_receipts", Json::array({Json{{"id", e["id"]}, {"at", e["at"]}}})}}, false);
                                    launched = true;
                                }
                            }
                            for (const auto& [rid, paths] : file_paths) {
                                scale_start(project, {{"scope", {{"root", rid}, {"files", paths}}},
                                                      {"actor", "watcher"},
                                                      {"event_receipts", file_receipts[rid]}}, false);
                                launched = true;
                            }
                            if (launched) scale_spawn(project);
                        }
                    }
                    due = (reconcile || db.integer("SELECT EXISTS(SELECT 1 FROM dirty_paths)")) ? tick + 500 : 0;
                }
            } catch (const std::exception& e) {
                if (stop.stop_requested() || cancel.requested) break;
                if (log) log(std::string("Watcher: ") + e.what());
                due = GetTickCount64() + 1000;
            }
            for (int n = 0; n < 5 && !stop.stop_requested(); ++n) Sleep(20);
        }
    }
};

IndexWatcher::IndexWatcher(const Project& p, unsigned interval, Log log, bool catch_up)
    : impl_(std::make_unique<Impl>(p, interval, log, catch_up)) {}
IndexWatcher::~IndexWatcher() = default;

} // namespace mt
