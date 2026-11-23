#include "migratool/duplicates.hpp"
#include "digest.hpp"

#include <zip.h>
#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace mt {
namespace {
struct Library {
    HMODULE handle{};
    Library() {
        // AUD-035: Search executable_dir()/libzip.dll first for portable deployments before compile-time MIGRATOOL_ZIP_DLL!
        std::vector<fs::path> candidates;
        try {
            const auto exe_dir = executable_dir();
            if (!exe_dir.empty()) candidates.push_back((exe_dir / L"libzip.dll").make_preferred());
        } catch (...) {}
        candidates.push_back(fs::path(MIGRATOOL_ZIP_DLL).make_preferred());
        for (const auto& cand : candidates) {
            std::error_code ec;
            if (!cand.empty() && fs::exists(native_path(cand), ec)) {
                handle = LoadLibraryExW(cand.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (handle) break;
            }
        }
        if (!handle) {
            handle = LoadLibraryExW(L"libzip.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
        if (!handle) throw std::runtime_error("No se pudo cargar libzip (Win32 " + std::to_string(GetLastError()) + ")");
    }
    ~Library() { FreeLibrary(handle); }
    template<class Function> Function load(const char* name) const {
        const auto address = GetProcAddress(handle, name);
        if (!address) throw std::runtime_error(std::string("Función ZIP no disponible: ") + name);
        static_assert(sizeof(Function) == sizeof(address));
        return std::bit_cast<Function>(address);
    }
};

struct Zip {
    Library library;
#define MT_ZIP(function) decltype(&::zip_##function) function = library.load<decltype(&::zip_##function)>("zip_" #function)
    MT_ZIP(error_init);
    MT_ZIP(error_fini);
    MT_ZIP(error_strerror);
    MT_ZIP(source_win32w_create);
    MT_ZIP(source_buffer_create);
    MT_ZIP(source_free);
    MT_ZIP(open_from_source);
    MT_ZIP(discard);
    MT_ZIP(get_num_entries);
    MT_ZIP(stat_index);
    MT_ZIP(file_get_external_attributes);
    MT_ZIP(fopen_index);
    MT_ZIP(fread);
    MT_ZIP(fclose);
    MT_ZIP(strerror);
    MT_ZIP(file_strerror);
#undef MT_ZIP
};

struct ZipScanState {
    const Zip& api;
    const CodeProbe& probe;
    Cancel* cancel{};
    const ZipScanOptions& options;
    int64_t next_nested_index{-10};
    size_t nested_entries{};
    bool nested_entry_limit_reported{};
};

void check_cancel(Cancel* cancel) {
    if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
}

using EmittedZipItem = std::pair<Json, bool>;

void scan_zip_archive(ZipScanState& state, zip_t* archive, const std::string& prefix, int depth, int64_t start_index,
                      const std::function<void(const EmittedZipItem&, int64_t outer_completed)>& emit) {
    const auto count = state.api.get_num_entries(archive, 0);
    if (count < 0) throw std::runtime_error(state.api.strerror(archive));
    std::array<char, 64 * 1024> buffer{};
    const zip_int64_t first = std::max<int64_t>(0, start_index);
    for (zip_int64_t index = first; index < count; ++index) {
        check_cancel(state.cancel);
        if (depth > 0 && state.nested_entries >= state.options.max_nested_entries) {
            if (!state.nested_entry_limit_reported) {
                state.nested_entry_limit_reported = true;
                emit({Json{{"entry_path", prefix}, {"entry_index", state.next_nested_index--}, {"error", "NESTED_ARCHIVE_ENTRY_LIMIT"}}, true}, -1);
            }
            break;
        }
        if (depth > 0) ++state.nested_entries;
        const int64_t assigned_index = (depth == 0) ? index : state.next_nested_index--;
        const int64_t outer_done = (depth == 0) ? (index + 1) : -1;
        zip_stat_t stat{};
        if (state.api.stat_index(archive, index, ZIP_FL_ENC_GUESS, &stat) < 0 || !(stat.valid & ZIP_STAT_NAME)) {
            emit({Json{{"entry_index", assigned_index}, {"error", state.api.strerror(archive)}}, true}, outer_done);
            continue;
        }
        const std::string name = stat.name;
        if (name.empty() || name.ends_with('/') || name.ends_with('\\')) {
            if (depth == 0 && state.options.on_progress) state.options.on_progress(outer_done, state.next_nested_index);
            continue;
        }
        const std::string full_name = prefix + name;
        try {
            zip_uint8_t os{};
            zip_uint32_t attributes{};
            if (state.api.file_get_external_attributes(archive, index, 0, &os, &attributes) == 0 &&
                os == ZIP_OPSYS_UNIX && ((attributes >> 16) & 0170000) == 0120000) {
                if (depth == 0 && state.options.on_progress) state.options.on_progress(outer_done, state.next_nested_index);
                continue;
            }
            const auto member = fs::path(wide(name));
            auto ext = member.extension().wstring();
            for (auto& ch : ext) ch = towlower(ch);
            if (ext == L".zip") {
                if (depth >= state.options.max_depth) {
                    const char* reason = (depth == 0 && state.options.max_depth <= 0) ? "NESTED_ARCHIVE_PENDING" : "NESTED_ARCHIVE_DEPTH_LIMIT";
                    emit({Json{{"entry_path", full_name}, {"entry_index", assigned_index}, {"error", reason}}, true}, outer_done);
                    continue;
                }
                if ((stat.valid & ZIP_STAT_ENCRYPTION_METHOD) && stat.encryption_method != ZIP_EM_NONE)
                    throw std::runtime_error("Entrada cifrada");
                if ((stat.valid & ZIP_STAT_SIZE) && stat.size > state.options.max_nested_bytes)
                    throw std::runtime_error("NESTED_ARCHIVE_SIZE_LIMIT");
                std::unique_ptr<zip_file_t, decltype(state.api.fclose)> entry(state.api.fopen_index(archive, index, 0), state.api.fclose);
                if (!entry) throw std::runtime_error(state.api.strerror(archive));
                std::string nested_bytes;
                if ((stat.valid & ZIP_STAT_SIZE) && stat.size <= state.options.max_nested_bytes)
                    nested_bytes.reserve(static_cast<size_t>(stat.size));
                for (;;) {
                    check_cancel(state.cancel);
                    const auto bytes = state.api.fread(entry.get(), buffer.data(), buffer.size());
                    if (bytes < 0) throw std::runtime_error(state.api.file_strerror(entry.get()));
                    if (bytes == 0) break;
                    if (nested_bytes.size() + static_cast<size_t>(bytes) > state.options.max_nested_bytes)
                        throw std::runtime_error("NESTED_ARCHIVE_SIZE_LIMIT");
                    nested_bytes.append(buffer.data(), static_cast<size_t>(bytes));
                }
                if ((stat.valid & ZIP_STAT_SIZE) && stat.size != nested_bytes.size())
                    throw std::runtime_error("Longitud de entrada distinta de la registrada en el ZIP");
                if (state.api.fclose(entry.release()) != 0) throw std::runtime_error("No se pudo validar la entrada ZIP");
                zip_error_t nested_error;
                state.api.error_init(&nested_error);
                const std::unique_ptr<zip_error_t, decltype(state.api.error_fini)> nested_error_scope(&nested_error, state.api.error_fini);
                auto* nested_source = state.api.source_buffer_create(nested_bytes.data(), nested_bytes.size(), 0, &nested_error);
                if (!nested_source) throw std::runtime_error(state.api.error_strerror(&nested_error));
                std::unique_ptr<zip_source_t, decltype(state.api.source_free)> nested_input(nested_source, state.api.source_free);
                std::unique_ptr<zip_t, decltype(state.api.discard)> nested_archive(state.api.open_from_source(nested_source, ZIP_RDONLY, &nested_error), state.api.discard);
                if (!nested_archive) throw std::runtime_error(state.api.error_strerror(&nested_error));
                nested_input.release();
                std::vector<EmittedZipItem> nested_buffered;
                scan_zip_archive(state, nested_archive.get(), full_name + "!/", depth + 1, 0,
                                 [&](const EmittedZipItem& nested_item, int64_t) { nested_buffered.push_back(nested_item); });
                if (depth == 0) {
                    for (const auto& item : nested_buffered) emit(item, -1);
                    if (state.options.on_progress) state.options.on_progress(outer_done, state.next_nested_index);
                } else {
                    for (const auto& item : nested_buffered) emit(item, -1);
                }
                continue;
            }
            if (!state.probe(member, std::nullopt)) {
                if (depth == 0 && state.options.on_progress) state.options.on_progress(outer_done, state.next_nested_index);
                continue;
            }
            if ((stat.valid & ZIP_STAT_ENCRYPTION_METHOD) && stat.encryption_method != ZIP_EM_NONE)
                throw std::runtime_error("Entrada cifrada");
            std::unique_ptr<zip_file_t, decltype(state.api.fclose)> entry(state.api.fopen_index(archive, index, 0), state.api.fclose);
            if (!entry) throw std::runtime_error(state.api.strerror(archive));
            const auto read = [&](size_t bytes) {
                const auto size = state.api.fread(entry.get(), buffer.data(), bytes);
                if (size < 0) throw std::runtime_error(state.api.file_strerror(entry.get()));
                return static_cast<size_t>(size);
            };
            const auto prefix_len = read(8192);
            if (!state.probe(member, std::string_view(buffer.data(), prefix_len))) {
                if (depth == 0 && state.options.on_progress) state.options.on_progress(outer_done, state.next_nested_index);
                continue;
            }
            Digest digest;
            digest.add(buffer.data(), prefix_len);
            uint64_t size = prefix_len;
            for (;;) {
                check_cancel(state.cancel);
                const auto bytes = read(buffer.size());
                if (bytes == 0) break;
                digest.add(buffer.data(), bytes);
                size += bytes;
            }
            if ((stat.valid & ZIP_STAT_SIZE) && stat.size != size)
                throw std::runtime_error("Longitud de entrada distinta de la registrada en el ZIP");
            if (state.api.fclose(entry.release()) != 0) throw std::runtime_error("No se pudo validar la entrada ZIP");
            emit({Json{{"entry_path", full_name}, {"entry_index", assigned_index}, {"bytes", size}, {"sha256", digest.finish()}}, false}, outer_done);
        } catch (const std::exception& failure) {
            check_cancel(state.cancel);
            emit({Json{{"entry_path", full_name}, {"entry_index", assigned_index}, {"error", failure.what()}}, true}, outer_done);
        }
    }
}
}

Json zip_code_entries(const fs::path& path, const CodeProbe& probe, Cancel* cancel, const ZipEntrySink& sink, const ZipScanOptions& options) {
    static const Zip api;
    zip_error_t error;
    api.error_init(&error);
    const std::unique_ptr<zip_error_t, decltype(api.error_fini)> error_scope(&error, api.error_fini);
    auto* source = api.source_win32w_create(path.c_str(), 0, -1, &error);
    if (!source) throw std::runtime_error(api.error_strerror(&error));
    std::unique_ptr<zip_source_t, decltype(api.source_free)> input(source, api.source_free);
    std::unique_ptr<zip_t, decltype(api.discard)> archive(api.open_from_source(source, ZIP_RDONLY, &error), api.discard);
    if (!archive) throw std::runtime_error(api.error_strerror(&error));
    input.release(); // El archivo abierto posee el origen.
    Json entries = Json::array(), errors = Json::array();
    ZipScanState state{api, probe, cancel, options, std::min<int64_t>(options.start_nested_index, -10)};
    scan_zip_archive(state, archive.get(), "", 0, options.start_index, [&](const EmittedZipItem& item, int64_t outer_done) {
        if (sink) sink(item.first, item.second);
        else if (item.second) errors.push_back(item.first);
        else entries.push_back(item.first);
        if (outer_done >= 0 && options.on_progress) options.on_progress(outer_done, state.next_nested_index);
    });
    return {{"entries", std::move(entries)}, {"errors", std::move(errors)}};
}

Json group_code_copies(const Json& files, const Json& archive_cache) {
    using Key = std::pair<std::string, uint64_t>;
    std::map<Key, Json> matches;
    for (const auto& file : files) {
        if (!file.value("source_code", false)) continue;
        matches[{file.at("sha256").get<std::string>(), file.at("bytes").get<uint64_t>()}].push_back({{"kind", "file"},
            {"root", file.at("root")}, {"role", file.at("role")}, {"absolute_path", file.at("absolute_path")}});
    }
    for (const auto& [path, archive] : archive_cache.items()) {
        for (const auto& entry : archive.at("entries")) {
            matches[{entry.at("sha256").get<std::string>(), entry.at("bytes").get<uint64_t>()}].push_back({{"kind", "zip"},
                {"root", archive.at("root")}, {"role", archive.at("role")}, {"absolute_path", path},
                {"entry_path", entry.at("entry_path")}, {"entry_index", entry.at("entry_index")}});
        }
    }
    Json groups = Json::array();
    for (auto& [key, copies] : matches) {
        if (copies.size() < 2) continue;
        groups.push_back({{"sha256", key.first}, {"bytes", key.second}, {"copy_count", copies.size()}, {"copies", std::move(copies)}});
    }
    return groups;
}

Json duplicate_code_report(const Project& project, Database& db) {
    const auto mode = project.config.value("code_duplicate_mode", "off");
    Json result = {{"mode", mode}, {"available", false}, {"fresh", false}, {"complete", false},
                   {"groups", Json::array()}, {"errors", Json::array()}};
    if (mode == "off") return result;
    const auto records = db.query("SELECT value FROM meta WHERE key='code_duplicate_report'");
    if (records.empty()) return result;
    result = Json::parse(records[0].at("value").get<std::string>());
    result["available"] = true;
    result["fresh"] = result.at("configuration") == sha256(project.config.dump()) &&
        db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'") == 0;
    result["mode"] = mode;
    return result;
}
}
