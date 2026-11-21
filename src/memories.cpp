#include "migratool/memories.hpp"
#include "migratool/fs_index.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mt {
namespace {

std::string sanitize_memory_name(std::string name) {
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    if (name.empty() || name.size() > 128) throw std::runtime_error("Nombre de memoria inválido (1..128 caracteres)");
    if (name == "." || name == ".." || name.find("..") != std::string::npos)
        throw std::runtime_error("Nombre de memoria contiene secuencia prohibida");
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) {
            throw std::runtime_error("Nombre de memoria contiene caracteres inválidos");
        }
    }
    if (name.ends_with(".md")) name.resize(name.size() - 3);
    if (name.empty()) throw std::runtime_error("Nombre de memoria vacío");
    return name;
}

fs::path memory_md_path(const Project& p, const std::string& safe_name) {
    return p.state_dir() / L"memories" / wide(safe_name + ".md");
}

std::string fts_quote(std::string_view q) {
    std::string out = "\"";
    for (char c : q) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

void memories_schema(Database& db) {
    if (db.integer("SELECT COUNT(*) FROM meta WHERE key='memories_schema' AND value='1'") != 0) return;
    const bool need_tx = (sqlite3_get_autocommit(db.native_handle()) != 0);
    if (need_tx) db.begin();
    try {
        if (db.integer("SELECT COUNT(*) FROM meta WHERE key='memories_schema' AND value='1'") == 0) {
            char* err = nullptr;
            const char* sql = R"sql(
CREATE TABLE IF NOT EXISTS project_memories (
  name        TEXT PRIMARY KEY COLLATE NOCASE,
  content     TEXT NOT NULL,
  tags        TEXT NOT NULL DEFAULT '[]',
  linked_frns TEXT NOT NULL DEFAULT '[]',
  updated_at  TEXT NOT NULL,
  actor       TEXT NOT NULL,
  revision    INTEGER NOT NULL
);
CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(name, content, tags, tokenize='unicode61 remove_diacritics 2');
)sql";
            if (sqlite3_exec(db.native_handle(), sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::string msg = err ? err : sqlite3_errmsg(db.native_handle());
                if (err) sqlite3_free(err);
                throw std::runtime_error(msg);
            }
            db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('memories_schema','1')");
        }
        if (need_tx) db.commit();
    } catch (...) {
        if (need_tx) db.rollback();
        throw;
    }
}

Json memory_request(const Project& p, Database& db, const Json& args) {
    memories_schema(db);
    const std::string action = args.value("action", args.value("op", "list"));
    const std::string actor = args.value("actor", "agent");

    if (action == "list") {
        const std::string tag_filter = args.value("tag", "");
        auto rows = db.query("SELECT name, length(content) AS bytes, tags, linked_frns, updated_at, actor, revision FROM project_memories ORDER BY name");
        Json items = Json::array();
        for (const auto& r : rows) {
            Json tags = Json::parse(r["tags"].get<std::string>(), nullptr, false);
            if (!tags.is_array()) tags = Json::array();
            if (!tag_filter.empty()) {
                bool found = false;
                for (const auto& t : tags) if (t.is_string() && t.get<std::string>() == tag_filter) { found = true; break; }
                if (!found) continue;
            }
            items.push_back({
                {"name", r["name"]},
                {"bytes", r["bytes"]},
                {"tags", tags},
                {"updated_at", r["updated_at"]},
                {"actor", r["actor"]},
                {"revision", r["revision"]}
            });
        }
        return {{"schema", "migratool.memories/1"}, {"action", "list"}, {"count", items.size()}, {"items", std::move(items)}};
    }

    if (action == "search") {
        const std::string q = args.value("query", args.value("text", ""));
        if (q.empty()) throw std::runtime_error("memory(search) requiere query");
        const int limit = std::clamp(args.value("limit", 20), 1, 200);
        auto rows = db.query(
            "SELECT m.name, m.tags, m.updated_at, m.actor, snippet(memories_fts, 1, '[', ']', '...', 24) AS excerpt "
            "FROM memories_fts JOIN project_memories m ON m.name = memories_fts.name "
            "WHERE memories_fts MATCH ? ORDER BY bm25(memories_fts) LIMIT ?",
            {fts_quote(q), limit});
        for (auto& r : rows) {
            r["tags"] = Json::parse(r["tags"].get<std::string>(), nullptr, false);
        }
        return {{"schema", "migratool.memories/1"}, {"action", "search"}, {"query", q}, {"count", rows.size()}, {"items", std::move(rows)}};
    }

    if (action == "read") {
        const std::string name = sanitize_memory_name(args.at("name").get<std::string>());
        auto rows = db.query("SELECT name, content, tags, linked_frns, updated_at, actor, revision FROM project_memories WHERE name=?", {name});
        if (rows.empty()) throw std::runtime_error("MEMORY_NOT_FOUND: " + name);
        const auto& r = rows[0];
        std::string content = r["content"].get<std::string>();
        auto lines = split_lines(content);
        const int total_lines = static_cast<int>(lines.size());
        if (args.contains("start_line") || args.contains("end_line")) {
            int s = std::max(1, args.value("start_line", 1));
            int e = std::min(total_lines, args.value("end_line", total_lines));
            std::ostringstream oss;
            for (int i = s; i <= e; ++i) {
                if (i > s) oss << '\n';
                oss << lines[static_cast<size_t>(i - 1)];
            }
            content = oss.str();
        }
        // Check linked file FRNs for staleness/moves if fs_nodes exists
        Json linked_status = Json::array();
        Json linked = Json::parse(r["linked_frns"].get<std::string>(), nullptr, false);
        if (linked.is_array() && !linked.empty() &&
            db.integer("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name='fs_nodes')") != 0) {
            for (const auto& lf : linked) {
                if (!lf.is_object()) continue;
                int64_t frn = lf.value("frn", int64_t(0));
                std::string orig_path = lf.value("path", "");
                std::string status = "unknown";
                std::string cur_path = orig_path;
                if (frn != 0) {
                    auto cur = db.query(
                        "SELECT n.name, d.rel_path AS prel, n.flags FROM fs_nodes n "
                        "LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn "
                        "WHERE n.frn=? LIMIT 1", {frn});
                    if (cur.empty() || (cur[0]["flags"].get<int64_t>() & 1) != 0) {
                        status = "deleted";
                        cur_path = "";
                    } else {
                        std::string prel = cur[0]["prel"].is_null() ? "" : cur[0]["prel"].get<std::string>();
                        cur_path = prel.empty() ? cur[0]["name"].get<std::string>() : (prel + "/" + cur[0]["name"].get<std::string>());
                        status = (cur_path == orig_path) ? "ok" : "moved";
                    }
                }
                linked_status.push_back({{"original_path", orig_path}, {"current_path", cur_path}, {"frn", frn}, {"status", status}});
            }
        }
        return {
            {"schema", "migratool.memories/1"},
            {"action", "read"},
            {"name", r["name"]},
            {"content", std::move(content)},
            {"total_lines", total_lines},
            {"tags", Json::parse(r["tags"].get<std::string>(), nullptr, false)},
            {"linked_files", std::move(linked_status)},
            {"updated_at", r["updated_at"]},
            {"actor", r["actor"]},
            {"revision", r["revision"]},
            {"mirror_path", path_text(memory_md_path(p, r["name"].get<std::string>()))}
        };
    }

    if (action == "write") {
        const std::string name = sanitize_memory_name(args.at("name").get<std::string>());
        const std::string content = args.at("content").get<std::string>();
        const bool overwrite = args.value("overwrite", true);
        Json tags = args.value("tags", Json::array());
        if (!tags.is_array()) throw std::runtime_error("tags debe ser un arreglo");

        // Resolve optional linked_paths to NTFS FRNs for automatic move/delete tracking
        Json linked_frns = Json::array();
        if (args.contains("linked_paths") && args["linked_paths"].is_array()) {
            for (const auto& lp : args["linked_paths"]) {
                if (!lp.is_string()) continue;
                std::string sp = lp.get<std::string>();
                fs::path abs_p = fs::path(wide(sp));
                if (!abs_p.is_absolute() && p.config.contains("roots") && !p.config["roots"].empty()) {
                    abs_p = p.resolve(p.config["roots"][0]["path"].get<std::string>()) / abs_p;
                }
                uint64_t frn = query_path_frn(abs_p);
                std::string norm = sp;
                std::replace(norm.begin(), norm.end(), '\\', '/');
                linked_frns.push_back({{"path", norm}, {"frn", static_cast<int64_t>(frn)}});
            }
        }

        const std::string ts = now();
        int64_t rev = 0;
        db.begin();
        try {
            if (!overwrite && db.integer("SELECT EXISTS(SELECT 1 FROM project_memories WHERE name=?)", {name}) != 0) {
                throw std::runtime_error("MEMORY_ALREADY_EXISTS: " + name);
            }
            rev = db.change(actor, "memory_write", {{"name", name}, {"bytes", content.size()}}, "M:" + name);
            db.exec("INSERT INTO project_memories(name,content,tags,linked_frns,updated_at,actor,revision) VALUES(?,?,?,?,?,?,?) "
                    "ON CONFLICT(name) DO UPDATE SET content=excluded.content, tags=excluded.tags, linked_frns=excluded.linked_frns, "
                    "updated_at=excluded.updated_at, actor=excluded.actor, revision=excluded.revision",
                    {name, content, tags.dump(), linked_frns.dump(), ts, actor, rev});
            db.exec("DELETE FROM memories_fts WHERE name=?", {name});
            db.exec("INSERT INTO memories_fts(name,content,tags) VALUES(?,?,?)", {name, content, tags.dump()});
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
        auto md = memory_md_path(p, name);
        write_text(md, content);
        return {{"ok", true}, {"action", "write"}, {"name", name}, {"bytes", content.size()},
                {"revision", rev}, {"mirror_path", path_text(md)}};
    }

    if (action == "edit") {
        const std::string name = sanitize_memory_name(args.at("name").get<std::string>());
        const std::string needle = args.at("needle").get<std::string>();
        const std::string repl = args.at("repl").get<std::string>();
        const std::string mode = args.value("mode", "literal");
        const bool allow_multi = args.value("allow_multiple_occurrences", false);
        if (needle.empty()) throw std::runtime_error("needle vacío");

        auto rows = db.query("SELECT content, tags, linked_frns FROM project_memories WHERE name=?", {name});
        if (rows.empty()) throw std::runtime_error("MEMORY_NOT_FOUND: " + name);
        std::string content = rows[0]["content"].get<std::string>();
        std::string updated;
        size_t matches = 0;

        if (mode == "regex") {
            std::regex re(needle, std::regex::ECMAScript);
            auto begin = std::sregex_iterator(content.begin(), content.end(), re);
            auto end = std::sregex_iterator();
            matches = static_cast<size_t>(std::distance(begin, end));
            if (matches == 0) throw std::runtime_error("MEMORY_NEEDLE_NOT_FOUND");
            if (matches > 1 && !allow_multi) throw std::runtime_error("MEMORY_MULTIPLE_MATCHES: use allow_multiple_occurrences:true");
            updated = std::regex_replace(content, re, repl);
        } else {
            size_t pos = 0;
            while ((pos = content.find(needle, pos)) != std::string::npos) {
                ++matches;
                pos += needle.size();
            }
            if (matches == 0) throw std::runtime_error("MEMORY_NEEDLE_NOT_FOUND");
            if (matches > 1 && !allow_multi) throw std::runtime_error("MEMORY_MULTIPLE_MATCHES: use allow_multiple_occurrences:true");
            updated = content;
            pos = 0;
            while ((pos = updated.find(needle, pos)) != std::string::npos) {
                updated.replace(pos, needle.size(), repl);
                pos += repl.size();
            }
        }

        const std::string ts = now();
        int64_t rev = 0;
        db.begin();
        try {
            rev = db.change(actor, "memory_edit", {{"name", name}, {"replacements", matches}}, "M:" + name);
            db.exec("UPDATE project_memories SET content=?, updated_at=?, actor=?, revision=? WHERE name=?",
                    {updated, ts, actor, rev, name});
            db.exec("DELETE FROM memories_fts WHERE name=?", {name});
            db.exec("INSERT INTO memories_fts(name,content,tags) VALUES(?,?,?)", {name, updated, rows[0]["tags"]});
            db.commit();
        } catch (...) { db.rollback(); throw; }
        write_text(memory_md_path(p, name), updated);
        return {{"ok", true}, {"action", "edit"}, {"name", name}, {"replacements", matches}, {"revision", rev}};
    }

    if (action == "rename") {
        const std::string old_name = sanitize_memory_name(args.at("old_name").get<std::string>());
        const std::string new_name = sanitize_memory_name(args.at("new_name").get<std::string>());
        auto rows = db.query("SELECT content, tags, linked_frns FROM project_memories WHERE name=?", {old_name});
        if (rows.empty()) throw std::runtime_error("MEMORY_NOT_FOUND: " + old_name);
        if (db.integer("SELECT EXISTS(SELECT 1 FROM project_memories WHERE name=?)", {new_name}) != 0)
            throw std::runtime_error("MEMORY_ALREADY_EXISTS: " + new_name);
        int64_t rev = 0;
        db.begin();
        try {
            rev = db.change(actor, "memory_rename", {{"old_name", old_name}, {"new_name", new_name}}, "M:" + new_name);
            db.exec("UPDATE project_memories SET name=?, updated_at=?, actor=?, revision=? WHERE name=?",
                    {new_name, now(), actor, rev, old_name});
            db.exec("DELETE FROM memories_fts WHERE name=?", {old_name});
            db.exec("INSERT INTO memories_fts(name,content,tags) VALUES(?,?,?)", {new_name, rows[0]["content"], rows[0]["tags"]});
            db.commit();
        } catch (...) { db.rollback(); throw; }
        std::error_code ec;
        auto old_md = memory_md_path(p, old_name), new_md = memory_md_path(p, new_name);
        if (fs::exists(old_md, ec)) fs::rename(old_md, new_md, ec);
        else write_text(new_md, rows[0]["content"].get<std::string>());
        return {{"ok", true}, {"action", "rename"}, {"old_name", old_name}, {"new_name", new_name}, {"revision", rev}};
    }

    if (action == "delete") {
        const std::string name = sanitize_memory_name(args.at("name").get<std::string>());
        if (db.integer("SELECT EXISTS(SELECT 1 FROM project_memories WHERE name=?)", {name}) == 0)
            throw std::runtime_error("MEMORY_NOT_FOUND: " + name);
        int64_t rev = 0;
        db.begin();
        try {
            rev = db.change(actor, "memory_delete", {{"name", name}}, "M:" + name);
            db.exec("DELETE FROM project_memories WHERE name=?", {name});
            db.exec("DELETE FROM memories_fts WHERE name=?", {name});
            db.commit();
        } catch (...) { db.rollback(); throw; }
        std::error_code ec;
        fs::remove(memory_md_path(p, name), ec);
        return {{"ok", true}, {"action", "delete"}, {"name", name}, {"revision", rev}};
    }

    throw std::runtime_error("memory.action desconocida (use write, read, edit, list, search, rename o delete): " + action);
}

} // namespace mt
