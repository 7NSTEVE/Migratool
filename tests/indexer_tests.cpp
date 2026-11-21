#include "migratool/gateway.hpp"

#include <iostream>
#include <map>
#include <optional>
#include <set>

namespace {
using namespace mt;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::map<std::string, std::string> ids(const Json& scan) {
    std::map<std::string, std::string> result;
    for (const auto& file : scan.at("files"))
        result.emplace(file.at("absolute_path").get<std::string>(), file.at("id").get<std::string>());
    return result;
}
}

int wmain(int argc, wchar_t** argv) try {
    using namespace mt;
    require(argc == 2, "Falta la raíz persistente del proyecto de pruebas");
    const auto qa = fs::path(argv[1]) / L"qa" / (L"indexer-" + wide(unique_id()));
    const auto source = qa / L"source", target = qa / L"target";
    fs::create_directories(source); fs::create_directories(target);
    for (int i = 0; i < 256; ++i)
        write_text(source / wide("item" + std::to_string(i) + ".py"), "value = " + std::to_string(i) + "\n");
    write_text(target / L"only_target.py", "value = 17\n");
    for (int i = 0; i < 24; ++i)
        write_text(source / wide("guide" + std::to_string(i) + ".md"),
            "item0.py\n" + path_text(source / L"item255.py") + "\nonly_target.py\n");
    auto project = Project::create(qa / L"migration.json", "Regresión de índice amplio",
        Json::array({{{"id", "original"}, {"role", "source"}, {"path", path_text(source)}},
                     {{"id", "nativo"}, {"role", "target"}, {"path", path_text(target)}}}));
    const auto first = index_sync(project, false);
    require(first.at("complete") == true, "El primer inventario no quedó completo");
    const auto initial = ids(index_snapshot(project));
    require(initial.size() == 281, "Se perdió un archivo en el inventario inicial");
    std::set<std::string> event_ids;
    for (const auto& change : first.at("changes")) {
        const auto path = change.at("path").get<std::string>();
        require(initial.at(path) == change.at("id").get<std::string>(), "El evento usa un ID distinto al archivo");
        event_ids.insert(change.at("id").get<std::string>());
    }
    require(event_ids.size() == initial.size(), "Se repitieron o faltaron IDs de creación");
    Database db(project);
    const auto stored_snapshot = Json::parse(db.query("SELECT value FROM meta WHERE key='snapshot'").at(0).at("value").get<std::string>());
    const auto exported_snapshot = read_json(project.state_dir() / L"scan.json");
    for (const auto* snapshot : {&stored_snapshot, &exported_snapshot}) {
        require(ids(*snapshot) == initial, "La instantánea compacta perdió identidades de archivos");
        for (const auto& file : snapshot->at("files"))
            require(!file.contains("text_content"), "La instantánea persistida duplica texto del catálogo");
    }
    bool text_retained = false;
    const auto detailed_snapshot = index_snapshot(project, true);
    for (const auto& file : detailed_snapshot.at("files"))
        if (file.at("absolute_path") == path_text(source / L"guide0.md"))
            text_retained = file.at("text_content").get<std::string>() ==
                "item0.py\n" + path_text(source / L"item255.py") + "\nonly_target.py\n";
    require(text_retained, "La consulta explícita perdió el texto documental indexado");
    require(db.integer("SELECT COUNT(*) FROM doc_links") == 48, "Los enlaces documentales no conservan su alcance por raíz");
    const auto intact_id = initial.at(path_text(source / L"item0.py"));
    const auto intact_row = db.query("SELECT rowid FROM search WHERE entity_type='file' AND entity_id=?", {intact_id}).at(0);
    const auto duplicate_id = initial.at(path_text(source / L"item127.py"));
    db.exec("INSERT INTO search(entity_type,entity_id,body) VALUES('file',?,'duplicate_to_remove')", {duplicate_id});
    db.exec("INSERT INTO search(entity_type,entity_id,body) VALUES('note','Ntest','preserved_note')");
    fs::rename(source / L"item128.py", source / L"renamed.py");
    const auto moved = source / std::wstring(120, L'a') / std::wstring(120, L'b') / L"item129.py";
    require(moved.native().size() > 260, "La prueba requiere una ruta de más de 260 caracteres");
    fs::create_directories(moved.parent_path());
    fs::rename(source / L"item129.py", moved);
    write_text(source / L"item127.py", "value = 9000\n");
    fs::remove(source / L"item126.py");
    const auto second = index_sync(project, false);
    require(second.at("reused_records").get<size_t>() > 0, "No se reutilizaron registros intactos");
    require(db.query("SELECT rowid FROM search WHERE entity_type='file' AND entity_id=?", {intact_id}).at(0) == intact_row,
            "La búsqueda de un archivo intacto se reescribió");
    const auto current = ids(index_snapshot(project));
    require(current.size() == 280, "No se registró la eliminación");
    require(current.at(path_text(source / L"renamed.py")) == initial.at(path_text(source / L"item128.py")), "El renombre perdió su identidad");
    require(current.at(path_text(moved)) == initial.at(path_text(source / L"item129.py")), "El traslado a una ruta larga perdió su identidad");
    require(current.at(path_text(source / L"item127.py")) == initial.at(path_text(source / L"item127.py")), "La modificación cambió el ID");
    std::set<std::string> actions;
    for (const auto& change : second.at("changes")) {
        actions.insert(change.at("action").get<std::string>());
        require(!change.at("id").get<std::string>().empty(), "Un cambio incremental quedó sin ID");
    }
    require(actions.contains("renamed") && actions.contains("moved") && actions.contains("modified") && actions.contains("deleted"),
            "Falta un evento incremental");
    require(db.integer("SELECT COUNT(*) FROM doc_links") == 48, "La sincronización incremental alteró los enlaces documentales");
    require(db.integer("SELECT COUNT(*) FROM search WHERE entity_type='file' AND entity_id=?", {duplicate_id}) == 1, "No se retiraron todos los registros FTS anteriores del archivo");
    require(db.integer("SELECT COUNT(*) FROM search WHERE entity_type='file'") == 280, "La eliminación dejó un archivo huérfano en FTS");
    require(db.integer("SELECT COUNT(*) FROM search WHERE entity_type='note' AND body='preserved_note'") == 1, "Se alteró una nota al reindexar archivos");
    // Mismos nombres cortos/calificados pueden designar uno o varios archivos.
    // No crear relaciones para ambigüedades, símbolos inactivos o distinto caso.
    const auto file_a = std::stoll(initial.at(path_text(source / L"item0.py")).substr(1));
    const auto file_b = std::stoll(initial.at(path_text(source / L"item1.py")).substr(1));
    db.begin();
    const auto symbol = [&](int64_t file, const std::string& key, const std::string& name,
                            const std::string& qualified, bool active=true) {
        const Json data={{"name",name},{"qualified_name",qualified},{"kind","function"},{"line",1}};
        db.exec("INSERT INTO symbols(file_id,stable_key,name,qualified_name,kind,line,data,active) VALUES(?,?,?,?,?,1,?,?)",
            {file,key,name,qualified,"function",data.dump(),active});
    };
    symbol(file_a,"unique-a","Unique","qa.Unique");
    symbol(file_a,"unique-alias","UniqueAlias","Unique");
    symbol(file_a,"shared-a","Shared","qa.Shared");
    symbol(file_b,"shared-b","Shared","else.Shared");
    symbol(file_b,"qualified-collision","qa.Unique","else.Unique");
    symbol(file_a,"inactive","Dormant","qa.Dormant",false);
    const std::map<std::string,std::optional<int64_t>> expected={
        {"Unique",file_a},{"UniqueAlias",file_a},{"Shared",{}},{"qa.Shared",file_a},
        {"qa.Unique",{}},{"else.Unique",file_b},{"Dormant",{}},{"unique",{}},{"missing",{}},
        {"ExactPreserved",file_b}
    };
    for(const auto& [name,expected_target]:expected) {
        const bool semantic=name=="ExactPreserved";
        const Json data={{"name",name},{"kind","reference"},
            {"resolution",semantic?"SEMANTIC_EXACT":"UNRESOLVED"}};
        db.exec("INSERT INTO dependencies(file_id,target_id,kind,name,data) VALUES(?,?,'reference',?,?)",
            {file_a,semantic?Json(file_b):Json(nullptr),name,data.dump()});
    }
    db.commit();
    write_text(source / L"item2.py", "value = 20002\n"); // Provoca una actualización real del índice.
    const auto dependencies_pass=index_sync(project,false);
    for(const auto& [name,expected_target]:expected) {
        const auto rows=db.query("SELECT target_id FROM dependencies WHERE name=?",{name});
        require(rows.size()==1,"Faltó una dependencia de la prueba");
        if(expected_target)require(rows[0].at("target_id")==*expected_target,
            ("No resolvió el único archivo de la referencia: "+name).c_str());
        else require(rows[0].at("target_id").is_null(),
            ("Se resolvió una referencia ambigua o ausente: "+name).c_str());
    }
    const auto deferred_qa = qa / L"cpp-deferred";
    const auto deferred_source = deferred_qa / L"source", deferred_target = deferred_qa / L"target";
    fs::create_directories(deferred_source); fs::create_directories(deferred_target);
    write_text(deferred_source / L"example.cpp", "int operation() { return 42; }\n");
    auto deferred_project = Project::create(deferred_qa / L"migration.json", "Análisis C++ explícitamente aplazado",
        Json::array({{{"id", "original"}, {"role", "source"}, {"path", path_text(deferred_source)}},
                     {{"id", "nativo"}, {"role", "target"}, {"path", path_text(deferred_target)}}}));
    deferred_project.config["analyze_cpp"] = false;
    deferred_project.save();
    deferred_project = Project::load(deferred_project.file);
    index_sync(deferred_project, false);
    auto deferred_scan = index_snapshot(deferred_project);
    require(deferred_scan.at("files").size() == 1, "Aplazar C++ excluyó su archivo del inventario");
    require(deferred_scan.at("files")[0].at("analysis") == "DEFERRED", "Se presentó análisis C++ aplazado como aprobado");
    const auto deferred_id = deferred_scan.at("files")[0].at("id");
    deferred_project.config["analyze_cpp"] = true;
    deferred_project.save();
    deferred_project = Project::load(deferred_project.file);
    index_sync(deferred_project, false);
    const auto resumed_scan = index_snapshot(deferred_project);
    require(resumed_scan.at("files")[0].at("id") == deferred_id, "Reanudar el análisis cambió la identidad del archivo");
    require(resumed_scan.at("files")[0].at("analysis") == "ANALYZED", "El cambio de configuración no reanudó el análisis C++");
    const auto scope_dir = source / L"seleccion";
    const auto scoped_file = scope_dir / L"seleccionado.py";
    fs::create_directory(scope_dir);
    write_text(scoped_file, "value = 501\n");
    const auto scope = Json{{"root","original"},{"subdirectory","seleccion"}};
    const auto scope_first = index_sync(project, false, {}, nullptr, scope);
    require(scope_first.at("scope").at("complete") == true, "El subdirectorio no quedó indexado");
    const auto scoped_id = ids(index_snapshot(project)).at(path_text(scoped_file));
    const auto outside = source / L"item2.py";
    const auto outside_moved = source / L"fuera_renombrado.py";
    fs::rename(outside, outside_moved);
    fs::remove(scoped_file);
    const auto scope_second = index_sync(project, false, {}, nullptr, scope);
    const auto scoped_ids = ids(index_snapshot(project));
    require(scope_second.at("scope").at("complete") == true && !scoped_ids.contains(path_text(scoped_file)),
            "El alcance no reconcilió su archivo retirado");
    require(scoped_ids.at(path_text(outside)) == initial.at(path_text(outside)) &&
            !scoped_ids.contains(path_text(outside_moved)),
            "El alcance alteró un registro que no estaba incluido");
    require(db.integer("SELECT deleted FROM files WHERE id=?", {std::stoll(scoped_id.substr(1))}) == 1,
            "El alcance perdió el historial del archivo retirado");
    const auto receipt = Json{{"ok", true}, {"initial_files", initial.size()}, {"current_files", current.size()},
        {"first_duration_ms", first.at("duration_ms")}, {"second_duration_ms", second.at("duration_ms")},
        {"documentation_links", 48}, {"dependency_cases",expected.size()},
        {"scoped_inventory",true},
        {"dependency_pass_duration_ms",dependencies_pass.at("duration_ms")}, {"evidence", path_text(qa)}};
    write_json(qa / L"indexer-receipt.json", receipt);
    std::cout << receipt.dump() << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
