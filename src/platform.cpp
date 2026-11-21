#include "migratool/core.hpp"
#include "migratool/gateway.hpp"
#include "digest.hpp"
#include <objbase.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <algorithm>
#include <chrono>
#include <memory>

namespace mt {
namespace {
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
};
std::runtime_error error(const std::string& what) {
    return std::runtime_error(what + " (Win32 " + std::to_string(GetLastError()) + ")");
}
}
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw error("UTF-16 inválido");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) throw error("UTF-8 inválido");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string path_text(const fs::path& p) { return utf8(p.generic_wstring()); }
fs::path native_path(const fs::path& path) {
    if (path.empty()) return path;
    auto value = fs::absolute(path).lexically_normal().make_preferred().wstring();
    if (value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\.\\") || value.size() < 248) return fs::path(value);
    if (value.starts_with(L"\\\\")) return fs::path(L"\\\\?\\UNC\\" + value.substr(2));
    return fs::path(L"\\\\?\\" + value);
}
std::string read_text(const fs::path& p, size_t limit) {
    const auto path = native_path(p);
    Handle input(CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (input.h == INVALID_HANDLE_VALUE) throw std::runtime_error("No se pudo leer: " + path_text(p));
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(input.h, &li) || li.QuadPart < 0) throw std::runtime_error("No se pudo leer tamaño: " + path_text(p));
    const auto size = static_cast<uint64_t>(li.QuadPart);
    if (size > limit) throw std::runtime_error("Archivo excede límite de lectura: " + path_text(p));
    std::string s(static_cast<size_t>(size), '\0');
    size_t offset = 0;
    while (offset < s.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(s.size() - offset, 16 * 1024 * 1024));
        DWORD got = 0;
        if (!ReadFile(input.h, s.data() + offset, chunk, &got, nullptr))
            throw std::runtime_error("Lectura incompleta: " + path_text(p));
        if (got == 0) break;
        offset += got;
    }
    if (offset != s.size()) throw std::runtime_error("Lectura incompleta: " + path_text(p));
    return s;
}
void write_text(const fs::path& p, const std::string& s) {
    const auto staged = fs::path(p.wstring() + L".new-" + wide(unique_id()));
    const auto native_staged = native_path(staged);
    const auto destination = native_path(p);
    if (!destination.parent_path().empty()) fs::create_directories(destination.parent_path());
    {
        std::ofstream out(native_staged, std::ios::binary);
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
        out.flush();
        if (!out) throw std::runtime_error("No se pudo escribir: " + path_text(staged));
    }
    const auto start = GetTickCount64();
    for (unsigned attempt = 0;; ++attempt) {
        if (MoveFileExW(native_staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;
        const DWORD err = GetLastError();
        if ((err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) && (GetTickCount64() - start) < 1500) {
            Sleep(attempt < 4 ? 5 : 15);
            continue;
        }
        DeleteFileW(native_staged.c_str());
        SetLastError(err);
        throw error("No se pudo guardar " + path_text(p));
    }
}
Json read_json(const fs::path& p) { return Json::parse(read_text(p)); }
void write_json(const fs::path& p, const Json& j) { write_text(p, j.dump(2, ' ', false, Json::error_handler_t::replace) + "\n"); }
std::string sha256(const std::string& s) { Digest d; d.add(s.data(), s.size()); return d.finish(); }
std::string hash_file(const fs::path& p) {
    Handle input(CreateFileW(native_path(p).c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (input.h == INVALID_HANDLE_VALUE) throw std::runtime_error("No se pudo calcular hash: " + path_text(p));
    Digest d;
    char buffer[64 * 1024];
    for (;;) {
        DWORD bytes{};
        if (!ReadFile(input.h, buffer, sizeof(buffer), &bytes, nullptr))
            throw std::runtime_error("Hash incompleto: " + path_text(p));
        if (!bytes) break;
        d.add(buffer, bytes);
    }
    return d.finish();
}
std::string hash_file(const fs::path& path, Cancel* cancel) {
    if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
    Handle input(CreateFileW(native_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (input.h == INVALID_HANDLE_VALUE) throw error("No se pudo comprobar " + path_text(path));
    Digest digest;
    char buffer[64 * 1024];
    for (;;) {
        if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
        DWORD bytes{};
        if (!ReadFile(input.h, buffer, sizeof(buffer), &bytes, nullptr))
            throw error("Lectura incompleta al comprobar " + path_text(path));
        if (!bytes) break;
        digest.add(buffer, bytes);
    }
    return digest.finish();
}
std::string now() {
    SYSTEMTIME t; GetSystemTime(&t);
    char s[40];
    snprintf(s, sizeof(s), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return s;
}
std::string unique_id() {
    GUID id; if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("No se pudo crear identificador");
    wchar_t buffer[40]; StringFromGUID2(id, buffer, 40);
    return utf8(std::wstring(buffer + 1, 36));
}
fs::path executable_dir() {
    std::wstring buffer(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!n || n >= buffer.size()) throw error("No se pudo obtener ruta del ejecutable");
    buffer.resize(n); return fs::path(buffer).parent_path();
}
bool within(const fs::path& child, const fs::path& parent) {
    auto c = fs::weakly_canonical(child); auto p = fs::weakly_canonical(parent);
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci)
        if (ci == c.end() || CompareStringOrdinal(ci->c_str(), -1, pi->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    return true;
}
std::wstring quote_arg(const std::wstring& s) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'\"'; return result;
}
Json run_process(const std::vector<std::string>& argv, const fs::path& cwd, const std::string& input,
                 const fs::path& logs, unsigned timeout_ms, Cancel* cancel, const Json& overrides) {
    if (argv.empty() || argv[0].empty()) throw std::runtime_error("Comando vacío");
    const auto native_cwd = native_path(cwd);
    const auto native_logs = native_path(logs);
    if (!fs::is_directory(native_cwd)) throw std::runtime_error("Directorio de ejecución inexistente: " + path_text(cwd));
    if (!timeout_ms || timeout_ms > 86400000) throw std::runtime_error("Tiempo límite fuera de rango");
    if (fs::exists(native_logs)) throw std::runtime_error("Directorio de evidencia ya existe: " + path_text(logs));
    fs::create_directories(native_logs);
    write_text(logs / L"stdin.bin", input);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle in(CreateFileW(native_path(logs / L"stdin.bin").c_str(), GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle out(CreateFileW(native_path(logs / L"stdout.bin").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle err(CreateFileW(native_path(logs / L"stderr.bin").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (in.h == INVALID_HANDLE_VALUE || out.h == INVALID_HANDLE_VALUE || err.h == INVALID_HANDLE_VALUE) throw error("No se pudieron abrir logs");
    std::wstring command;
    for (const auto& arg : argv) { if (!command.empty()) command += L' '; command += quote_arg(wide(arg)); }
    // Un bloque de entorno propio evita modificar PATH o variables del proceso de la GUI.
    struct CaseLess { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
    std::map<std::wstring, std::wstring, CaseLess> env;
    auto inherited = GetEnvironmentStringsW();
    if (!inherited) throw error("No se pudo leer entorno");
    for (const wchar_t* e = inherited; *e; e += wcslen(e) + 1) {
        std::wstring value(e); auto eq = value.find(L'=', 1);
        if (eq != std::wstring::npos) env[value.substr(0, eq)] = value.substr(eq + 1);
    }
    FreeEnvironmentStringsW(inherited);
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (it.key().empty() || it.key().find('=') != std::string::npos) throw std::runtime_error("Variable de entorno inválida");
        env[wide(it.key())] = wide(it.value().get<std::string>());
    }
    std::vector<wchar_t> block;
    for (const auto& [k,v] : env) { auto e = k + L"=" + v; block.insert(block.end(), e.begin(), e.end()); block.push_back(0); }
    block.push_back(0);
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.h || !SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) throw error("No se pudo crear trabajo aislado");
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in.h; si.hStdOutput = out.h; si.hStdError = err.h;
    PROCESS_INFORMATION pi{};
    const auto started = now();
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, block.data(), native_cwd.c_str(), &si, &pi))
        throw error("No se pudo iniciar " + argv[0]);
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.h, process.h)) { TerminateProcess(process.h, 1); throw error("No se pudo controlar proceso hijo"); }
    ResumeThread(thread.h);
    const auto tick = GetTickCount64();
    bool timed_out = false, cancelled = false, overflow = false;
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job.h, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) throw error("No se pudo observar el proceso");
        if (!accounting.ActiveProcesses) break;
        cancelled = cancel && cancel->requested;
        timed_out = GetTickCount64() - tick > timeout_ms;
        LARGE_INTEGER os{}, es{}; GetFileSizeEx(out.h, &os); GetFileSizeEx(err.h, &es);
        overflow = os.QuadPart > 128LL*1024*1024 || es.QuadPart > 128LL*1024*1024;
        if (cancelled || timed_out || overflow) {
            if (timed_out && !cancelled && timeout_ms <= 500) WaitForSingleObject(process.h, 400);
            TerminateJobObject(job.h, 0xdead);
            WaitForSingleObject(process.h, 5000);
            break;
        }
        Sleep(15);
    }
    DWORD code = 0; GetExitCodeProcess(process.h, &code);
    FlushFileBuffers(out.h); FlushFileBuffers(err.h);
    Json result = {{"argv",argv},{"cwd",path_text(cwd)},{"started",started},{"finished",now()},
        {"duration_ms",GetTickCount64()-tick},{"exit_code",code},{"timed_out",timed_out},{"cancelled",cancelled},
        {"output_limit",overflow},{"stdout",path_text(logs/L"stdout.bin")},{"stderr",path_text(logs/L"stderr.bin")},
        {"stdin_sha256",sha256(input)},{"stdout_sha256",hash_file(logs/L"stdout.bin")},{"stderr_sha256",hash_file(logs/L"stderr.bin")}};
    write_json(logs / L"process.json", result);
    return result;
}
fs::path Project::resolve(const std::string& s) const {
    fs::path p(wide(s)); return fs::weakly_canonical(p.is_absolute() ? p : directory() / p);
}
fs::path Project::state_dir() const {
    if (config.contains("state_dir") && config["state_dir"].is_string() && !config["state_dir"].get<std::string>().empty()) {
        fs::path custom(wide(config["state_dir"].get<std::string>()));
        return custom.is_absolute() ? custom : (directory() / custom);
    }
    return directory() / L".migratool" / file.stem();
}
namespace {
struct FileLock {
    Handle h;
    explicit FileLock(const fs::path& p) : h() {
        fs::create_directories(p.parent_path());
        auto start = GetTickCount64();
        do {
            h.h = CreateFileW(native_path(p).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h.h != INVALID_HANDLE_VALUE) return;
            if (GetLastError() != ERROR_SHARING_VIOLATION) throw error("No se pudo bloquear proyecto");
            Sleep(30);
        } while (GetTickCount64() - start < 5000);
        throw std::runtime_error("Proyecto ocupado por otra operación; reintente");
    }
};
}
void Project::save() const {
    validate(config);
    FileLock lock(state_dir() / L"project.lock");
    if (fs::exists(file) && (revision.empty() || hash_file(file) != revision))
        throw std::runtime_error("El proyecto cambió en otro proceso. Recargue antes de guardar; no se sobrescribió.");
    write_json(file, config); revision = hash_file(file);
}
Project Project::load(const fs::path& f) {
    auto bytes = read_text(f);
    Project p{fs::absolute(f), Json::parse(bytes), sha256(bytes)}; validate(p.config); return p;
}
Project Project::create(const fs::path& f, const std::string& name, const Json& roots) {
    if (fs::exists(f)) throw std::runtime_error("El proyecto ya existe");
    Project p{fs::absolute(f), {{"schema","migratool.project/1"},{"name",name},{"roots",roots},
        {"exclude_dirs",{".git",".svn",".hg",".vs",".migratool","bin","obj","build","out","node_modules","__pycache__",".venv","venv","target","dist"}},
        {"defines",Json::array()},{"clang_args",Json::array()},
        {"analyzers",Json::array()}, {"builds",Json::array()},{"cases",Json::array()},{"mappings",Json::array()}}};
    p.save(); event(p,"project_created",{{"name",name},{"roots",roots}}); return p;
}
void event(const Project& p, const std::string& action, const Json& detail, const std::string& actor) {
    FileLock lock(p.state_dir() / L"journal.lock");
    std::wstring user(256, L'\0'); DWORD n = static_cast<DWORD>(user.size());
    if (GetUserNameW(user.data(), &n)) user.resize(n - 1); else user = L"desconocido";
    Json entry = {{"id",unique_id()},{"at",now()},{"actor",actor.empty()?utf8(user):actor},{"action",action},{"detail",detail}};
    index_record_event(p,action,detail,entry["actor"]);
    const auto log = p.state_dir() / L"changelog.jsonl";
    std::ofstream out(log, std::ios::binary | std::ios::app);
    out << entry.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n'; out.flush();
    if (!out) throw std::runtime_error("No se pudo registrar el changelog");
}
void add_note(const Project& p, const std::string& text, const std::string& actor, const std::string& source) {
    if (text.empty()) throw std::runtime_error("Nota vacía");
    event(p,"note",{{"text",text},{"source",source}},actor);
}
void validate(const Json& c) {
    if (c.value("schema","") != "migratool.project/1") throw std::runtime_error("Esquema de proyecto desconocido");
    if (c.contains("mode")) {
        const auto m = c["mode"].is_string() ? c["mode"].get<std::string>() : "";
        if (m != "migration" && m != "workspace" && m != "archive")
            throw std::runtime_error("mode debe ser migration, workspace o archive");
    }
    if (c.contains("index_tier")) {
        const auto t = c["index_tier"].is_string() ? c["index_tier"].get<std::string>() : "";
        if (t != "metadata" && t != "code" && t != "full")
            throw std::runtime_error("index_tier debe ser metadata, code o full");
    }
    if (c.contains("state_dir") && !c["state_dir"].is_string())
        throw std::runtime_error("state_dir debe ser texto");
    for (const auto* arr_field : {"watch_ignore_globs", "archive_exclude_dirs"}) {
        if (c.contains(arr_field)) {
            if (!c[arr_field].is_array()) throw std::runtime_error(std::string(arr_field) + " debe ser un arreglo");
            for (const auto& entry : c[arr_field])
                if (!entry.is_string()) throw std::runtime_error(std::string(arr_field) + " admite solo texto");
        }
    }
    if (c.contains("inventory_mode") && c["inventory_mode"] != "universal" && c["inventory_mode"] != "source_only")
        throw std::runtime_error("inventory_mode debe ser universal o source_only");
    if (c.contains("code_duplicate_mode") && c["code_duplicate_mode"] != "off" &&
        c["code_duplicate_mode"] != "files" && c["code_duplicate_mode"] != "files_and_zip")
        throw std::runtime_error("code_duplicate_mode debe ser off, files o files_and_zip");
    for (const auto* field : {"code_extensions", "code_filenames"}) {
        if (!c.contains(field)) continue;
        if (!c[field].is_array()) throw std::runtime_error(std::string(field) + " debe ser un arreglo");
        for (const auto& entry : c[field]) {
            if (!entry.is_string()) throw std::runtime_error(std::string(field) + " admite solo texto");
            const auto name = entry.get<std::string>();
            if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:*?\"<>|") != std::string::npos ||
                (std::string_view(field) == "code_extensions" && (name.front() != '.' || name.find('.', 1) != std::string::npos)))
                throw std::runtime_error(std::string(field) + " requiere extensiones con punto o nombres de archivo sin ruta");
        }
    }
    if (!c.contains("roots") || !c["roots"].is_array() || c["roots"].empty()) throw std::runtime_error("Defina al menos una raíz");
    std::set<std::string> ids;
    for (const auto& r : c["roots"]) {
        auto id = r.at("id").get<std::string>();
        if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos || !ids.insert(id).second)
            throw std::runtime_error("Identificador de raíz vacío, repetido o inválido");
        if (r.at("path").get<std::string>().empty()) throw std::runtime_error("Ruta de raíz vacía");
        const auto role = r.value("role", "");
        if (role != "source" && role != "target" && role != "workspace" && role != "archive" && role != "reference")
            throw std::runtime_error("Rol de raíz debe ser source, target, workspace, archive o reference");
        if (r.contains("include_all_code") && !r["include_all_code"].is_boolean())
            throw std::runtime_error("include_all_code debe ser true o false");
    }
    for (const auto* section : {"analyzers","builds","cases","mappings"}) {
        if (c.contains(section) && !c[section].is_array()) throw std::runtime_error(std::string(section) + " debe ser un arreglo");
        std::set<std::string> seen;
        if (!c.contains(section)) continue;
        for (const auto& v : c[section]) {
            const auto id = v.at("id").get<std::string>();
            if (id.empty() || !seen.insert(id).second) throw std::runtime_error(std::string(section) + ": id vacío o repetido");
        }
    }
    for (const auto& b : c.value("builds",Json::array())) {
        if (!ids.contains(b.at("root").get<std::string>()) || !b.at("argv").is_array() || b["argv"].empty()) throw std::runtime_error("Compilación inválida");
    }
    for (const auto& t : c.value("cases",Json::array())) {
        const bool has_lr = (t.contains("left") && t["left"].is_object()) || (t.contains("right") && t["right"].is_object());
        const std::string kind = t.value("kind", has_lr ? "differential" : (((t.contains("target") && t["target"].is_object()) || (t.contains("root") && t.contains("argv"))) ? "target_test" : "differential"));
        if (kind == "target_test") {
            if (has_lr) throw std::runtime_error("Caso target_test no debe mezclar left/right");
            const Json& runner = (t.contains("target") && t["target"].is_object()) ? t.at("target") : t;
            if (!runner.is_object() || !runner.contains("root") || !ids.contains(runner.at("root").get<std::string>()) ||
                !runner.contains("argv") || !runner.at("argv").is_array() || runner["argv"].empty())
                throw std::runtime_error("Ejecutor target_test inválido");
        } else if (kind == "differential") {
            if (!t.contains("left") || !t["left"].is_object() || !t.contains("right") || !t["right"].is_object()) throw std::runtime_error("Caso requiere left y right");
            for (const auto* side : {"left","right"})
                if (!ids.contains(t[side].at("root").get<std::string>()) || !t[side].at("argv").is_array() || t[side]["argv"].empty()) throw std::runtime_error("Ejecutor inválido");
        } else {
            throw std::runtime_error("Tipo de caso desconocido: " + kind);
        }
        const auto comp = t.value("comparator","bytes");
        if (comp != "bytes" && comp != "json" && comp != "json_numeric" && comp != "numeric")
            throw std::runtime_error("Comparador desconocido");
    }
}
}
