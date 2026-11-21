#include "migratool/analysis_context.hpp"
#include "migratool/duplicates.hpp"
#include "migratool/gateway.hpp"
#include "migratool/live.hpp"
#include "migratool/scale.hpp"
#include "migratool/sql.hpp"

#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace mt;

int checks = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error("FAILED CHECK: " + message);
}

uint32_t crc32_bytes(const std::string& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) {
            const uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

void put_u16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put_u32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

struct ZipEntryInput {
    std::string name;
    std::string content;
    bool encrypted = false;
};

std::string build_store_zip(const std::vector<ZipEntryInput>& entries) {
    std::string out;
    std::string cd;
    for (const auto& e : entries) {
        const uint32_t offset = static_cast<uint32_t>(out.size());
        const uint32_t crc = crc32_bytes(e.content);
        const uint32_t sz = static_cast<uint32_t>(e.content.size());
        const uint16_t flags = e.encrypted ? 0x0001u : 0x0000u;
        const uint16_t nlen = static_cast<uint16_t>(e.name.size());

        // Local file header
        put_u32(out, 0x04034B50u);
        put_u16(out, 20);
        put_u16(out, flags);
        put_u16(out, 0); // STORE
        put_u16(out, 0);
        put_u16(out, 0);
        put_u32(out, crc);
        put_u32(out, sz);
        put_u32(out, sz);
        put_u16(out, nlen);
        put_u16(out, 0);
        out.append(e.name);
        out.append(e.content);

        // Central directory header
        put_u32(cd, 0x02014B50u);
        put_u16(cd, 20);
        put_u16(cd, 20);
        put_u16(cd, flags);
        put_u16(cd, 0);
        put_u16(cd, 0);
        put_u16(cd, 0);
        put_u32(cd, crc);
        put_u32(cd, sz);
        put_u32(cd, sz);
        put_u16(cd, nlen);
        put_u16(cd, 0);
        put_u16(cd, 0);
        put_u16(cd, 0);
        put_u16(cd, 0);
        put_u32(cd, 0);
        put_u32(cd, offset);
        cd.append(e.name);
    }
    const uint32_t cd_offset = static_cast<uint32_t>(out.size());
    const uint32_t cd_size = static_cast<uint32_t>(cd.size());
    out.append(cd);

    // End of central directory
    put_u32(out, 0x06054B50u);
    put_u16(out, 0);
    put_u16(out, 0);
    put_u16(out, static_cast<uint16_t>(entries.size()));
    put_u16(out, static_cast<uint16_t>(entries.size()));
    put_u32(out, cd_size);
    put_u32(out, cd_offset);
    put_u16(out, 0);
    return out;
}

void write_binary(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("Cannot write binary file: " + path_text(path));
}

void exec_raw_sql(const fs::path& db_path, const std::string& sql) {
    sqlite3* raw{};
    if (sqlite3_open_v2(path_text(db_path).c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        const std::string err = raw ? sqlite3_errmsg(raw) : "open failed";
        if (raw) sqlite3_close(raw);
        throw std::runtime_error(err);
    }
    char* msg{};
    const int rc = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, &msg);
    const std::string err = msg ? msg : "";
    if (msg) sqlite3_free(msg);
    sqlite3_close(raw);
    if (rc != SQLITE_OK) throw std::runtime_error(err.empty() ? "raw sqlite error" : err);
}

void test_schema_upgrades(const fs::path& qa) {
    for (int version : {1, 3, 5}) {
        const auto dir = qa / wide("schema_v" + std::to_string(version));
        fs::create_directories(dir / L"source");
        fs::create_directories(dir / L"target");
        const auto src_file = dir / L"source/keep.py";
        const auto tgt_file = dir / L"target/keep_tgt.py";
        write_text(src_file, "def preserved_fn():\n    return 42\n");
        write_text(tgt_file, "def preserved_tgt():\n    return 42\n");

        write_json(dir / L"migration.json", {
            {"schema", "migratool.project/1"},
            {"name", "schema-v" + std::to_string(version)},
            {"roots", Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                                   {{"id", "target"}, {"role", "target"}, {"path", "target"}}})},
            {"exclude_dirs", Json::array({".migratool"})},
            {"defines", Json::array()}, {"clang_args", Json::array()},
            {"analyzers", Json::array()}, {"builds", Json::array()},
            {"cases", Json::array()}, {"mappings", Json::array()}
        });
        const auto proj = Project::load(dir / L"migration.json");
        fs::create_directories(proj.state_dir());
        const auto db_path = proj.state_dir() / L"catalog.sqlite";

        std::string sql = R"sql(
            PRAGMA journal_mode=WAL;
            BEGIN IMMEDIATE;
            CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
            INSERT INTO meta VALUES('revision','12'),('schema','1');
            CREATE TABLE roots(id TEXT PRIMARY KEY,role TEXT,path TEXT,complete INTEGER,data TEXT);
            CREATE TABLE files(id INTEGER PRIMARY KEY AUTOINCREMENT,root TEXT NOT NULL,path TEXT NOT NULL,absolute_path TEXT NOT NULL UNIQUE COLLATE NOCASE,
                winid TEXT,sha256 TEXT,bytes INTEGER,mtime INTEGER,category TEXT,language TEXT,relevant INTEGER,git_state TEXT,build_state TEXT,
                analysis TEXT,deleted INTEGER NOT NULL DEFAULT 0,first_revision INTEGER,last_revision INTEGER,data TEXT);
            CREATE INDEX file_identity ON files(winid);
            CREATE INDEX file_root ON files(root,deleted,relevant);
            CREATE INDEX file_hash ON files(sha256);
            CREATE TABLE symbols(id INTEGER PRIMARY KEY AUTOINCREMENT,file_id INTEGER REFERENCES files(id),stable_key TEXT,
                name TEXT,qualified_name TEXT,kind TEXT,line INTEGER,data TEXT,active INTEGER NOT NULL DEFAULT 1,UNIQUE(file_id,stable_key));
            CREATE TABLE dependencies(id INTEGER PRIMARY KEY,file_id INTEGER REFERENCES files(id),target_id INTEGER,kind TEXT,name TEXT,data TEXT);
            CREATE TABLE doc_links(doc_id INTEGER REFERENCES files(id),code_id INTEGER REFERENCES files(id),reason TEXT,PRIMARY KEY(doc_id,code_id));
            CREATE TABLE mappings(source TEXT PRIMARY KEY,targets TEXT NOT NULL,complete INTEGER NOT NULL,hashes TEXT NOT NULL,
                cases TEXT NOT NULL,note TEXT,actor TEXT,at TEXT,revision INTEGER NOT NULL);
            CREATE TABLE notes(id INTEGER PRIMARY KEY AUTOINCREMENT,entity TEXT,text TEXT,actor TEXT,at TEXT,revision INTEGER);
            CREATE TABLE history(id INTEGER PRIMARY KEY AUTOINCREMENT,revision INTEGER NOT NULL,at TEXT NOT NULL,actor TEXT NOT NULL,action TEXT NOT NULL,entity TEXT,detail TEXT NOT NULL);
            CREATE TRIGGER history_no_update BEFORE UPDATE ON history BEGIN SELECT RAISE(ABORT,'immutable history'); END;
            CREATE TRIGGER history_no_delete BEFORE DELETE ON history BEGIN SELECT RAISE(ABORT,'immutable history'); END;
            CREATE TABLE evidence(id TEXT PRIMARY KEY,kind TEXT,data TEXT,revision INTEGER,at TEXT);
            CREATE VIRTUAL TABLE search USING fts5(entity_type UNINDEXED,entity_id UNINDEXED,body,tokenize='unicode61 remove_diacritics 2');
            CREATE TABLE requests(id INTEGER PRIMARY KEY AUTOINCREMENT,at TEXT,tool TEXT,request_bytes INTEGER,response_bytes INTEGER,duration_us INTEGER,cache_revision INTEGER);
            CREATE TABLE connections(id TEXT PRIMARY KEY,pid INTEGER,client TEXT,opened TEXT,closed TEXT);
            CREATE TABLE watchers(id TEXT PRIMARY KEY,pid INTEGER,root TEXT,opened TEXT,last_event TEXT,closed TEXT,error TEXT);
            CREATE TABLE task_claims(entity TEXT PRIMARY KEY,actor TEXT NOT NULL,session_id TEXT,claimed_at TEXT NOT NULL,expires_at TEXT NOT NULL,lease_seconds INTEGER NOT NULL,note TEXT,revision INTEGER NOT NULL);
        )sql";
        if (version >= 3) {
            sql += R"sql(
                CREATE TABLE inventory_jobs(id TEXT PRIMARY KEY,state TEXT NOT NULL,phase TEXT NOT NULL,scope TEXT NOT NULL,
                    config_sha256 TEXT NOT NULL,created TEXT NOT NULL,updated TEXT NOT NULL,started TEXT,finished TEXT,pid INTEGER,
                    control TEXT NOT NULL DEFAULT '',error TEXT NOT NULL DEFAULT '',files INTEGER NOT NULL DEFAULT 0,dirs INTEGER NOT NULL DEFAULT 0,
                    links INTEGER NOT NULL DEFAULT 0,bytes INTEGER NOT NULL DEFAULT 0,processed INTEGER NOT NULL DEFAULT 0,failed INTEGER NOT NULL DEFAULT 0,
                    generation INTEGER NOT NULL DEFAULT 0,resources TEXT NOT NULL,from_revision INTEGER NOT NULL,census_complete INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE inventory_tasks(job_id TEXT NOT NULL,file_id INTEGER NOT NULL,state TEXT NOT NULL DEFAULT 'queued',
                    phase TEXT NOT NULL DEFAULT 'content',error TEXT NOT NULL DEFAULT '',PRIMARY KEY(job_id,file_id)) WITHOUT ROWID;
                CREATE TABLE census_entries(id INTEGER PRIMARY KEY,generation INTEGER NOT NULL,root TEXT NOT NULL,path TEXT NOT NULL,
                    parent TEXT NOT NULL,name TEXT NOT NULL,absolute_path TEXT NOT NULL,kind TEXT NOT NULL,bytes INTEGER NOT NULL,mtime INTEGER NOT NULL,
                    attrs INTEGER NOT NULL,link_target TEXT,file_id INTEGER,UNIQUE(generation,root,path));
            )sql";
        }
        if (version >= 5) {
            sql += "ALTER TABLE files ADD COLUMN seen_generation INTEGER NOT NULL DEFAULT 0;";
        }
        sql += "PRAGMA user_version=" + std::to_string(version) + "; COMMIT;";
        exec_raw_sql(db_path, sql);

        // Insert authentic user data that must survive migration to v6
        sqlite3* raw{};
        check(sqlite3_open_v2(path_text(db_path).c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
              "Reopen legacy catalog for seed rows");
        auto seed = [&](const char* stmt_sql, const std::vector<std::string>& vals) {
            sqlite3_stmt* s{};
            check(sqlite3_prepare_v2(raw, stmt_sql, -1, &s, nullptr) == SQLITE_OK, sqlite3_errmsg(raw));
            for (size_t i = 0; i < vals.size(); ++i)
                sqlite3_bind_text(s, static_cast<int>(i + 1), vals[i].c_str(), static_cast<int>(vals[i].size()), SQLITE_TRANSIENT);
            const int rc = sqlite3_step(s);
            sqlite3_finalize(s);
            check(rc == SQLITE_DONE, sqlite3_errmsg(raw));
        };
        const auto h_src = hash_file(src_file);
        const auto h_tgt = hash_file(tgt_file);
        seed("INSERT INTO files(id,root,path,absolute_path,sha256,bytes,mtime,category,language,relevant,analysis,deleted,first_revision,last_revision,data) VALUES(42,'source','keep.py',?,?,30,100,'source_code','Python',1,'ANALYZED',0,1,12,?)",
             {path_text(src_file), h_src, Json{{"id", "F42"}, {"source_code", true}, {"present", true}}.dump()});
        seed("INSERT INTO files(id,root,path,absolute_path,sha256,bytes,mtime,category,language,relevant,analysis,deleted,first_revision,last_revision,data) VALUES(43,'target','keep_tgt.py',?,?,30,100,'source_code','Python',1,'ANALYZED',0,1,12,?)",
             {path_text(tgt_file), h_tgt, Json{{"id", "F43"}, {"source_code", true}, {"present", true}}.dump()});
        seed("INSERT INTO symbols(id,file_id,stable_key,name,qualified_name,kind,line,data,active) VALUES(77,42,'preserved_fn@1','preserved_fn','preserved_fn','function',1,?,1)",
             {Json{{"id", "S77"}, {"file_id", "F42"}, {"name", "preserved_fn"}}.dump()});
        seed("INSERT INTO mappings(source,targets,complete,hashes,cases,note,actor,at,revision) VALUES('F42','[\"F43\"]',1,?,'[]','nota v' || ?,'legacy-actor','2026-01-01T00:00:00Z',11)",
             {Json{{"F42", h_src}, {"F43", h_tgt}}.dump(), std::to_string(version)});
        seed("INSERT INTO notes(entity,text,actor,at,revision) VALUES('F42','nota conservada','legacy-actor','2026-01-01T00:00:00Z',10)", {});
        seed("INSERT INTO history(revision,at,actor,action,entity,detail) VALUES(11,'2026-01-01T00:00:00Z','legacy-actor','migration_complete','F42','{\"legacy\":true}')", {});
        sqlite3_close(raw);

        // Open with Database -> must auto-backup schema-v<N>-*.sqlite & upgrade to v6
        {
            Database upgraded(proj);
            check(upgraded.integer("PRAGMA user_version") == 6,
                  "schema v" + std::to_string(version) + " debe actualizarse a user_version=6");
            check(upgraded.integer("SELECT COUNT(*) FROM files WHERE id=42 AND deleted=0") == 1,
                  "ID F42 conservado tras migración v" + std::to_string(version));
            check(upgraded.integer("SELECT COUNT(*) FROM symbols WHERE id=77 AND file_id=42 AND active=1") == 1,
                  "ID S77 conservado tras migración v" + std::to_string(version));
            check(upgraded.integer("SELECT COUNT(*) FROM mappings WHERE source='F42' AND complete=1") == 1,
                  "Mapping de F42 conservado tras migración v" + std::to_string(version));
            check(upgraded.integer("SELECT COUNT(*) FROM notes WHERE entity='F42' AND text='nota conservada'") == 1,
                  "Nota de F42 conservada tras migración v" + std::to_string(version));
            check(upgraded.integer("SELECT COUNT(*) FROM history WHERE revision=11 AND entity='F42'") == 1,
                  "Historial de F42 conservado tras migración v" + std::to_string(version));
            const auto summary = scale_summary(proj, upgraded);
            check(summary.at("known_files").get<int64_t>() >= 2, "scale_summary opera sobre catálogo migrado");
        }
        bool backup_found = false;
        const std::string expected_prefix = "schema-v" + std::to_string(version) + "-";
        for (const auto& entry : fs::directory_iterator(proj.state_dir())) {
            if (path_text(entry.path().filename()).starts_with(expected_prefix)) {
                backup_found = true;
                break;
            }
        }
        check(backup_found, "Debe existir respaldo automático " + expected_prefix + "*.sqlite");

        // Run durable census/analysis after upgrade to ensure full operational readiness
        const auto run_res = scale_run(proj, {{"actor", "schema-upgrade-test"}});
        check(run_res.at("state") == "completed", "scale_run tras migración v" + std::to_string(version) + " debe completar");
        Database after_db(proj);
        check(after_db.integer("SELECT COUNT(*) FROM mappings WHERE source='F42' AND complete=1") == 1,
              "Mapping de F42 persiste incluso tras primer scale_run post-migración");
    }
}

