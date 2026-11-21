#include "migratool/fs_organize.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/path_lineage.hpp"
#include "digest.hpp"

#include <zip.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mt {
namespace {

constexpr DWORD kCloudOfflineAttrs = FILE_ATTRIBUTE_OFFLINE | 0x00400000u /* FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS */;

struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleGuard(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~HandleGuard() { if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

std::string lower_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string norm_rel_fwd(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

std::string top_segment(const std::string& rel_fwd) {
    auto sl = rel_fwd.find('/');
    return (sl == std::string::npos) ? std::string("<root>") : rel_fwd.substr(0, sl);
}

std::string join_rel(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "/" + b;
}

// Fast table-driven IEEE 802.3 CRC32 (matches ZIP header CRC32)
uint32_t crc32_update(uint32_t crc, const unsigned char* data, size_t len) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string sha256_file_bounded(const fs::path& p, size_t max_bytes, Cancel* cancel) {
    HandleGuard h(CreateFileW(native_path(p).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (h.h == INVALID_HANDLE_VALUE) return {};
    Digest d;
    std::vector<char> buf(max_bytes > 0 ? std::min<size_t>(max_bytes, 65536) : 1024 * 1024);
    size_t remaining = max_bytes;
    for (;;) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
        DWORD to_read = static_cast<DWORD>(max_bytes > 0 ? std::min(remaining, buf.size()) : buf.size());
        if (to_read == 0) break;
        DWORD got = 0;
        if (!ReadFile(h.h, buf.data(), to_read, &got, nullptr)) return {};
        if (got == 0) break;
        d.add(buf.data(), got);
        if (max_bytes > 0) {
            remaining -= got;
            if (remaining == 0) break;
        }
    }
    return d.finish();
}

bool crc32_file(const fs::path& p, uint64_t* out_size, uint32_t* out_crc, Cancel* cancel) {
    HandleGuard h(CreateFileW(native_path(p).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (h.h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h.h, &li) || li.QuadPart < 0) return false;
    if (out_size) *out_size = static_cast<uint64_t>(li.QuadPart);
    std::vector<unsigned char> buf(1024 * 1024);
    uint32_t crc = 0;
    for (;;) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
        DWORD got = 0;
        if (!ReadFile(h.h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) return false;
        if (got == 0) break;
        crc = crc32_update(crc, buf.data(), got);
    }
    if (out_crc) *out_crc = crc;
    return true;
}

// Win32 safe directory helpers (handle \\?\ extended paths, trailing spaces, and ReadOnly empty dirs)
bool win32_exists(const fs::path& p) {
    return GetFileAttributesW(native_path(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool win32_is_dir(const fs::path& p) {
    DWORD a = GetFileAttributesW(native_path(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::vector<std::string> win32_ensure_dir(const fs::path& dir_path) {
    std::wstring np = native_path(dir_path).wstring();
    std::wstring prefix;
    std::wstring body = np;
    if (body.starts_with(L"\\\\?\\UNC\\")) {
        prefix = L"\\\\?\\UNC\\";
        body = body.substr(8);
    } else if (body.starts_with(L"\\\\?\\")) {
        prefix = L"\\\\?\\";
        body = body.substr(4);
    }
    std::vector<std::wstring> segs;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t sep = body.find(L'\\', pos);
        std::wstring s = (sep == std::wstring::npos) ? body.substr(pos) : body.substr(pos, sep - pos);
        if (!s.empty()) segs.push_back(std::move(s));
        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }
    std::vector<std::string> created;
    if (segs.empty()) return created;
    std::wstring cur = segs[0];
    size_t start_i = 1;
    if (prefix == L"\\\\?\\UNC\\" && segs.size() >= 2) {
        cur += L"\\" + segs[1];
        start_i = 2;
    }
    for (size_t i = start_i; i < segs.size(); ++i) {
        cur += L"\\" + segs[i];
        std::wstring full_w = prefix + cur;
        if (GetFileAttributesW(full_w.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(full_w.c_str(), nullptr)) {
                DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    throw std::runtime_error("CreateDirectoryW falló (Win32 " + std::to_string(err) + "): " + utf8(cur));
                }
            }
            created.push_back(utf8(cur));
        }
    }
    return created;
}

bool win32_rmdir_empty(const fs::path& p) {
    const auto np = native_path(p);
    DWORD attrs = GetFileAttributesW(np.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;
    if (attrs & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(np.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    return RemoveDirectoryW(np.c_str()) != FALSE;
}

bool win32_delete_file_clear_ro(const fs::path& p) {
    const auto np = native_path(p);
    DWORD attrs = GetFileAttributesW(np.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;
    if (attrs & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(np.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    return DeleteFileW(np.c_str()) != FALSE;
}

struct WalkedFile {
    std::string rel_fwd;
    int64_t size_bytes = 0;
    int64_t mtime_ns = 0;
    uint32_t attrs = 0;
};

struct WalkResult {
    std::vector<WalkedFile> files;
    std::vector<std::string> dirs;
    std::vector<std::string> junctions;
};

WalkResult win32_safe_walk(const fs::path& root, Cancel* cancel = nullptr) {
    WalkResult res;
    if (!win32_is_dir(root)) return res;
    std::vector<std::pair<fs::path, std::string>> stack;
    stack.emplace_back(root, "");
    while (!stack.empty()) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
        auto [cur_p, cur_rel] = std::move(stack.back());
        stack.pop_back();
        std::wstring pattern = native_path(cur_p).wstring();
        if (!pattern.ends_with(L"\\")) pattern.push_back(L'\\');
        pattern.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        struct FindGuard { HANDLE v; ~FindGuard() { if (v != INVALID_HANDLE_VALUE) FindClose(v); } } fg{h};

        std::vector<std::pair<fs::path, std::string>> subdirs;
        do {
            std::wstring_view wname(fd.cFileName);
            if (wname == L"." || wname == L"..") continue;
            std::string name;
            try { name = utf8(std::wstring(wname)); } catch (...) { continue; }
            const std::string rel = join_rel(cur_rel, name);
            const DWORD attrs = fd.dwFileAttributes;
            const DWORD rtag = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
            const FsNodeKind k = classify_win32_node(attrs, rtag);
            if (k == FsNodeKind::Junction || k == FsNodeKind::SymlinkDir || k == FsNodeKind::SymlinkFile) {
                res.junctions.push_back(rel);
            } else if (k == FsNodeKind::Directory) {
                res.dirs.push_back(rel);
                subdirs.emplace_back(cur_p / std::wstring(wname), rel);
            } else {
                const int64_t sz = (static_cast<int64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                const uint64_t ft = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                const int64_t mt = (ft > 116444736000000000ULL) ? static_cast<int64_t>((ft - 116444736000000000ULL) * 100ULL) : 0;
                res.files.push_back(WalkedFile{rel, sz, mt, attrs});
            }
        } while (FindNextFileW(h, &fd));

        std::sort(subdirs.begin(), subdirs.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (auto& sd : subdirs) stack.push_back(std::move(sd));
    }
    return res;
}

void win32_remove_tree_no_reparse(const fs::path& root) {
    DWORD attrs = GetFileAttributesW(native_path(root).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        win32_delete_file_clear_ro(root);
        return;
    }
    auto w = win32_safe_walk(root);
    for (const auto& f : w.files) {
        win32_delete_file_clear_ro(root / fs::path(wide(f.rel_fwd)));
    }
    std::sort(w.dirs.begin(), w.dirs.end(), [](const std::string& a, const std::string& b) {
        return a.size() > b.size();
    });
    for (const auto& d : w.dirs) {
        win32_rmdir_empty(root / fs::path(wide(d)));
    }
    win32_rmdir_empty(root);
}

void win32_copy_verified_file(const fs::path& src_f, const fs::path& dst_f) {
    win32_ensure_dir(dst_f.parent_path());
    if (!CopyFileExW(native_path(src_f).c_str(), native_path(dst_f).c_str(), nullptr, nullptr, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
        DWORD err = GetLastError();
        throw std::runtime_error("CopyFileExW falló (Win32 " + std::to_string(err) + "): " +
                                 path_text(src_f) + " -> " + path_text(dst_f));
    }
    const std::string h_src = sha256_file_bounded(src_f, 0, nullptr);
    const std::string h_dst = sha256_file_bounded(dst_f, 0, nullptr);
    if (h_src.empty() || h_src != h_dst) {
        win32_delete_file_clear_ro(dst_f);
        throw std::runtime_error("Verificación SHA-256 falló en copia cross-volume: " + path_text(src_f));
    }
}

void win32_move_no_overwrite(const fs::path& src, const fs::path& dst) {
    const auto nsrc = native_path(src);
    const auto ndst = native_path(dst);
    const DWORD attrs = GetFileAttributesW(nsrc.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("Origen no existe: " + path_text(src));
    }
    if (GetFileAttributesW(ndst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("Destino ya existe (sin sobrescritura): " + path_text(dst));
    }
    win32_ensure_dir(dst.parent_path());

    wchar_t force_xvol[16]{};
    const bool simulate_xvol = (GetEnvironmentVariableW(L"MIGRATOOL_FORCE_CROSS_VOLUME_MOVE", force_xvol, 16) > 0 &&
                                std::wstring_view(force_xvol) == L"1");

    if (!simulate_xvol && MoveFileExW(nsrc.c_str(), ndst.c_str(), 0)) {
        return;
    }
    const DWORD err = simulate_xvol ? ERROR_NOT_SAME_DEVICE : GetLastError();
    if (err != ERROR_NOT_SAME_DEVICE) {
        throw std::runtime_error("MoveFileExW falló (Win32 " + std::to_string(err) + "): " +
                                 path_text(src) + " -> " + path_text(dst));
    }

    // Cross-volume durable move: Stage on destination volume -> Verify SHA-256 -> Atomic Rename on destination -> Remove source
    const fs::path stage = dst.parent_path() / (dst.filename().wstring() + L".xvol_stage_" + std::to_wstring(GetCurrentProcessId()));
    win32_remove_tree_no_reparse(stage);

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        win32_copy_verified_file(src, stage);
        if (!MoveFileExW(native_path(stage).c_str(), ndst.c_str(), 0)) {
            DWORD rerr = GetLastError();
            win32_delete_file_clear_ro(stage);
            throw std::runtime_error("MoveFileExW stage->dst falló (Win32 " + std::to_string(rerr) + "): " + path_text(dst));
        }
        if (!win32_delete_file_clear_ro(src)) {
            throw std::runtime_error("No se pudo eliminar archivo origen tras copia cross-volume verificada: " + path_text(src));
        }
    } else {
        const auto walk = win32_safe_walk(src);
        if (!walk.junctions.empty()) {
            throw std::runtime_error("Cross-volume move rechazado por contener junctions/symlinks internos: " + path_text(src));
        }
        win32_ensure_dir(stage);
        for (const auto& d : walk.dirs) {
            win32_ensure_dir(stage / fs::path(wide(d)));
        }
        for (const auto& f : walk.files) {
            const fs::path r = fs::path(wide(f.rel_fwd));
            win32_copy_verified_file(src / r, stage / r);
        }
        if (!MoveFileExW(native_path(stage).c_str(), ndst.c_str(), 0)) {
            DWORD rerr = GetLastError();
            win32_remove_tree_no_reparse(stage);
            throw std::runtime_error("MoveFileExW dir stage->dst falló (Win32 " + std::to_string(rerr) + "): " + path_text(dst));
        }
        win32_remove_tree_no_reparse(src);
    }
}

// Fast pipe-based git invocation (no temp files on disk!)
fs::path find_git_exe() {
    static const fs::path cached = []() -> fs::path {
        std::error_code ec;
        for (const wchar_t* cand : {
                 L"D:\\DevTools\\Tools\\Git\\cmd\\git.exe",
                 L"C:\\Program Files\\Git\\cmd\\git.exe"}) {
            if (fs::is_regular_file(cand, ec)) return fs::path(cand);
        }
        wchar_t buf[32768]{};
        if (SearchPathW(nullptr, L"git.exe", nullptr, 32768, buf, nullptr)) return fs::path(buf);
        return {};
    }();
    return cached;
}

struct CmdOut {
    int exit_code = -1;
    std::string stdout_text;
};

CmdOut run_git_fast(const fs::path& repo_dir, const std::vector<std::string>& git_args, DWORD timeout_ms = 15000) {
    CmdOut res;
    wchar_t dis_buf[16]{};
    if (GetEnvironmentVariableW(L"MIGRATOOL_DISABLE_GIT", dis_buf, 16) > 0 && std::wstring_view(dis_buf) == L"1") {
        res.stdout_text = "git disabled by MIGRATOOL_DISABLE_GIT";
        return res;
    }
    const auto git = find_git_exe();
    if (git.empty()) return res;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rpipe = nullptr, wpipe = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return res;
    HandleGuard rg(rpipe), wg(wpipe);
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdline = quote_arg(git.wstring()) + L" -C " + quote_arg(repo_dir.wstring());
    for (const auto& a : git_args) cmdline += L" " + quote_arg(wide(a));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_mut(cmdline.begin(), cmdline.end());
    cmd_mut.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmd_mut.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return res;
    }
    CloseHandle(wpipe);
    wg.h = INVALID_HANDLE_VALUE;
    HandleGuard ph(pi.hProcess), th(pi.hThread);

    std::string out;
    std::array<char, 8192> buf{};
    DWORD got = 0;
    while (ReadFile(rpipe, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
        out.append(buf.data(), got);
        if (out.size() > 32 * 1024 * 1024) break;
    }
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        return res;
    }
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    res.exit_code = static_cast<int>(ec);
    res.stdout_text = std::move(out);
    return res;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        std::string line = (e == std::string::npos) ? s.substr(i) : s.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return out;
}

struct LibZipDyn {
    HMODULE handle{};
    decltype(&::zip_error_init) error_init{};
    decltype(&::zip_error_fini) error_fini{};
    decltype(&::zip_error_strerror) error_strerror{};
    decltype(&::zip_source_win32w_create) source_win32w_create{};
    decltype(&::zip_source_free) source_free{};
    decltype(&::zip_open_from_source) open_from_source{};
    decltype(&::zip_discard) discard{};
    decltype(&::zip_get_num_entries) get_num_entries{};
    decltype(&::zip_stat_index) stat_index{};

    LibZipDyn() {
        std::vector<fs::path> candidates;
        try {
            const auto exe_dir = executable_dir();
            if (!exe_dir.empty()) {
                candidates.push_back((exe_dir / L"libzip.dll").make_preferred());
                candidates.push_back((exe_dir / L"zip.dll").make_preferred());
            }
        } catch (...) {}
        candidates.push_back(fs::path(MIGRATOOL_ZIP_DLL).make_preferred());
        for (const auto& cand : candidates) {
            if (!cand.empty() && win32_exists(cand)) {
                handle = LoadLibraryExW(cand.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (handle) break;
            }
        }
        if (!handle) handle = LoadLibraryW(L"libzip.dll");
        if (!handle) handle = LoadLibraryW(L"zip.dll");
        if (!handle) throw std::runtime_error("No se pudo cargar libzip (Win32 " + std::to_string(GetLastError()) + ")");
        error_init = load<decltype(&::zip_error_init)>("zip_error_init");
        error_fini = load<decltype(&::zip_error_fini)>("zip_error_fini");
        error_strerror = load<decltype(&::zip_error_strerror)>("zip_error_strerror");
        source_win32w_create = load<decltype(&::zip_source_win32w_create)>("zip_source_win32w_create");
        source_free = load<decltype(&::zip_source_free)>("zip_source_free");
        open_from_source = load<decltype(&::zip_open_from_source)>("zip_open_from_source");
        discard = load<decltype(&::zip_discard)>("zip_discard");
        get_num_entries = load<decltype(&::zip_get_num_entries)>("zip_get_num_entries");
        stat_index = load<decltype(&::zip_stat_index)>("zip_stat_index");
    }
    ~LibZipDyn() { if (handle) FreeLibrary(handle); }
    template <class Fn> Fn load(const char* name) const {
        auto addr = GetProcAddress(handle, name);
        if (!addr) throw std::runtime_error(std::string("Función libzip no disponible: ") + name);
        return std::bit_cast<Fn>(addr);
    }
};

// Resolve primary root info from fs_roots (ensuring index exists)
struct RootRow {
    int64_t root_id = 0;
    std::string root_key;
    fs::path abs_path;
};

std::vector<RootRow> load_fs_roots(const Project& p, Database& db, const std::string& filter_root) {
    fs_schema(db);
    auto rows = filter_root.empty()
        ? db.query("SELECT root_id, root_key, abs_path FROM fs_roots ORDER BY root_id")
        : db.query("SELECT root_id, root_key, abs_path FROM fs_roots WHERE root_key=?", {filter_root});
    if (rows.empty() && p.config.contains("roots") && p.config["roots"].is_array()) {
        for (const auto& rcfg : p.config["roots"]) {
            if (!filter_root.empty() && rcfg.value("id", "") != filter_root) continue;
            int64_t rid = fs_ensure_root(p, db, rcfg);
            rows.push_back({{"root_id", rid}, {"root_key", rcfg.at("id")}, {"abs_path", path_text(p.resolve(rcfg.at("path").get<std::string>()))}});
        }
    }
    std::vector<RootRow> out;
    for (const auto& r : rows) {
        out.push_back(RootRow{r["root_id"].get<int64_t>(), r["root_key"].get<std::string>(), fs::path(wide(r["abs_path"].get<std::string>()))});
    }
    return out;
}

bool is_regenerable_dir_name(const std::string& lower_name) {
    static const std::unordered_set<std::string> exact = {
        "build", "builds", "build32", "build64", "bin", "obj", "out",
        "__pycache__", ".dart_tool", "node_modules", ".gradle", ".pytest_cache",
        ".mypy_cache", ".cxx", ".vs", "intermediates", "cmake-build-debug",
        "cmake-build-release", "target", ".next", ".nuxt", "dist"
    };
    if (exact.contains(lower_name)) return true;
    if (lower_name.starts_with("build-") || lower_name.starts_with("build_")) return true;
    return false;
}

std::unordered_set<std::string> git_tracked_files_rel(const fs::path& repo_dir, bool* out_ok = nullptr) {
    std::unordered_set<std::string> out;
    auto r = run_git_fast(repo_dir, {"ls-files", "-z"});
    if (out_ok) *out_ok = (r.exit_code == 0);
    if (r.exit_code != 0) return out;
    size_t i = 0;
    while (i < r.stdout_text.size()) {
        size_t z = r.stdout_text.find('\0', i);
        std::string item = (z == std::string::npos) ? r.stdout_text.substr(i) : r.stdout_text.substr(i, z - i);
        if (!item.empty()) out.insert(lower_ascii(norm_rel_fwd(item)));
        if (z == std::string::npos) break;
        i = z + 1;
    }
    return out;
}

bool git_is_path_ignored(const fs::path& repo_dir, const std::string& rel_inside_repo) {
    auto r = run_git_fast(repo_dir, {"check-ignore", norm_rel_fwd(rel_inside_repo)});
    return r.exit_code == 0 && !r.stdout_text.empty();
}

Json snapshot_single_git_repo(const fs::path& repo_dir, const std::string& rel_label) {
    Json info = {{"rel", rel_label}, {"exists", win32_is_dir(repo_dir)}, {"git_verified", false}};
    if (!info["exists"].get<bool>()) return info;
    fs::path dotgit = repo_dir / L".git";
    const bool is_file = win32_exists(dotgit) && !win32_is_dir(dotgit);
    info["dotgit_type"] = is_file ? "file" : (win32_is_dir(dotgit) ? "dir" : "missing");
    if (is_file) {
        try {
            std::string gf = read_text(dotgit, 16384);
            while (!gf.empty() && (gf.back() == '\r' || gf.back() == '\n' || gf.back() == ' ')) gf.pop_back();
            info["gitfile"] = gf;
            if (gf.starts_with("gitdir:")) {
                std::string t = gf.substr(7);
                while (!t.empty() && t.front() == ' ') t.erase(t.begin());
                fs::path tp(wide(t));
                if (tp.is_relative()) tp = repo_dir / tp;
                info["worktree_target"] = t;
                info["worktree_broken"] = !win32_is_dir(tp);
            }
        } catch (...) {
            info["worktree_broken"] = true;
        }
    }
    auto head_r = run_git_fast(repo_dir, {"rev-parse", "HEAD"});
    info["head_rc"] = head_r.exit_code;
    if (head_r.exit_code == 0) {
        auto hlines = split_lines(head_r.stdout_text);
        info["head"] = hlines.empty() ? "" : hlines[0];
        auto br = run_git_fast(repo_dir, {"rev-parse", "--abbrev-ref", "HEAD"});
        auto bl = split_lines(br.stdout_text);
        info["branch"] = bl.empty() ? "" : bl[0];
        auto wt = run_git_fast(repo_dir, {"worktree", "list"});
        auto wl = split_lines(wt.stdout_text);
        int64_t prunable = 0;
        for (const auto& l : wl) if (l.find("prunable") != std::string::npos) ++prunable;
        info["worktree_count"] = static_cast<int64_t>(wl.size());
        info["worktree_prunable"] = prunable;
    } else {
        auto el = split_lines(head_r.stdout_text);
        info["error"] = el.empty() ? "git error" : el[0].substr(0, 120);
    }

    auto st = run_git_fast(repo_dir, {"status", "--porcelain"});
    info["status_rc"] = st.exit_code;
    if (st.exit_code == 0) {
        auto sl = split_lines(st.stdout_text);
        int64_t untrk = 0;
        for (const auto& l : sl) if (l.starts_with("??")) ++untrk;
        std::sort(sl.begin(), sl.end());
        std::string joined;
        for (const auto& l : sl) { joined += l; joined.push_back('\n'); }
        info["status_count"] = static_cast<int64_t>(sl.size());
        info["dirty_untracked"] = untrk;
        info["dirty_tracked_mod"] = static_cast<int64_t>(sl.size()) - untrk;
        info["status_hash"] = sha256(joined);
    }

    bool ls_ok = false;
    auto tracked = git_tracked_files_rel(repo_dir, &ls_ok);
    info["ls_files_rc"] = ls_ok ? 0 : -1;
    if (ls_ok) {
        info["tracked_count"] = static_cast<int64_t>(tracked.size());
    }
    info["git_verified"] = (st.exit_code == 0 && ls_ok && !info.value("worktree_broken", false));
    return info;
}

void validate_safe_id(const std::string& id, const char* field) {
    if (id.empty() || id.size() > 128 || id == "." || id == "..") {
        throw std::runtime_error(std::string("SAFETY_VIOLATION: identificador inválido en ") + field + ": " + id);
    }
    for (unsigned char c : id) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            throw std::runtime_error(std::string("SAFETY_VIOLATION: carácter no permitido en ") + field + ": " + id);
        }
    }
    std::string trimmed = id;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '.')) trimmed.pop_back();
    if (trimmed.empty()) {
        throw std::runtime_error(std::string("SAFETY_VIOLATION: identificador vacío o de puntos en ") + field + ": " + id);
    }
}

bool has_traversal_or_abs_segment(const std::string& raw) {
    if (raw.empty()) return true;
    if (raw.find(':') != std::string::npos) return true;
    if (raw.starts_with("/") || raw.starts_with("\\")) return true;
    fs::path wp(wide(raw));
    if (wp.is_absolute() || wp.has_root_name() || wp.has_root_directory()) return true;
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t sl = s.find('/', pos);
        std::string seg = (sl == std::string::npos) ? s.substr(pos) : s.substr(pos, sl - pos);
        if (seg.empty() || seg == "." || seg == "..") return true;
        std::string trimmed = seg;
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '.')) trimmed.pop_back();
        if (trimmed.empty()) return true; // rejects "... " or ".. "
        for (unsigned char c : seg) {
            if (c < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return true;
        }
        if (sl == std::string::npos) break;
        pos = sl + 1;
    }
    return false;
}

bool is_confined_within(const fs::path& candidate, const fs::path& base, bool allow_equal = false) {
    for (const auto& part : candidate) {
        if (part == L"..") return false;
    }
    std::string c_str = norm_rel_fwd(lower_ascii(path_text(candidate)));
    std::string b_str = norm_rel_fwd(lower_ascii(path_text(base)));
    if (b_str.empty()) return false;
    if (allow_equal && c_str == b_str) return true;
    return c_str.size() > b_str.size() && c_str.starts_with(b_str + "/");
}

std::string validate_rel_no_traversal(const std::string& raw, const char* field) {
    if (has_traversal_or_abs_segment(raw)) {
        throw std::runtime_error(std::string("SAFETY_VIOLATION: ruta relativa inválida o traversal en ") + field + ": " + raw);
    }
    std::string fwd = norm_rel_fwd(raw);
    if (fwd.empty() || has_traversal_or_abs_segment(fwd)) {
        throw std::runtime_error(std::string("SAFETY_VIOLATION: ruta relativa inválida o traversal en ") + field + ": " + raw);
    }
    return fwd;
}

fs::path resolve_confined_under(const fs::path& base, const std::string& raw, const char* field) {
    const std::string fwd = validate_rel_no_traversal(raw, field);
    std::wstring wrel = wide(fwd);
    for (auto& c : wrel) if (c == L'/') c = L'\\';
    const fs::path cand = base / fs::path(wrel);
    if (!is_confined_within(cand, base, false)) {
        throw std::runtime_error(std::string("SAFETY_VIOLATION: ruta fuera de la raíz autorizada en ") + field + ": " + raw);
    }
    return cand;
}

fs::path resolve_authorized_org_path(const Project& p, const fs::path& primary_base, const std::string& raw, const char* field, bool allow_root_equal = false) {
    if (raw.empty()) throw std::runtime_error(std::string("SAFETY_VIOLATION: ruta vacía en ") + field);
    fs::path wp(wide(raw));
    for (const auto& part : wp) {
        if (part == L"..") {
            throw std::runtime_error(std::string("SAFETY_VIOLATION: traversal '..' rechazado en ") + field + ": " + raw);
        }
    }
    if (!wp.is_absolute() && !wp.has_root_name() && !wp.has_root_directory()) {
        if (primary_base.empty()) throw std::runtime_error(std::string("SAFETY_VIOLATION: sin raíz base para ") + field);
        return resolve_confined_under(primary_base, raw, field);
    }
    // Absolute path must be confined within primary_base or one of the project's authorized roots
    if (!primary_base.empty() && is_confined_within(wp, primary_base, allow_root_equal)) return wp;
    if (p.config.contains("roots") && p.config["roots"].is_array()) {
        for (const auto& r : p.config["roots"]) {
            fs::path rbase = p.resolve(r.at("path").get<std::string>());
            if (is_confined_within(wp, rbase, allow_root_equal)) return wp;
        }
    }
    throw std::runtime_error(std::string("SAFETY_VIOLATION: ruta absoluta fuera de las raíces autorizadas en ") + field + ": " + raw);
}

struct RootFrnKey {
    int64_t root_id = 0;
    uint64_t frn = 0;
    bool operator==(const RootFrnKey&) const = default;
};

struct RootFrnKeyHash {
    size_t operator()(const RootFrnKey& k) const noexcept {
        const size_t h1 = std::hash<int64_t>{}(k.root_id);
        const size_t h2 = std::hash<uint64_t>{}(k.frn);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

bool stat_live_file_size_mtime(const fs::path& p, int64_t* out_size, int64_t* out_mtime_ns) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(native_path(p).c_str(), GetFileExInfoStandard, &fad)) return false;
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    const int64_t sz = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    const uint64_t ft = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
    const int64_t mt = (ft > 116444736000000000ULL) ? static_cast<int64_t>((ft - 116444736000000000ULL) * 100ULL) : 0;
    if (out_size) *out_size = sz;
    if (out_mtime_ns) *out_mtime_ns = mt;
    return true;
}

} // namespace

// ============================================================================
// 1. tool_find_duplicates
// ============================================================================
Json tool_find_duplicates(const Project& p, Database& db, const Json& args, Cancel* cancel) {
    fs_schema(db);
    auto t0 = std::chrono::steady_clock::now();

    const int64_t min_bytes = std::max<int64_t>(1, args.value("min_bytes", int64_t(1024)));
    const int limit = std::clamp(args.value("limit", 50), 1, 500);
    const std::string filter_root = args.value("root", "");
    const std::string path_prefix = norm_rel_fwd(args.value("path_prefix", ""));
    const bool skip_cloud_only = args.value("skip_cloud_only", true);
    const bool refresh_hashes = args.value("refresh_hashes", false);
    const bool verify_strict = args.value("verify_strict", false);
    const bool persist_cache = args.value("persist_cache", true);
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int threads = std::clamp(args.value("threads", std::clamp(hw, 2, 8)), 1, 16);

    std::unordered_set<std::string> excl_dirs;
    if (args.contains("exclude_dirs") && args["exclude_dirs"].is_array()) {
        for (const auto& d : args["exclude_dirs"]) {
            if (d.is_string()) excl_dirs.insert(lower_ascii(d.get<std::string>()));
        }
    }

    auto roots = load_fs_roots(p, db, filter_root);
    std::unordered_map<int64_t, RootRow> root_map;
    for (const auto& r : roots) root_map[r.root_id] = r;
    if (root_map.empty()) {
        return {{"schema", "migratool.find_duplicates/1"}, {"dup_groups", 0}, {"items", Json::array()}};
    }

    // Load cached hashes into RAM using collision-free composite key (root_id, frn)
    struct HashCacheEntry { int64_t size_bytes; int64_t mtime_ns; std::string partial; std::string full; };
    std::unordered_map<RootFrnKey, HashCacheEntry, RootFrnKeyHash> hash_cache;
    if (!refresh_hashes) {
        db.each("SELECT root_id, frn, size_bytes, mtime_ns, partial_sha256, full_sha256 FROM fs_hashes", Json::array(), [&](const Json& r) {
            int64_t rid = r["root_id"].get<int64_t>();
            uint64_t frn = static_cast<uint64_t>(r["frn"].get<int64_t>());
            hash_cache[RootFrnKey{rid, frn}] = HashCacheEntry{
                r["size_bytes"].get<int64_t>(), r["mtime_ns"].get<int64_t>(),
                r["partial_sha256"].get<std::string>(), r["full_sha256"].get<std::string>()
            };
            return true;
        });
    }

    // Stage 1: Group candidates by exact size_bytes in SQLite
    struct CandFile {
        int64_t root_id = 0;
        uint64_t frn = 0;
        int64_t size_bytes = 0;
        int64_t mtime_ns = 0;
        std::string root_key;
        std::string rel_fwd;
        fs::path full_path;
        std::string partial_hash;
        std::string full_hash;
        bool cache_dirty = false;
        bool from_cache = false;
    };

    std::unordered_map<int64_t, std::vector<CandFile>> by_size;
    int64_t cloud_skipped = 0;
    int64_t scanned_candidates = 0;

    std::string sql =
        "SELECT n.root_id, n.frn, n.name, n.size_bytes, n.mtime_ns, n.attrs, COALESCE(d.rel_path,'') AS parent_rel "
        "FROM fs_nodes n LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn "
        "WHERE n.kind=0 AND (n.flags & 1)=0 AND n.size_bytes>=?";
    Json params = {min_bytes};
    if (!filter_root.empty() && !roots.empty()) {
        sql += " AND n.root_id=?";
        params.push_back(roots[0].root_id);
    }
    if (!path_prefix.empty()) {
        sql += " AND (d.rel_path=? OR (d.rel_path>=? AND d.rel_path<?))";
        params.push_back(path_prefix);
        params.push_back(path_prefix + "/");
        params.push_back(path_prefix + "0");
    }

    db.each(sql, params, [&](const Json& row) {
        if (cancel && cancel->requested) return false;
        const int64_t rid = row["root_id"].get<int64_t>();
        auto rit = root_map.find(rid);
        if (rit == root_map.end()) return true;
        const uint32_t attrs = static_cast<uint32_t>(row["attrs"].get<int64_t>());
        if (skip_cloud_only && (attrs & kCloudOfflineAttrs) != 0) {
            ++cloud_skipped;
            return true;
        }
        const std::string rel = join_rel(row["parent_rel"].get<std::string>(), row["name"].get<std::string>());
        if (!excl_dirs.empty()) {
            const std::string low = lower_ascii(rel);
            size_t pos = 0;
            while (true) {
                size_t sl = low.find('/', pos);
                if (sl == std::string::npos) break;
                if (excl_dirs.contains(low.substr(pos, sl - pos))) return true;
                pos = sl + 1;
            }
        }
        CandFile cf;
        cf.root_id = rid;
        cf.frn = static_cast<uint64_t>(row["frn"].get<int64_t>());
        cf.size_bytes = row["size_bytes"].get<int64_t>();
        cf.mtime_ns = row["mtime_ns"].get<int64_t>();
        cf.root_key = rit->second.root_key;
        cf.rel_fwd = rel;
        std::wstring wrel = wide(rel);
        for (auto& c : wrel) if (c == L'/') c = L'\\';
        cf.full_path = rit->second.abs_path / fs::path(wrel);
        by_size[cf.size_bytes].push_back(std::move(cf));
        ++scanned_candidates;
        return true;
    });
    if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");

    // Collect only size buckets with >= 2 files
    std::vector<CandFile*> stage2_work;
    for (auto& [sz, vec] : by_size) {
        if (vec.size() >= 2) {
            for (auto& c : vec) stage2_work.push_back(&c);
        }
    }

    std::atomic<int64_t> cache_hits{0}, hashed_partial{0}, hashed_full{0};
    auto run_parallel = [&](size_t n, auto fn) {
        if (n == 0) return;
        std::atomic<size_t> next_i{0};
        std::atomic<bool> stop{false};
        std::mutex err_mtx;
        std::exception_ptr first_err;
        const int w = std::min<int>(threads, static_cast<int>(n));
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(w));
        for (int t = 0; t < w; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    if (stop.load(std::memory_order_relaxed) || (cancel && cancel->requested)) break;
                    size_t i = next_i.fetch_add(1);
                    if (i >= n) break;
                    try {
                        fn(i);
                    } catch (...) {
                        stop.store(true, std::memory_order_relaxed);
                        std::lock_guard lk(err_mtx);
                        if (!first_err) first_err = std::current_exception();
                        break;
                    }
                }
            });
        }
        for (auto& th : pool) if (th.joinable()) th.join();
        if (first_err) std::rethrow_exception(first_err);
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
    };

    // Stage 2: 64 KiB partial SHA-256 (validates live disk size & mtime against fs_hashes!)
    run_parallel(stage2_work.size(), [&](size_t i) {
        CandFile* c = stage2_work[i];
        int64_t live_size = 0, live_mtime = 0;
        if (!stat_live_file_size_mtime(c->full_path, &live_size, &live_mtime)) {
            return; // File deleted or inaccessible on disk
        }
        const bool size_changed_on_disk = (live_size != c->size_bytes);
        c->size_bytes = live_size;
        c->mtime_ns = live_mtime;
        if (c->size_bytes < min_bytes) return;

        if (!refresh_hashes && !verify_strict && !size_changed_on_disk) {
            auto it = hash_cache.find(RootFrnKey{c->root_id, c->frn});
            if (it != hash_cache.end() && it->second.size_bytes == c->size_bytes &&
                it->second.mtime_ns == c->mtime_ns && !it->second.partial.empty()) {
                c->partial_hash = it->second.partial;
                c->full_hash = it->second.full;
                c->from_cache = true;
                cache_hits.fetch_add(1);
                return;
            }
        }
        c->partial_hash = sha256_file_bounded(c->full_path, 65536, cancel);
        if (!c->partial_hash.empty()) {
            hashed_partial.fetch_add(1);
            if (c->size_bytes <= 65536) c->full_hash = c->partial_hash;
            c->cache_dirty = true;
        }
    });

    std::unordered_map<std::string, std::vector<CandFile*>> by_partial;
    for (CandFile* c : stage2_work) {
        if (!c->partial_hash.empty()) {
            by_partial[std::to_string(c->size_bytes) + ":" + c->partial_hash].push_back(c);
        }
    }

    // Stage 3: Full SHA-256 only for partial collisions > 64 KiB (or strict re-verification)
    std::vector<CandFile*> stage3_work;
    for (auto& [_, vec] : by_partial) {
        if (vec.size() >= 2) {
            for (CandFile* c : vec) {
                if (c->full_hash.empty() || (verify_strict && c->from_cache)) stage3_work.push_back(c);
            }
        }
    }
    run_parallel(stage3_work.size(), [&](size_t i) {
        CandFile* c = stage3_work[i];
        c->full_hash = sha256_file_bounded(c->full_path, 0, cancel);
        if (!c->full_hash.empty()) {
            hashed_full.fetch_add(1);
            c->cache_dirty = true;
        }
    });

    // Persist newly computed hashes to fs_hashes cache (when persist_cache is enabled)
    if (persist_cache) {
        std::vector<CandFile*> dirty;
        for (CandFile* c : stage2_work) if (c->cache_dirty) dirty.push_back(c);
        if (!dirty.empty()) {
            const int64_t ts = now_ns();
            db.begin();
            try {
                sqlite3* raw = db.native_handle();
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(raw,
                        "INSERT OR REPLACE INTO fs_hashes(root_id,frn,size_bytes,mtime_ns,partial_sha256,full_sha256,computed_ns) "
                        "VALUES(?,?,?,?,?,?,?)", -1, &st, nullptr) == SQLITE_OK) {
                    for (CandFile* c : dirty) {
                        sqlite3_reset(st);
                        sqlite3_clear_bindings(st);
                        sqlite3_bind_int64(st, 1, c->root_id);
                        sqlite3_bind_int64(st, 2, static_cast<int64_t>(c->frn));
                        sqlite3_bind_int64(st, 3, c->size_bytes);
                        sqlite3_bind_int64(st, 4, c->mtime_ns);
                        sqlite3_bind_text(st, 5, c->partial_hash.c_str(), static_cast<int>(c->partial_hash.size()), SQLITE_STATIC);
                        sqlite3_bind_text(st, 6, c->full_hash.c_str(), static_cast<int>(c->full_hash.size()), SQLITE_STATIC);
                        sqlite3_bind_int64(st, 7, ts);
                        sqlite3_step(st);
                    }
                    sqlite3_finalize(st);
                }
                db.commit();
            } catch (...) {
                db.rollback();
            }
        }
    }

    // Assemble final exact-duplicate groups + tree_overlap matrix
    struct DupGroup {
        std::string sha256_hex;
        int64_t size_bytes = 0;
        std::vector<std::string> paths;
        int64_t reclaimable() const { return size_bytes * static_cast<int64_t>(paths.size() - 1); }
    };
    std::unordered_map<std::string, std::vector<CandFile*>> by_full;
    for (auto& [_, vec] : by_partial) {
        if (vec.size() < 2) continue;
        for (CandFile* c : vec) {
            if (!c->full_hash.empty()) by_full[c->full_hash].push_back(c);
        }
    }

    std::vector<DupGroup> groups;
    std::map<std::pair<std::string, std::string>, std::pair<int64_t, int64_t>> pair_overlap;
    int64_t redundant_files = 0, redundant_bytes = 0;

    for (auto& [h, vec] : by_full) {
        if (vec.size() < 2) continue;
        DupGroup g;
        g.sha256_hex = h;
        g.size_bytes = vec[0]->size_bytes;
        std::set<std::string> tops;
        for (CandFile* c : vec) {
            g.paths.push_back(roots.size() > 1 ? (c->root_key + ":" + c->rel_fwd) : c->rel_fwd);
            tops.insert(top_segment(c->rel_fwd));
        }
        std::sort(g.paths.begin(), g.paths.end());
        redundant_files += static_cast<int64_t>(g.paths.size() - 1);
        redundant_bytes += g.reclaimable();
        std::vector<std::string> tv(tops.begin(), tops.end());
        for (size_t i = 0; i < tv.size(); ++i) {
            for (size_t j = i + 1; j < tv.size(); ++j) {
                auto& acc = pair_overlap[{tv[i], tv[j]}];
                acc.first += 1;
                acc.second += g.size_bytes;
            }
        }
        groups.push_back(std::move(g));
    }

    std::sort(groups.begin(), groups.end(), [](const DupGroup& a, const DupGroup& b) {
        return a.reclaimable() > b.reclaimable();
    });

    Json items = Json::array();
    const size_t emit_n = std::min<size_t>(groups.size(), static_cast<size_t>(limit));
    for (size_t i = 0; i < emit_n; ++i) {
        const auto& g = groups[i];
        items.push_back({
            {"sha256", g.sha256_hex},
            {"size_bytes", g.size_bytes},
            {"count", g.paths.size()},
            {"reclaimable_bytes", g.reclaimable()},
            {"paths", g.paths}
        });
    }

    std::vector<std::tuple<std::string, std::string, int64_t, int64_t>> ov;
    for (const auto& [k, v] : pair_overlap) ov.emplace_back(k.first, k.second, v.first, v.second);
    std::sort(ov.begin(), ov.end(), [](const auto& a, const auto& b) { return std::get<3>(a) > std::get<3>(b); });
    Json overlap = Json::array();
    for (size_t i = 0; i < std::min<size_t>(ov.size(), 25); ++i) {
        overlap.push_back({
            {"left", std::get<0>(ov[i])},
            {"right", std::get<1>(ov[i])},
            {"shared_groups", std::get<2>(ov[i])},
            {"shared_bytes", std::get<3>(ov[i])}
        });
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const size_t ret_cnt = items.size();
    return {
        {"schema", "migratool.find_duplicates/1"},
        {"min_bytes", min_bytes},
        {"scanned_files", scanned_candidates},
        {"cloud_only_skipped", cloud_skipped},
        {"cache_hits", cache_hits.load()},
        {"hashed_partial", hashed_partial.load()},
        {"hashed_full", hashed_full.load()},
        {"dup_groups", groups.size()},
        {"redundant_files", redundant_files},
        {"redundant_bytes", redundant_bytes},
        {"returned", ret_cnt},
        {"tree_overlap", std::move(overlap)},
        {"elapsed_ms", elapsed_ms},
        {"items", std::move(items)}
    };
}

// ============================================================================
// 2. tool_verify_archive_vs_dir
// ============================================================================
Json tool_verify_archive_vs_dir(const Project& p, Database& db, const Json& args, Cancel* cancel) {
    (void)db;
    auto t0 = std::chrono::steady_clock::now();
    const std::string zip_arg = args.value("zip", args.value("archive", ""));
    const std::string dir_arg = args.value("target_dir", args.value("dir", ""));
    if (zip_arg.empty() || dir_arg.empty()) {
        throw std::runtime_error("verify_archive_vs_dir requiere zip y target_dir");
    }

    fs::path base;
    if (p.config.contains("roots") && p.config["roots"].is_array() && !p.config["roots"].empty()) {
        base = p.resolve(p.config["roots"][0].at("path").get<std::string>());
    }
    const fs::path zip_path = resolve_authorized_org_path(p, base, zip_arg, "zip", false);
    const fs::path target_dir = resolve_authorized_org_path(p, base, dir_arg, "target_dir", true);
    if (!win32_exists(zip_path)) throw std::runtime_error("ZIP no encontrado: " + path_text(zip_path));
    if (!win32_is_dir(target_dir)) throw std::runtime_error("Directorio destino no encontrado: " + path_text(target_dir));

    std::vector<std::string> ignore_suffixes = {".git/index"};
    if (args.contains("ignore_suffixes") && args["ignore_suffixes"].is_array()) {
        ignore_suffixes.clear();
        for (const auto& s : args["ignore_suffixes"]) {
            if (s.is_string()) ignore_suffixes.push_back(lower_ascii(norm_rel_fwd(s.get<std::string>())));
        }
    }
    const size_t sample_limit = static_cast<size_t>(std::clamp(args.value("sample_limit", 20), 1, 200));

    LibZipDyn z;
    zip_error_t ze{};
    z.error_init(&ze);
    struct ErrGuard { const LibZipDyn& api; zip_error_t& e; ~ErrGuard() { api.error_fini(&e); } } eg{z, ze};

    const auto nzip = native_path(zip_path);
    zip_source_t* src = z.source_win32w_create(nzip.c_str(), 0, -1, &ze);
    if (!src) throw std::runtime_error(std::string("No se pudo abrir fuente ZIP: ") + z.error_strerror(&ze));
    zip_t* za = z.open_from_source(src, ZIP_RDONLY, &ze);
    if (!za) {
        z.source_free(src);
        throw std::runtime_error(std::string("No se pudo abrir archivo ZIP: ") + z.error_strerror(&ze));
    }
    struct ZipGuard { const LibZipDyn& api; zip_t* a; ~ZipGuard() { if (a) api.discard(a); } } zg{z, za};

    const zip_int64_t num = z.get_num_entries(za, 0);
    int64_t total_files = 0, matched = 0, ignored = 0, missing = 0, size_mm = 0, crc_mm = 0;
    Json missing_sample = Json::array(), mismatch_sample = Json::array();

    for (zip_int64_t i = 0; i < num; ++i) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
        zip_stat_t st{};
        if (z.stat_index(za, static_cast<zip_uint64_t>(i), 0, &st) != 0 || !st.name) continue;
        std::string ename = norm_rel_fwd(st.name);
        if (ename.empty() || std::string_view(st.name).ends_with('/')) continue;
        if (has_traversal_or_abs_segment(ename)) {
            throw std::runtime_error("SAFETY_VIOLATION: entrada ZIP con path traversal (Zip-Slip): " + ename);
        }
        ++total_files;

        const std::string low_e = lower_ascii(ename);
        bool skip = false;
        for (const auto& sfx : ignore_suffixes) {
            if (!sfx.empty() && low_e.ends_with(sfx)) { skip = true; break; }
        }
        if (skip) { ++ignored; continue; }

        const fs::path cand = resolve_confined_under(target_dir, ename, "zip_entry");
        if (!win32_exists(cand) || win32_is_dir(cand)) {
            ++missing;
            if (missing_sample.size() < sample_limit) missing_sample.push_back(ename);
            continue;
        }
        uint64_t disk_sz = 0;
        uint32_t disk_crc = 0;
        if (!crc32_file(cand, &disk_sz, &disk_crc, cancel)) {
            ++missing;
            if (missing_sample.size() < sample_limit) missing_sample.push_back(ename);
            continue;
        }
        if (disk_sz != st.size) {
            ++size_mm;
            if (mismatch_sample.size() < sample_limit) {
                mismatch_sample.push_back({{"entry", ename}, {"kind", "size"}, {"zip_size", st.size}, {"disk_size", disk_sz}});
            }
            continue;
        }
        if (disk_crc != st.crc) {
            ++crc_mm;
            if (mismatch_sample.size() < sample_limit) {
                mismatch_sample.push_back({{"entry", ename}, {"kind", "crc32"}, {"zip_crc", st.crc}, {"disk_crc", disk_crc}});
            }
            continue;
        }
        ++matched;
    }

    const bool exact_match = (missing == 0 && size_mm == 0 && crc_mm == 0 && matched > 0);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return {
        {"schema", "migratool.verify_archive_vs_dir/1"},
        {"exact_match", exact_match},
        {"zip", path_text(zip_path)},
        {"target_dir", path_text(target_dir)},
        {"total_files", total_files},
        {"matched_crc", matched},
        {"ignored", ignored},
        {"missing", missing},
        {"size_mismatch", size_mm},
        {"crc_mismatch", crc_mm},
        {"missing_sample", std::move(missing_sample)},
        {"mismatch_sample", std::move(mismatch_sample)},
        {"elapsed_ms", elapsed_ms}
    };
}

// ============================================================================
// 3. tool_inspect_workspace_topology
// ============================================================================
Json tool_inspect_workspace_topology(const Project& p, Database& db, const Json& args, Cancel* cancel) {
    fs_schema(db);
    auto t0 = std::chrono::steady_clock::now();
    const std::string filter_root = args.value("root", "");
    const bool scan_path_refs = args.value("scan_path_refs", true);
    const bool inspect_git = args.value("inspect_git", true);

    auto roots = load_fs_roots(p, db, filter_root);
    if (roots.empty()) {
        return {{"schema", "migratool.workspace_topology/1"}, {"roots", Json::array()}};
    }

    Json out_roots = Json::array();
    for (const auto& r : roots) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");

        // 1. NTFS junctions / symlinks
        Json links = Json::array();
        db.each("SELECT n.name, n.kind, n.link_target, COALESCE(d.rel_path,'') AS prel "
                "FROM fs_nodes n LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn "
                "WHERE n.root_id=? AND n.kind IN (2,3,4) AND (n.flags & 1)=0",
                {r.root_id}, [&](const Json& row) {
                    links.push_back({
                        {"path", join_rel(row["prel"].get<std::string>(), row["name"].get<std::string>())},
                        {"kind", row["kind"].get<int>() == 4 ? "junction" : (row["kind"].get<int>() == 3 ? "symlink_dir" : "symlink_file")},
                        {"target", row["link_target"]}
                    });
                    return true;
                });

        // 2. OneDrive cloud-only placeholders (0 disk I/O!)
        auto cloud_row = db.query(
            "SELECT COUNT(*) AS c, COALESCE(SUM(size_bytes),0) AS b FROM fs_nodes "
            "WHERE root_id=? AND kind=0 AND (flags & 1)=0 AND (attrs & ?)<>0",
            {r.root_id, static_cast<int64_t>(kCloudOfflineAttrs)});
        const int64_t cloud_files = cloud_row.empty() ? 0 : cloud_row[0]["c"].get<int64_t>();
        const int64_t cloud_bytes = cloud_row.empty() ? 0 : cloud_row[0]["b"].get<int64_t>();

        // 3. Git repositories & linked/broken worktrees
        std::vector<std::string> repo_rels;
        db.each("SELECT COALESCE(d.rel_path,'') AS prel FROM fs_nodes n "
                "LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn "
                "WHERE n.root_id=? AND lower(n.name)='.git' AND (n.flags & 1)=0",
                {r.root_id}, [&](const Json& row) {
                    repo_rels.push_back(row["prel"].get<std::string>());
                    return true;
                });
        std::sort(repo_rels.begin(), repo_rels.end());
        repo_rels.erase(std::unique(repo_rels.begin(), repo_rels.end()), repo_rels.end());

        Json git_repos = Json::array();
        std::unordered_map<std::string, std::unordered_set<std::string>> repo_tracked_cache;
        std::unordered_map<std::string, bool> repo_verified_map;
        for (const auto& rrel : repo_rels) {
            if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
            fs::path rp = rrel.empty() ? r.abs_path : resolve_confined_under(r.abs_path, rrel, "git_repo");
            const std::string rlow = lower_ascii(rrel);
            if (inspect_git) {
                auto snap = snapshot_single_git_repo(rp, rrel.empty() ? "." : rrel);
                bool ls_ok = false;
                auto tracked = git_tracked_files_rel(rp, &ls_ok);
                const bool verified = snap.value("git_verified", false) && ls_ok;
                repo_verified_map[rlow] = verified;
                if (ls_ok) {
                    repo_tracked_cache[rlow] = std::move(tracked);
                }
                git_repos.push_back(std::move(snap));
            } else {
                repo_verified_map[rlow] = false;
                git_repos.push_back({{"rel", rrel.empty() ? "." : rrel}, {"git_verified", false}});
            }
        }

        // 4. Regenerable build/cache dirs annotated with git tracked/ignored status (fail-closed!)
        struct RegenDir { std::string rel; int64_t files; int64_t bytes; };
        std::vector<RegenDir> regen_all;
        db.each("SELECT rel_path, total_files, total_bytes FROM fs_dirs WHERE root_id=? AND rel_path<>'' ORDER BY depth ASC, total_bytes DESC",
                {r.root_id}, [&](const Json& row) {
                    const std::string rp = row["rel_path"].get<std::string>();
                    if (rp.find("/.git") != std::string::npos || lower_ascii(rp).starts_with(".git")) return true;
                    auto sl = rp.rfind('/');
                    const std::string bname = lower_ascii((sl == std::string::npos) ? rp : rp.substr(sl + 1));
                    if (is_regenerable_dir_name(bname)) {
                        regen_all.push_back(RegenDir{rp, row["total_files"].get<int64_t>(), row["total_bytes"].get<int64_t>()});
                    }
                    return true;
                });
        // Keep outermost regenerable dirs (avoid double-counting build/sub/obj)
        std::vector<std::string> accepted_prefixes;
        std::vector<RegenDir> regen_top;
        for (const auto& rd : regen_all) {
            const std::string low = lower_ascii(rd.rel);
            bool nested = false;
            for (const auto& pfx : accepted_prefixes) {
                if (low.starts_with(pfx)) { nested = true; break; }
            }
            if (!nested) {
                accepted_prefixes.push_back(low + "/");
                regen_top.push_back(rd);
            }
        }
        std::sort(regen_top.begin(), regen_top.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; });

        Json regen_json = Json::array();
        int64_t safe_regen_files = 0, safe_regen_bytes = 0;
        for (size_t i = 0; i < std::min<size_t>(regen_top.size(), 60); ++i) {
            const auto& rd = regen_top[i];
            // Find deepest containing git repo (including unverified repos so we fail-closed!)
            std::string owner_repo;
            bool has_owner_repo = false;
            std::string rel_in_repo = rd.rel;
            const std::string rd_low = lower_ascii(rd.rel);
            for (const auto& rrel : repo_rels) {
                const std::string rlow = lower_ascii(rrel);
                if (rlow.empty()) {
                    if (!has_owner_repo) {
                        has_owner_repo = true;
                        owner_repo = "";
                        rel_in_repo = rd.rel;
                    }
                } else if (rd_low == rlow || rd_low.starts_with(rlow + "/")) {
                    if (!has_owner_repo || rlow.size() >= owner_repo.size()) {
                        has_owner_repo = true;
                        owner_repo = rrel;
                        rel_in_repo = rd.rel.substr(rrel.size() + (rd.rel.size() > rrel.size() ? 1 : 0));
                    }
                }
            }
            int64_t tracked_under = 0;
            bool git_ign = false;
            const bool in_git = has_owner_repo;
            const std::string owner_low = lower_ascii(owner_repo);
            const bool git_verified = !in_git || (inspect_git && repo_verified_map[owner_low]);
            if (in_git && git_verified && repo_tracked_cache.contains(owner_low)) {
                const auto& tset = repo_tracked_cache[owner_low];
                const std::string pfx = lower_ascii(rel_in_repo) + "/";
                for (const auto& tf : tset) {
                    if (tf == lower_ascii(rel_in_repo) || tf.starts_with(pfx)) ++tracked_under;
                }
                if (tracked_under == 0) {
                    fs::path owner_p = owner_repo.empty() ? r.abs_path : resolve_confined_under(r.abs_path, owner_repo, "git_repo");
                    git_ign = git_is_path_ignored(owner_p, rel_in_repo);
                }
            }
            // Fail-closed: if inside a Git repo, it is ONLY safe if Git was verified AND 0 tracked files AND git_ignored
            const bool safe_q = (tracked_under == 0) && (!in_git || (git_verified && git_ign));
            if (safe_q) { safe_regen_files += rd.files; safe_regen_bytes += rd.bytes; }
            regen_json.push_back({
                {"path", rd.rel},
                {"files", rd.files},
                {"bytes", rd.bytes},
                {"in_git_repo", in_git ? (owner_repo.empty() ? "." : owner_repo) : ""},
                {"git_verified", git_verified},
                {"git_tracked_files", tracked_under},
                {"git_ignored", git_ign},
                {"safe_to_quarantine", safe_q}
            });
        }

        // 5. Hardcoded absolute root-path references scan (fast bounded scan over small text files)
        Json abs_refs = Json::object();
        int64_t abs_ref_files_total = 0;
        if (scan_path_refs) {
            static const std::unordered_set<std::string> text_exts = {
                ".py", ".ps1", ".cmd", ".bat", ".json", ".jsonc", ".md", ".txt", ".cmake",
                ".yaml", ".yml", ".toml", ".ini", ".vcxproj", ".props", ".sln", ".dart",
                ".cpp", ".hpp", ".h", ".c", ".cs", ".js", ".ts", ".xml", ".cfg"
            };
            const std::string root_leaf = lower_ascii(utf8(r.abs_path.filename().wstring()));
            std::map<std::string, int64_t> refs_by_top;
            if (!root_leaf.empty()) {
                db.each("SELECT n.name, n.size_bytes, COALESCE(d.rel_path,'') AS prel "
                        "FROM fs_nodes n LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn "
                        "WHERE n.root_id=? AND n.kind=0 AND (n.flags & 1)=0 AND n.size_bytes>0 AND n.size_bytes<=1048576",
                        {r.root_id}, [&](const Json& row) {
                            if (cancel && cancel->requested) return false;
                            const std::string nm = row["name"].get<std::string>();
                            auto dot = nm.rfind('.');
                            const std::string ext = (dot == std::string::npos) ? "" : lower_ascii(nm.substr(dot));
                            if (!text_exts.contains(ext) && lower_ascii(nm) != "cmakelists.txt") return true;
                            const std::string rel = join_rel(row["prel"].get<std::string>(), nm);
                            if (rel.find("/.git/") != std::string::npos || lower_ascii(rel).starts_with(".git/")) return true;
                            std::wstring wrel = wide(rel);
                            for (auto& c : wrel) if (c == L'/') c = L'\\';
                            try {
                                std::string content = read_text(r.abs_path / fs::path(wrel), 1048576);
                                if (lower_ascii(content).find(root_leaf) != std::string::npos) {
                                    ++abs_ref_files_total;
                                    refs_by_top[top_segment(rel)] += 1;
                                }
                            } catch (...) {}
                            return true;
                        });
            }
            for (const auto& [k, v] : refs_by_top) abs_refs[k] = v;
        }

        out_roots.push_back({
            {"root", r.root_key},
            {"abs_path", path_text(r.abs_path)},
            {"ntfs_links_count", links.size()},
            {"ntfs_links", std::move(links)},
            {"cloud_only_files", cloud_files},
            {"cloud_only_bytes", cloud_bytes},
            {"git_repos_count", git_repos.size()},
            {"git_repos", std::move(git_repos)},
            {"regenerable_dirs_safe_files", safe_regen_files},
            {"regenerable_dirs_safe_bytes", safe_regen_bytes},
            {"regenerable_dirs", std::move(regen_json)},
            {"hardcoded_root_ref_files", abs_ref_files_total},
            {"hardcoded_root_refs_by_top", std::move(abs_refs)}
        });
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return {
        {"schema", "migratool.workspace_topology/1"},
        {"elapsed_ms", elapsed_ms},
        {"roots", std::move(out_roots)}
    };
}

// ============================================================================
// 4. tool_organize_plan (Dry-run validator + manifest generator)
// ============================================================================
Json tool_organize_plan(const Project& p, Database& db, const Json& args, Cancel* cancel) {
    fs_schema(db);
    const std::string req_root = args.value("root", "");
    auto roots = load_fs_roots(p, db, req_root);
    if (roots.empty()) throw std::runtime_error("No hay raíces configuradas en el proyecto");
    const RootRow& root = roots[0];

    if (!args.contains("operations") || !args["operations"].is_array() || args["operations"].empty()) {
        throw std::runtime_error("organize_plan requiere arreglo operations no vacío");
    }

    const std::string plan_id = args.value("plan_id", "plan_" + unique_id().substr(0, 12));
    validate_safe_id(plan_id, "plan_id");
    const fs::path plan_dir = p.state_dir() / L"organize" / wide(plan_id);

    const std::string q_arg = args.value("quarantine_dir", "");
    fs::path quarantine_base;
    if (q_arg.empty()) {
        quarantine_base = plan_dir / L"quarantine";
    } else {
        fs::path qp(wide(q_arg));
        for (const auto& part : qp) {
            if (part == L"..") {
                throw std::runtime_error("SAFETY_VIOLATION: quarantine_dir contiene '..': " + q_arg);
            }
        }
        if (qp.is_relative()) {
            quarantine_base = resolve_confined_under(plan_dir, q_arg, "quarantine_dir");
        } else {
            quarantine_base = qp;
        }
    }
    if (is_confined_within(root.abs_path, quarantine_base, true) || is_confined_within(quarantine_base, root.abs_path, true)) {
        throw std::runtime_error("SAFETY_VIOLATION: quarantine_dir no puede solaparse con la raíz del proyecto");
    }
    const fs::path q_ret = quarantine_base / L"retirados";

    win32_ensure_dir(plan_dir);

    // 1. Fresh Win32 safe baseline walk (never crosses junctions!)
    auto inv = win32_safe_walk(root.abs_path, cancel);
    int64_t base_bytes = 0;
    for (const auto& f : inv.files) base_bytes += f.size_bytes;

    std::unordered_set<std::string> junc_low;
    for (const auto& j : inv.junctions) junc_low.insert(lower_ascii(j));

    // 2. Discover git repos + build protected tracked-file set (fail-closed if Git fails!)
    std::vector<std::string> repo_rels;
    for (const auto& d : inv.dirs) {
        auto sl = d.rfind('/');
        std::string bn = lower_ascii((sl == std::string::npos) ? d : d.substr(sl + 1));
        if (bn == ".git") repo_rels.push_back((sl == std::string::npos) ? "" : d.substr(0, sl));
    }
    for (const auto& f : inv.files) {
        auto sl = f.rel_fwd.rfind('/');
        std::string bn = lower_ascii((sl == std::string::npos) ? f.rel_fwd : f.rel_fwd.substr(sl + 1));
        if (bn == ".git") repo_rels.push_back((sl == std::string::npos) ? "" : f.rel_fwd.substr(0, sl));
    }
    std::sort(repo_rels.begin(), repo_rels.end());
    repo_rels.erase(std::unique(repo_rels.begin(), repo_rels.end()), repo_rels.end());

    Json baseline_git = Json::object();
    std::unordered_set<std::string> all_tracked_low;
    std::unordered_set<std::string> unverified_git_repos;
    for (const auto& rrel : repo_rels) {
        fs::path rp = rrel.empty() ? root.abs_path : resolve_confined_under(root.abs_path, rrel, "git_repo");
        auto snap = snapshot_single_git_repo(rp, rrel.empty() ? "." : rrel);
        bool ls_ok = false;
        auto tracked = git_tracked_files_rel(rp, &ls_ok);
        if (ls_ok) {
            for (const auto& tf : tracked) {
                all_tracked_low.insert(lower_ascii(join_rel(rrel, tf)));
            }
        }
        if (!snap.value("git_verified", false) || !ls_ok) {
            unverified_git_repos.insert(lower_ascii(rrel));
        }
        baseline_git[rrel.empty() ? "." : rrel] = std::move(snap);
    }

    std::unordered_set<std::string> protected_low;
    if (args.contains("protected_paths") && args["protected_paths"].is_array()) {
        for (const auto& pp : args["protected_paths"]) {
            if (pp.is_string()) protected_low.insert(lower_ascii(norm_rel_fwd(pp.get<std::string>())));
        }
    }
    const bool allow_git_tracked = args.value("allow_git_tracked", false);

    auto is_protected = [&](const std::string& rel_low) -> bool {
        for (const auto& pp : protected_low) {
            if (rel_low == pp || rel_low.starts_with(pp + "/")) return true;
        }
        return false;
    };
    auto touches_tracked = [&](const std::string& rel_low, bool is_dir) -> std::string {
        if (all_tracked_low.contains(rel_low)) return rel_low;
        if (is_dir) {
            const std::string pfx = rel_low + "/";
            for (const auto& tf : all_tracked_low) {
                if (tf.starts_with(pfx)) return tf;
            }
        }
        return {};
    };
    auto touches_unverified_git = [&](const std::string& rel_low, bool is_dir) -> std::string {
        for (const auto& urepo : unverified_git_repos) {
            if (urepo.empty()) return ".";
            if (rel_low == urepo || rel_low.starts_with(urepo + "/")) return urepo;
            if (is_dir && urepo.starts_with(rel_low + "/")) return urepo;
        }
        return {};
    };
    auto touches_junction = [&](const std::string& rel_low, bool is_dir) -> std::string {
        if (junc_low.contains(rel_low)) return rel_low;
        if (is_dir) {
            const std::string pfx = rel_low + "/";
            for (const auto& j : junc_low) {
                if (j.starts_with(pfx)) return j;
            }
        }
        return {};
    };

    // 3. Validate & materialize every operation with strict path confinement
    std::vector<Json> plan_ops;
    std::unordered_set<std::string> seen_dsts;
    std::map<std::string, std::tuple<int64_t, int64_t, int64_t>> by_cat; // ops, files, bytes
    int64_t seq = 0, q_files = 0, q_bytes = 0;

    for (const auto& raw_op : args["operations"]) {
        if (!raw_op.is_object()) throw std::runtime_error("Cada operación debe ser un objeto JSON");
        const std::string op = raw_op.at("op").get<std::string>();
        const std::string cat = raw_op.value("category", op);
        validate_safe_id(cat, "category");
        const std::string reason = raw_op.value("reason", "");

        Json norm_op = {
            {"seq", ++seq},
            {"op", op},
            {"category", cat},
            {"reason", reason}
        };

        if (op == "quarantine_dir" || op == "quarantine_file" || op == "move_dir" || op == "move_file") {
            const std::string src_rel = validate_rel_no_traversal(raw_op.at("src").get<std::string>(), "src");
            const std::string src_low = lower_ascii(src_rel);
            const bool is_dir = (op == "quarantine_dir" || op == "move_dir");
            const bool is_q = (op == "quarantine_dir" || op == "quarantine_file");
            if (is_protected(src_low)) {
                throw std::runtime_error("SAFETY_VIOLATION: ruta protegida en operación: " + src_rel);
            }
            if (auto jhit = touches_junction(src_low, is_dir); !jhit.empty()) {
                throw std::runtime_error("SAFETY_VIOLATION: la operación toca junction/symlink NTFS: " + jhit);
            }
            if (is_q && !allow_git_tracked) {
                if (auto uhit = touches_unverified_git(src_low, is_dir); !uhit.empty()) {
                    throw std::runtime_error("SAFETY_VIOLATION: repositorio Git no verificable (fail-closed) en cuarentena: " + uhit);
                }
                if (auto thit = touches_tracked(src_low, is_dir); !thit.empty()) {
                    throw std::runtime_error("SAFETY_VIOLATION: archivo rastreado por Git en cuarentena: " + thit);
                }
            }
            const fs::path src_abs = resolve_confined_under(root.abs_path, src_rel, "src");
            if (!win32_exists(src_abs)) {
                throw std::runtime_error("Origen no existe en disco: " + src_rel);
            }
            if (is_dir && !win32_is_dir(src_abs)) throw std::runtime_error("Se esperaba directorio: " + src_rel);
            if (!is_dir && win32_is_dir(src_abs)) throw std::runtime_error("Se esperaba archivo: " + src_rel);

            fs::path dst_abs;
            std::string dst_rel;
            if (is_q) {
                dst_rel = raw_op.contains("dst_rel")
                    ? validate_rel_no_traversal(raw_op["dst_rel"].get<std::string>(), "dst_rel")
                    : src_rel;
                dst_abs = resolve_confined_under(q_ret / wide(cat), dst_rel, "dst_rel");
            } else {
                dst_rel = validate_rel_no_traversal(raw_op.at("dst").get<std::string>(), "dst");
                dst_abs = resolve_confined_under(root.abs_path, dst_rel, "dst");
                if (win32_exists(dst_abs)) {
                    throw std::runtime_error("COLLISION: el destino ya existe en disco: " + dst_rel);
                }
            }
            const std::string dst_key = lower_ascii(path_text(dst_abs));
            if (!seen_dsts.insert(dst_key).second) {
                throw std::runtime_error("COLLISION: dos operaciones comparten destino: " + path_text(dst_abs));
            }

            int64_t fcnt = 0, fbytes = 0;
            if (is_dir) {
                const std::string pfx = src_low + "/";
                for (const auto& f : inv.files) {
                    if (lower_ascii(f.rel_fwd).starts_with(pfx)) { ++fcnt; fbytes += f.size_bytes; }
                }
            } else {
                fcnt = 1;
                std::error_code ec;
                fbytes = static_cast<int64_t>(fs::file_size(native_path(src_abs), ec));
            }
            norm_op["src_rel"] = src_rel;
            norm_op["dst_rel"] = dst_rel;
            norm_op["src"] = path_text(src_abs);
            norm_op["dst"] = path_text(dst_abs);
            norm_op["files"] = fcnt;
            norm_op["bytes"] = fbytes;
            if (is_q) { q_files += fcnt; q_bytes += fbytes; }
            auto& acc = by_cat[cat];
            std::get<0>(acc) += 1; std::get<1>(acc) += fcnt; std::get<2>(acc) += fbytes;
        } else if (op == "rmdir_empty_tree" || op == "prune_empty_subdirs") {
            const std::string src_rel = validate_rel_no_traversal(raw_op.at("src").get<std::string>(), "src");
            if (is_protected(lower_ascii(src_rel))) {
                throw std::runtime_error("SAFETY_VIOLATION: ruta protegida: " + src_rel);
            }
            norm_op["src_rel"] = src_rel;
            norm_op["src"] = path_text(resolve_confined_under(root.abs_path, src_rel, "src"));
            std::get<0>(by_cat[cat]) += 1;
        } else if (op == "edit_text_paths") {
            const std::string target_rel = validate_rel_no_traversal(raw_op.at("target").get<std::string>(), "target");
            const std::string tlow = lower_ascii(target_rel);
            if (is_protected(tlow)) throw std::runtime_error("SAFETY_VIOLATION: ruta protegida: " + target_rel);
            if (!allow_git_tracked) {
                if (auto uhit = touches_unverified_git(tlow, false); !uhit.empty()) {
                    throw std::runtime_error("SAFETY_VIOLATION: repositorio Git no verificable (fail-closed) en edit_text_paths: " + uhit);
                }
                if (all_tracked_low.contains(tlow)) {
                    throw std::runtime_error("SAFETY_VIOLATION: archivo rastreado por Git en edit_text_paths: " + target_rel);
                }
            }
            if (!raw_op.contains("replacements") || !raw_op["replacements"].is_array()) {
                throw std::runtime_error("edit_text_paths requiere arreglo replacements [[old,new],...]");
            }
            norm_op["target_rel"] = target_rel;
            norm_op["target"] = path_text(resolve_confined_under(root.abs_path, target_rel, "target"));
            norm_op["replacements"] = raw_op["replacements"];
            std::get<0>(by_cat[cat]) += 1;
        } else {
            throw std::runtime_error("Operación organize desconocida: " + op);
        }
        plan_ops.push_back(std::move(norm_op));
    }

    // 4. Write baseline_files.jsonl, baseline_git.json, plan.jsonl, plan_summary.json
    const bool hash_baseline = args.value("hash_baseline", inv.files.size() <= 500);
    {
        std::ofstream bf(native_path(plan_dir / L"baseline_files.jsonl"), std::ios::binary);
        for (const auto& f : inv.files) {
            Json row = {{"rel", f.rel_fwd}, {"size", f.size_bytes}, {"mtime_ns", f.mtime_ns}, {"attrs", f.attrs}};
            if (hash_baseline) {
                std::wstring wrel = wide(f.rel_fwd);
                for (auto& c : wrel) if (c == L'/') c = L'\\';
                row["sha256"] = sha256_file_bounded(root.abs_path / fs::path(wrel), 0, cancel);
            }
            bf << row.dump() << '\n';
        }
    }
    write_json(plan_dir / L"baseline_git.json", baseline_git);
    {
        std::ofstream pf(native_path(plan_dir / L"plan.jsonl"), std::ios::binary);
        for (const auto& op : plan_ops) pf << op.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
    }

    Json cats = Json::object();
    for (const auto& [k, v] : by_cat) {
        cats[k] = {{"ops", std::get<0>(v)}, {"files", std::get<1>(v)}, {"bytes", std::get<2>(v)}};
    }
    Json summary = {
        {"schema", "migratool.organize_plan/1"},
        {"plan_id", plan_id},
        {"plan_dir", path_text(plan_dir)},
        {"root_key", root.root_key},
        {"root_path", path_text(root.abs_path)},
        {"quarantine_dir", path_text(quarantine_base)},
        {"baseline", {{"files", inv.files.size()}, {"dirs", inv.dirs.size()}, {"junctions", inv.junctions.size()}, {"bytes", base_bytes}, {"hashed", hash_baseline}}},
        {"total_ops", plan_ops.size()},
        {"quarantine_files", q_files},
        {"quarantine_bytes", q_bytes},
        {"by_category", std::move(cats)},
        {"safety_checks_passed", true}
    };
    write_json(plan_dir / L"plan_summary.json", summary);
    return summary;
}

namespace {

// Reconcile two-phase WAL journal (intent + committed) so interrupted operations are deterministically recovered
std::vector<Json> load_reconciled_journal(const fs::path& journal_path,
                                          const fs::path& manifest_path = {},
                                          bool write_back_reconciled = false) {
    std::vector<Json> out;
    if (!win32_exists(journal_path)) return out;

    std::vector<int64_t> seq_order;
    std::unordered_map<int64_t, Json> committed_by_seq;
    std::unordered_map<int64_t, Json> intent_by_seq;
    {
        std::ifstream jf(native_path(journal_path), std::ios::binary);
        std::string line;
        while (std::getline(jf, line)) {
            if (line.empty()) continue;
            auto j = Json::parse(line, nullptr, false);
            if (!j.is_object() || !j.contains("seq")) continue;
            const int64_t seq = j["seq"].get<int64_t>();
            if (std::find(seq_order.begin(), seq_order.end(), seq) == seq_order.end()) {
                seq_order.push_back(seq);
            }
            const std::string phase = j.value("phase", "committed");
            if (phase == "intent") {
                intent_by_seq[seq] = std::move(j);
            } else {
                committed_by_seq[seq] = std::move(j);
            }
        }
    }

    std::unordered_set<std::string> manifest_origs;
    if (!manifest_path.empty() && win32_exists(manifest_path)) {
        std::ifstream mf(native_path(manifest_path), std::ios::binary);
        std::string line;
        while (std::getline(mf, line)) {
            if (line.empty()) continue;
            auto m = Json::parse(line, nullptr, false);
            if (m.is_object() && m.contains("orig_path")) {
                manifest_origs.insert(lower_ascii(m["orig_path"].get<std::string>()));
            }
        }
    }

    for (int64_t seq : seq_order) {
        if (auto cit = committed_by_seq.find(seq); cit != committed_by_seq.end()) {
            out.push_back(cit->second);
            continue;
        }
        auto iit = intent_by_seq.find(seq);
        if (iit == intent_by_seq.end()) continue;
        const Json& intent = iit->second;
        const std::string op = intent.value("op", "");
        bool completed_on_disk = false;
        if (op == "move") {
            const fs::path src(wide(intent.at("src").get<std::string>()));
            const fs::path dst(wide(intent.at("dst").get<std::string>()));
            if (win32_exists(dst)) {
                if (write_back_reconciled && win32_exists(src)) {
                    win32_remove_tree_no_reparse(src);
                }
                if (!win32_exists(src)) {
                    completed_on_disk = true;
                }
            }
        } else if (op == "edit_text") {
            const fs::path target(wide(intent.at("target").get<std::string>()));
            if (win32_exists(target)) {
                try {
                    if (sha256(read_text(target)) == intent.value("new_sha256", "")) {
                        completed_on_disk = true;
                    }
                } catch (...) {}
            }
        } else if (op == "rmdir_list") {
            completed_on_disk = true;
        }

        if (completed_on_disk) {
            Json rec = intent;
            rec["phase"] = "committed";
            if (write_back_reconciled) {
                if (!manifest_path.empty() && rec.contains("manifest_entries") && rec["manifest_entries"].is_array()) {
                    std::ofstream mfh(native_path(manifest_path), std::ios::binary | std::ios::app);
                    for (const auto& me : rec["manifest_entries"]) {
                        const std::string ok = lower_ascii(me.value("orig_path", ""));
                        if (!ok.empty() && !manifest_origs.contains(ok)) {
                            mfh << me.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
                            manifest_origs.insert(ok);
                        }
                    }
                    mfh.flush();
                }
                rec.erase("manifest_entries");
                std::ofstream jfh(native_path(journal_path), std::ios::binary | std::ios::app);
                jfh << rec.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
                jfh.flush();
            } else {
                rec.erase("manifest_entries");
            }
            out.push_back(std::move(rec));
        }
    }
    return out;
}

} // namespace

// ============================================================================
// 5. tool_organize_apply (Transactional executor + SHA-256 manifest + WAL journal)
// ============================================================================
Json tool_organize_apply(const Project& p, Database& db, const Json& args, Log log, Cancel* cancel) {
    const std::string plan_id = args.at("plan_id").get<std::string>();
    validate_safe_id(plan_id, "plan_id");
    const fs::path plan_dir = p.state_dir() / L"organize" / wide(plan_id);
    const fs::path summary_path = plan_dir / L"plan_summary.json";
    if (!win32_exists(summary_path)) throw std::runtime_error("Plan no encontrado: " + plan_id);

    Json summary = read_json(summary_path);
    if (!args.value("execute", false)) {
        summary["dry_run"] = true;
        summary["hint"] = "Pase execute:true para ejecutar las operaciones de forma transaccional.";
        return summary;
    }

    const bool resume = args.value("resume", false);
    const int64_t simulate_crash_after_move_seq = args.value("simulate_crash_after_move_seq", int64_t(-1));
    const fs::path root_path(wide(summary.at("root_path").get<std::string>()));
    const fs::path quarantine_base(wide(summary.at("quarantine_dir").get<std::string>()));
    const fs::path q_ret = quarantine_base / L"retirados";
    const fs::path q_meta = quarantine_base / L"_meta";
    const fs::path backups_dir = q_meta / L"pre_edit_backups";
    win32_ensure_dir(q_ret);
    win32_ensure_dir(q_meta);
    win32_ensure_dir(backups_dir);

    const fs::path journal_path = plan_dir / L"journal.jsonl";
    const fs::path manifest_path = plan_dir / L"quarantine_manifest.jsonl";
    if (!resume && win32_exists(journal_path)) {
        throw std::runtime_error("El plan ya tiene journal.jsonl; use resume:true o genere un nuevo plan_id");
    }

    std::unordered_set<int64_t> done_seqs;
    int64_t q_files = 0, q_bytes = 0, edits_done = 0, edit_byte_delta = 0;
    if (resume && win32_exists(journal_path)) {
        auto reconciled = load_reconciled_journal(journal_path, manifest_path, true);
        for (const auto& j : reconciled) {
            done_seqs.insert(j["seq"].get<int64_t>());
            if (j["op"] == "edit_text") {
                ++edits_done;
                edit_byte_delta += (j["new_bytes"].get<int64_t>() - j["old_bytes"].get<int64_t>());
            }
        }
        if (win32_exists(manifest_path)) {
            std::ifstream mf(native_path(manifest_path), std::ios::binary);
            std::string line;
            while (std::getline(mf, line)) {
                if (line.empty()) continue;
                auto m = Json::parse(line);
                ++q_files;
                q_bytes += m["size"].get<int64_t>();
            }
        }
    }

    std::vector<Json> ops;
    {
        std::ifstream pf(native_path(plan_dir / L"plan.jsonl"), std::ios::binary);
        std::string line;
        while (std::getline(pf, line)) {
            if (!line.empty()) ops.push_back(Json::parse(line));
        }
    }

    auto mode = resume ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc);
    std::ofstream jfh(native_path(journal_path), mode);
    std::ofstream mfh(native_path(manifest_path), mode);

    int64_t executed_now = 0;
    for (const auto& op : ops) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
        const int64_t seq = op.at("seq").get<int64_t>();
        if (done_seqs.contains(seq)) continue;
        const std::string kind = op.at("op").get<std::string>();

        if (kind == "quarantine_dir") {
            const fs::path src(wide(op.at("src").get<std::string>()));
            const fs::path dst(wide(op.at("dst").get<std::string>()));
            if (!is_confined_within(src, root_path, false) || !is_confined_within(dst, quarantine_base, false)) {
                throw std::runtime_error("SAFETY_ABORT: ruta fuera de confinamiento en quarantine_dir");
            }
            auto sub = win32_safe_walk(src, cancel);
            if (!sub.junctions.empty()) {
                throw std::runtime_error("SAFETY_ABORT: junction detectada dentro de " + path_text(src));
            }
            Json pending_manifest = Json::array();
            int64_t op_q_files = 0, op_q_bytes = 0;
            for (const auto& f : sub.files) {
                std::wstring wrel = wide(f.rel_fwd);
                for (auto& c : wrel) if (c == L'/') c = L'\\';
                const fs::path sf = src / fs::path(wrel);
                const fs::path df = dst / fs::path(wrel);
                const std::string h = sha256_file_bounded(sf, 0, cancel);
                pending_manifest.push_back({
                    {"category", op["category"]},
                    {"orig_path", path_text(sf)},
                    {"quarantine_path", path_text(df)},
                    {"size", f.size_bytes},
                    {"sha256", h}
                });
                ++op_q_files;
                op_q_bytes += f.size_bytes;
            }
            auto created = win32_ensure_dir(dst.parent_path());
            jfh << Json{
                {"seq", seq}, {"phase", "intent"}, {"op", "move"},
                {"src", path_text(src)}, {"dst", path_text(dst)},
                {"created_dirs", created}, {"manifest_entries", pending_manifest}
            }.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            jfh.flush();

            win32_move_no_overwrite(src, dst);
            if (simulate_crash_after_move_seq == seq) {
                throw std::runtime_error("SIMULATED_CRASH_AFTER_MOVE");
            }

            for (const auto& me : pending_manifest) {
                mfh << me.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            }
            mfh.flush();
            q_files += op_q_files;
            q_bytes += op_q_bytes;

            jfh << Json{
                {"seq", seq}, {"phase", "committed"}, {"op", "move"},
                {"src", path_text(src)}, {"dst", path_text(dst)},
                {"created_dirs", created}
            }.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            jfh.flush();
        } else if (kind == "quarantine_file") {
            const fs::path src(wide(op.at("src").get<std::string>()));
            const fs::path dst(wide(op.at("dst").get<std::string>()));
            if (!is_confined_within(src, root_path, false) || !is_confined_within(dst, quarantine_base, false)) {
                throw std::runtime_error("SAFETY_ABORT: ruta fuera de confinamiento en quarantine_file");
            }
            std::error_code ec;
            const int64_t sz = static_cast<int64_t>(fs::file_size(native_path(src), ec));
            const std::string h = sha256_file_bounded(src, 0, cancel);
            Json me = {
                {"category", op["category"]},
                {"orig_path", path_text(src)},
                {"quarantine_path", path_text(dst)},
                {"size", sz},
                {"sha256", h}
            };
            auto created = win32_ensure_dir(dst.parent_path());
            jfh << Json{
                {"seq", seq}, {"phase", "intent"}, {"op", "move"},
                {"src", path_text(src)}, {"dst", path_text(dst)},
                {"created_dirs", created}, {"manifest_entries", Json::array({me})}
            }.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            jfh.flush();

            win32_move_no_overwrite(src, dst);
            if (simulate_crash_after_move_seq == seq) {
                throw std::runtime_error("SIMULATED_CRASH_AFTER_MOVE");
            }

            mfh << me.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            mfh.flush();
            ++q_files;
            q_bytes += sz;

            jfh << Json{
                {"seq", seq}, {"phase", "committed"}, {"op", "move"},
                {"src", path_text(src)}, {"dst", path_text(dst)},
                {"created_dirs", created}
            }.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            jfh.flush();
            try {
                record_path_event(db, 1, query_path_frn(dst), "file", "quarantine",
                                  op.value("src_rel", path_text(src)), path_text(dst), sz,
                                  "organize_apply", "fs_organize", "exact_transactional", "", h, h, plan_id);
            } catch (...) {}
        } else if (kind == "move_dir" || kind == "move_file") {
            const fs::path src(wide(op.at("src").get<std::string>()));
            const fs::path dst(wide(op.at("dst").get<std::string>()));
            if (!is_confined_within(src, root_path, false) || !is_confined_within(dst, root_path, false)) {
                throw std::runtime_error("SAFETY_ABORT: ruta fuera de confinamiento en " + kind);
            }
            Json moved_files = Json::array();
            if (kind == "move_file") {
                std::error_code ec;
                const int64_t sz = static_cast<int64_t>(fs::file_size(native_path(src), ec));
                moved_files.push_back({
                    {"dst", path_text(dst)},
                    {"size", sz},
                    {"sha256", sha256_file_bounded(src, 0, cancel)}
                });
            } else {
                auto sub = win32_safe_walk(src, cancel);
                for (const auto& f : sub.files) {
                    std::wstring wrel = wide(f.rel_fwd);
                    for (auto& c : wrel) if (c == L'/') c = L'\\';
                    moved_files.push_back({
                        {"dst", path_text(dst / fs::path(wrel))},
                        {"size", f.size_bytes},
                        {"sha256", sha256_file_bounded(src / fs::path(wrel), 0, cancel)}
                    });
                }
            }
            auto created = win32_ensure_dir(dst.parent_path());
            jfh << Json{
                {"seq", seq}, {"phase", "intent"}, {"op", "move"},
                {"src", path_text(src)}, {"dst", path_text(dst)},
                {"created_dirs", created}, {"moved_files", moved_files}
            }.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            jfh.flush();

            win32_move_no_overwrite(src, dst);
            if (simulate_crash_after_move_seq == seq) {
                throw std::runtime_error("SIMULATED_CRASH_AFTER_MOVE");
            }

            jfh << Json{
                {"seq", seq}, {"phase", "committed"}, {"op", "move"},
                {"src", path_text(src)}, {"dst", path_text(dst)},
                {"created_dirs", created}, {"moved_files", std::move(moved_files)}
            }.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            jfh.flush();
            try {
                record_path_event(db, 1, query_path_frn(dst), (kind == "move_dir") ? "dir" : "file", "move",
                                  op.value("src_rel", path_text(src)), op.value("dst_rel", path_text(dst)), 0,
                                  "organize_apply", "fs_organize", "exact_transactional", "", "", "", plan_id);
            } catch (...) {}
        } else if (kind == "rmdir_empty_tree") {
            const fs::path src(wide(op.at("src").get<std::string>()));
            if (!is_confined_within(src, root_path, false)) {
                throw std::runtime_error("SAFETY_ABORT: ruta fuera de confinamiento en rmdir_empty_tree");
            }
            if (win32_is_dir(src)) {
                auto sub = win32_safe_walk(src, cancel);
                if (!sub.files.empty() || !sub.junctions.empty()) {
                    throw std::runtime_error("SAFETY_ABORT: rmdir_empty_tree encontró archivos/junctions en " + path_text(src));
                }
                std::vector<fs::path> to_rm;
                for (const auto& d : sub.dirs) to_rm.push_back(resolve_confined_under(src, d, "subdir"));
                to_rm.push_back(src);
                std::sort(to_rm.begin(), to_rm.end(), [](const fs::path& a, const fs::path& b) {
                    return a.wstring().size() > b.wstring().size();
                });
                std::vector<std::string> removed;
                for (const auto& d : to_rm) {
                    if (!win32_rmdir_empty(d)) throw std::runtime_error("No se pudo eliminar carpeta vacía: " + path_text(d));
                    removed.push_back(path_text(d));
                }
                std::reverse(removed.begin(), removed.end());
                jfh << Json{{"seq", seq}, {"phase", "committed"}, {"op", "rmdir_list"}, {"dirs", removed}}.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
                jfh.flush();
            }
        } else if (kind == "prune_empty_subdirs") {
            const fs::path src(wide(op.at("src").get<std::string>()));
            if (!is_confined_within(src, root_path, true)) {
                throw std::runtime_error("SAFETY_ABORT: ruta fuera de confinamiento en prune_empty_subdirs");
            }
            if (win32_is_dir(src)) {
                auto sub = win32_safe_walk(src, cancel);
                std::vector<fs::path> cands;
                for (const auto& d : sub.dirs) {
                    if (lower_ascii(d).find(".git") != std::string::npos) continue;
                    cands.push_back(resolve_confined_under(src, d, "subdir"));
                }
                std::sort(cands.begin(), cands.end(), [](const fs::path& a, const fs::path& b) {
                    return a.wstring().size() > b.wstring().size();
                });
                std::vector<std::string> removed;
                for (const auto& d : cands) {
                    if (win32_rmdir_empty(d)) removed.push_back(path_text(d));
                }
                if (!removed.empty()) {
                    std::reverse(removed.begin(), removed.end());
                    jfh << Json{{"seq", seq}, {"phase", "committed"}, {"op", "rmdir_list"}, {"dirs", removed}}.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
                    jfh.flush();
                }
            }
        } else if (kind == "edit_text_paths") {
            const fs::path target(wide(op.at("target").get<std::string>()));
            const std::string trel = validate_rel_no_traversal(op.at("target_rel").get<std::string>(), "target_rel");
            if (!is_confined_within(target, root_path, false)) {
                throw std::runtime_error("SAFETY_ABORT: target fuera de confinamiento en edit_text_paths");
            }
            const std::string raw = read_text(target);
            const std::string old_sha = sha256(raw);
            std::string updated = raw;
            for (const auto& pair : op.at("replacements")) {
                const std::string old_s = pair.at(0).get<std::string>();
                const std::string new_s = pair.at(1).get<std::string>();
                if (old_s.empty()) continue;
                size_t pos = 0;
                while ((pos = updated.find(old_s, pos)) != std::string::npos) {
                    updated.replace(pos, old_s.size(), new_s);
                    pos += new_s.size();
                }
            }
            if (updated != raw) {
                const fs::path bak = resolve_confined_under(backups_dir, trel, "backup_rel");
                win32_ensure_dir(bak.parent_path());
                write_text(bak, raw);
                const std::string new_sha = sha256(updated);
                jfh << Json{
                    {"seq", seq}, {"phase", "intent"}, {"op", "edit_text"},
                    {"target", path_text(target)}, {"backup", path_text(bak)},
                    {"old_sha256", old_sha}, {"new_sha256", new_sha},
                    {"old_bytes", raw.size()}, {"new_bytes", updated.size()}
                }.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
                jfh.flush();

                write_text(target, updated);
                ++edits_done;
                edit_byte_delta += static_cast<int64_t>(updated.size()) - static_cast<int64_t>(raw.size());
                jfh << Json{
                    {"seq", seq}, {"phase", "committed"}, {"op", "edit_text"},
                    {"target", path_text(target)}, {"backup", path_text(bak)},
                    {"old_sha256", old_sha}, {"new_sha256", new_sha},
                    {"old_bytes", raw.size()}, {"new_bytes", updated.size()}
                }.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
                jfh.flush();
            }
        }
        ++executed_now;
    }

    // Mirror metadata into quarantine _meta for self-contained recovery
    try {
        std::error_code ec;
        for (const wchar_t* fn : {L"plan.jsonl", L"plan_summary.json", L"journal.jsonl", L"quarantine_manifest.jsonl", L"baseline_git.json"}) {
            if (win32_exists(plan_dir / fn)) fs::copy_file(native_path(plan_dir / fn), native_path(q_meta / fn), fs::copy_options::overwrite_existing, ec);
        }
    } catch (...) {}

    Json sync_res = nullptr;
    if (args.value("sync_after", true)) {
        try { sync_res = fs_sync(p, db, {{"full", false}}, log, cancel); } catch (...) {}
    }

    Json result = {
        {"schema", "migratool.organize_apply/1"},
        {"plan_id", plan_id},
        {"executed_now", executed_now},
        {"total_ops", ops.size()},
        {"quarantined_files", q_files},
        {"quarantined_bytes", q_bytes},
        {"edits_done", edits_done},
        {"edit_byte_delta", edit_byte_delta},
        {"journal_path", path_text(journal_path)},
        {"manifest_path", path_text(manifest_path)},
        {"sync", std::move(sync_res)}
    };
    write_json(plan_dir / L"apply_result.json", result);
    return result;
}

// ============================================================================
// 6. tool_organize_undo (Idempotent + resumable via undo_journal.jsonl)
// ============================================================================
Json tool_organize_undo(const Project& p, Database& db, const Json& args, Log log, Cancel* cancel) {
    const std::string plan_id = args.at("plan_id").get<std::string>();
    validate_safe_id(plan_id, "plan_id");
    const fs::path plan_dir = p.state_dir() / L"organize" / wide(plan_id);
    const fs::path journal_path = plan_dir / L"journal.jsonl";
    const fs::path manifest_path = plan_dir / L"quarantine_manifest.jsonl";
    const fs::path undo_journal_path = plan_dir / L"undo_journal.jsonl";
    if (!win32_exists(journal_path)) throw std::runtime_error("journal.jsonl no encontrado para plan_id: " + plan_id);

    std::vector<Json> entries = load_reconciled_journal(journal_path, manifest_path, false);
    if (!args.value("execute", false)) {
        return {
            {"schema", "migratool.organize_undo/1"},
            {"plan_id", plan_id},
            {"dry_run", true},
            {"journal_entries", entries.size()},
            {"hint", "Pase execute:true para revertir todas las operaciones en orden inverso."}
        };
    }

    std::unordered_set<int64_t> undone_seqs;
    if (win32_exists(undo_journal_path)) {
        std::ifstream ujf(native_path(undo_journal_path), std::ios::binary);
        std::string line;
        while (std::getline(ujf, line)) {
            if (line.empty()) continue;
            auto uj = Json::parse(line, nullptr, false);
            if (uj.is_object() && uj.contains("seq")) {
                undone_seqs.insert(uj["seq"].get<int64_t>());
            }
        }
    }

    std::ofstream ufh(native_path(undo_journal_path), std::ios::binary | std::ios::app);
    const int64_t simulate_undo_cancel_after = args.value("simulate_undo_cancel_after", int64_t(-1));
    int64_t moves_undone = 0, edits_undone = 0, dirs_recreated = 0, skipped_already_undone = 0, step_count = 0;

    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
        const auto& e = *it;
        const int64_t seq = e.value("seq", int64_t(0));
        if (seq > 0 && undone_seqs.contains(seq)) {
            ++skipped_already_undone;
            continue;
        }
        const std::string op = e.at("op").get<std::string>();
        if (op == "edit_text") {
            const fs::path target(wide(e.at("target").get<std::string>()));
            const fs::path bak(wide(e.at("backup").get<std::string>()));
            const std::string raw = read_text(bak);
            if (sha256(raw) != e.at("old_sha256").get<std::string>()) {
                throw std::runtime_error("SHA-256 del respaldo pre-edición no coincide: " + path_text(bak));
            }
            write_text(target, raw);
            ++edits_undone;
        } else if (op == "rmdir_list") {
            for (const auto& d : e.at("dirs")) {
                win32_ensure_dir(fs::path(wide(d.get<std::string>())));
                ++dirs_recreated;
            }
        } else if (op == "move") {
            const fs::path src(wide(e.at("src").get<std::string>()));
            const fs::path dst(wide(e.at("dst").get<std::string>()));
            // Idempotent physical state check: if dst is already moved back to src, do not fail!
            if (!win32_exists(dst) && win32_exists(src)) {
                ++skipped_already_undone;
            } else {
                win32_move_no_overwrite(dst, src);
                ++moves_undone;
            }
            if (e.contains("created_dirs") && e["created_dirs"].is_array()) {
                const auto& cdirs = e["created_dirs"];
                for (auto dit = cdirs.rbegin(); dit != cdirs.rend(); ++dit) {
                    win32_rmdir_empty(fs::path(wide(dit->get<std::string>())));
                }
            }
        }
        if (seq > 0) {
            ufh << Json{{"seq", seq}, {"op", op}, {"undone", true}}.dump() << '\n';
            ufh.flush();
            undone_seqs.insert(seq);
        }
        ++step_count;
        if (simulate_undo_cancel_after > 0 && step_count >= simulate_undo_cancel_after) {
            throw std::runtime_error("CANCELLED");
        }
    }

    Json sync_res = nullptr;
    if (args.value("sync_after", true)) {
        try { sync_res = fs_sync(p, db, {{"full", false}}, log, cancel); } catch (...) {}
    }

    return {
        {"schema", "migratool.organize_undo/1"},
        {"plan_id", plan_id},
        {"undone", true},
        {"moves_undone", moves_undone},
        {"edits_undone", edits_undone},
        {"dirs_recreated", dirs_recreated},
        {"skipped_already_undone", skipped_already_undone},
        {"sync", std::move(sync_res)}
    };
}

// ============================================================================
// 7. tool_organize_verify
// ============================================================================
Json tool_organize_verify(const Project& p, Database& db, const Json& args, Cancel* cancel) {
    (void)db;
    const std::string plan_id = args.at("plan_id").get<std::string>();
    validate_safe_id(plan_id, "plan_id");
    const fs::path plan_dir = p.state_dir() / L"organize" / wide(plan_id);
    if (!win32_exists(plan_dir / L"plan_summary.json")) throw std::runtime_error("Plan no encontrado: " + plan_id);

    Json summary = read_json(plan_dir / L"plan_summary.json");
    Json baseline_git = read_json(plan_dir / L"baseline_git.json");
    const fs::path root_path(wide(summary.at("root_path").get<std::string>()));
    const fs::path q_ret = fs::path(wide(summary.at("quarantine_dir").get<std::string>())) / L"retirados";

    // Load baseline_files.jsonl (including optional sha256)
    std::unordered_map<std::string, int64_t> base_files;
    std::unordered_map<std::string, std::string> base_sha256;
    int64_t base_bytes = 0;
    {
        std::ifstream bf(native_path(plan_dir / L"baseline_files.jsonl"), std::ios::binary);
        std::string line;
        while (std::getline(bf, line)) {
            if (line.empty()) continue;
            auto j = Json::parse(line);
            const std::string k = lower_ascii(j["rel"].get<std::string>());
            int64_t sz = j["size"].get<int64_t>();
            base_files[k] = sz;
            if (j.contains("sha256") && j["sha256"].is_string() && !j["sha256"].get<std::string>().empty()) {
                base_sha256[k] = j["sha256"].get<std::string>();
            }
            base_bytes += sz;
        }
    }

    // Load reconciled journal & manifest
    const fs::path journal_path = plan_dir / L"journal.jsonl";
    const fs::path manifest_path = plan_dir / L"quarantine_manifest.jsonl";
    std::vector<Json> journal = load_reconciled_journal(journal_path, manifest_path, false);
    std::vector<Json> manifest;
    if (win32_exists(manifest_path)) {
        std::ifstream mf(native_path(manifest_path), std::ios::binary);
        std::string line;
        while (std::getline(mf, line)) if (!line.empty()) manifest.push_back(Json::parse(line));
    }

    // Gate 1: Strict 1-to-1 Conservation (existence + exact byte size + zero unexpected/duplicate mappings)
    auto post_root = win32_safe_walk(root_path, cancel);
    auto post_q = win32_safe_walk(q_ret, cancel);

    std::string root_pfx = lower_ascii(path_text(root_path));
    if (!root_pfx.ends_with("/")) root_pfx.push_back('/');
    std::unordered_map<std::string, std::string> rev_file_map;
    std::vector<std::pair<std::string, std::string>> rev_dir_map;
    std::unordered_map<std::string, std::string> repo_remap;
    std::unordered_map<std::string, int64_t> edited_new_bytes;
    std::unordered_map<std::string, std::string> edited_new_sha;

    for (const auto& e : journal) {
        if (e["op"] == "move") {
            const std::string s = lower_ascii(e["src"].get<std::string>());
            const std::string d = lower_ascii(e["dst"].get<std::string>());
            if (s.starts_with(root_pfx) && d.starts_with(root_pfx)) {
                const std::string srel = s.substr(root_pfx.size());
                const std::string drel = d.substr(root_pfx.size());
                rev_file_map[drel] = srel;
                rev_dir_map.emplace_back(drel + "/", srel + "/");
                repo_remap[srel] = drel;
            }
        } else if (e["op"] == "edit_text") {
            const std::string t = lower_ascii(e["target"].get<std::string>());
            if (t.starts_with(root_pfx)) {
                const std::string trel = t.substr(root_pfx.size());
                edited_new_bytes[trel] = e["new_bytes"].get<int64_t>();
                edited_new_sha[trel] = e["new_sha256"].get<std::string>();
            }
        }
    }

    std::unordered_set<std::string> accounted;
    int64_t post_root_bytes = 0, post_q_bytes = 0;
    int64_t size_mismatch_cnt = 0, unexpected_files_cnt = 0, duplicate_mapping_cnt = 0;
    int64_t root_sha_ok = 0, root_sha_fail = 0;

    for (const auto& f : post_root.files) {
        post_root_bytes += f.size_bytes;
        std::string cur = lower_ascii(f.rel_fwd);
        std::string orig = cur;
        if (auto it = rev_file_map.find(cur); it != rev_file_map.end()) {
            orig = it->second;
        } else {
            for (const auto& [dp, sp] : rev_dir_map) {
                if (cur.starts_with(dp)) { orig = sp + cur.substr(dp.size()); break; }
            }
        }
        auto bit = base_files.find(orig);
        if (bit == base_files.end()) {
            ++unexpected_files_cnt;
            continue;
        }
        const int64_t expected_sz = edited_new_bytes.contains(cur) ? edited_new_bytes[cur] : bit->second;
        if (f.size_bytes != expected_sz) {
            ++size_mismatch_cnt;
        }
        if (!accounted.insert(orig).second) {
            ++duplicate_mapping_cnt;
        }
        if (!edited_new_sha.contains(cur)) {
            if (auto sit = base_sha256.find(orig); sit != base_sha256.end()) {
                std::wstring wrel = wide(f.rel_fwd);
                for (auto& c : wrel) if (c == L'/') c = L'\\';
                if (sha256_file_bounded(root_path / fs::path(wrel), 0, cancel) == sit->second) ++root_sha_ok;
                else ++root_sha_fail;
            }
        }
    }
    for (const auto& f : post_q.files) post_q_bytes += f.size_bytes;
    for (const auto& m : manifest) {
        std::string op = lower_ascii(m["orig_path"].get<std::string>());
        if (op.starts_with(root_pfx)) {
            std::string orig = op.substr(root_pfx.size());
            auto bit = base_files.find(orig);
            if (bit != base_files.end()) {
                if (m["size"].get<int64_t>() != bit->second) ++size_mismatch_cnt;
                if (!accounted.insert(orig).second) ++duplicate_mapping_cnt;
            } else {
                ++unexpected_files_cnt;
            }
        }
    }

    Json missing_sample = Json::array();
    int64_t missing_cnt = 0;
    for (const auto& [k, _] : base_files) {
        if (!accounted.contains(k)) {
            ++missing_cnt;
            if (missing_sample.size() < 15) missing_sample.push_back(k);
        }
    }
    const bool g1_pass = (missing_cnt == 0 && size_mismatch_cnt == 0 &&
                          unexpected_files_cnt == 0 && duplicate_mapping_cnt == 0 &&
                          post_q.files.size() == manifest.size());

    // Gate 2: 100% Quarantine SHA-256 + Pre-edit backups + Moved files SHA-256 + Baseline SHA-256
    int64_t q_sha_ok = 0, q_sha_fail = 0, edit_ok = 0, edit_fail = 0, moved_sha_ok = 0, moved_sha_fail = 0;
    for (const auto& m : manifest) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
        fs::path qp(wide(m["quarantine_path"].get<std::string>()));
        if (!win32_exists(qp) || sha256_file_bounded(qp, 0, cancel) != m["sha256"].get<std::string>()) ++q_sha_fail;
        else ++q_sha_ok;
    }
    for (const auto& e : journal) {
        if (e["op"] == "edit_text") {
            const std::string bh = sha256_file_bounded(fs::path(wide(e["backup"].get<std::string>())), 0, cancel);
            const std::string th = sha256_file_bounded(fs::path(wide(e["target"].get<std::string>())), 0, cancel);
            if (bh == e["old_sha256"].get<std::string>() && th == e["new_sha256"].get<std::string>()) ++edit_ok;
            else ++edit_fail;
        } else if (e["op"] == "move" && e.contains("moved_files") && e["moved_files"].is_array()) {
            for (const auto& mf : e["moved_files"]) {
                fs::path mp(wide(mf["dst"].get<std::string>()));
                std::string mlow = lower_ascii(mf["dst"].get<std::string>());
                std::string expected_h = mf["sha256"].get<std::string>();
                if (mlow.starts_with(root_pfx)) {
                    std::string mrel = mlow.substr(root_pfx.size());
                    if (auto eit = edited_new_sha.find(mrel); eit != edited_new_sha.end()) {
                        expected_h = eit->second;
                    }
                }
                if (!win32_exists(mp) || sha256_file_bounded(mp, 0, cancel) != expected_h) ++moved_sha_fail;
                else ++moved_sha_ok;
            }
        }
    }
    const bool g2_pass = (q_sha_fail == 0 && edit_fail == 0 && moved_sha_fail == 0 && root_sha_fail == 0);

    // Gate 3: Git repository integrity
    Json git_diffs = Json::array();
    for (auto it = baseline_git.begin(); it != baseline_git.end(); ++it) {
        const std::string old_r = it.key();
        std::string new_r = old_r;
        const std::string low_r = lower_ascii(old_r);
        if (auto mit = repo_remap.find(low_r); mit != repo_remap.end()) {
            new_r = mit->second;
        } else {
            for (const auto& [dp, sp] : rev_dir_map) {
                std::string sp_trim = sp.substr(0, sp.size() - 1);
                std::string dp_trim = dp.substr(0, dp.size() - 1);
                if (low_r == sp_trim) { new_r = dp_trim; break; }
                if (low_r.starts_with(sp)) { new_r = dp + low_r.substr(sp.size()); break; }
            }
        }
        fs::path rp = (new_r == ".") ? root_path : resolve_confined_under(root_path, new_r, "git_repo");
        auto post_snap = snapshot_single_git_repo(rp, new_r);
        const auto& pre = it.value();
        if (pre.value("head_rc", -1) != post_snap.value("head_rc", -1)) {
            git_diffs.push_back({{"repo", old_r}, {"new_repo", new_r}, {"field", "head_rc"}});
        } else if (pre.value("head_rc", -1) == 0) {
            for (const auto* k : {"head", "tracked_count", "status_count", "status_hash", "worktree_prunable"}) {
                if (pre[k] != post_snap[k]) {
                    git_diffs.push_back({{"repo", old_r}, {"new_repo", new_r}, {"field", k}, {"before", pre[k]}, {"after", post_snap[k]}});
                }
            }
        }
    }
    const bool g3_pass = git_diffs.empty();

    return {
        {"schema", "migratool.organize_verify/1"},
        {"plan_id", plan_id},
        {"all_passed", g1_pass && g2_pass && g3_pass},
        {"gates", {
            {"conservation", {
                {"status", g1_pass ? "CONFIRMED" : "FAILED"},
                {"baseline_files", base_files.size()},
                {"missing_baseline_files", missing_cnt},
                {"size_mismatch_files", size_mismatch_cnt},
                {"unexpected_files", unexpected_files_cnt},
                {"duplicate_mappings", duplicate_mapping_cnt},
                {"missing_sample", std::move(missing_sample)},
                {"post_root_files", post_root.files.size()},
                {"post_quarantine_files", post_q.files.size()},
                {"manifest_entries", manifest.size()},
                {"baseline_bytes", base_bytes},
                {"post_root_bytes", post_root_bytes},
                {"post_quarantine_bytes", post_q_bytes}
            }},
            {"integrity", {
                {"status", g2_pass ? "CONFIRMED" : "FAILED"},
                {"quarantine_sha256_verified", q_sha_ok},
                {"quarantine_sha256_failed", q_sha_fail},
                {"moved_sha256_verified", moved_sha_ok},
                {"moved_sha256_failed", moved_sha_fail},
                {"root_sha256_verified", root_sha_ok},
                {"root_sha256_failed", root_sha_fail},
                {"edits_verified", edit_ok},
                {"edits_failed", edit_fail}
            }},
            {"git", {
                {"status", g3_pass ? "CONFIRMED" : "FAILED"},
                {"repos_checked", baseline_git.size()},
                {"diffs", std::move(git_diffs)}
            }}
        }}
    };
}

} // namespace mt
