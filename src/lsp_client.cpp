#include "migratool/lsp_client.hpp"
#include "migratool/fs_index.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace mt {
namespace {

std::string lower_lsp(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string percent_encode_uri(std::string_view path_generic) {
    std::string out = "file:///";
    for (size_t i = 0; i < path_generic.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(path_generic[i]);
        if (c == '\\') c = '/';
        if (i == 0 && c == '/') continue;
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            out.push_back(static_cast<char>(c));
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

fs::path uri_to_path(std::string uri) {
    if (uri.starts_with("file:///")) uri = uri.substr(8);
    else if (uri.starts_with("file://")) uri = uri.substr(7);
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            auto hex_val = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = hex_val(uri[i + 1]), lo = hex_val(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(uri[i]);
    }
    if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
        decoded.erase(decoded.begin());
    }
    return fs::weakly_canonical(fs::path(wide(decoded)));
}

std::string search_exe_on_path(const std::wstring& exe_name, const std::vector<fs::path>& extra_dirs = {}) {
    std::error_code ec;
    for (const auto& d : extra_dirs) {
        fs::path cand = d / exe_name;
        if (fs::is_regular_file(cand, ec)) return path_text(cand);
    }
    std::wstring buf(32768, L'\0');
    DWORD n = SearchPathW(nullptr, exe_name.c_str(), nullptr, static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n > 0 && n < buf.size()) {
        buf.resize(n);
        return path_text(fs::path(buf));
    }
    return {};
}

std::string normalize_lsp_lang(std::string_view raw, const fs::path& file_path = {}) {
    std::string s = lower_lsp(raw);
    if (s.empty() && !file_path.empty()) {
        std::string ext = lower_lsp(utf8(file_path.extension().wstring()));
        if (ext == ".py" || ext == ".pyi") return "python";
        if (ext == ".ts" || ext == ".tsx") return "typescript";
        if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs") return "javascript";
        if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" || ext == ".hh" || ext == ".mm" || ext == ".m") return "cpp";
        if (ext == ".rs") return "rust";
        if (ext == ".go") return "go";
        if (ext == ".cs") return "csharp";
        if (ext == ".dart") return "dart";
        if (ext == ".java") return "java";
        if (ext == ".kt" || ext == ".kts") return "kotlin";
        if (ext == ".rb") return "ruby";
        if (ext == ".php") return "php";
        if (ext == ".lua") return "lua";
        if (ext == ".swift") return "swift";
        return "";
    }
    if (s == "c++" || s == "c" || s == "c/c++" || s == "cxx" || s == "cc" || s == "objective-c" || s == "objective-c++") return "cpp";
    if (s == "py" || s == "python3") return "python";
    if (s == "ts" || s == "tsx") return "typescript";
    if (s == "js" || s == "jsx") return "javascript";
    if (s == "rs") return "rust";
    if (s == "c#" || s == "cs") return "csharp";
    if (s == "golang") return "go";
    return s;
}

std::vector<std::string> resolve_server_command(const Project& p, const std::string& lang) {
    // 1. Project-configured custom LSP servers (also enables deterministic mock LSP testing!)
    if (p.config.contains("lsp_servers") && p.config["lsp_servers"].is_object()) {
        const auto& cfg = p.config["lsp_servers"];
        if (cfg.contains(lang) && cfg[lang].is_array() && !cfg[lang].empty()) {
            return cfg[lang].get<std::vector<std::string>>();
        }
    }
    // 2. Discover already-installed servers on PATH or standard toolchain env vars (never downloads/installs!)
    auto env_dir = [](const wchar_t* name, const std::wstring& suffix = L"") -> std::vector<fs::path> {
        DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
        if (!len) return {};
        std::wstring val(len, L'\0');
        DWORD got = GetEnvironmentVariableW(name, val.data(), len);
        if (!got || got >= len) return {};
        val.resize(got);
        if (!suffix.empty()) val += suffix;
        return {fs::path(val)};
    };
    if (lang == "cpp") {
        auto hints = env_dir(L"LLVM_DIR", L"\\bin");
        if (auto pf = env_dir(L"ProgramFiles", L"\\LLVM\\bin"); !pf.empty()) hints.push_back(pf[0]);
        if (auto exe = search_exe_on_path(L"clangd.exe", hints); !exe.empty()) {
            return {exe, "--background-index=false", "--log=error"};
        }
    } else if (lang == "python") {
        if (auto exe = search_exe_on_path(L"basedpyright-langserver.cmd"); !exe.empty()) return {exe, "--stdio"};
        if (auto exe = search_exe_on_path(L"basedpyright-langserver.exe"); !exe.empty()) return {exe, "--stdio"};
        if (auto exe = search_exe_on_path(L"pyright-langserver.cmd"); !exe.empty()) return {exe, "--stdio"};
        if (auto exe = search_exe_on_path(L"pyright-langserver.exe"); !exe.empty()) return {exe, "--stdio"};
        if (auto exe = search_exe_on_path(L"pylsp.exe"); !exe.empty()) return {exe};
    } else if (lang == "typescript" || lang == "javascript") {
        if (auto exe = search_exe_on_path(L"typescript-language-server.cmd"); !exe.empty()) return {exe, "--stdio"};
        if (auto exe = search_exe_on_path(L"typescript-language-server.exe"); !exe.empty()) return {exe, "--stdio"};
    } else if (lang == "rust") {
        auto hints = env_dir(L"CARGO_HOME", L"\\bin");
        if (auto up = env_dir(L"USERPROFILE", L"\\.cargo\\bin"); !up.empty()) hints.push_back(up[0]);
        if (auto exe = search_exe_on_path(L"rust-analyzer.exe", hints); !exe.empty()) return {exe};
    } else if (lang == "go") {
        auto hints = env_dir(L"GOPATH", L"\\bin");
        if (auto up = env_dir(L"USERPROFILE", L"\\go\\bin"); !up.empty()) hints.push_back(up[0]);
        if (auto exe = search_exe_on_path(L"gopls.exe", hints); !exe.empty()) return {exe};
    } else if (lang == "dart") {
        auto hints = env_dir(L"FLUTTER_ROOT", L"\\bin\\cache\\dart-sdk\\bin");
        if (auto exe = search_exe_on_path(L"dart.exe", hints); !exe.empty()) {
            return {exe, "language-server", "--protocol=lsp"};
        }
    } else if (lang == "csharp") {
        if (auto exe = search_exe_on_path(L"csharp-ls.exe"); !exe.empty()) return {exe};
    }
    return {};
}

class LspSession {
    std::vector<std::string> argv_;
    fs::path workspace_root_;
    std::string lang_;
    HANDLE job_ = nullptr;
    HANDLE proc_ = nullptr;
    HANDLE stdin_wr_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_rd_ = INVALID_HANDLE_VALUE;
    std::thread reader_;
    std::atomic<bool> alive_{false};
    std::atomic<int64_t> next_id_{1};
    std::mutex io_mu_;
    std::mutex state_mu_;
    std::condition_variable cv_;
    std::unordered_map<int64_t, Json> responses_;
    std::unordered_map<std::string, Json> diagnostics_; // uri_lower -> array of diagnostics
    std::unordered_map<std::string, std::pair<std::string, int>> opened_docs_; // uri -> {sha256, version}
    Json server_caps_ = Json::object();
    std::chrono::steady_clock::time_point last_used_ = std::chrono::steady_clock::now();

    bool write_all(const std::string& data) {
        std::lock_guard lk(io_mu_);
        if (stdin_wr_ == INVALID_HANDLE_VALUE) return false;
        size_t off = 0;
        while (off < data.size()) {
            DWORD written = 0;
            if (!WriteFile(stdin_wr_, data.data() + off, static_cast<DWORD>(data.size() - off), &written, nullptr) || written == 0) {
                return false;
            }
            off += written;
        }
        return true;
    }

    bool send_json(const Json& msg) {
        std::string body = msg.dump(-1, ' ', false, Json::error_handler_t::replace);
        std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        return write_all(frame);
    }

    void reader_loop() {
        std::string buf;
        char chunk[8192];
        while (alive_.load()) {
            DWORD got = 0;
            if (!ReadFile(stdout_rd_, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
            buf.append(chunk, got);
            for (;;) {
                auto hdr_end = buf.find("\r\n\r\n");
                if (hdr_end == std::string::npos) break;
                size_t content_len = 0;
                std::istringstream hss(buf.substr(0, hdr_end));
                std::string line;
                while (std::getline(hss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    auto low = lower_lsp(line);
                    if (low.starts_with("content-length:")) {
                        try { content_len = static_cast<size_t>(std::stoull(line.substr(15))); } catch (...) {}
                    }
                }
                if (buf.size() < hdr_end + 4 + content_len) break;
                std::string payload = buf.substr(hdr_end + 4, content_len);
                buf.erase(0, hdr_end + 4 + content_len);
                try {
                    Json msg = Json::parse(payload);
                    if (msg.contains("id") && !msg.contains("method")) {
                        int64_t id = msg["id"].is_number_integer() ? msg["id"].get<int64_t>() : -1;
                        if (id >= 0) {
                            std::lock_guard lk(state_mu_);
                            responses_[id] = std::move(msg);
                            cv_.notify_all();
                        }
                    } else if (msg.value("method", "") == "textDocument/publishDiagnostics") {
                        auto params = msg.value("params", Json::object());
                        if (params.contains("uri") && params["uri"].is_string()) {
                            std::lock_guard lk(state_mu_);
                            diagnostics_[lower_lsp(params["uri"].get<std::string>())] = params.value("diagnostics", Json::array());
                            cv_.notify_all();
                        }
                    } else if (msg.contains("id") && msg.contains("method")) {
                        // Reply to server-initiated requests (e.g., workspace/configuration) so server never stalls
                        send_json({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", nullptr}});
                    }
                } catch (...) {}
            }
        }
        alive_.store(false);
        cv_.notify_all();
    }

public:
    LspSession(std::vector<std::string> argv, fs::path root, std::string lang)
        : argv_(std::move(argv)), workspace_root_(std::move(root)), lang_(std::move(lang)) {}

    ~LspSession() { stop(); }

    bool start(unsigned timeout_ms = 10000) {
        if (argv_.empty()) return false;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE in_rd = INVALID_HANDLE_VALUE, in_wr = INVALID_HANDLE_VALUE;
        HANDLE out_rd = INVALID_HANDLE_VALUE, out_wr = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&in_rd, &in_wr, &sa, 0) || !CreatePipe(&out_rd, &out_wr, &sa, 0)) {
            if (in_rd != INVALID_HANDLE_VALUE) CloseHandle(in_rd);
            if (in_wr != INVALID_HANDLE_VALUE) CloseHandle(in_wr);
            if (out_rd != INVALID_HANDLE_VALUE) CloseHandle(out_rd);
            if (out_wr != INVALID_HANDLE_VALUE) CloseHandle(out_wr);
            return false;
        }
        SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);

        HANDLE nul_err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jinfo{};
        jinfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_) SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &jinfo, sizeof(jinfo));

        std::wstring cmd;
        for (const auto& a : argv_) {
            if (!cmd.empty()) cmd += L' ';
            cmd += quote_arg(wide(a));
        }
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_rd;
        si.hStdOutput = out_wr;
        si.hStdError = (nul_err != INVALID_HANDLE_VALUE) ? nul_err : out_wr;
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                 native_path(workspace_root_).c_str(), &si, &pi);
        CloseHandle(in_rd);
        CloseHandle(out_wr);
        if (nul_err != INVALID_HANDLE_VALUE) CloseHandle(nul_err);
        if (!ok) {
            CloseHandle(in_wr);
            CloseHandle(out_rd);
            if (job_) { CloseHandle(job_); job_ = nullptr; }
            return false;
        }
        if (job_) AssignProcessToJobObject(job_, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        proc_ = pi.hProcess;
        stdin_wr_ = in_wr;
        stdout_rd_ = out_rd;
        alive_.store(true);
        reader_ = std::thread([this]() { reader_loop(); });

        // LSP initialize handshake
        std::string root_uri = percent_encode_uri(path_text(workspace_root_));
        Json init_params = {
            {"processId", static_cast<int64_t>(GetCurrentProcessId())},
            {"rootUri", root_uri},
            {"workspaceFolders", Json::array({{{"uri", root_uri}, {"name", utf8(workspace_root_.filename().wstring())}}})},
            {"capabilities", {
                {"textDocument", {
                    {"publishDiagnostics", {{"relatedInformation", true}}},
                    {"diagnostic", {{"dynamicRegistration", false}}},
                    {"declaration", {{"linkSupport", false}}},
                    {"definition", {{"linkSupport", false}}},
                    {"implementation", {{"linkSupport", false}}},
                    {"references", Json::object()},
                    {"rename", {{"prepareSupport", false}}}
                }},
                {"workspace", {{"workspaceEdit", {{"documentChanges", true}}}}}
            }}
        };
        auto init_resp = request("initialize", init_params, timeout_ms);
        if (!init_resp.is_object() || init_resp.contains("error") || !init_resp.contains("result")) {
            stop();
            return false;
        }
        server_caps_ = init_resp["result"].value("capabilities", Json::object());
        notify("initialized", Json::object());
        touch();
        return true;
    }

    void stop() {
        bool was_alive = alive_.exchange(false);
        if (was_alive && stdin_wr_ != INVALID_HANDLE_VALUE) {
            try {
                send_json({{"jsonrpc", "2.0"}, {"id", next_id_.fetch_add(1)}, {"method", "shutdown"}, {"params", nullptr}});
                send_json({{"jsonrpc", "2.0"}, {"method", "exit"}, {"params", nullptr}});
            } catch (...) {}
        }
        if (stdin_wr_ != INVALID_HANDLE_VALUE) { CloseHandle(stdin_wr_); stdin_wr_ = INVALID_HANDLE_VALUE; }
        if (proc_) {
            if (WaitForSingleObject(proc_, 400) == WAIT_TIMEOUT && job_) {
                TerminateJobObject(job_, 0);
                WaitForSingleObject(proc_, 1000);
            }
            CloseHandle(proc_);
            proc_ = nullptr;
        }
        if (job_) { CloseHandle(job_); job_ = nullptr; }
        if (stdout_rd_ != INVALID_HANDLE_VALUE) { CloseHandle(stdout_rd_); stdout_rd_ = INVALID_HANDLE_VALUE; }
        if (reader_.joinable()) reader_.join();
    }

    bool is_alive() const { return alive_.load(); }
    void touch() { std::lock_guard lk(state_mu_); last_used_ = std::chrono::steady_clock::now(); }
    int64_t idle_seconds() {
        std::lock_guard lk(state_mu_);
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_used_).count();
    }
    std::string server_name() const { return argv_.empty() ? "" : utf8(fs::path(wide(argv_[0])).filename().wstring()); }

    void notify(const std::string& method, const Json& params) {
        touch();
        send_json({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
    }

    Json request(const std::string& method, const Json& params, unsigned timeout_ms = 10000) {
        touch();
        if (!alive_.load()) return {{"error", {{"code", -32000}, {"message", "LSP_NOT_ALIVE"}}}};
        int64_t id = next_id_.fetch_add(1);
        if (!send_json({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})) {
            return {{"error", {{"code", -32000}, {"message", "LSP_WRITE_FAILED"}}}};
        }
        std::unique_lock lk(state_mu_);
        bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]() {
            return responses_.contains(id) || !alive_.load();
        });
        if (!ok || !responses_.contains(id)) {
            return {{"error", {{"code", -32001}, {"message", "LSP_TIMEOUT"}}}};
        }
        Json resp = std::move(responses_[id]);
        responses_.erase(id);
        return resp;
    }

    void sync_document(const fs::path& file_path) {
        std::string text = read_text(file_path, 16 * 1024 * 1024);
        std::string hash = sha256(text);
        std::string uri = percent_encode_uri(path_text(file_path));
        std::string key = lower_lsp(uri);
        int ver = 1;
        bool already = false;
        {
            std::lock_guard lk(state_mu_);
            auto it = opened_docs_.find(key);
            if (it != opened_docs_.end()) {
                if (it->second.first == hash) return;
                ver = ++it->second.second;
                it->second.first = hash;
                already = true;
            } else {
                opened_docs_[key] = {hash, 1};
            }
            diagnostics_.erase(key);
        }
        if (!already) {
            notify("textDocument/didOpen", {
                {"textDocument", {
                    {"uri", uri},
                    {"languageId", lang_},
                    {"version", 1},
                    {"text", text}
                }}
            });
        } else {
            notify("textDocument/didChange", {
                {"textDocument", {{"uri", uri}, {"version", ver}}},
                {"contentChanges", Json::array({{{"text", text}}})}
            });
        }
    }

    Json collect_diagnostics(const fs::path& file_path, unsigned wait_ms = 1200) {
        sync_document(file_path);
        std::string uri = percent_encode_uri(path_text(file_path));
        std::string key = lower_lsp(uri);

        // Try LSP 3.17 pull diagnostics first if advertised
        if (server_caps_.contains("diagnosticProvider") && !server_caps_["diagnosticProvider"].is_null()) {
            auto pull = request("textDocument/diagnostic", {{"textDocument", {{"uri", uri}}}}, 4000);
            if (pull.contains("result") && pull["result"].is_object() && pull["result"].contains("items")) {
                return pull["result"]["items"];
            }
        }
        // Otherwise wait briefly for publishDiagnostics notification
        std::unique_lock lk(state_mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(wait_ms), [&]() {
            return diagnostics_.contains(key) || !alive_.load();
        });
        auto it = diagnostics_.find(key);
        return (it != diagnostics_.end()) ? it->second : Json::array();
    }
};

class LspManager {
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<LspSession>> sessions_;

    void reap_idle_locked(int64_t idle_limit_s = 600) {
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (!it->second->is_alive() || it->second->idle_seconds() > idle_limit_s) {
                it->second->stop();
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

public:
    std::shared_ptr<LspSession> get_or_start(const Project& p, const std::string& lang, const fs::path& root_dir) {
        auto cmd = resolve_server_command(p, lang);
        if (cmd.empty()) return nullptr;
        std::string key = lower_lsp(path_text(root_dir)) + "|" + lang;
        std::lock_guard lk(mu_);
        reap_idle_locked(p.config.value("lsp_idle_timeout_s", int64_t(600)));
        if (auto it = sessions_.find(key); it != sessions_.end() && it->second->is_alive()) {
            it->second->touch();
            return it->second;
        }
        auto sess = std::make_shared<LspSession>(std::move(cmd), root_dir, lang);
        if (!sess->start(static_cast<unsigned>(p.config.value("lsp_init_timeout_ms", 10000)))) {
            return nullptr;
        }
        sessions_[key] = sess;
        return sess;
    }

    int restart(const Project& p, const std::string& lang_filter) {
        std::lock_guard lk(mu_);
        int stopped = 0;
        std::string prefix;
        if (!p.config["roots"].empty()) prefix = lower_lsp(path_text(p.resolve(p.config["roots"][0]["path"]))) + "|";
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            bool match = prefix.empty() || it->first.starts_with(prefix);
            if (match && !lang_filter.empty()) match = it->first.ends_with("|" + lang_filter);
            if (match) {
                it->second->stop();
                it = sessions_.erase(it);
                ++stopped;
            } else {
                ++it;
            }
        }
        return stopped;
    }

    void shutdown_all() {
        std::lock_guard lk(mu_);
        for (auto& [_, sess] : sessions_) sess->stop();
        sessions_.clear();
    }
};

LspManager& lsp_manager() {
    static LspManager mgr;
    return mgr;
}

fs::path resolve_lsp_target_file(const Project& p, Database& db, const Json& args,
                                 int& out_line_0, int& out_col_0, fs::path& out_root_dir, std::string& out_lang) {
    out_line_0 = std::max(0, args.value("line", 1) - 1);
    out_col_0 = std::max(0, args.value("column", 1) - 1);

    std::string raw = args.value("path", args.value("file", ""));
    if (raw.empty() && args.contains("symbol") && args["symbol"].is_string()) {
        std::string sym = args["symbol"].get<std::string>();
        auto srows = db.query(
            "SELECT f.absolute_path, f.language, s.line, s.data FROM symbols s JOIN files f ON f.id=s.file_id "
            "WHERE f.deleted=0 AND s.active=1 AND (s.name=? OR s.qualified_name=? OR ('S'||s.id)=?) LIMIT 1",
            {sym, sym, sym});
        if (!srows.empty()) {
            raw = srows[0]["absolute_path"].get<std::string>();
            out_line_0 = std::max(0, srows[0]["line"].get<int>() - 1);
            auto sd = Json::parse(srows[0]["data"].get<std::string>(), nullptr, false);
            if (sd.is_object() && sd.contains("column")) out_col_0 = std::max(0, sd["column"].get<int>() - 1);
            out_lang = normalize_lsp_lang(srows[0]["language"].is_string() ? srows[0]["language"].get<std::string>() : "");
        }
    }
    if (raw.empty()) throw std::runtime_error("Se requiere path, file o symbol");

    fs::path abs;
    fs::path in_p(wide(raw));
    if (in_p.is_absolute()) {
        abs = fs::weakly_canonical(in_p);
        for (const auto& r : p.config["roots"]) {
            fs::path rb = p.resolve(r["path"]);
            if (within(abs, rb)) { out_root_dir = rb; break; }
        }
        if (out_root_dir.empty()) throw std::runtime_error("PATH_OUTSIDE_WORKSPACE: " + raw);
    } else {
        for (const auto& r : p.config["roots"]) {
            fs::path rb = p.resolve(r["path"]);
            fs::path cand = fs::weakly_canonical(rb / in_p);
            if (within(cand, rb) && fs::is_regular_file(native_path(cand))) {
                abs = cand;
                out_root_dir = rb;
                break;
            }
        }
        if (abs.empty()) throw std::runtime_error("FILE_NOT_FOUND: " + raw);
    }
    if (out_lang.empty()) out_lang = normalize_lsp_lang(args.value("language", ""), abs);
    return abs;
}

Json normalize_locations(const Json& lsp_result) {
    Json out = Json::array();
    auto push_loc = [&](const Json& loc) {
        if (!loc.is_object()) return;
        std::string uri = loc.value("uri", loc.value("targetUri", ""));
        Json range = loc.contains("range") ? loc["range"] : loc.value("targetSelectionRange", loc.value("targetRange", Json::object()));
        if (uri.empty() || !range.is_object()) return;
        out.push_back({
            {"path", path_text(uri_to_path(uri))},
            {"line", range["start"].value("line", 0) + 1},
            {"column", range["start"].value("character", 0) + 1},
            {"end_line", range["end"].value("line", 0) + 1},
            {"end_column", range["end"].value("character", 0) + 1}
        });
    };
    if (lsp_result.is_object()) push_loc(lsp_result);
    else if (lsp_result.is_array()) for (const auto& el : lsp_result) push_loc(el);
    return out;
}

// Convert 0-based (line, UTF-16 code unit column) per LSP 3.17 specification to UTF-8 byte offset
size_t line_col_to_offset(const std::string& text, int line_0, int utf16_col_0) {
    int cur_line = 0;
    size_t pos = 0;
    while (cur_line < line_0 && pos < text.size()) {
        if (text[pos] == '\n') ++cur_line;
        ++pos;
    }
    size_t byte_off = 0;
    int u16_col = 0;
    while (u16_col < utf16_col_0 && pos + byte_off < text.size()) {
        const char ch = text[pos + byte_off];
        if (ch == '\n' || ch == '\r') break;
        const unsigned char c = static_cast<unsigned char>(ch);
        size_t seq_len = 1;
        int u16_step = 1;
        if (c < 0x80) {
            seq_len = 1;
            u16_step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            seq_len = 2;
            u16_step = 1;
        } else if ((c & 0xF0) == 0xE0) {
            seq_len = 3;
            u16_step = 1;
        } else if ((c & 0xF8) == 0xF0) {
            seq_len = 4;
            u16_step = 2; // Astral code point -> UTF-16 surrogate pair (2 code units)
        }
        if (pos + byte_off + seq_len > text.size()) {
            seq_len = text.size() - (pos + byte_off);
        }
        byte_off += seq_len;
        u16_col += u16_step;
    }
    return pos + byte_off;
}

} // namespace

void lsp_shutdown_all() { lsp_manager().shutdown_all(); }

Json lsp_discover_servers(const Project& p) {
    static const std::vector<std::string> langs = {"cpp", "python", "typescript", "javascript", "rust", "go", "dart", "csharp"};
    Json found = Json::object();
    for (const auto& l : langs) {
        auto cmd = resolve_server_command(p, l);
        found[l] = cmd.empty() ? Json({{"available", false}}) : Json({{"available", true}, {"command", cmd}});
    }
    return {{"schema", "migratool.lsp_servers/1"}, {"servers", std::move(found)}};
}

Json tool_get_diagnostics(const Project& p, Database& db, const Json& args) {
    int line_0 = 0, col_0 = 0;
    fs::path root_dir;
    std::string lang;
    fs::path abs = resolve_lsp_target_file(p, db, args, line_0, col_0, root_dir, lang);

    if (auto sess = lsp_manager().get_or_start(p, lang, root_dir)) {
        auto diags = sess->collect_diagnostics(abs, static_cast<unsigned>(args.value("wait_ms", 1200)));
        Json items = Json::array();
        for (const auto& d : diags) {
            int sev = d.value("severity", 1);
            std::string sev_str = (sev == 1) ? "error" : (sev == 2 ? "warning" : (sev == 3 ? "info" : "hint"));
            int sl = d.contains("range") ? (d["range"]["start"].value("line", 0) + 1) : 1;
            int sc = d.contains("range") ? (d["range"]["start"].value("character", 0) + 1) : 1;
            items.push_back({
                {"severity", sev_str},
                {"line", sl},
                {"column", sc},
                {"message", d.value("message", "")},
                {"source", d.value("source", sess->server_name())},
                {"code", d.contains("code") ? d["code"] : Json(nullptr)}
            });
        }
        return {
            {"schema", "migratool.diagnostics/1"},
            {"backend", "lsp:" + sess->server_name()},
            {"language", lang},
            {"path", path_text(abs)},
            {"count", items.size()},
            {"items", std::move(items)}
        };
    }

    // Transparent fallback to Migratool's indexed Roslyn / libclang diagnostics
    Json items = Json::array();
    auto rows = db.query("SELECT data FROM files WHERE absolute_path=? COLLATE NOCASE AND deleted=0 LIMIT 1", {path_text(abs)});
    if (!rows.empty()) {
        auto d = Json::parse(rows[0]["data"].get<std::string>(), nullptr, false);
        if (d.is_object() && d.contains("diagnostics") && d["diagnostics"].is_array()) {
            items = d["diagnostics"];
        }
    }
    return {
        {"schema", "migratool.diagnostics/1"},
        {"backend", "native_index_fallback"},
        {"language", lang},
        {"path", path_text(abs)},
        {"count", items.size()},
        {"items", std::move(items)}
    };
}

Json tool_find_declaration(const Project& p, Database& db, const Json& args) {
    int line_0 = 0, col_0 = 0;
    fs::path root_dir;
    std::string lang;
    fs::path abs = resolve_lsp_target_file(p, db, args, line_0, col_0, root_dir, lang);

    if (auto sess = lsp_manager().get_or_start(p, lang, root_dir)) {
        sess->sync_document(abs);
        Json params = {
            {"textDocument", {{"uri", percent_encode_uri(path_text(abs))}}},
            {"position", {{"line", line_0}, {"character", col_0}}}
        };
        auto resp = sess->request("textDocument/declaration", params, 6000);
        Json locs = resp.contains("result") ? normalize_locations(resp["result"]) : Json::array();
        if (locs.empty()) {
            resp = sess->request("textDocument/definition", params, 6000);
            if (resp.contains("result")) locs = normalize_locations(resp["result"]);
        }
        return {
            {"schema", "migratool.find_declaration/1"},
            {"backend", "lsp:" + sess->server_name()},
            {"language", lang},
            {"count", locs.size()},
            {"items", std::move(locs)}
        };
    }

    // Fallback to native SQLite symbols table
    Json items = Json::array();
    if (args.contains("symbol") && args["symbol"].is_string()) {
        std::string sym = args["symbol"].get<std::string>();
        for (const auto& r : db.query(
                "SELECT f.absolute_path AS path, s.name, s.qualified_name, s.kind, s.line FROM symbols s JOIN files f ON f.id=s.file_id "
                "WHERE f.deleted=0 AND s.active=1 AND (s.name=? OR s.qualified_name=?) LIMIT 20", {sym, sym})) {
            items.push_back(r);
        }
    }
    return {
        {"schema", "migratool.find_declaration/1"},
        {"backend", "native_index_fallback"},
        {"language", lang},
        {"count", items.size()},
        {"items", std::move(items)}
    };
}

Json tool_find_implementations(const Project& p, Database& db, const Json& args) {
    int line_0 = 0, col_0 = 0;
    fs::path root_dir;
    std::string lang;
    fs::path abs = resolve_lsp_target_file(p, db, args, line_0, col_0, root_dir, lang);

    if (auto sess = lsp_manager().get_or_start(p, lang, root_dir)) {
        sess->sync_document(abs);
        Json params = {
            {"textDocument", {{"uri", percent_encode_uri(path_text(abs))}}},
            {"position", {{"line", line_0}, {"character", col_0}}}
        };
        auto resp = sess->request("textDocument/implementation", params, 6000);
        Json locs = resp.contains("result") ? normalize_locations(resp["result"]) : Json::array();
        return {
            {"schema", "migratool.find_implementations/1"},
            {"backend", "lsp:" + sess->server_name()},
            {"language", lang},
            {"count", locs.size()},
            {"items", std::move(locs)}
        };
    }

    // Fallback to native indexed base_types / overrides
    Json items = Json::array();
    if (args.contains("symbol") && args["symbol"].is_string()) {
        std::string sym = args["symbol"].get<std::string>();
        for (const auto& r : db.query(
                "SELECT f.absolute_path AS path, s.name, s.qualified_name, s.kind, s.line FROM symbols s JOIN files f ON f.id=s.file_id "
                "WHERE f.deleted=0 AND s.active=1 AND instr(s.data, ?)>0 LIMIT 50", {sym})) {
            items.push_back(r);
        }
    }
    return {
        {"schema", "migratool.find_implementations/1"},
        {"backend", "native_index_fallback"},
        {"language", lang},
        {"count", items.size()},
        {"items", std::move(items)}
    };
}

Json tool_lsp_rename_symbol(const Project& p, Database& db, const Json& args) {
    const std::string new_name = args.at("new_name").get<std::string>();
    if (new_name.empty()) throw std::runtime_error("new_name vacío");
    const bool dry_run = args.value("dry_run", false);
    const std::string actor = args.value("actor", "agent");
    int line_0 = 0, col_0 = 0;
    fs::path root_dir;
    std::string lang;
    fs::path abs = resolve_lsp_target_file(p, db, args, line_0, col_0, root_dir, lang);

    auto sess = lsp_manager().get_or_start(p, lang, root_dir);
    if (!sess) {
        return {{"ok", false}, {"backend", "none"}, {"reason", "NO_LSP_SERVER_INSTALLED_FOR_LANGUAGE"}, {"language", lang}};
    }

    sess->sync_document(abs);
    Json params = {
        {"textDocument", {{"uri", percent_encode_uri(path_text(abs))}}},
        {"position", {{"line", line_0}, {"character", col_0}}},
        {"newName", new_name}
    };
    auto resp = sess->request("textDocument/rename", params, 10000);
    if (!resp.contains("result") || !resp["result"].is_object()) {
        throw std::runtime_error("LSP_RENAME_FAILED: " + resp.dump());
    }
    const Json& we = resp["result"];

    // Parse LSP WorkspaceEdit (supports both `changes` and `documentChanges`)
    std::map<fs::path, std::vector<Json>> file_edits;
    if (we.contains("changes") && we["changes"].is_object()) {
        for (auto it = we["changes"].begin(); it != we["changes"].end(); ++it) {
            if (it.value().is_array()) {
                fs::path fp = uri_to_path(it.key());
                for (const auto& te : it.value()) file_edits[fp].push_back(te);
            }
        }
    }
    if (we.contains("documentChanges") && we["documentChanges"].is_array()) {
        for (const auto& dc : we["documentChanges"]) {
            if (dc.is_object() && dc.contains("textDocument") && dc.contains("edits") && dc["edits"].is_array()) {
                fs::path fp = uri_to_path(dc["textDocument"].at("uri").get<std::string>());
                for (const auto& te : dc["edits"]) file_edits[fp].push_back(te);
            }
        }
    }

    // Validate all targets are inside authorized workspace roots & compute transactional edit plan
    struct PlannedFile { fs::path path; std::string original; std::string updated; size_t edit_count; };
    std::vector<PlannedFile> plan;
    for (const auto& [fp, edits] : file_edits) {
        bool authorized = false;
        for (const auto& r : p.config["roots"]) if (within(fp, p.resolve(r["path"]))) { authorized = true; break; }
        if (!authorized) throw std::runtime_error("LSP_RENAME_OUTSIDE_WORKSPACE: " + path_text(fp));

        std::string orig = read_text(fp, 16 * 1024 * 1024);
        struct SpanEdit { size_t start; size_t end; std::string text; };
        std::vector<SpanEdit> spans;
        for (const auto& te : edits) {
            const auto& rng = te.at("range");
            size_t s = line_col_to_offset(orig, rng["start"].at("line").get<int>(), rng["start"].at("character").get<int>());
            size_t e = line_col_to_offset(orig, rng["end"].at("line").get<int>(), rng["end"].at("character").get<int>());
            spans.push_back(SpanEdit{s, e, te.at("newText").get<std::string>()});
        }
        // Apply right-to-left so offsets stay exact
        std::sort(spans.begin(), spans.end(), [](const SpanEdit& a, const SpanEdit& b) { return a.start > b.start; });
        std::string upd = orig;
        for (const auto& sp : spans) {
            if (sp.end <= upd.size() && sp.start <= sp.end) upd.replace(sp.start, sp.end - sp.start, sp.text);
        }
        plan.push_back(PlannedFile{fp, std::move(orig), std::move(upd), spans.size()});
    }

    if (dry_run) {
        Json files = Json::array();
        for (const auto& pl : plan) files.push_back({{"path", path_text(pl.path)}, {"edits", pl.edit_count}});
        return {{"ok", true}, {"dry_run", true}, {"backend", "lsp:" + sess->server_name()}, {"files", std::move(files)}};
    }

    // Unified atomic transactional write + SQLite sync/commit with full disk & DB rollback on failure
    const bool inject_db_failure = args.value("inject_db_failure", false);
    size_t written = 0;
    bool tx_active = false;
    int64_t rev = 0;
    Json files = Json::array();
    try {
        for (size_t i = 0; i < plan.size(); ++i) {
            write_text(plan[i].path, plan[i].updated);
            written = i + 1;
        }
        db.begin();
        tx_active = true;
        for (const auto& pl : plan) {
            fs_sync_single_path(p, db, pl.path, true);
            files.push_back({{"path", path_text(pl.path)}, {"edits", pl.edit_count}, {"sha256", sha256(pl.updated)}});
        }
        if (inject_db_failure) {
            throw std::runtime_error("INJECTED_DB_FAILURE_FOR_LSP_ROLLBACK_TEST");
        }
        rev = db.change(actor, "lsp_rename_symbol", {{"new_name", new_name}, {"files", plan.size()}}, new_name);
        db.commit();
        tx_active = false;
    } catch (const std::exception& e) {
        if (tx_active) {
            try { db.rollback(); } catch (...) {}
        }
        std::string rb_errors;
        for (size_t i = 0; i < written; ++i) {
            try {
                write_text(plan[i].path, plan[i].original);
            } catch (const std::exception& re) {
                rb_errors += " [ROLLBACK_ERROR " + path_text(plan[i].path) + ": " + re.what() + "]";
            }
        }
        throw std::runtime_error(std::string(e.what()) + rb_errors);
    }

    return {
        {"ok", true},
        {"backend", "lsp:" + sess->server_name()},
        {"new_name", new_name},
        {"files_modified", plan.size()},
        {"revision", rev},
        {"files", std::move(files)}
    };
}

Json tool_restart_language_server(const Project& p, const Json& args) {
    const std::string lang = normalize_lsp_lang(args.value("language", ""));
    int stopped = lsp_manager().restart(p, lang);
    return {{"ok", true}, {"language", lang.empty() ? "all" : lang}, {"servers_restarted", stopped}};
}

} // namespace mt