void test_transitive_and_cache(const fs::path& qa) {
    const auto dir = qa / L"cache_and_transitives";
    const auto src = dir / L"source";
    const auto tgt = dir / L"target";
    fs::create_directories(src / L"include");
    fs::create_directories(src / L"src");
    fs::create_directories(src / L"scripts");
    fs::create_directories(src / L"csapp" / L"Extra");
    fs::create_directories(tgt);

    const auto leaf_h = src / L"include/leaf.h";
    const auto mid_h = src / L"include/mid.h";
    const auto main_cpp = src / L"src/main.cpp";
    const auto other_cpp = src / L"src/other.cpp";
    const auto cc_json = src / L"compile_commands.json";

    write_text(leaf_h, "#pragma once\ntypedef int LeafType;\n#define LEAF_VAL 10\n");
    write_text(mid_h, "#pragma once\n#include \"leaf.h\"\nstruct MidBox { LeafType value; };\n");
    write_text(main_cpp, "#include \"mid.h\"\nLeafType compute_leaf(LeafType in) { return in + LEAF_VAL; }\n");
    write_text(other_cpp, "int untouched_helper() { return 7; }\n");

    auto write_cc = [&](const std::string& std_flag, const std::string& extra_def) {
        Json cc = Json::array({
            {{"directory", path_text(src)},
             {"arguments", {"clang++", std_flag, "-D" + extra_def, "-I" + path_text(src / L"include"), "-c", path_text(main_cpp)}},
             {"file", path_text(main_cpp)}},
            {{"directory", path_text(src)},
             {"arguments", {"clang++", std_flag, "-c", path_text(other_cpp)}},
             {"file", path_text(other_cpp)}}
        });
        write_text(cc_json, cc.dump(2));
    };
    write_cc("-std=c++17", "CC_REV=1");

    // C# Directory.Build.props / .targets / $(...) / globs
    const auto props_file = src / L"csapp/Directory.Build.props";
    const auto targets_file = src / L"csapp/Directory.Build.targets";
    const auto csproj_file = src / L"csapp/CsApp.csproj";
    const auto cs_extra_file = src / L"csapp/Extra/PropsGadget.cs";

    auto write_props = [&](const std::string& custom_flag) {
        write_text(props_file,
            "<Project>\n  <PropertyGroup>\n    <CustomMacro>" + custom_flag + "</CustomMacro>\n"
            "    <DefineConstants>$(DefineConstants);$(CustomMacro)</DefineConstants>\n  </PropertyGroup>\n</Project>\n");
    };
    write_props("PROPS_ENABLED");
    write_text(targets_file,
        "<Project>\n  <PropertyGroup>\n    <DefineConstants>$(DefineConstants);TARGETS_ENABLED</DefineConstants>\n  </PropertyGroup>\n</Project>\n");
    write_text(csproj_file,
        "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
        "  <PropertyGroup><TargetFramework>net8.0</TargetFramework><EnableDefaultCompileItems>false</EnableDefaultCompileItems></PropertyGroup>\n"
        "  <ItemGroup><Compile Include=\"Extra/**/*.cs\" /></ItemGroup>\n"
        "</Project>\n");
    write_text(cs_extra_file,
        "#if PROPS_ENABLED && TARGETS_ENABLED\n"
        "public class PropsTargetVisible { public int Value() => 1; }\n"
        "#else\n"
        "public class PropsTargetHidden { public int Value() => 0; }\n"
        "#endif\n");

    // Python file + custom analyzer cmd script
    const auto py_file = src / L"scripts/sample.py";
    write_text(py_file, "def builtin_py_symbol():\n    return 1\n");
    const auto adapter_cmd = dir / L"custom_py_adapter.cmd";
    auto write_adapter = [&](const std::string& sym_name) {
        write_text(adapter_cmd,
            "@echo off\r\n"
            "(echo {\"protocol\":\"migratool.analyzer/1\",\"analysis\":\"ANALYZED\",\"symbols\":[{\"id\":\"custom_py_1\",\"name\":\"" + sym_name + "\",\"kind\":\"function\",\"line\":1}]})>\"%~2\"\r\n");
    };
    write_adapter("adapter_sym_v1");

    auto proj = Project::create(dir / L"migration.json", "cache-transitives",
        Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                     {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
    proj.config["inventory_engine"] = "durable";
    proj.config["analyze_cpp"] = true;
    proj.config["analyzers"] = Json::array({
        {{"id", "custom_py"}, {"extensions", {".py"}}, {"argv", {"C:\\Windows\\System32\\cmd.exe", "/c", path_text(adapter_cmd), "{file}", "{output}"}}}
    });
    proj.save();
    proj = Project::load(proj.file);

    // Run 1: initial full analysis
    const auto run1 = scale_run(proj, {{"actor", "test-1"}});
    check(run1.at("state") == "completed", "Run 1 inicial debe completar: " + run1.dump());

    Database db(proj);
    auto file_cache_key = [&](const fs::path& fp) -> std::string {
        auto rows = db.query("SELECT json_extract(data,'$.analysis_cache_key') AS k, analysis FROM files WHERE absolute_path=? AND deleted=0", {path_text(fp)});
        check(rows.size() == 1 && !rows[0]["k"].is_null(), "analysis_cache_key presente para " + path_text(fp));
        return rows[0]["k"].get<std::string>();
    };
    auto has_symbol = [&](const fs::path& fp, const std::string& sym_name) -> bool {
        return db.integer(
            "SELECT EXISTS(SELECT 1 FROM symbols s JOIN files f ON f.id=s.file_id WHERE f.absolute_path=? AND s.active=1 AND s.name=?)",
            {path_text(fp), sym_name}) != 0;
    };

    check(has_symbol(main_cpp, "compute_leaf"), "Libclang analiza main.cpp");
    check(has_symbol(cs_extra_file, "PropsTargetVisible"), "Roslyn resuelve Directory.Build.props + .targets + $(CustomMacro) + glob");
    check(has_symbol(py_file, "adapter_sym_v1"), "Analizador externo personalizado produce adapter_sym_v1");
    check(db.integer("SELECT EXISTS(SELECT 1 FROM dependencies d JOIN files f ON f.id=d.file_id JOIN files t ON t.id=d.target_id WHERE f.absolute_path=? AND t.absolute_path=?)",
                     {path_text(main_cpp), path_text(leaf_h)}) != 0,
          "Dependencia transitiva main.cpp -> leaf.h registrada en dependencies");

    const auto key_other_1 = file_cache_key(other_cpp);
    const auto key_main_1 = file_cache_key(main_cpp);
    const auto key_cs_1 = file_cache_key(cs_extra_file);
    const auto key_py_1 = file_cache_key(py_file);

    // Run 2: full=true with ZERO edits -> MUST hit analysis_cache for unchanged units!
    const auto run2 = scale_run(proj, {{"actor", "test-2"}, {"full", true}});
    check(run2.at("state") == "completed", "Run 2 full sin cambios debe completar");
    const int64_t cache_hits = run2.at("resources").value("analysis_cache_hits", int64_t(0));
    check(cache_hits >= 5, "Run 2 sin cambios debe reutilizar análisis desde cache (hits=" + std::to_string(cache_hits) + ")");
    check(file_cache_key(other_cpp) == key_other_1, "Clave de cache de other.cpp permanece idéntica");
    check(file_cache_key(main_cpp) == key_main_1, "Clave de cache de main.cpp permanece idéntica sin cambios");
    check(file_cache_key(cs_extra_file) == key_cs_1, "Clave de cache de PropsGadget.cs permanece idéntica sin cambios");

    // Change 1: Modify ONLY leaf.h (ensure mtime + size advance)
    Sleep(30);
    write_text(leaf_h, "#pragma once\ntypedef double LeafType;\n#define LEAF_VAL 999\n// changed leaf\n");
    const auto run3 = scale_run(proj, {{"actor", "test-3"}});
    check(run3.at("state") == "completed", "Run 3 tras cambio en leaf.h debe completar");
    check(file_cache_key(main_cpp) != key_main_1, "Cambio en leaf.h invalida transitivamente a main.cpp");
    check(file_cache_key(other_cpp) == key_other_1, "other.cpp no afectado conserva su clave de cache");

    // Change 2: Modify compile_commands.json
    Sleep(30);
    write_cc("-std=c++20", "CC_REV=222");
    const auto key_main_before_cc = file_cache_key(main_cpp);
    const auto run4 = scale_run(proj, {{"actor", "test-4"}});
    check(run4.at("state") == "completed", "Run 4 tras cambio en compile_commands.json debe completar");
    check(file_cache_key(main_cpp) != key_main_before_cc, "Cambio en compile_commands.json invalida unidades C++");
    check(file_cache_key(other_cpp) != key_other_1, "Cambio en compile_commands.json invalida other.cpp en raíz");
    check(file_cache_key(cs_extra_file) == key_cs_1, "Cambio C++ no invalida cache C#");

    // Change 3: Modify Directory.Build.props (removes PROPS_ENABLED)
    Sleep(30);
    write_props("PROPS_DISABLED");
    const auto run5 = scale_run(proj, {{"actor", "test-5"}});
    check(run5.at("state") == "completed", "Run 5 tras cambio en Directory.Build.props debe completar");
    check(file_cache_key(cs_extra_file) != key_cs_1, "Cambio en Directory.Build.props invalida PropsGadget.cs");
    check(has_symbol(cs_extra_file, "PropsTargetHidden"), "Roslyn reevalúa defines condicionales tras cambio de Directory.Build.props");
    check(!has_symbol(cs_extra_file, "PropsTargetVisible"), "Símbolo anterior condicional ya no activo tras cambio de props");

    // Change 4: Modify external analyzer script content (without editing .py or migration.json!)
    write_adapter("adapter_sym_v2");
    const auto run6 = scale_run(proj, {{"actor", "test-6"}});
    check(run6.at("state") == "completed", "Run 6 tras cambio en script de analizador externo debe completar");
    check(file_cache_key(py_file) != key_py_1, "Cambio en binario/script externo invalida clave de cache de sample.py");
    check(has_symbol(py_file, "adapter_sym_v2"), "Analizador externo actualizado produce adapter_sym_v2");

    // Change 5: Remove custom analyzer from settings -> must invalidate .py cache & fall back to built-in AST
    const auto key_py_before_remove = file_cache_key(py_file);
    auto applied = scale_settings(proj, {{"action", "set"}, {"actor", "test-settings"},
        {"expected_config_revision", proj.revision}, {"settings", {{"analyzers", Json::array()}}}});
    check(applied.at("analyzers").empty(), "scale_settings removió analizadores externos");
    proj = Project::load(proj.file);
    const auto run7 = scale_run(proj, {{"actor", "test-7"}});
    check(run7.at("state") == "completed", "Run 7 tras eliminar analizador externo debe completar");
    check(file_cache_key(py_file) != key_py_before_remove, "Quitar analizador externo invalida sample.py");
    check(has_symbol(py_file, "builtin_py_symbol"), "AST sintáctico integrado analiza sample.py al retirar analizador externo");
}

void test_zip_limits_and_resume(const fs::path& qa) {
    // 1. Direct zip_code_entries unit checks for depth limit, size limit, entry limit, and encrypted flag
    const std::string dup_code(16384, '#');
    const std::string l2_zip = build_store_zip({{"deep.py", dup_code + "\nprint('l2')\n"}});
    const std::string l1_zip = build_store_zip({{"level2.zip", l2_zip}});
    const std::string outer_zip_bytes = build_store_zip({{"level1.zip", l1_zip}, {"top_copy.py", dup_code + "\nprint('top')\n"}});
    const CodeProbe probe = [](const fs::path& member, std::optional<std::string_view>) -> bool {
        auto ext = member.extension().wstring();
        for (auto& ch : ext) ch = towlower(ch);
        return ext == L".py";
    };

    const auto unit_dir = qa / L"zip_units";
    fs::create_directories(unit_dir);
    const auto nested_zip_path = unit_dir / L"nested.zip";
    write_binary(nested_zip_path, outer_zip_bytes);

    {
        Json items = Json::array();
        ZipScanOptions opts;
        opts.max_depth = 1; // level1 ok (depth 1), level2 at depth 1 >= max_depth 1 -> NESTED_ARCHIVE_DEPTH_LIMIT
        zip_code_entries(nested_zip_path, probe, nullptr, [&](const Json& item, bool) { items.push_back(item); }, opts);
        bool depth_limit_hit = false;
        for (const auto& it : items) if (it.value("error", "") == "NESTED_ARCHIVE_DEPTH_LIMIT") depth_limit_hit = true;
        check(depth_limit_hit, "max_depth=1 informa NESTED_ARCHIVE_DEPTH_LIMIT en nivel 2");
    }
    {
        Json items = Json::array();
        ZipScanOptions opts;
        opts.max_depth = 2;
        opts.max_nested_bytes = 32; // level1/level2 exceeded
        zip_code_entries(nested_zip_path, probe, nullptr, [&](const Json& item, bool) { items.push_back(item); }, opts);
        bool size_limit_hit = false;
        for (const auto& it : items) if (it.value("error", "") == "NESTED_ARCHIVE_SIZE_LIMIT") size_limit_hit = true;
        check(size_limit_hit, "max_nested_bytes informa NESTED_ARCHIVE_SIZE_LIMIT");
    }
    {
        const std::string multi_nested = build_store_zip({
            {"inner.zip", build_store_zip({{"a.py", "x=1\n"}, {"b.py", "x=2\n"}, {"c.py", "x=3\n"}})}
        });
        const auto multi_path = unit_dir / L"entry_limit.zip";
        write_binary(multi_path, multi_nested);
        Json items = Json::array();
        ZipScanOptions opts;
        opts.max_depth = 2;
        opts.max_nested_entries = 1;
        zip_code_entries(multi_path, probe, nullptr, [&](const Json& item, bool) { items.push_back(item); }, opts);
        bool entry_limit_hit = false;
        for (const auto& it : items) if (it.value("error", "") == "NESTED_ARCHIVE_ENTRY_LIMIT") entry_limit_hit = true;
        check(entry_limit_hit, "max_nested_entries informa NESTED_ARCHIVE_ENTRY_LIMIT");
    }
    {
        const auto enc_path = unit_dir / L"encrypted.zip";
        write_binary(enc_path, build_store_zip({{"secret.py", "x=999\n", true}}));
        Json items = Json::array();
        zip_code_entries(enc_path, probe, nullptr, [&](const Json& item, bool) { items.push_back(item); }, ZipScanOptions{});
        check(!items.empty() && items[0].value("error", "") == "Entrada cifrada",
              "ZIP con entrada cifrada informa error controlado sin bloquear catálogo");
    }

    // 2. End-to-end durable project: nested_zip_depth 0 vs 2, 1-byte diff, corrupt zip, pause & resume inside ZIP
    const auto dir = qa / L"zip_durable";
    fs::create_directories(dir / L"source");
    fs::create_directories(dir / L"target");
    const std::string shared_body = dup_code + "\nprint('shared_deep')\n";
    const std::string near_body = dup_code + "\nprint('shared_deeq')\n"; // 1-byte difference
    write_text(dir / L"target/loose_deep.py", shared_body);
    write_text(dir / L"source/near_copy.py", near_body);
    write_text(dir / L"source/corrupt.zip", "NOT A REAL ZIP FILE\n");

    const std::string deep_l2 = build_store_zip({{"deep_clone.py", shared_body}});
    const std::string deep_l1 = build_store_zip({{"l2.zip", deep_l2}});
    write_binary(dir / L"source/bundle.zip", build_store_zip({{"l1.zip", deep_l1}}));

    auto proj = Project::create(dir / L"migration.json", "zip-durable",
        Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                     {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
    proj.config["inventory_engine"] = "durable";
    proj.config["code_duplicate_mode"] = "files_and_zip";
    proj.config["nested_zip_depth"] = 0;
    proj.save();

    const auto depth0_run = scale_run(proj, {{"actor", "zip-d0"}});
    check(depth0_run.at("state") == "completed_with_errors", "Con nested_zip_depth=0 y ZIP corrupto completa con errores");
    {
        Database db(proj);
        auto err_rows = db.query("SELECT entry_path, error FROM archive_members WHERE error='NESTED_ARCHIVE_PENDING'");
        check(!err_rows.empty(), "nested_zip_depth=0 deja archivo anidado como NESTED_ARCHIVE_PENDING");
    }

    // Upgrade to nested_zip_depth=2 via scale_settings
    proj = Project::load(proj.file);
    scale_settings(proj, {{"action", "set"}, {"actor", "zip-cfg"},
        {"expected_config_revision", proj.revision}, {"settings", {{"nested_zip_depth", 2}}}});
    proj = Project::load(proj.file);

    const auto depth2_run = scale_run(proj, {{"actor", "zip-d2"}, {"full", true}});
    check(depth2_run.at("state") == "completed_with_errors", "nested_zip_depth=2 completa dejando solo corrupt.zip como error");
    {
        Database db(proj);
        Json all_dups = Json::array();
        std::string dup_cur;
        for (int p = 0; p < 10; ++p) {
            Json q = {{"kind", "code_duplicates"}, {"limit", 50}};
            if (!dup_cur.empty()) q["cursor"] = dup_cur;
            const auto page = agent_request(proj, "query", q);
            for (const auto& it : page.at("items")) all_dups.push_back(it);
            if (!page.contains("next_cursor") || !page["next_cursor"].is_string()) break;
            dup_cur = page["next_cursor"].get<std::string>();
        }
        check(all_dups.size() == 2, "Solo loose_deep.py y l1.zip!/l2.zip!/deep_clone.py forman grupo (near_copy a 1 byte excluido, count=" + std::to_string(all_dups.size()) + ")");
        bool deep_entry_found = false;
        for (const auto& it : all_dups) {
            if (it.at("kind") == "zip" && it.value("entry_path", "").find("deep_clone.py") != std::string::npos) {
                deep_entry_found = true;
            }
        }
        check(deep_entry_found, "Duplicado a profundidad 2 conserva ruta interna completa");
        const auto dup_errs = agent_request(proj, "query", {{"kind", "code_duplicate_errors"}, {"limit", 50}});
        check(dup_errs.at("items").size() == 1 && dup_errs["items"][0]["absolute_path"] == path_text(dir / L"source/corrupt.zip"),
              "code_duplicate_errors informa corrupt.zip paginablemente");
    }

    // 3. Pause and Resume inside large 3500-entry archive cleanly preserving committed batches
    const auto resume_dir = qa / L"zip_pause_resume";
    fs::create_directories(resume_dir / L"source");
    fs::create_directories(resume_dir / L"target");
    std::vector<ZipEntryInput> bulk_entries;
    constexpr int TOTAL_BULK = 3500;
    bulk_entries.reserve(TOTAL_BULK);
    const std::string chunk_pad(4096, 'x');
    for (int i = 0; i < TOTAL_BULK; ++i) {
        bulk_entries.push_back({"m_" + std::to_string(i) + ".py", "VAL = " + std::to_string(i) + "\n#" + chunk_pad + "\n"});
    }
    const auto bulk_zip_path = resume_dir / L"source/bulk_archive.zip";
    write_binary(bulk_zip_path, build_store_zip(bulk_entries));

    auto rproj = Project::create(resume_dir / L"migration.json", "zip-resume",
        Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                     {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
    rproj.config["inventory_engine"] = "durable";
    rproj.config["code_duplicate_mode"] = "files_and_zip";
    rproj.config["nested_zip_depth"] = 0;
    rproj.save();

    const auto queued = scale_start(rproj, {{"actor", "zip-resume-test"}}, false);
    const std::string jid = queued.at("job_id").get<std::string>();
    std::jthread coord([&]() { scale_coordinator(rproj, true); });

    bool pause_sent = false;
    uint64_t pause_started = 0;
    for (int wait_tick = 0; wait_tick < 600; ++wait_tick) {
        Database rdb(rproj);
        const auto hashed = rdb.integer("SELECT COUNT(*) FROM archive_members WHERE state='hashed'");
        const auto jstate = rdb.query("SELECT state FROM inventory_jobs WHERE id=?", {jid})[0]["state"].get<std::string>();
        if (!pause_sent && hashed >= 128 && hashed < TOTAL_BULK - 200) {
            pause_started = GetTickCount64();
            scale_control(rproj, {{"id", jid}, {"action", "pause"}, {"actor", "zip-pause-controller"}});
            pause_sent = true;
        }
        if (pause_sent && jstate == "paused") break;
        if (jstate == "completed" || jstate == "failed") break;
        Sleep(15);
    }
    coord.join();
    check(pause_sent, "Se envió pausa mientras el ZIP aún procesaba lotes confirmados (>=128 entradas)");
    const auto pause_elapsed = GetTickCount64() - pause_started;
    check(pause_elapsed < 5000, "Pausa en ZIP respondió en menos de 5s (ms=" + std::to_string(pause_elapsed) + ")");

    int64_t paused_count = 0;
    {
        Database rdb(rproj);
        const auto jrow = scale_jobs(rproj, {{"id", jid}}).at("items").at(0);
        check(jrow.at("state") == "paused", "El trabajo quedó en estado paused tras pausar en ZIP");
        paused_count = rdb.integer("SELECT COUNT(*) FROM archive_members WHERE state='hashed'");
        check(paused_count >= 128 && paused_count < TOTAL_BULK,
              "Lotes confirmados antes de la pausa se conservan (" + std::to_string(paused_count) + " / " + std::to_string(TOTAL_BULK) + ")");
        const auto zstate = rdb.query("SELECT json_extract(data,'$.zip_scan_state') AS s, json_extract(data,'$.zip_next_index') AS idx FROM files WHERE absolute_path=?",
                                      {path_text(bulk_zip_path)})[0];
        check(zstate["s"] == "paused" && zstate["idx"].get<int64_t>() == paused_count,
              "Checkpoint zip_next_index coincide exactamente con los miembros confirmados");
    }

    scale_control(rproj, {{"id", jid}, {"action", "resume"}, {"actor", "zip-resume-controller"}});
    scale_coordinator(rproj, true);
    {
        Database rdb(rproj);
        Json final_job;
        for (int wait_tick = 0; wait_tick < 400; ++wait_tick) {
            final_job = scale_jobs(rproj, {{"id", jid}}).at("items").at(0);
            const auto st = final_job.at("state").get<std::string>();
            if (st == "completed" || st == "completed_with_errors" || st == "failed" || st == "cancelled") break;
            Sleep(50);
        }
        check(final_job.at("state") == "completed", "Reanudación completa trabajo de ZIP pausado: " + final_job.dump());
        check(rdb.integer("SELECT COUNT(*) FROM archive_members WHERE state='hashed'") == TOTAL_BULK,
              "Tras reanudar se completan exactamente " + std::to_string(TOTAL_BULK) + " entradas sin duplicados");
        check(rdb.integer("SELECT COUNT(DISTINCT entry_index) FROM archive_members") == TOTAL_BULK,
              "Índices de entrada ZIP son únicos tras pausa y reanudación");
    }
}

void test_reconciliation_budget_usn_snapshot(const fs::path& qa) {
    // 1. Entity/Domain Conflict Reconciliation + expected_entity_revision + Negative Bounds
    const auto dir = qa / L"reconcile";
    fs::create_directories(dir / L"source");
    fs::create_directories(dir / L"target");
    write_text(dir / L"source/f1.py", "def one():\n    return 1\n");
    write_text(dir / L"source/f2.py", "def two():\n    return 2\n");
    write_text(dir / L"target/t1.py", "def tone():\n    return 1\n");
    write_text(dir / L"target/t2.py", "def ttwo():\n    return 2\n");

    auto proj = Project::create(dir / L"migration.json", "reconcile-test",
        Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                     {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
    proj.config["inventory_engine"] = "durable";
    proj.save();
    check(scale_run(proj, {{"actor", "setup"}}).at("state") == "completed", "Censo base de conciliación");

    Database db(proj);
    auto fid_of = [&](const std::string& name) {
        return "F" + std::to_string(db.integer("SELECT id FROM files WHERE root='source' AND path=? AND deleted=0", {name}));
    };
    auto tid_of = [&](const std::string& name) {
        return "F" + std::to_string(db.integer("SELECT id FROM files WHERE root='target' AND path=? AND deleted=0", {name}));
    };
    const auto f1 = fid_of("f1.py"), f2 = fid_of("f2.py"), t1 = tid_of("t1.py"), t2 = tid_of("t2.py");
    const auto r0 = db.revision();

    // Actor A maps F1 -> t1
    const auto res_a = agent_batch(proj, {{"actor", "agentA"}, {"expected_revision", r0},
        {"operations", {{{"op", "map"}, {"source", f1}, {"targets", {t1}}, {"complete", false}, {"note", "map A"}}}}});
    check(res_a.at("ok") == true, "Agent A aplica mapping en F1");

    // Actor B using stale r0 adds a note to F1 (disjoint domain: note vs mapping -> succeeds!)
    const auto res_b = agent_batch(proj, {{"actor", "agentB"}, {"expected_revision", r0},
        {"operations", {{{"op", "note"}, {"entity", f1}, {"text", "nota independiente"}}}}});
    check(res_b.at("ok") == true, "Conciliación por dominio permite nota en misma entidad con expected_revision r0");

    // Actor C using stale r0 maps DIFFERENT entity F2 -> t2 (disjoint entity -> succeeds!)
    const auto res_c = agent_batch(proj, {{"actor", "agentC"}, {"expected_revision", r0},
        {"operations", {{{"op", "map"}, {"source", f2}, {"targets", {t2}}, {"complete", false}, {"note", "map F2"}}}}});
    check(res_c.at("ok") == true, "Conciliación por entidad permite mapear F2 con expected_revision r0");

    // Actor D using stale r0 tries conflicting mapping on F1 -> MUST throw REVISION_CONFLICT
    bool conflict_thrown = false;
    try {
        agent_batch(proj, {{"actor", "agentD"}, {"expected_revision", r0},
            {"operations", {{{"op", "map"}, {"source", f1}, {"targets", {t2}}, {"complete", true}, {"note", "conflict"}}}}});
    } catch (const std::exception& e) {
        conflict_thrown = std::string(e.what()).find("REVISION_CONFLICT") != std::string::npos &&
                          std::string(e.what()).find(f1) != std::string::npos;
    }
    check(conflict_thrown, "Misma entidad y misma familia con expected_revision obsoleto lanza REVISION_CONFLICT");

    // Test expected_entity_revision conflict with expected_revision=-1 (no global rev lock)
    const int64_t f1_rev = db.integer("SELECT COALESCE(MAX(revision),0) FROM history WHERE entity=?", {f1});
    agent_batch(proj, {{"actor", "agentE"}, {"expected_revision", db.revision()},
        {"operations", {{{"op", "note"}, {"entity", f1}, {"text", "actualiza rev entidad"}}}}});
    bool entity_conflict = false;
    std::string entity_conflict_err;
    try {
        agent_batch(proj, {{"actor", "agentF"}, {"expected_revision", -1}, {"operations",
            {{{"op", "note"}, {"entity", f1}, {"expected_entity_revision", f1_rev}, {"text", "debe fallar"}}}}});
    } catch (const std::exception& e) {
        entity_conflict_err = e.what();
        entity_conflict = entity_conflict_err.find("ENTITY_REVISION_CONFLICT") != std::string::npos;
    }
    check(entity_conflict, "expected_entity_revision obsoleto lanza ENTITY_REVISION_CONFLICT (err=" + entity_conflict_err + ")");

    // 2. Negative bounds validation for scale_settings
    auto expect_settings_reject = [&](const Json& settings, const std::string& token, const char* msg) {
        bool rejected = false;
        proj = Project::load(proj.file);
        try {
            scale_settings(proj, {{"action", "set"}, {"actor", "bounds"}, {"expected_config_revision", proj.revision}, {"settings", settings}});
        } catch (const std::exception& e) {
            rejected = std::string(e.what()).find(token) != std::string::npos;
        }
        check(rejected, msg);
    };
    expect_settings_reject({{"resources", {{"memory_mib", 5000}, {"workers", 4}, {"analyzers", 2}}}}, "RESOURCE_RANGE", "Rechaza memory_mib > 4096");
    expect_settings_reject({{"resources", {{"memory_mib", 1024}, {"workers", 9}, {"analyzers", 2}}}}, "RESOURCE_RANGE", "Rechaza workers > 8");
    expect_settings_reject({{"resources", {{"memory_mib", 1024}, {"workers", 4}, {"analyzers", 3}}}}, "RESOURCE_RANGE", "Rechaza analyzers > 2");
    expect_settings_reject({{"nested_zip_depth", 5}}, "NESTED_ZIP_DEPTH_RANGE", "Rechaza nested_zip_depth > 4");

    // 3. Shared 4 GiB Budget across concurrent projects
    auto make_budget_proj = [&](const wchar_t* folder, int mib) {
        const auto pdir = qa / folder;
        fs::create_directories(pdir / L"source");
        fs::create_directories(pdir / L"target");
        for (int i = 0; i < 40; ++i) write_text(pdir / L"source" / wide("u_" + std::to_string(i) + ".py"), "def f():\n    return " + std::to_string(i) + "\n");
        auto p = Project::create(pdir / L"migration.json", utf8(folder),
            Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                         {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
        p.config["inventory_engine"] = "durable";
        p.config["resources"] = {{"memory_mib", mib}, {"workers", 4}, {"analyzers", 1}};
        p.save();
        return p;
    };
    auto pa = make_budget_proj(L"budget_a", 3072);
    auto pb = make_budget_proj(L"budget_b", 2048);
    std::jthread t_a([&] { check(scale_run(pa, {{"actor", "pa"}}).at("state") == "completed", "Proyecto A bajo presupuesto compartido completa"); });
    std::jthread t_b([&] { check(scale_run(pb, {{"actor", "pb"}}).at("state") == "completed", "Proyecto B bajo presupuesto compartido completa"); });
    t_a.join();
    t_b.join();
    const auto doc_a = scale_doctor(pa);
    check(doc_a.at("shared_memory").at("budget_mib") == 4096, "Límite agregado compartido es 4096 MiB");
    check(doc_a.at("shared_memory").at("total_allocated_mib").get<int>() <= 4096, "Memoria asignada no excede 4096 MiB");
    {
        Database bdb(pa);
        const auto srow = bdb.query("SELECT value FROM meta WHERE key='shared_memory_status'");
        check(!srow.empty(), "shared_memory_status registrado en meta durante ejecución");
        const auto sjson = Json::parse(srow[0]["value"].get<std::string>());
        check(sjson.at("budget_mib") == 4096 && sjson.at("total_allocated_mib").get<int>() <= 4096,
              "Presupuesto compartido registrado <= 4096 MiB: " + sjson.dump());
    }

    // 4. Bounded scale_snapshot on 300-file catalog
    const auto snap_dir = qa / L"bounded_snap";
    fs::create_directories(snap_dir / L"source");
    fs::create_directories(snap_dir / L"target");
    for (int i = 0; i < 280; ++i) {
        write_text(snap_dir / L"source" / wide("item_" + std::to_string(i) + ".py"), "def fn_" + std::to_string(i) + "():\n    return 1\n");
    }
    auto sproj = Project::create(snap_dir / L"migration.json", "bounded-snap",
        Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                     {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
    sproj.config["inventory_engine"] = "durable";
    sproj.save();
    check(scale_run(sproj, {{"actor", "snap"}}).at("state") == "completed", "Proyecto de snapshot acotado completado");
    {
        Database sdb(sproj);
        const auto snap = scale_snapshot(sproj, sdb);
        check(snap.at("schema") == "migratool.scan/2", "Snapshot usa esquema v2 acotado");
        check(snap.at("snapshot_complete") == false, "Snapshot parcial indica snapshot_complete=false");
        check(snap.at("files").size() <= 200 && snap.at("symbols").size() <= 200, "Snapshot acota arrays en memoria a <=200 filas");
        check(snap.at("files_count").get<int64_t>() >= 280, "Snapshot conserva conteo real de archivos conocidos");
        check(snap.at("continuation").at("files").is_string(), "Snapshot entrega cursor de continuación");
    }

    // 5. USN Journal fallback status check in IndexWatcher
    {
        IndexWatcher watcher(sproj, 60000);
        Sleep(150);
        Database wdb(sproj);
        const auto js = wdb.query("SELECT value FROM meta WHERE key='journal_status:source'");
        check(!js.empty(), "IndexWatcher registra journal_status:source en meta");
        const auto jmeta = Json::parse(js[0]["value"].get<std::string>());
        check(jmeta.at("fallback") == "metadata_reconciliation", "Fallback documentado a metadata_reconciliation");
        const auto state = jmeta.at("state").get<std::string>();
        check(state == "UNAVAILABLE" || state == "BASELINE_REQUIRED" || state == "REPLAY_READY",
              "Estado USN válido en unprivileged o elevado: " + state);
    }

    // 6. generation_scopes legacy v5->v6 seen_generation=0 bootstrap
    {
        const auto leg_dir = qa / L"legacy_genscope";
        fs::create_directories(leg_dir / L"source");
        fs::create_directories(leg_dir / L"target");
        write_text(leg_dir / L"source/a.py", "def a():\n    return 1\n");
        auto lp = Project::create(leg_dir / L"migration.json", "legacy-gen-scope",
            Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                         {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
        lp.config["inventory_engine"] = "durable";
        lp.save();
        check(scale_run(lp, {{"actor", "seed"}}).at("state") == "completed", "Proyecto base para generation_scopes");
        Database ldb(lp);
        ldb.exec("DELETE FROM generation_scopes");
        ldb.exec("UPDATE files SET seen_generation=0 WHERE deleted=0");
        const auto gs = query_page(lp, ldb, {{"kind", "files"}, {"limit", 20}});
        check(!gs.at("items").empty() && gs.at("items")[0].at("path") == "a.py",
              "generation_scopes sintetiza filas legadas con seen_generation=0");
    }

    // 7. Tool-hash revert V1 -> V2 -> V1 and scoped tool-hash invalidation
    {
        const auto rev_dir = qa / L"tool_revert";
        fs::create_directories(rev_dir / L"source");
        fs::create_directories(rev_dir / L"target");
        write_text(rev_dir / L"source/one.cpp", "int one() { return 1; }\n");
        write_text(rev_dir / L"source/two.cpp", "int two() { return 2; }\n");
        auto rp = Project::create(rev_dir / L"migration.json", "tool-revert",
            Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                         {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
        rp.config["inventory_engine"] = "durable";
        rp.config["clang_args"] = Json::array({"-DM1_V1"});
        rp.save();
        check(scale_run(rp, {{"actor", "t1"}}).at("state") == "completed", "Run V1 completado");
        auto key_of = [&](const std::string& p_rel) {
            Database rdb(rp);
            auto rows = rdb.query("SELECT json_extract(data,'$.analysis_cache_key') AS k FROM files WHERE path=? AND deleted=0", {p_rel});
            check(!rows.empty() && rows[0]["k"].is_string(), "analysis_cache_key registrado para " + p_rel);
            return rows[0]["k"].get<std::string>();
        };
        const auto k1 = key_of("one.cpp");
        rp.config["clang_args"] = Json::array({"-DM1_V2"});
        rp.save();
        check(scale_run(rp, {{"actor", "t2"}}).at("state") == "completed", "Run V2 completado");
        const auto k2 = key_of("one.cpp");
        check(k1 != k2, "Cambio de configuracion de herramienta cambia analysis_cache_key");
        // Revert back to V1 via SCOPED run on one.cpp only -> both scoped invalidation & prior-key cache miss check
        rp.config["clang_args"] = Json::array({"-DM1_V1"});
        rp.save();
        check(scale_run(rp, {{"actor", "t3"}, {"scope", {{"root", "source"}, {"file", "one.cpp"}}}}).at("state") == "completed",
              "Run scoped V1 revertido completado");
        const auto k3 = key_of("one.cpp");
        check(k3 == k1, "Revertir herramienta V1 -> V2 -> V1 restaura analysis_cache_key original en ejecucion acotada");
    }

    // 8. target_test with explicit null left/right null-safety check
    {
        const auto tt_dir = qa / L"null_lr_case";
        fs::create_directories(tt_dir / L"source");
        fs::create_directories(tt_dir / L"target");
        write_text(tt_dir / L"source/s.py", "def s():\n    return 1\n");
        auto tp = Project::create(tt_dir / L"migration.json", "null-lr-case",
            Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                         {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
        tp.config["cases"] = Json::array({
            {{"id", "tt_null_lr"}, {"kind", "target_test"}, {"left", nullptr}, {"right", nullptr},
             {"root", "target"}, {"argv", {"cmd.exe", "/c", "echo", "OK"}}, {"expected_stdout_contains", {"OK"}}}
        });
        tp.save();
        index_sync(tp, false);
        const auto t_run = run_cases(tp);
        check(t_run.at("cases")[0].at("state") == "PASS", "target_test con left/right=null pasa sin excepcion de tipo");
        const auto st = status(tp, false);
        check(st.at("cases")[0].at("state") == "PASS", "status() maneja target_test con left/right=null");
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) return 2;
        const auto qa = fs::path(argv[1]) / L"build/qa-scale/tests-tmp" / wide(unique_id());
        fs::create_directories(qa);
        test_schema_upgrades(qa);
        test_transitive_and_cache(qa);
        test_zip_limits_and_resume(qa);
        test_reconciliation_budget_usn_snapshot(qa);
        write_json(qa / L"receipt.json", {{"passed", true}, {"checks", checks}, {"build", build_identity()}, {"at", now()}});
        std::cout << "PASS " << checks << " P0-P3 verification checks. Evidencia " << path_text(qa) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
