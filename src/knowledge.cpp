#include "migratool/knowledge.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/workstream.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace mt {
namespace {

std::string lower_kn(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim_kn(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string norm_rel_kn(std::string s) {
    s = trim_kn(std::move(s));
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.starts_with("./")) s.erase(0, 2);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string fts_quote_kn(std::string_view q) {
    std::string out = "\"";
    for (char c : q) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::vector<std::string> split_lines_kn(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

fs::path resolve_project_file_kn(const Project& p, const std::string& raw_path) {
    if (raw_path.empty()) return {};
    const std::string norm = norm_rel_kn(raw_path);
    fs::path fp(wide(norm));
    std::error_code ec;
    if (fp.is_absolute()) return fp;

    if (p.config.contains("roots") && p.config["roots"].is_array() && !p.config["roots"].empty()) {
        for (const auto& r : p.config["roots"]) {
            const std::string rid = r.value("id", "");
            const std::string rpath = r.value("path", "");
            if (rpath.empty()) continue;
            fs::path base = p.resolve(rpath);
            if (!rid.empty() && norm.starts_with(rid + ":")) {
                fs::path cand = base / fs::path(wide(norm.substr(rid.size() + 1)));
                if (fs::exists(native_path(cand), ec)) return cand;
            }
            fs::path cand = base / fp;
            if (fs::exists(native_path(cand), ec)) return cand;
        }
        const std::string first_rpath = p.config["roots"][0].value("path", "");
        if (!first_rpath.empty()) {
            return p.resolve(first_rpath) / fp;
        }
    }
    return p.resolve(norm);
}

std::string to_project_rel_kn(const Project& p, const std::string& raw_path) {
    std::string norm = norm_rel_kn(raw_path);
    if (norm.empty()) return norm;
    fs::path fp(wide(norm));
    if (!fp.is_absolute()) return norm;

    std::error_code ec;
    fs::path canon_fp = fs::weakly_canonical(fp, ec);
    if (ec) canon_fp = fp;

    if (p.config.contains("roots") && p.config["roots"].is_array()) {
        for (const auto& r : p.config["roots"]) {
            const std::string rpath = r.value("path", "");
            if (rpath.empty()) continue;
            fs::path base = fs::weakly_canonical(p.resolve(rpath), ec);
            if (ec) base = p.resolve(rpath);
            if (within(canon_fp, base)) {
                fs::path rel = fs::relative(canon_fp, base, ec);
                if (!ec && !rel.empty()) {
                    return norm_rel_kn(path_text(rel));
                }
            }
        }
    }
    fs::path pdir = fs::weakly_canonical(p.directory(), ec);
    if (!ec && within(canon_fp, pdir)) {
        fs::path rel = fs::relative(canon_fp, pdir, ec);
        if (!ec && !rel.empty()) return norm_rel_kn(path_text(rel));
    }
    return norm;
}

std::string compute_symbol_or_range_sha(const fs::path& abs_p, int start_line, int end_line, const std::string& symbol_name) {
    std::error_code ec;
    if (abs_p.empty() || !fs::is_regular_file(native_path(abs_p), ec)) return "";
    if (start_line <= 0 && end_line <= 0 && symbol_name.empty()) return "";
    std::string text;
    try {
        text = read_text(abs_p, 16 * 1024 * 1024);
    } catch (...) {
        return "";
    }
    auto lines = split_lines_kn(text);
    if (lines.empty()) return "";
    if (start_line > 0) {
        const int s = std::clamp(start_line, 1, static_cast<int>(lines.size()));
        const int e = std::clamp(end_line >= start_line ? end_line : start_line, s, static_cast<int>(lines.size()));
        std::ostringstream oss;
        for (int i = s; i <= e; ++i) {
            if (i > s) oss << '\n';
            oss << lines[static_cast<size_t>(i - 1)];
        }
        return sha256(oss.str());
    }
    if (!symbol_name.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find(symbol_name) != std::string::npos) {
                const size_t s = i;
                const size_t e = std::min(lines.size() - 1, i + 15);
                for (size_t j = s; j <= e; ++j) {
                    oss << lines[j] << '\n';
                }
                break;
            }
        }
        const std::string slice = oss.str();
        if (!slice.empty()) return sha256(slice);
    }
    return "";
}

std::string read_git_head_fast_kn(const Project& p) {
    std::vector<fs::path> candidates;
    if (p.config.contains("roots") && p.config["roots"].is_array()) {
        for (const auto& r : p.config["roots"]) {
            const std::string rp = r.value("path", "");
            if (!rp.empty()) candidates.push_back(p.resolve(rp));
        }
    }
    candidates.push_back(p.directory());
    std::error_code ec;
    for (const auto& dir : candidates) {
        fs::path git_dir = dir / L".git";
        if (fs::is_regular_file(native_path(git_dir), ec)) {
            try {
                std::string gitfile = trim_kn(read_text(git_dir, 4096));
                if (gitfile.starts_with("gitdir:")) {
                    std::string rel = trim_kn(gitfile.substr(7));
                    fs::path gd(wide(rel));
                    git_dir = gd.is_absolute() ? gd : (dir / gd);
                }
            } catch (...) {}
        }
        fs::path head_file = git_dir / L"HEAD";
        if (!fs::is_regular_file(native_path(head_file), ec)) continue;
        try {
            std::string head = trim_kn(read_text(head_file, 4096));
            if (head.starts_with("ref:")) {
                std::string ref_rel = trim_kn(head.substr(4));
                fs::path ref_path = git_dir / fs::path(wide(ref_rel));
                if (fs::is_regular_file(native_path(ref_path), ec)) {
                    return trim_kn(read_text(ref_path, 256));
                }
                fs::path packed = git_dir / L"packed-refs";
                if (fs::is_regular_file(native_path(packed), ec)) {
                    auto plines = split_lines_kn(read_text(packed, 512 * 1024));
                    for (const auto& pl : plines) {
                        if (pl.empty() || pl[0] == '#' || pl[0] == '^') continue;
                        auto sp = pl.find(' ');
                        if (sp != std::string::npos && trim_kn(pl.substr(sp + 1)) == ref_rel) {
                            return pl.substr(0, sp);
                        }
                    }
                }
            } else if (head.size() >= 7) {
                return head;
            }
        } catch (...) {}
    }
    return "";
}

std::string normalize_kind_kn(const std::string& raw) {
    const std::string k = lower_kn(trim_kn(raw));
    if (k == "finding" || k == "method_tried" || k == "decision" ||
        k == "implementation" || k == "pitfall" || k == "progress") {
        return k;
    }
    if (k == "adr" || k == "architecture_decision") return "decision";
    if (k == "method" || k == "attempt" || k == "experiment") return "method_tried";
    if (k == "gotcha" || k == "warning" || k == "bug" || k == "trap") return "pitfall";
    if (k == "observation" || k == "fact" || k == "discovery") return "finding";
    throw std::runtime_error("INVALID_KNOWLEDGE_KIND: '" + raw +
                             "' (use finding, method_tried, decision, implementation, pitfall o progress)");
}

std::string normalize_outcome_kn(const std::string& raw) {
    const std::string o = lower_kn(trim_kn(raw));
    if (o == "succeeded" || o == "failed" || o == "partial" ||
        o == "rejected" || o == "active" || o == "superseded") {
        return o;
    }
    if (o == "success" || o == "passed" || o == "pass" || o == "ok") return "succeeded";
    if (o == "failure" || o == "fail" || o == "error") return "failed";
    throw std::runtime_error("INVALID_KNOWLEDGE_OUTCOME: '" + raw +
                             "' (use succeeded, failed, partial, rejected, active o superseded)");
}

std::string normalize_severity_kn(const std::string& raw) {
    const std::string s = lower_kn(trim_kn(raw));
    if (s == "low" || s == "normal" || s == "high" || s == "critical") return s;
    if (s == "info" || s == "minor") return "low";
    if (s == "medium" || s == "default") return "normal";
    if (s == "warning" || s == "warn" || s == "major") return "high";
    if (s == "error" || s == "fatal" || s == "blocker") return "critical";
    return "normal";
}

std::string epistemic_label_kn(const std::string& vstatus) {
    if (vstatus == "verified_fact") return "CONFIRMED";
    if (vstatus == "test_result") return "TEST_RESULT";
    if (vstatus == "stale") return "STALE";
    return "UNVERIFIED_HYPOTHESIS";
}

std::string resolve_verification_status_kn(const Json& args, const std::string& verified_run_id,
                                           const std::string& evidence_cmd, const std::string& evidence_output,
                                           double confidence) {
    const std::string req = lower_kn(trim_kn(args.value("verification_status", "")));
    if (args.value("is_hypothesis", false) || req == "agent_hypothesis" || req == "hypothesis") {
        return "agent_hypothesis";
    }
    if (req == "stale") return "stale";
    if (req == "test_result") return "test_result";
    if (!verified_run_id.empty()) return "verified_fact";
    if (req == "verified_fact") {
        if (args.value("require_evidence", false) &&
            verified_run_id.empty() && evidence_cmd.empty() && evidence_output.empty()) {
            return "unverified";
        }
        return "verified_fact";
    }
    if (req == "unverified") return "unverified";
    if (!evidence_cmd.empty() || !evidence_output.empty()) return "test_result";
    if (confidence < 0.7) return "agent_hypothesis";
    return "unverified";
}

Json parse_json_array_or_wrap(const Json& val) {
    if (val.is_array()) return val;
    if (val.is_string()) {
        const std::string s = trim_kn(val.get<std::string>());
        if (s.empty()) return Json::array();
        if (s.front() == '[') {
            Json parsed = Json::parse(s, nullptr, false);
            if (parsed.is_array()) return parsed;
        }
        return Json::array({s});
    }
    return Json::array();
}

struct ResolvedLinkKn {
    std::string target_kind = "file";
    std::string target_ref;
    uint64_t frn = 0;
    int start_line = 0;
    int end_line = 0;
    std::string sha256_at_rec;
    std::string symbol_sha256_at_rec;
};

std::vector<ResolvedLinkKn> extract_links_from_args(const Project& p, Database& db, const Json& args) {
    std::vector<ResolvedLinkKn> out;
    std::set<std::pair<std::string, std::string>> seen;

    const bool has_fs_nodes =
        db.integer("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name='fs_nodes')") != 0;

    auto add_link_item = [&](const Json& item, const std::string& default_kind) {
        ResolvedLinkKn lk;
        std::string symbol_name;
        if (item.is_string()) {
            lk.target_kind = default_kind;
            lk.target_ref = item.get<std::string>();
        } else if (item.is_object()) {
            lk.target_kind = lower_kn(item.value("target_kind", item.value("kind", default_kind)));
            lk.target_ref = item.value("target_ref", item.value("path", item.value("file", "")));
            symbol_name = item.value("symbol", "");
            if (lk.target_ref.empty() && !symbol_name.empty()) {
                lk.target_kind = "symbol";
                lk.target_ref = symbol_name;
            }
            lk.frn = static_cast<uint64_t>(std::max<int64_t>(0, item.value("frn", int64_t(0))));
            lk.start_line = std::max(0, item.value("start_line", 0));
            lk.end_line = std::max(0, item.value("end_line", 0));
            lk.sha256_at_rec = item.value("sha256_at_rec", item.value("sha256", ""));
            lk.symbol_sha256_at_rec = item.value("symbol_sha256_at_rec", item.value("symbol_sha256", ""));
        } else {
            return;
        }

        lk.target_ref = trim_kn(lk.target_ref);
        if (lk.target_ref.empty()) return;

        if (lk.target_kind == "file" || lk.target_kind == "code" || lk.target_kind == "doc" ||
            lk.target_ref.find('/') != std::string::npos || lk.target_ref.find('\\') != std::string::npos ||
            lk.target_ref.find('.') != std::string::npos) {
            if (lk.target_kind == "code") lk.target_kind = "file";
            fs::path abs_p = resolve_project_file_kn(p, lk.target_ref);
            lk.target_ref = to_project_rel_kn(p, lk.target_ref);

            std::error_code ec;
            if (!abs_p.empty() && fs::is_regular_file(native_path(abs_p), ec)) {
                if (lk.frn == 0) {
                    lk.frn = query_path_frn(abs_p);
                }
                if (lk.sha256_at_rec.empty()) {
                    try { lk.sha256_at_rec = hash_file(abs_p); } catch (...) {}
                }
                if (lk.symbol_sha256_at_rec.empty() && (lk.start_line > 0 || !symbol_name.empty())) {
                    lk.symbol_sha256_at_rec = compute_symbol_or_range_sha(abs_p, lk.start_line, lk.end_line, symbol_name);
                }
            } else if (lk.frn == 0 && has_fs_nodes) {
                auto slash = lk.target_ref.rfind('/');
                std::string fname = (slash == std::string::npos) ? lk.target_ref : lk.target_ref.substr(slash + 1);
                std::string drel = (slash == std::string::npos) ? "" : lk.target_ref.substr(0, slash);
                auto rows = db.query(
                    "SELECT n.frn FROM fs_nodes n "
                    "LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn "
                    "WHERE n.name=? COLLATE NOCASE AND COALESCE(d.rel_path,'')=? COLLATE NOCASE AND (n.flags & 1)=0 LIMIT 1",
                    {fname, drel});
                if (!rows.empty()) {
                    lk.frn = static_cast<uint64_t>(rows[0]["frn"].get<int64_t>());
                }
            }
        }

        auto key = std::make_pair(lk.target_kind, lower_kn(lk.target_ref));
        if (seen.insert(key).second) {
            out.push_back(std::move(lk));
        }
    };

    for (const char* field : {"files", "target_files", "paths"}) {
        if (args.contains(field) && args[field].is_array()) {
            for (const auto& it : args[field]) add_link_item(it, "file");
        }
    }
    if (args.contains("file") && args["file"].is_string()) {
        add_link_item(args["file"], "file");
    }
    if (args.contains("path") && args["path"].is_string()) {
        add_link_item(args["path"], "file");
    }
    if (args.contains("links") && args["links"].is_array()) {
        for (const auto& it : args["links"]) add_link_item(it, "file");
    }
    return out;
}

bool check_and_refresh_entry_staleness(const Project& p, Database& db, const std::string& entry_id,
                                       std::string& inout_vstatus, Json* out_links_json = nullptr) {
    auto link_rows = db.query(
        "SELECT target_kind, target_ref, frn, start_line, end_line, sha256_at_rec, symbol_sha256_at_rec "
        "FROM knowledge_links WHERE entry_id=? ORDER BY target_kind, target_ref",
        {entry_id});

    bool stale_detected = (inout_vstatus == "stale");
    Json links_arr = Json::array();

    const bool has_fs_nodes =
        db.integer("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name='fs_nodes')") != 0;

    for (const auto& lr : link_rows) {
        const std::string tkind = lr["target_kind"].get<std::string>();
        const std::string tref = lr["target_ref"].get<std::string>();
        const uint64_t frn = static_cast<uint64_t>(std::max<int64_t>(0, lr["frn"].get<int64_t>()));
        const int sline = static_cast<int>(lr["start_line"].get<int64_t>());
        const int eline = static_cast<int>(lr["end_line"].get<int64_t>());
        const std::string rec_sha = lr["sha256_at_rec"].get<std::string>();
        const std::string rec_sym_sha = lr["symbol_sha256_at_rec"].get<std::string>();

        bool link_stale = false;
        std::string cur_sha = rec_sha;
        std::string cur_path = tref;

        if (tkind == "file" || tkind == "doc" || !rec_sha.empty()) {
            fs::path abs_p = resolve_project_file_kn(p, tref);
            std::error_code ec;
            bool exists = !abs_p.empty() && fs::is_regular_file(native_path(abs_p), ec);
            if (!exists && frn != 0 && has_fs_nodes) {
                auto fn = db.query(
                    "SELECT n.name, d.rel_path AS prel, r.abs_path AS rpath, n.flags "
                    "FROM fs_nodes n "
                    "JOIN fs_roots r ON r.root_id=n.root_id "
                    "LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn "
                    "WHERE n.frn=? LIMIT 1",
                    {static_cast<int64_t>(frn)});
                if (!fn.empty() && (fn[0]["flags"].get<int64_t>() & 1) == 0) {
                    const std::string prel = fn[0]["prel"].is_null() ? "" : fn[0]["prel"].get<std::string>();
                    const std::string nm = fn[0]["name"].get<std::string>();
                    cur_path = prel.empty() ? nm : (prel + "/" + nm);
                    abs_p = p.resolve(fn[0]["rpath"].get<std::string>()) / fs::path(wide(cur_path));
                    exists = fs::is_regular_file(native_path(abs_p), ec);
                }
            }

            if (!rec_sha.empty()) {
                if (!exists) {
                    link_stale = true;
                    cur_sha = "";
                } else {
                    try {
                        cur_sha = hash_file(abs_p);
                        if (cur_sha != rec_sha) link_stale = true;
                    } catch (...) {
                        link_stale = true;
                    }
                    if (!link_stale && !rec_sym_sha.empty() && sline > 0) {
                        const std::string cur_sym = compute_symbol_or_range_sha(abs_p, sline, eline, "");
                        if (!cur_sym.empty() && cur_sym != rec_sym_sha) link_stale = true;
                    }
                }
            }
        }

        if (link_stale) stale_detected = true;
        if (out_links_json) {
            links_arr.push_back({
                {"target_kind", tkind},
                {"target_ref", tref},
                {"current_path", cur_path},
                {"frn", static_cast<int64_t>(frn)},
                {"start_line", sline},
                {"end_line", eline},
                {"sha256_at_rec", rec_sha},
                {"current_sha256", cur_sha},
                {"symbol_sha256_at_rec", rec_sym_sha},
                {"stale", link_stale}
            });
        }
    }

    if (stale_detected && inout_vstatus != "stale") {
        inout_vstatus = "stale";
        try {
            db.exec("UPDATE knowledge_entries SET verification_status='stale', updated_at=? WHERE id=? AND verification_status!='stale'",
                    {now(), entry_id});
        } catch (...) {}
    }

    if (out_links_json) *out_links_json = std::move(links_arr);
    return stale_detected;
}

Json format_entry_row(const Project& p, Database& db, const Json& r, bool check_live_staleness = true) {
    const std::string id = r["id"].get<std::string>();
    std::string vstatus = r["verification_status"].get<std::string>();
    Json links = Json::array();
    bool disk_stale = false;
    if (check_live_staleness) {
        disk_stale = check_and_refresh_entry_staleness(p, db, id, vstatus, &links);
    } else {
        links = db.query(
            "SELECT target_kind, target_ref, frn, start_line, end_line, sha256_at_rec, symbol_sha256_at_rec "
            "FROM knowledge_links WHERE entry_id=? ORDER BY target_kind, target_ref",
            {id});
    }

    Json alts = Json::parse(r["alternatives"].get<std::string>(), nullptr, false);
    if (!alts.is_array()) alts = Json::array();
    Json tags = Json::parse(r["tags"].get<std::string>(), nullptr, false);
    if (!tags.is_array()) tags = Json::array();

    const bool needs_reverify = disk_stale || (vstatus == "stale") ||
                                (vstatus == "agent_hypothesis") || (vstatus == "unverified");
    const bool is_verified = (vstatus == "verified_fact") && !disk_stale;

    Json out = {
        {"id", id},
        {"entry_id", id},
        {"kind", r["kind"]},
        {"title", r["title"]},
        {"summary", r["summary"]},
        {"details", r["details"]},
        {"outcome", r["outcome"]},
        {"verification_status", vstatus},
        {"epistemic_label", epistemic_label_kn(vstatus)},
        {"is_verified_fact", is_verified},
        {"needs_reverification", needs_reverify},
        {"confidence", r["confidence"]},
        {"severity", r["severity"]},
        {"evidence_cmd", r["evidence_cmd"]},
        {"evidence_output", r["evidence_output"]},
        {"verified_run_id", r["verified_run_id"]},
        {"alternatives", std::move(alts)},
        {"tags", std::move(tags)},
        {"git_commit", r["git_commit"]},
        {"workstream_id", r["workstream_id"]},
        {"task_id", r["task_id"]},
        {"superseded_by", r["superseded_by"]},
        {"pinned", r["pinned"].get<int64_t>() != 0},
        {"created_at", r["created_at"]},
        {"updated_at", r["updated_at"]},
        {"actor", r["actor"]},
        {"revision", r["revision"]},
        {"links", std::move(links)}
    };
    return out;
}

} // namespace

void knowledge_schema(Database& db) {
    if (db.integer("SELECT COUNT(*) FROM meta WHERE key='knowledge_schema' AND value='1'") != 0) return;
    const bool need_tx = (sqlite3_get_autocommit(db.native_handle()) != 0);
    if (need_tx) db.begin();
    try {
        if (db.integer("SELECT COUNT(*) FROM meta WHERE key='knowledge_schema' AND value='1'") == 0) {
            char* err = nullptr;
            const char* sql = R"sql(
CREATE TABLE IF NOT EXISTS knowledge_entries (
  id                  TEXT PRIMARY KEY,
  kind                TEXT NOT NULL,
  title               TEXT NOT NULL,
  summary             TEXT NOT NULL,
  details             TEXT NOT NULL DEFAULT '',
  outcome             TEXT NOT NULL DEFAULT 'active',
  verification_status TEXT NOT NULL DEFAULT 'unverified',
  confidence          REAL NOT NULL DEFAULT 1.0,
  severity            TEXT NOT NULL DEFAULT 'normal',
  evidence_cmd        TEXT NOT NULL DEFAULT '',
  evidence_output     TEXT NOT NULL DEFAULT '',
  verified_run_id     TEXT NOT NULL DEFAULT '',
  alternatives        TEXT NOT NULL DEFAULT '[]',
  tags                TEXT NOT NULL DEFAULT '[]',
  git_commit          TEXT NOT NULL DEFAULT '',
  workstream_id       TEXT NOT NULL DEFAULT '',
  task_id             TEXT NOT NULL DEFAULT '',
  superseded_by       TEXT NOT NULL DEFAULT '',
  pinned              INTEGER NOT NULL DEFAULT 0,
  created_at          TEXT NOT NULL,
  updated_at          TEXT NOT NULL,
  actor               TEXT NOT NULL,
  revision            INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_knowledge_kind_outcome ON knowledge_entries(kind, outcome, verification_status);
CREATE INDEX IF NOT EXISTS idx_knowledge_workstream ON knowledge_entries(workstream_id);
CREATE TABLE IF NOT EXISTS knowledge_links (
  entry_id             TEXT NOT NULL,
  target_kind          TEXT NOT NULL,
  target_ref           TEXT NOT NULL,
  frn                  INTEGER NOT NULL DEFAULT 0,
  start_line           INTEGER NOT NULL DEFAULT 0,
  end_line             INTEGER NOT NULL DEFAULT 0,
  sha256_at_rec        TEXT NOT NULL DEFAULT '',
  symbol_sha256_at_rec TEXT NOT NULL DEFAULT '',
  PRIMARY KEY(entry_id, target_kind, target_ref)
);
CREATE INDEX IF NOT EXISTS idx_knowledge_links_frn ON knowledge_links(frn);
CREATE INDEX IF NOT EXISTS idx_knowledge_links_ref ON knowledge_links(target_ref COLLATE NOCASE);
CREATE VIRTUAL TABLE IF NOT EXISTS knowledge_fts USING fts5(
  entry_id UNINDEXED,
  kind,
  title,
  summary,
  details,
  tags,
  tokenize='unicode61 remove_diacritics 2'
);
)sql";
            if (sqlite3_exec(db.native_handle(), sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::string msg = err ? err : sqlite3_errmsg(db.native_handle());
                if (err) sqlite3_free(err);
                throw std::runtime_error(msg);
            }
            db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('knowledge_schema','1')");
        }
        if (need_tx) db.commit();
    } catch (...) {
        if (need_tx) db.rollback();
        throw;
    }
}

void knowledge_invalidate_for_path(Database& db, uint64_t frn, const std::string& rel_path,
                                   const std::string& new_sha256) {
    knowledge_schema(db);
    const std::string norm = norm_rel_kn(rel_path);
    Json rows = Json::array();
    if (frn > 0 && !norm.empty()) {
        rows = db.query(
            "SELECT DISTINCT entry_id, sha256_at_rec FROM knowledge_links "
            "WHERE (frn=? OR target_ref=? COLLATE NOCASE)",
            {static_cast<int64_t>(frn), norm});
    } else if (frn > 0) {
        rows = db.query(
            "SELECT DISTINCT entry_id, sha256_at_rec FROM knowledge_links WHERE frn=?",
            {static_cast<int64_t>(frn)});
    } else if (!norm.empty()) {
        rows = db.query(
            "SELECT DISTINCT entry_id, sha256_at_rec FROM knowledge_links WHERE target_ref=? COLLATE NOCASE",
            {norm});
    }
    if (rows.empty()) return;

    const std::string ts = now();
    for (const auto& r : rows) {
        const std::string rec_sha = r["sha256_at_rec"].get<std::string>();
        if (!rec_sha.empty() && (new_sha256.empty() || rec_sha != new_sha256)) {
            db.exec(
                "UPDATE knowledge_entries SET verification_status='stale', updated_at=? "
                "WHERE id=? AND verification_status!='stale'",
                {ts, r["entry_id"].get<std::string>()});
        }
    }
}

Json knowledge_warnings_for_files(const Project& p, Database& db, const std::vector<std::string>& rel_paths,
                                  const std::vector<uint64_t>& frns, int limit) {
    knowledge_schema(db);
    const int max_items = std::clamp(limit, 1, 100);

    std::vector<std::string> norm_paths;
    std::set<std::string> path_set;
    std::set<uint64_t> frn_set;
    for (uint64_t f : frns) {
        if (f > 0) frn_set.insert(f);
    }
    for (const auto& rp : rel_paths) {
        std::string np = to_project_rel_kn(p, rp);
        if (np.empty()) continue;
        if (path_set.insert(lower_kn(np)).second) {
            norm_paths.push_back(np);
        }
        fs::path abs_p = resolve_project_file_kn(p, np);
        if (!abs_p.empty()) {
            uint64_t f = query_path_frn(abs_p);
            if (f > 0) frn_set.insert(f);
        }
    }

    std::unordered_set<std::string> candidate_ids;
    for (const auto& np : norm_paths) {
        auto rows = db.query(
            "SELECT DISTINCT entry_id FROM knowledge_links WHERE target_ref=? COLLATE NOCASE",
            {np});
        for (const auto& r : rows) candidate_ids.insert(r["entry_id"].get<std::string>());
    }
    for (uint64_t f : frn_set) {
        auto rows = db.query(
            "SELECT DISTINCT entry_id FROM knowledge_links WHERE frn=?",
            {static_cast<int64_t>(f)});
        for (const auto& r : rows) candidate_ids.insert(r["entry_id"].get<std::string>());
    }

    // Also include globally pinned pitfalls / failed methods that have no specific file links
    auto global_pinned = db.query(
        "SELECT e.id FROM knowledge_entries e "
        "WHERE e.superseded_by='' AND e.pinned=1 AND "
        "NOT EXISTS(SELECT 1 FROM knowledge_links l WHERE l.entry_id=e.id)");
    for (const auto& r : global_pinned) {
        candidate_ids.insert(r["id"].get<std::string>());
    }

    struct ScoredWarning {
        int priority_score = 0;
        std::string updated_at;
        Json entry;
    };
    std::vector<ScoredWarning> scored;
    int64_t stale_count = 0;
    int64_t pitfall_count = 0;
    int64_t failed_method_count = 0;

    for (const auto& cid : candidate_ids) {
        auto erows = db.query("SELECT * FROM knowledge_entries WHERE id=? AND superseded_by=''", {cid});
        if (erows.empty()) continue;
        Json formatted = format_entry_row(p, db, erows[0], true);
        const std::string kind = formatted["kind"].get<std::string>();
        const std::string outcome = formatted["outcome"].get<std::string>();
        const std::string severity = formatted["severity"].get<std::string>();
        const std::string vstatus = formatted["verification_status"].get<std::string>();
        const bool pinned = formatted["pinned"].get<bool>();

        if (vstatus == "stale" || formatted["needs_reverification"].get<bool>()) ++stale_count;
        if (kind == "pitfall") ++pitfall_count;
        if (kind == "method_tried" && outcome == "failed") ++failed_method_count;

        int score = 0;
        if (pinned) score += 1000;
        if (kind == "pitfall") score += 500;
        else if (kind == "method_tried" && outcome == "failed") score += 480;
        else if (kind == "decision") score += 350;
        else if (kind == "finding") score += 250;
        else if (kind == "method_tried") score += 200;
        else score += 100;

        if (outcome == "failed" || outcome == "rejected") score += 150;
        if (severity == "critical") score += 120;
        else if (severity == "high") score += 80;
        else if (severity == "normal") score += 40;

        scored.push_back(ScoredWarning{score, formatted["updated_at"].get<std::string>(), std::move(formatted)});
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredWarning& a, const ScoredWarning& b) {
        if (a.priority_score != b.priority_score) return a.priority_score > b.priority_score;
        return a.updated_at > b.updated_at;
    });

    Json items = Json::array();
    for (size_t i = 0; i < scored.size() && static_cast<int>(i) < max_items; ++i) {
        items.push_back(std::move(scored[i].entry));
    }

    return {
        {"schema", "migratool.knowledge/1"},
        {"ok", true},
        {"action", "for_files"},
        {"files", norm_paths},
        {"count", items.size()},
        {"stale_count", stale_count},
        {"pitfall_count", pitfall_count},
        {"failed_method_count", failed_method_count},
        {"items", items},
        {"warnings", items}
    };
}

Json tool_knowledge(const Project& p, Database& db, const Json& args) {
    knowledge_schema(db);
    const std::string action = lower_kn(args.value("action", args.value("op", "list")));
    const std::string actor = args.value("actor", "agent");

    if (action == "record" || action == "add" || action == "create") {
        const std::string idem_key = trim_kn(args.value("idempotency_key", ""));
        std::string args_sha;
        if (!idem_key.empty()) {
            workstream_schema(db);
            args_sha = sha256(args.dump());
            if (auto cached = check_idempotency_ledger(db, idem_key, "knowledge", args_sha)) {
                Json replay = *cached;
                replay["idempotent_replay"] = true;
                return replay;
            }
        }

        const std::string id = trim_kn(args.value("id", args.value("entry_id", "kn_" + unique_id().substr(0, 16))));
        if (id.empty()) throw std::runtime_error("knowledge(record) requiere un id válido");

        const std::string kind = normalize_kind_kn(args.value("kind", "finding"));
        std::string title = trim_kn(args.value("title", args.value("name", "")));
        std::string summary = trim_kn(args.value("summary", args.value("content", args.value("description", ""))));
        if (title.empty() && !summary.empty()) {
            title = summary.substr(0, std::min<size_t>(summary.size(), 80));
        }
        if (summary.empty() && !title.empty()) {
            summary = title;
        }
        if (title.empty() || summary.empty()) {
            throw std::runtime_error("knowledge(record) requiere title o summary");
        }

        const std::string details = args.value("details", args.value("body", ""));
        const std::string outcome = normalize_outcome_kn(args.value("outcome", "active"));
        const double confidence = std::clamp(args.value("confidence", 1.0), 0.0, 1.0);
        const std::string severity = normalize_severity_kn(args.value("severity", "normal"));
        const std::string evidence_cmd = args.value("evidence_cmd", "");
        const std::string evidence_output = args.value("evidence_output", "");
        const std::string verified_run_id = args.value("verified_run_id", "");
        const std::string vstatus = resolve_verification_status_kn(
            args, verified_run_id, evidence_cmd, evidence_output, confidence);

        const Json alternatives = parse_json_array_or_wrap(args.value("alternatives", Json::array()));
        const Json tags = parse_json_array_or_wrap(args.value("tags", Json::array()));
        std::string git_commit = args.value("git_commit", "");
        if (git_commit.empty()) git_commit = read_git_head_fast_kn(p);

        std::string workstream_id = args.value("workstream_id", "");
        if (workstream_id.empty()) {
            try { workstream_id = active_workstream_id(db, actor); } catch (...) {}
        }
        const std::string task_id = args.value("task_id", "");
        const std::string superseded_by = args.value("superseded_by", "");
        int pinned = 0;
        if (args.contains("pinned")) {
            if (args["pinned"].is_boolean()) pinned = args["pinned"].get<bool>() ? 1 : 0;
            else if (args["pinned"].is_number_integer()) pinned = args["pinned"].get<int>() != 0 ? 1 : 0;
        }

        auto resolved_links = extract_links_from_args(p, db, args);
        const std::string ts = now();
        int64_t rev = 0;

        db.begin();
        try {
            rev = db.change(actor, "knowledge_record",
                            {{"id", id}, {"kind", kind}, {"title", title}, {"outcome", outcome}, {"verification_status", vstatus}},
                            "K:" + id);
            db.exec(
                "INSERT INTO knowledge_entries("
                "id,kind,title,summary,details,outcome,verification_status,confidence,severity,"
                "evidence_cmd,evidence_output,verified_run_id,alternatives,tags,git_commit,"
                "workstream_id,task_id,superseded_by,pinned,created_at,updated_at,actor,revision"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(id) DO UPDATE SET "
                "kind=excluded.kind, title=excluded.title, summary=excluded.summary, details=excluded.details, "
                "outcome=excluded.outcome, verification_status=excluded.verification_status, "
                "confidence=excluded.confidence, severity=excluded.severity, "
                "evidence_cmd=excluded.evidence_cmd, evidence_output=excluded.evidence_output, "
                "verified_run_id=excluded.verified_run_id, alternatives=excluded.alternatives, "
                "tags=excluded.tags, git_commit=excluded.git_commit, workstream_id=excluded.workstream_id, "
                "task_id=excluded.task_id, superseded_by=excluded.superseded_by, pinned=excluded.pinned, "
                "updated_at=excluded.updated_at, actor=excluded.actor, revision=excluded.revision",
                {id, kind, title, summary, details, outcome, vstatus, confidence, severity,
                 evidence_cmd, evidence_output, verified_run_id, alternatives.dump(), tags.dump(),
                 git_commit, workstream_id, task_id, superseded_by, pinned, ts, ts, actor, rev});

            db.exec("DELETE FROM knowledge_links WHERE entry_id=?", {id});
            for (const auto& lk : resolved_links) {
                db.exec(
                    "INSERT OR REPLACE INTO knowledge_links("
                    "entry_id,target_kind,target_ref,frn,start_line,end_line,sha256_at_rec,symbol_sha256_at_rec"
                    ") VALUES(?,?,?,?,?,?,?,?)",
                    {id, lk.target_kind, lk.target_ref, static_cast<int64_t>(lk.frn),
                     lk.start_line, lk.end_line, lk.sha256_at_rec, lk.symbol_sha256_at_rec});
            }

            db.exec("DELETE FROM knowledge_fts WHERE entry_id=?", {id});
            db.exec(
                "INSERT INTO knowledge_fts(entry_id,kind,title,summary,details,tags) VALUES(?,?,?,?,?,?)",
                {id, kind, title, summary, details, tags.dump()});

            const std::string supersedes_old = trim_kn(args.value("supersedes", ""));
            if (!supersedes_old.empty() && supersedes_old != id) {
                db.exec(
                    "UPDATE knowledge_entries SET superseded_by=?, outcome='superseded', updated_at=?, revision=? WHERE id=?",
                    {id, ts, rev, supersedes_old});
            }

            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }

        auto erows = db.query("SELECT * FROM knowledge_entries WHERE id=?", {id});
        Json entry_obj = erows.empty() ? Json::object() : format_entry_row(p, db, erows[0], false);

        Json resp = {
            {"schema", "migratool.knowledge/1"},
            {"ok", true},
            {"action", "record"},
            {"id", id},
            {"entry_id", id},
            {"kind", kind},
            {"title", title},
            {"summary", summary},
            {"outcome", outcome},
            {"verification_status", vstatus},
            {"epistemic_label", epistemic_label_kn(vstatus)},
            {"is_verified_fact", vstatus == "verified_fact"},
            {"needs_reverification", vstatus != "verified_fact" && vstatus != "test_result"},
            {"confidence", confidence},
            {"severity", severity},
            {"pinned", pinned != 0},
            {"links", entry_obj.value("links", Json::array())},
            {"revision", rev},
            {"entry", std::move(entry_obj)}
        };
        if (!idem_key.empty()) {
            store_idempotency_ledger(db, idem_key, "knowledge", args_sha, resp);
        }
        return resp;
    }

    if (action == "get" || action == "read") {
        const std::string id = trim_kn(args.value("id", args.value("entry_id", "")));
        if (id.empty()) throw std::runtime_error("knowledge(get) requiere id");
        auto rows = db.query("SELECT * FROM knowledge_entries WHERE id=?", {id});
        if (rows.empty()) throw std::runtime_error("KNOWLEDGE_ENTRY_NOT_FOUND: " + id);
        Json entry = format_entry_row(p, db, rows[0], true);
        Json resp = entry;
        resp["schema"] = "migratool.knowledge/1";
        resp["ok"] = true;
        resp["action"] = "get";
        resp["entry"] = entry;
        return resp;
    }

    if (action == "list") {
        const std::string kind_f = trim_kn(args.value("kind", ""));
        const std::string outcome_f = trim_kn(args.value("outcome", ""));
        const std::string vstatus_f = trim_kn(args.value("verification_status", ""));
        const std::string tag_f = trim_kn(args.value("tag", ""));
        const std::string ws_f = trim_kn(args.value("workstream_id", ""));
        const bool pinned_only = args.value("pinned_only", false);
        const bool include_superseded = args.value("include_superseded", true);
        const int limit = std::clamp(args.value("limit", 50), 1, 500);

        auto rows = db.query(
            "SELECT * FROM knowledge_entries ORDER BY pinned DESC, updated_at DESC, id ASC");
        Json items = Json::array();
        for (const auto& r : rows) {
            if (!include_superseded && !r["superseded_by"].get<std::string>().empty()) continue;
            if (pinned_only && r["pinned"].get<int64_t>() == 0) continue;
            if (!kind_f.empty() && r["kind"].get<std::string>() != normalize_kind_kn(kind_f)) continue;
            if (!outcome_f.empty() && r["outcome"].get<std::string>() != normalize_outcome_kn(outcome_f)) continue;
            if (!ws_f.empty() && r["workstream_id"].get<std::string>() != ws_f) continue;

            Json formatted = format_entry_row(p, db, r, true);
            if (!vstatus_f.empty() && formatted["verification_status"].get<std::string>() != lower_kn(vstatus_f)) {
                continue;
            }
            if (!tag_f.empty()) {
                bool found_tag = false;
                for (const auto& t : formatted["tags"]) {
                    if (t.is_string() && lower_kn(t.get<std::string>()) == lower_kn(tag_f)) {
                        found_tag = true;
                        break;
                    }
                }
                if (!found_tag) continue;
            }
            items.push_back(std::move(formatted));
            if (static_cast<int>(items.size()) >= limit) break;
        }
        return {
            {"schema", "migratool.knowledge/1"},
            {"ok", true},
            {"action", "list"},
            {"count", items.size()},
            {"items", std::move(items)}
        };
    }

    if (action == "search") {
        const std::string q = trim_kn(args.value("query", args.value("text", args.value("q", ""))));
        const std::string kind_f = trim_kn(args.value("kind", ""));
        const std::string outcome_f = trim_kn(args.value("outcome", ""));
        const std::string vstatus_f = trim_kn(args.value("verification_status", ""));
        const bool include_superseded = args.value("include_superseded", false);
        const int limit = std::clamp(args.value("limit", 20), 1, 200);

        std::vector<std::string> matched_ids;
        std::unordered_set<std::string> seen_ids;

        if (!q.empty()) {
            try {
                auto fts_rows = db.query(
                    "SELECT entry_id FROM knowledge_fts WHERE knowledge_fts MATCH ? ORDER BY bm25(knowledge_fts) LIMIT ?",
                    {fts_quote_kn(q), limit * 3});
                for (const auto& r : fts_rows) {
                    const std::string eid = r["entry_id"].get<std::string>();
                    if (seen_ids.insert(eid).second) matched_ids.push_back(eid);
                }
            } catch (...) {}

            if (matched_ids.empty()) {
                const std::string pat = "%" + q + "%";
                auto like_rows = db.query(
                    "SELECT id FROM knowledge_entries "
                    "WHERE title LIKE ? OR summary LIKE ? OR details LIKE ? OR tags LIKE ? "
                    "ORDER BY pinned DESC, updated_at DESC LIMIT ?",
                    {pat, pat, pat, pat, limit * 3});
                for (const auto& r : like_rows) {
                    const std::string eid = r["id"].get<std::string>();
                    if (seen_ids.insert(eid).second) matched_ids.push_back(eid);
                }
            }
        } else {
            auto all_rows = db.query(
                "SELECT id FROM knowledge_entries ORDER BY pinned DESC, updated_at DESC LIMIT ?",
                {limit * 3});
            for (const auto& r : all_rows) {
                const std::string eid = r["id"].get<std::string>();
                if (seen_ids.insert(eid).second) matched_ids.push_back(eid);
            }
        }

        Json items = Json::array();
        for (const auto& eid : matched_ids) {
            auto erows = db.query("SELECT * FROM knowledge_entries WHERE id=?", {eid});
            if (erows.empty()) continue;
            const auto& r = erows[0];
            if (!include_superseded && !r["superseded_by"].get<std::string>().empty()) continue;
            if (!kind_f.empty() && r["kind"].get<std::string>() != normalize_kind_kn(kind_f)) continue;
            if (!outcome_f.empty() && r["outcome"].get<std::string>() != normalize_outcome_kn(outcome_f)) continue;

            Json formatted = format_entry_row(p, db, r, true);
            if (!vstatus_f.empty() && formatted["verification_status"].get<std::string>() != lower_kn(vstatus_f)) {
                continue;
            }
            items.push_back(std::move(formatted));
            if (static_cast<int>(items.size()) >= limit) break;
        }

        return {
            {"schema", "migratool.knowledge/1"},
            {"ok", true},
            {"action", "search"},
            {"query", q},
            {"count", items.size()},
            {"items", std::move(items)}
        };
    }

    if (action == "update") {
        const std::string id = trim_kn(args.value("id", args.value("entry_id", "")));
        if (id.empty()) throw std::runtime_error("knowledge(update) requiere id");
        auto rows = db.query("SELECT * FROM knowledge_entries WHERE id=?", {id});
        if (rows.empty()) throw std::runtime_error("KNOWLEDGE_ENTRY_NOT_FOUND: " + id);
        const auto& cur = rows[0];

        const std::string kind = args.contains("kind")
            ? normalize_kind_kn(args["kind"].get<std::string>())
            : cur["kind"].get<std::string>();
        const std::string title = args.value("title", cur["title"].get<std::string>());
        const std::string summary = args.value("summary", cur["summary"].get<std::string>());
        const std::string details = args.value("details", cur["details"].get<std::string>());
        const std::string outcome = args.contains("outcome")
            ? normalize_outcome_kn(args["outcome"].get<std::string>())
            : cur["outcome"].get<std::string>();
        const double confidence = std::clamp(args.value("confidence", cur["confidence"].get<double>()), 0.0, 1.0);
        const std::string severity = args.contains("severity")
            ? normalize_severity_kn(args["severity"].get<std::string>())
            : cur["severity"].get<std::string>();
        const std::string evidence_cmd = args.value("evidence_cmd", cur["evidence_cmd"].get<std::string>());
        const std::string evidence_output = args.value("evidence_output", cur["evidence_output"].get<std::string>());
        const std::string verified_run_id = args.value("verified_run_id", cur["verified_run_id"].get<std::string>());
        const std::string vstatus = args.contains("verification_status") || args.contains("verified_run_id")
            ? resolve_verification_status_kn(args, verified_run_id, evidence_cmd, evidence_output, confidence)
            : cur["verification_status"].get<std::string>();

        const Json alternatives = args.contains("alternatives")
            ? parse_json_array_or_wrap(args["alternatives"])
            : Json::parse(cur["alternatives"].get<std::string>(), nullptr, false);
        const Json tags = args.contains("tags")
            ? parse_json_array_or_wrap(args["tags"])
            : Json::parse(cur["tags"].get<std::string>(), nullptr, false);

        int pinned = static_cast<int>(cur["pinned"].get<int64_t>());
        if (args.contains("pinned")) {
            if (args["pinned"].is_boolean()) pinned = args["pinned"].get<bool>() ? 1 : 0;
            else if (args["pinned"].is_number_integer()) pinned = args["pinned"].get<int>() != 0 ? 1 : 0;
        }
        const std::string workstream_id = args.value("workstream_id", cur["workstream_id"].get<std::string>());
        const std::string task_id = args.value("task_id", cur["task_id"].get<std::string>());
        const std::string superseded_by = args.value("superseded_by", cur["superseded_by"].get<std::string>());

        const bool has_new_links = args.contains("files") || args.contains("links") ||
                                   args.contains("file") || args.contains("path") || args.contains("paths");
        auto new_links = has_new_links ? extract_links_from_args(p, db, args) : std::vector<ResolvedLinkKn>{};

        const std::string ts = now();
        int64_t rev = 0;
        db.begin();
        try {
            rev = db.change(actor, "knowledge_update", {{"id", id}, {"verification_status", vstatus}}, "K:" + id);
            db.exec(
                "UPDATE knowledge_entries SET kind=?, title=?, summary=?, details=?, outcome=?, "
                "verification_status=?, confidence=?, severity=?, evidence_cmd=?, evidence_output=?, "
                "verified_run_id=?, alternatives=?, tags=?, workstream_id=?, task_id=?, superseded_by=?, "
                "pinned=?, updated_at=?, actor=?, revision=? WHERE id=?",
                {kind, title, summary, details, outcome, vstatus, confidence, severity,
                 evidence_cmd, evidence_output, verified_run_id, alternatives.dump(), tags.dump(),
                 workstream_id, task_id, superseded_by, pinned, ts, actor, rev, id});

            if (has_new_links) {
                db.exec("DELETE FROM knowledge_links WHERE entry_id=?", {id});
                for (const auto& lk : new_links) {
                    db.exec(
                        "INSERT OR REPLACE INTO knowledge_links("
                        "entry_id,target_kind,target_ref,frn,start_line,end_line,sha256_at_rec,symbol_sha256_at_rec"
                        ") VALUES(?,?,?,?,?,?,?,?)",
                        {id, lk.target_kind, lk.target_ref, static_cast<int64_t>(lk.frn),
                         lk.start_line, lk.end_line, lk.sha256_at_rec, lk.symbol_sha256_at_rec});
                }
            }

            db.exec("DELETE FROM knowledge_fts WHERE entry_id=?", {id});
            db.exec(
                "INSERT INTO knowledge_fts(entry_id,kind,title,summary,details,tags) VALUES(?,?,?,?,?,?)",
                {id, kind, title, summary, details, tags.dump()});
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }

        auto updated_rows = db.query("SELECT * FROM knowledge_entries WHERE id=?", {id});
        Json entry = format_entry_row(p, db, updated_rows[0], true);
        Json resp = entry;
        resp["schema"] = "migratool.knowledge/1";
        resp["ok"] = true;
        resp["action"] = "update";
        resp["entry"] = entry;
        return resp;
    }

    if (action == "supersede") {
        const std::string old_id = trim_kn(args.value("old_id", args.value("supersedes", args.value("id", args.value("entry_id", "")))));
        if (old_id.empty()) throw std::runtime_error("knowledge(supersede) requiere id u old_id");
        auto old_rows = db.query("SELECT * FROM knowledge_entries WHERE id=?", {old_id});
        if (old_rows.empty()) throw std::runtime_error("KNOWLEDGE_ENTRY_NOT_FOUND: " + old_id);
        const auto& old_e = old_rows[0];

        std::string new_id = trim_kn(args.value("new_id", args.value("superseded_by", "")));
        const bool create_new = new_id.empty() ||
                                db.integer("SELECT EXISTS(SELECT 1 FROM knowledge_entries WHERE id=?)", {new_id}) == 0 ||
                                args.contains("title") || args.contains("summary") || args.contains("new_entry");

        Json new_entry_obj = Json::object();
        if (create_new) {
            Json rec_args = args.contains("new_entry") && args["new_entry"].is_object()
                ? args["new_entry"]
                : args;
            rec_args["action"] = "record";
            if (!new_id.empty() && new_id != old_id) {
                rec_args["id"] = new_id;
            } else {
                rec_args["id"] = "kn_" + unique_id().substr(0, 16);
            }
            if (!rec_args.contains("kind")) rec_args["kind"] = old_e["kind"];
            if (!rec_args.contains("title")) rec_args["title"] = old_e["title"].get<std::string>() + " (revised)";
            if (!rec_args.contains("summary")) rec_args["summary"] = old_e["summary"];
            if (!rec_args.contains("details")) rec_args["details"] = old_e["details"];
            if (!rec_args.contains("tags")) {
                rec_args["tags"] = Json::parse(old_e["tags"].get<std::string>(), nullptr, false);
            }
            if (!rec_args.contains("files") && !rec_args.contains("links")) {
                auto old_links = db.query(
                    "SELECT target_kind, target_ref, frn, start_line, end_line FROM knowledge_links WHERE entry_id=?",
                    {old_id});
                if (!old_links.empty()) rec_args["links"] = old_links;
            }
            rec_args["supersedes"] = old_id;
            auto rec_res = tool_knowledge(p, db, rec_args);
            new_id = rec_res["id"].get<std::string>();
            new_entry_obj = rec_res["entry"];
        } else {
            const std::string ts = now();
            db.begin();
            try {
                const int64_t rev = db.change(actor, "knowledge_supersede",
                                              {{"old_id", old_id}, {"new_id", new_id}}, "K:" + old_id);
                db.exec(
                    "UPDATE knowledge_entries SET superseded_by=?, outcome='superseded', updated_at=?, actor=?, revision=? WHERE id=?",
                    {new_id, ts, actor, rev, old_id});
                db.commit();
            } catch (...) {
                db.rollback();
                throw;
            }
            auto nr = db.query("SELECT * FROM knowledge_entries WHERE id=?", {new_id});
            if (!nr.empty()) new_entry_obj = format_entry_row(p, db, nr[0], true);
        }

        return {
            {"schema", "migratool.knowledge/1"},
            {"ok", true},
            {"action", "supersede"},
            {"old_id", old_id},
            {"superseded_id", old_id},
            {"new_id", new_id},
            {"superseded_by", new_id},
            {"id", new_id},
            {"entry", std::move(new_entry_obj)}
        };
    }

    if (action == "verify") {
        const std::string id = trim_kn(args.value("id", args.value("entry_id", "")));
        if (id.empty()) throw std::runtime_error("knowledge(verify) requiere id");
        auto rows = db.query("SELECT * FROM knowledge_entries WHERE id=?", {id});
        if (rows.empty()) throw std::runtime_error("KNOWLEDGE_ENTRY_NOT_FOUND: " + id);

        auto link_rows = db.query(
            "SELECT target_kind, target_ref, frn, start_line, end_line FROM knowledge_links WHERE entry_id=?",
            {id});

        const std::string ts = now();
        const std::string verified_run_id = args.value("verified_run_id", rows[0]["verified_run_id"].get<std::string>());
        const std::string evidence_cmd = args.value("evidence_cmd", rows[0]["evidence_cmd"].get<std::string>());
        const std::string evidence_output = args.value("evidence_output", rows[0]["evidence_output"].get<std::string>());
        const std::string new_vstatus = args.value("verification_status", "verified_fact");

        int64_t rev = 0;
        db.begin();
        try {
            for (const auto& lr : link_rows) {
                const std::string tkind = lr["target_kind"].get<std::string>();
                const std::string tref = lr["target_ref"].get<std::string>();
                const int sline = static_cast<int>(lr["start_line"].get<int64_t>());
                const int eline = static_cast<int>(lr["end_line"].get<int64_t>());
                fs::path abs_p = resolve_project_file_kn(p, tref);
                std::error_code ec;
                if (!abs_p.empty() && fs::is_regular_file(native_path(abs_p), ec)) {
                    const uint64_t frn = query_path_frn(abs_p);
                    const std::string sha = hash_file(abs_p);
                    const std::string sym_sha = (sline > 0) ? compute_symbol_or_range_sha(abs_p, sline, eline, "") : "";
                    db.exec(
                        "UPDATE knowledge_links SET frn=?, sha256_at_rec=?, symbol_sha256_at_rec=? "
                        "WHERE entry_id=? AND target_kind=? AND target_ref=?",
                        {static_cast<int64_t>(frn), sha, sym_sha, id, tkind, tref});
                }
            }
            rev = db.change(actor, "knowledge_verify", {{"id", id}, {"verification_status", new_vstatus}}, "K:" + id);
            db.exec(
                "UPDATE knowledge_entries SET verification_status=?, verified_run_id=?, evidence_cmd=?, "
                "evidence_output=?, updated_at=?, actor=?, revision=? WHERE id=?",
                {new_vstatus, verified_run_id, evidence_cmd, evidence_output, ts, actor, rev, id});
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }

        auto updated = db.query("SELECT * FROM knowledge_entries WHERE id=?", {id});
        Json entry = format_entry_row(p, db, updated[0], false);
        Json resp = entry;
        resp["schema"] = "migratool.knowledge/1";
        resp["ok"] = true;
        resp["action"] = "verify";
        resp["entry"] = entry;
        return resp;
    }

    if (action == "for_files" || action == "warnings") {
        std::vector<std::string> rel_paths;
        std::vector<uint64_t> frns;
        for (const char* field : {"files", "paths", "rel_paths"}) {
            if (args.contains(field) && args[field].is_array()) {
                for (const auto& it : args[field]) {
                    if (it.is_string()) rel_paths.push_back(it.get<std::string>());
                }
            }
        }
        if (args.contains("path") && args["path"].is_string()) {
            rel_paths.push_back(args["path"].get<std::string>());
        }
        if (args.contains("file") && args["file"].is_string()) {
            rel_paths.push_back(args["file"].get<std::string>());
        }
        if (args.contains("frns") && args["frns"].is_array()) {
            for (const auto& f : args["frns"]) {
                if (f.is_number_integer() || f.is_number_unsigned()) {
                    frns.push_back(static_cast<uint64_t>(f.get<int64_t>()));
                }
            }
        }
        const int limit = args.value("limit", 10);
        return knowledge_warnings_for_files(p, db, rel_paths, frns, limit);
    }

    throw std::runtime_error(
        "knowledge.action desconocida (use record, search, list, get, update, supersede, verify o for_files): " + action);
}

} // namespace mt
