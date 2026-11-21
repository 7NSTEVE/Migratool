#include "migratool/docs_hub.hpp"
#include "migratool/fs_index.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mt {
namespace {

std::string lower_dh(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string norm_rel_dh(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.starts_with("./")) p.erase(0, 2);
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string slug_anchor(std::string_view heading) {
    std::string out;
    out.reserve(heading.size());
    for (char c : heading) {
        auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        } else if ((c == ' ' || c == '-' || c == '_') && !out.empty() && out.back() != '-') {
            out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "section" : out;
}

std::string fts_quote_dh(std::string_view q) {
    std::string out = "\"";
    for (char c : q) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::vector<std::string> split_lines_dh(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

bool is_doc_extension(const std::string& ext_low) {
    return ext_low == ".md" || ext_low == ".markdown" || ext_low == ".txt" ||
           ext_low == ".rst" || ext_low == ".adoc";
}

bool should_skip_doc_dir(const std::string& dir_name_low) {
    return dir_name_low == ".git" || dir_name_low == ".migratool" || dir_name_low == "node_modules" ||
           dir_name_low == "build" || dir_name_low == "dist" || dir_name_low == "out" ||
           dir_name_low == "target" || dir_name_low == ".vs" || dir_name_low == "__pycache__";
}

std::pair<std::string, std::string> classify_doc_kind_and_authority(const std::string& rel_path) {
    const std::string low = lower_dh(rel_path);
    if (low.find("agents.md") != std::string::npos || low.find("skill.md") != std::string::npos ||
        low.find("claude.md") != std::string::npos) {
        return {"agent_contract", "canonical"};
    }
    if (low.find("readme") != std::string::npos) {
        return {"readme", "canonical"};
    }
    if (low.find("plan") != std::string::npos || low.find("roadmap") != std::string::npos ||
        low.find("todo") != std::string::npos) {
        return {"plan", "active_plan"};
    }
    if (low.find("arch") != std::string::npos || low.find("design") != std::string::npos ||
        low.find("spec") != std::string::npos) {
        return {"architecture", "canonical"};
    }
    if (low.find("changelog") != std::string::npos || low.find("history") != std::string::npos ||
        low.find("audit") != std::string::npos) {
        return {"changelog", "historical"};
    }
    return {"guide", "reference"};
}

struct ParsedSection {
    std::string heading;
    int level = 1;
    std::string anchor;
    int start_line = 1;
    int end_line = 1;
    int char_count = 0;
    std::string body;
};

struct ParsedDoc {
    std::string title;
    std::string summary;
    std::vector<ParsedSection> sections;
    Json extracted_tasks = Json::array();
    Json linked_code = Json::array();
};

ParsedDoc parse_markdown_document(const std::string& rel_path, const std::vector<std::string>& lines) {
    ParsedDoc doc;
    doc.title = fs::path(wide(rel_path)).filename().string();

    bool in_fence = false;
    std::set<std::string> code_refs;
    static const std::regex code_re(R"(([A-Za-z0-9_./\\-]+\.(?:cpp|hpp|h|c|cc|cs|py|ts|tsx|js|jsx|rs|go|java|kt|cmake|sql|ps1|sh)|CMakeLists\.txt))");

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const std::string& raw = lines[idx];
        const int line_no = static_cast<int>(idx + 1);

        std::string trimmed = raw;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());

        if (trimmed.starts_with("```") || trimmed.starts_with("~~~")) {
            in_fence = !in_fence;
            continue;
        }

        // Extract code path mentions
        for (std::sregex_iterator it(raw.begin(), raw.end(), code_re), end; it != end; ++it) {
            std::string ref = norm_rel_dh((*it)[1].str());
            if (!ref.empty() && ref != rel_path) code_refs.insert(ref);
        }

        if (in_fence) continue;

        // Extract checklist items (- [ ] or - [x])
        if (trimmed.starts_with("- [ ] ") || trimmed.starts_with("* [ ] ")) {
            doc.extracted_tasks.push_back({{"line", line_no}, {"done", false}, {"text", trimmed.substr(6)}});
        } else if (trimmed.starts_with("- [x] ") || trimmed.starts_with("- [X] ") ||
                   trimmed.starts_with("* [x] ") || trimmed.starts_with("* [X] ")) {
            doc.extracted_tasks.push_back({{"line", line_no}, {"done", true}, {"text", trimmed.substr(6)}});
        }

        // Extract H1..H4 headings (# .. ####)
        if (!trimmed.empty() && trimmed.front() == '#') {
            int lvl = 0;
            while (lvl < static_cast<int>(trimmed.size()) && trimmed[static_cast<size_t>(lvl)] == '#') ++lvl;
            if (lvl >= 1 && lvl <= 4 && static_cast<size_t>(lvl) < trimmed.size() &&
                (trimmed[static_cast<size_t>(lvl)] == ' ' || trimmed[static_cast<size_t>(lvl)] == '\t')) {
                std::string htext = trimmed.substr(static_cast<size_t>(lvl) + 1);
                while (!htext.empty() && (htext.front() == ' ' || htext.front() == '\t')) htext.erase(htext.begin());
                while (!htext.empty() && (htext.back() == ' ' || htext.back() == '#' || htext.back() == '\t')) htext.pop_back();
                if (!htext.empty()) {
                    if (!doc.sections.empty()) {
                        doc.sections.back().end_line = std::max(doc.sections.back().start_line, line_no - 1);
                    }
                    if (lvl == 1 && doc.sections.empty()) {
                        doc.title = htext;
                    }
                    ParsedSection sec;
                    sec.heading = htext;
                    sec.level = lvl;
                    sec.anchor = slug_anchor(htext);
                    sec.start_line = line_no;
                    sec.end_line = static_cast<int>(lines.size());
                    doc.sections.push_back(std::move(sec));
                }
            }
        } else if (doc.summary.size() < 240 && !trimmed.empty() && trimmed.front() != '>' && trimmed.front() != '|') {
            if (!doc.summary.empty()) doc.summary += ' ';
            doc.summary += trimmed;
            if (doc.summary.size() > 240) doc.summary.resize(240);
        }
    }

    if (doc.sections.empty()) {
        ParsedSection sec;
        sec.heading = doc.title;
        sec.level = 1;
        sec.anchor = slug_anchor(doc.title);
        sec.start_line = 1;
        sec.end_line = std::max(1, static_cast<int>(lines.size()));
        doc.sections.push_back(std::move(sec));
    } else {
        doc.sections.back().end_line = std::max(doc.sections.back().start_line, static_cast<int>(lines.size()));
    }

    for (auto& sec : doc.sections) {
        std::ostringstream oss;
        for (int ln = sec.start_line; ln <= sec.end_line && ln <= static_cast<int>(lines.size()); ++ln) {
            if (ln > sec.start_line) oss << '\n';
            oss << lines[static_cast<size_t>(ln - 1)];
        }
        sec.body = oss.str();
        sec.char_count = static_cast<int>(sec.body.size());
    }

    for (const auto& r : code_refs) doc.linked_code.push_back(r);
    return doc;
}

int64_t file_mtime_ns_dh(const fs::path& p) {
    uint64_t pfrn = 0;
    int64_t sz = 0, mt = 0;
    uint32_t at = 0;
    query_path_frn(p, &pfrn, &sz, &mt, &at, nullptr);
    return mt;
}

fs::path resolve_project_file_dh(const Project& p, const std::string& raw_rel) {
    if (raw_rel.empty()) return {};
    const std::string norm = norm_rel_dh(raw_rel);
    fs::path fp(wide(norm));
    if (fp.is_absolute()) return fp;
    std::error_code ec;
    if (p.config.contains("roots") && p.config["roots"].is_array() && !p.config["roots"].empty()) {
        for (const auto& r : p.config["roots"]) {
            const std::string rpath = r.value("path", "");
            if (rpath.empty()) continue;
            const fs::path base = p.resolve(rpath);
            const fs::path cand = base / fp;
            if (fs::exists(native_path(cand), ec)) return cand;
            const fs::path direct = p.resolve(norm);
            if (within(direct, base) && fs::exists(native_path(direct), ec)) return direct;
        }
        const std::string first_rpath = p.config["roots"][0].value("path", "");
        if (!first_rpath.empty()) return p.resolve(first_rpath) / fp;
    }
    return p.resolve(norm);
}

} // namespace

void docs_hub_schema(Database& db) {
    if (db.integer("SELECT COUNT(*) FROM meta WHERE key='docs_hub_schema' AND value='1'") != 0) return;
    const bool need_tx = (sqlite3_get_autocommit(db.native_handle()) != 0);
    if (need_tx) db.begin();
    try {
        if (db.integer("SELECT COUNT(*) FROM meta WHERE key='docs_hub_schema' AND value='1'") == 0) {
            char* err = nullptr;
            const char* sql = R"sql(
CREATE TABLE IF NOT EXISTS project_docs (
  doc_key         TEXT PRIMARY KEY,
  root_key        TEXT NOT NULL DEFAULT 'root',
  rel_path        TEXT NOT NULL COLLATE NOCASE,
  frn             INTEGER NOT NULL DEFAULT 0,
  title           TEXT NOT NULL,
  doc_kind        TEXT NOT NULL DEFAULT 'guide',
  authority       TEXT NOT NULL DEFAULT 'reference',
  sha256          TEXT NOT NULL,
  bytes           INTEGER NOT NULL DEFAULT 0,
  line_count      INTEGER NOT NULL DEFAULT 0,
  mtime_ns        INTEGER NOT NULL DEFAULT 0,
  linked_code     TEXT NOT NULL DEFAULT '[]',
  extracted_tasks TEXT NOT NULL DEFAULT '[]',
  summary         TEXT NOT NULL DEFAULT '',
  owner           TEXT NOT NULL DEFAULT '',
  indexed_at      TEXT NOT NULL,
  verified_at     TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_project_docs_path ON project_docs(rel_path COLLATE NOCASE);

CREATE TABLE IF NOT EXISTS doc_sections (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  doc_key    TEXT NOT NULL,
  heading    TEXT NOT NULL,
  level      INTEGER NOT NULL,
  anchor     TEXT NOT NULL,
  start_line INTEGER NOT NULL,
  end_line   INTEGER NOT NULL,
  char_count INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_doc_sections_doc ON doc_sections(doc_key, start_line);

CREATE VIRTUAL TABLE IF NOT EXISTS doc_sections_fts USING fts5(
  doc_key UNINDEXED,
  anchor UNINDEXED,
  heading,
  body,
  tokenize='unicode61 remove_diacritics 2'
);
)sql";
            if (sqlite3_exec(db.native_handle(), sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::string msg = err ? err : sqlite3_errmsg(db.native_handle());
                if (err) sqlite3_free(err);
                throw std::runtime_error(msg);
            }
            db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('docs_hub_schema','1')");
        }
        if (need_tx) db.commit();
    } catch (...) {
        if (need_tx) db.rollback();
        throw;
    }
}

Json tool_docs_hub(const Project& p, Database& db, const Json& args) {
    docs_hub_schema(db);
    const std::string action = args.value("action", args.value("op", "list"));

    auto index_all_docs = [&]() -> Json {
        struct CandidateDoc {
            std::string root_key;
            std::string rel_path;
            fs::path abs_path;
        };
        std::vector<CandidateDoc> cands;
        std::set<std::string> seen_abs;

        for (const auto& r : p.config.value("roots", Json::array())) {
            const std::string rkey = r.value("id", "root");
            const fs::path rbase = p.resolve(r.value("path", ""));
            std::error_code ec;
            if (!fs::is_directory(native_path(rbase), ec)) continue;

            for (fs::recursive_directory_iterator it(native_path(rbase), fs::directory_options::skip_permission_denied, ec), end;
                 it != end && !ec; it.increment(ec)) {
                if (it->is_directory(ec)) {
                    const std::string dname = lower_dh(utf8(it->path().filename().wstring()));
                    if (should_skip_doc_dir(dname)) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = lower_dh(utf8(it->path().extension().wstring()));
                if (!is_doc_extension(ext)) continue;

                const fs::path abs_p = fs::path(it->path());
                const std::string abs_s = lower_dh(path_text(abs_p));
                if (!seen_abs.insert(abs_s).second) continue;

                fs::path rel_p = fs::relative(abs_p, native_path(rbase), ec);
                std::string rel = norm_rel_dh(path_text(rel_p));
                if (rel.empty()) rel = utf8(abs_p.filename().wstring());
                cands.push_back(CandidateDoc{rkey, rel, abs_p});
                if (cands.size() >= 500) break;
            }
        }

        const std::string ts = now();
        int64_t docs_indexed = 0, sections_indexed = 0;

        db.begin();
        try {
            for (const auto& c : cands) {
                std::string content;
                try { content = read_text(c.abs_path, 4 * 1024 * 1024); } catch (...) { continue; }
                const std::string digest = sha256(content);
                const auto lines = split_lines_dh(content);
                uint64_t pfrn = 0;
                int64_t sz = 0, mt = 0;
                uint32_t at = 0;
                const uint64_t frn = query_path_frn(c.abs_path, &pfrn, &sz, &mt, &at, nullptr);
                auto [dkind, auth] = classify_doc_kind_and_authority(c.rel_path);
                ParsedDoc parsed = parse_markdown_document(c.rel_path, lines);

                // Preserve existing manual linked_code additions if already present
                auto prev = db.query("SELECT linked_code, verified_at FROM project_docs WHERE doc_key=?", {c.rel_path});
                std::string verified_at = ts;
                if (!prev.empty()) {
                    verified_at = prev[0]["verified_at"].get<std::string>();
                    auto old_links = Json::parse(prev[0]["linked_code"].get<std::string>(), nullptr, false);
                    if (old_links.is_array()) {
                        for (const auto& ol : old_links) {
                            if (ol.is_string() && std::find(parsed.linked_code.begin(), parsed.linked_code.end(), ol) == parsed.linked_code.end()) {
                                parsed.linked_code.push_back(ol);
                            }
                        }
                    }
                }

                db.exec(
                    "INSERT OR REPLACE INTO project_docs("
                    "doc_key,root_key,rel_path,frn,title,doc_kind,authority,sha256,bytes,line_count,mtime_ns,"
                    "linked_code,extracted_tasks,summary,owner,indexed_at,verified_at)"
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    {c.rel_path, c.root_key, c.rel_path, static_cast<int64_t>(frn), parsed.title, dkind, auth,
                     digest, static_cast<int64_t>(content.size()), static_cast<int64_t>(lines.size()), mt,
                     parsed.linked_code.dump(), parsed.extracted_tasks.dump(), parsed.summary, "project", ts, verified_at});

                db.exec("DELETE FROM doc_sections WHERE doc_key=?", {c.rel_path});
                db.exec("DELETE FROM doc_sections_fts WHERE doc_key=?", {c.rel_path});

                for (const auto& sec : parsed.sections) {
                    db.exec(
                        "INSERT INTO doc_sections(doc_key,heading,level,anchor,start_line,end_line,char_count)"
                        "VALUES(?,?,?,?,?,?,?)",
                        {c.rel_path, sec.heading, sec.level, sec.anchor, sec.start_line, sec.end_line, sec.char_count});
                    db.exec(
                        "INSERT INTO doc_sections_fts(doc_key,anchor,heading,body) VALUES(?,?,?,?)",
                        {c.rel_path, sec.anchor, sec.heading, sec.body});
                    ++sections_indexed;
                }
                ++docs_indexed;
            }
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }

        return {
            {"schema", "migratool.docs_hub/1"},
            {"action", "index"},
            {"ok", true},
            {"docs_indexed", docs_indexed},
            {"sections_indexed", sections_indexed}
        };
    };

    if (action == "index") {
        return index_all_docs();
    }

    if (db.integer("SELECT COUNT(*) FROM project_docs") == 0) {
        index_all_docs();
    }

    if (action == "list") {
        auto rows = db.query(
            "SELECT d.doc_key, d.root_key, d.rel_path, d.frn, d.title, d.doc_kind, d.authority, d.sha256, d.bytes, d.line_count, "
            "d.linked_code, d.extracted_tasks, d.summary, d.indexed_at, d.verified_at, "
            "(SELECT COUNT(*) FROM doc_sections s WHERE s.doc_key=d.doc_key) AS section_count "
            "FROM project_docs d ORDER BY d.rel_path");
        return {
            {"schema", "migratool.docs_hub/1"},
            {"action", "list"},
            {"count", rows.size()},
            {"items", std::move(rows)}
        };
    }

    if (action == "outline") {
        const std::string doc_key = norm_rel_dh(args.value("path", args.value("doc_key", "")));
        if (doc_key.empty()) throw std::runtime_error("docs_hub(outline) requiere path o doc_key");
        auto doc = db.query("SELECT doc_key, title, doc_kind, authority, sha256, bytes, line_count, linked_code, extracted_tasks FROM project_docs WHERE doc_key=? COLLATE NOCASE", {doc_key});
        if (doc.empty()) {
            index_all_docs();
            doc = db.query("SELECT doc_key, title, doc_kind, authority, sha256, bytes, line_count, linked_code, extracted_tasks FROM project_docs WHERE doc_key=? COLLATE NOCASE", {doc_key});
        }
        if (doc.empty()) throw std::runtime_error("DOC_NOT_FOUND: " + doc_key);
        const std::string real_key = doc[0]["doc_key"].get<std::string>();
        auto secs = db.query(
            "SELECT id, heading, level, anchor, start_line, end_line, char_count "
            "FROM doc_sections WHERE doc_key=? ORDER BY start_line ASC",
            {real_key});
        return {
            {"schema", "migratool.docs_hub/1"},
            {"action", "outline"},
            {"doc_key", real_key},
            {"title", doc[0]["title"]},
            {"doc_kind", doc[0]["doc_kind"]},
            {"authority", doc[0]["authority"]},
            {"line_count", doc[0]["line_count"]},
            {"section_count", secs.size()},
            {"sections", std::move(secs)}
        };
    }

    if (action == "read_section") {
        const std::string doc_key = norm_rel_dh(args.value("path", args.value("doc_key", "")));
        if (doc_key.empty()) throw std::runtime_error("docs_hub(read_section) requiere path o doc_key");
        const std::string anchor = lower_dh(args.value("anchor", ""));
        const std::string heading = args.value("heading", "");
        const int64_t sec_id = args.value("section_id", int64_t{0});

        Json rows;
        if (sec_id > 0) {
            rows = db.query("SELECT id, doc_key, heading, level, anchor, start_line, end_line FROM doc_sections WHERE id=?", {sec_id});
        } else if (!anchor.empty()) {
            rows = db.query("SELECT id, doc_key, heading, level, anchor, start_line, end_line FROM doc_sections WHERE doc_key=? COLLATE NOCASE AND (anchor=? COLLATE NOCASE OR anchor LIKE ? COLLATE NOCASE) ORDER BY start_line LIMIT 1",
                            {doc_key, anchor, "%" + anchor + "%"});
        } else if (!heading.empty()) {
            rows = db.query("SELECT id, doc_key, heading, level, anchor, start_line, end_line FROM doc_sections WHERE doc_key=? COLLATE NOCASE AND heading LIKE ? COLLATE NOCASE ORDER BY start_line LIMIT 1",
                            {doc_key, "%" + heading + "%"});
        } else {
            rows = db.query("SELECT id, doc_key, heading, level, anchor, start_line, end_line FROM doc_sections WHERE doc_key=? COLLATE NOCASE ORDER BY start_line LIMIT 1",
                            {doc_key});
        }
        if (rows.empty()) throw std::runtime_error("DOC_SECTION_NOT_FOUND en " + doc_key);

        const auto& s = rows[0];
        const std::string rkey = s["doc_key"].get<std::string>();
        const fs::path abs = resolve_project_file_dh(p, rkey);
        const auto lines = split_lines_dh(read_text(abs, 4 * 1024 * 1024));
        const int sl = std::clamp(static_cast<int>(s["start_line"].get<int64_t>()), 1, std::max(1, static_cast<int>(lines.size())));
        const int el = std::clamp(static_cast<int>(s["end_line"].get<int64_t>()), sl, std::max(1, static_cast<int>(lines.size())));
        std::ostringstream oss;
        for (int i = sl; i <= el && i <= static_cast<int>(lines.size()); ++i) {
            if (i > sl) oss << '\n';
            oss << lines[static_cast<size_t>(i - 1)];
        }
        return {
            {"schema", "migratool.docs_hub/1"},
            {"action", "read_section"},
            {"doc_key", rkey},
            {"heading", s["heading"]},
            {"level", s["level"]},
            {"anchor", s["anchor"]},
            {"start_line", sl},
            {"end_line", el},
            {"content", oss.str()}
        };
    }

    if (action == "link") {
        const std::string doc_key = norm_rel_dh(args.value("path", args.value("doc_key", "")));
        if (doc_key.empty()) throw std::runtime_error("docs_hub(link) requiere path o doc_key");
        auto rows = db.query("SELECT doc_key, linked_code FROM project_docs WHERE doc_key=? COLLATE NOCASE", {doc_key});
        if (rows.empty()) throw std::runtime_error("DOC_NOT_FOUND: " + doc_key);
        const std::string rkey = rows[0]["doc_key"].get<std::string>();
        Json links = Json::parse(rows[0]["linked_code"].get<std::string>(), nullptr, false);
        if (!links.is_array()) links = Json::array();

        if (args.contains("code_paths") && args["code_paths"].is_array()) {
            for (const auto& cp : args["code_paths"]) {
                if (!cp.is_string()) continue;
                const std::string norm_c = norm_rel_dh(cp.get<std::string>());
                if (!norm_c.empty() && std::find(links.begin(), links.end(), norm_c) == links.end()) {
                    links.push_back(norm_c);
                }
            }
        }
        const std::string ts = now();
        const int64_t cur_mt = file_mtime_ns_dh(resolve_project_file_dh(p, rkey));
        db.exec("UPDATE project_docs SET linked_code=?, mtime_ns=CASE WHEN ?!=0 THEN ? ELSE mtime_ns END, verified_at=? WHERE doc_key=?",
                {links.dump(), cur_mt, cur_mt, ts, rkey});
        return {
            {"schema", "migratool.docs_hub/1"},
            {"action", "link"},
            {"ok", true},
            {"doc_key", rkey},
            {"linked_code", std::move(links)}
        };
    }

    if (action == "drift_report" || action == "audit") {
        auto docs = db.query("SELECT doc_key, rel_path, title, doc_kind, authority, sha256, mtime_ns, linked_code, verified_at FROM project_docs ORDER BY rel_path");
        Json drifted = Json::array();
        Json healthy = Json::array();
        for (const auto& d : docs) {
            const std::string dkey = d["doc_key"].get<std::string>();
            const int64_t doc_mt = d["mtime_ns"].get<int64_t>();
            const Json links = Json::parse(d["linked_code"].get<std::string>(), nullptr, false);
            Json newer_code = Json::array();
            Json missing_code = Json::array();

            if (links.is_array()) {
                for (const auto& lk : links) {
                    if (!lk.is_string()) continue;
                    const std::string crel = norm_rel_dh(lk.get<std::string>());
                    const fs::path cabs = resolve_project_file_dh(p, crel);
                    std::error_code ec;
                    if (!fs::is_regular_file(native_path(cabs), ec)) {
                        missing_code.push_back(crel);
                    } else {
                        const int64_t cmt = file_mtime_ns_dh(cabs);
                        if (cmt > doc_mt && doc_mt > 0) {
                            newer_code.push_back({{"path", crel}, {"code_mtime_ns", cmt}, {"doc_mtime_ns", doc_mt}});
                        }
                    }
                }
            }

            if (!newer_code.empty() || !missing_code.empty()) {
                drifted.push_back({
                    {"doc_key", dkey},
                    {"title", d["title"]},
                    {"authority", d["authority"]},
                    {"status", "DRIFT_DETECTED"},
                    {"newer_code_files", std::move(newer_code)},
                    {"missing_code_refs", std::move(missing_code)}
                });
            } else {
                healthy.push_back({
                    {"doc_key", dkey},
                    {"title", d["title"]},
                    {"authority", d["authority"]},
                    {"status", "VIGENTE"}
                });
            }
        }
        return {
            {"schema", "migratool.docs_hub/1"},
            {"action", action},
            {"drifted_count", drifted.size()},
            {"healthy_count", healthy.size()},
            {"drifted_docs", std::move(drifted)},
            {"healthy_docs", std::move(healthy)}
        };
    }

    if (action == "search") {
        const std::string q = args.value("query", args.value("text", ""));
        if (q.empty()) throw std::runtime_error("docs_hub(search) requiere query");
        const int limit = std::clamp(args.value("limit", 20), 1, 100);
        Json rows;
        try {
            rows = db.query(
                "SELECT s.id AS section_id, s.doc_key, s.heading, s.level, s.anchor, s.start_line, s.end_line, "
                "snippet(doc_sections_fts, 3, '[', ']', '...', 24) AS snippet "
                "FROM doc_sections_fts f "
                "JOIN doc_sections s ON s.doc_key=f.doc_key AND s.anchor=f.anchor "
                "WHERE doc_sections_fts MATCH ? LIMIT ?",
                {fts_quote_dh(q), limit});
        } catch (...) {
            rows = db.query(
                "SELECT id AS section_id, doc_key, heading, level, anchor, start_line, end_line, heading AS snippet "
                "FROM doc_sections WHERE heading LIKE ? COLLATE NOCASE LIMIT ?",
                {"%" + q + "%", limit});
        }
        return {
            {"schema", "migratool.docs_hub/1"},
            {"action", "search"},
            {"query", q},
            {"count", rows.size()},
            {"items", std::move(rows)}
        };
    }

    throw std::runtime_error("docs_hub action desconocida: " + action);
}

} // namespace mt
