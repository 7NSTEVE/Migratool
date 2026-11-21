#include "migratool/gateway.hpp"
#include "migratool/live.hpp"

#include <commctrl.h>
#include <objbase.h>
#include <uiautomation.h>
#include <iostream>
#include <set>
#include <thread>
#include <future>

namespace {
using namespace mt;
std::atomic_int checks = 0;
Json operations = Json::array();
fs::path qa;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void wait_until(Predicate predicate, const char* message) {
    const auto started = GetTickCount64();
    while (GetTickCount64() - started < 15000) {
        if (predicate()) { check(true, message); return; }
        Sleep(50);
    }
    check(false, message);
}
Json cli(const std::vector<std::string>& arguments) {
    std::vector<std::string> command{path_text(executable_dir() / L"migratool-cli-0.3.exe")};
    command.insert(command.end(), arguments.begin(), arguments.end());
    const auto process = run_process(command, qa, "", qa / L"processes" / wide(unique_id()), 30000);
    const auto result = read_json(fs::path(wide(process.at("stdout"))));
    check(process.at("exit_code") == 0, "CLI terminó con error");
    operations.push_back({{"process", process}, {"result", result}});
    return result;
}
HWND child(HWND parent, int id) {
    struct Search { int id; HWND window{}; } search{id};
    EnumChildWindows(parent, [](HWND window, LPARAM value) -> BOOL {
        auto& found = *reinterpret_cast<Search*>(value);
        if (GetDlgCtrlID(window) == found.id) { found.window = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}
void command(HWND window, int id) {
    DWORD_PTR ignored{};
    check(SendMessageTimeoutW(window, WM_COMMAND, id, 0, SMTO_ABORTIFHUNG, 3000, &ignored) != 0,
          "La GUI dejó de responder");
}
void choose_filter(HWND window, int index) {
    const auto filter = child(window, 52);
    SendMessageW(filter, CB_SETCURSEL, index, 0);
    SendMessageW(GetParent(filter), WM_COMMAND, MAKEWPARAM(52, CBN_SELCHANGE), reinterpret_cast<LPARAM>(filter));
}
void enable_source_only(HWND window,int duplicate_choice=-1) {
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    PostMessageW(window, WM_COMMAND, 12, 0);
    struct Search { DWORD pid; HWND window{}; } search{pid};
    wait_until([&] {
        EnumWindows([](HWND candidate, LPARAM data) -> BOOL {
            auto& found = *reinterpret_cast<Search*>(data);
            DWORD owner{};
            GetWindowThreadProcessId(candidate, &owner);
            wchar_t name[64]{};
            GetClassNameW(candidate, name, 64);
            if (owner == found.pid && std::wstring_view(name) == L"MigratoolForm") {
                found.window = candidate;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.window && child(search.window, 90);
    }, "Configuración muestra el selector de alcance");
    const auto mode=child(search.window,90);
    check(SendMessageW(mode,CB_GETCURSEL,0,0)==0,"Configuración refleja el modo universal actual");
    SendMessageW(mode,CB_SETCURSEL,1,0);
    if(duplicate_choice>=0){
        const auto duplicates=child(search.window,91);
        check(SendMessageW(duplicates,CB_GETCOUNT,0,0)==3,"Configuración ofrece los tres alcances de duplicados");
        SendMessageW(duplicates,CB_SETCURSEL,duplicate_choice,0);
    }
    PostMessageW(search.window,WM_COMMAND,IDOK,0);
}
class Gui {
    HANDLE process_{};
public:
    HWND window{};
    Gui() {
        const auto executable = executable_dir() / L"Migratool-0.3.exe";
        auto command_line = quote_arg(executable.wstring());
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION process{};
        check(CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                             nullptr, qa.c_str(), &startup, &process) != FALSE, "Inicio de GUI");
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        struct Find { DWORD pid; HWND window{}; } found{process.dwProcessId};
        wait_until([&] {
            EnumWindows([](HWND candidate, LPARAM data) -> BOOL {
                auto& match = *reinterpret_cast<Find*>(data);
                DWORD pid{};
                GetWindowThreadProcessId(candidate, &pid);
                wchar_t name[80]{};
                GetClassNameW(candidate, name, 80);
                if (pid == match.pid && std::wstring(name) == L"MigratoolWindow") {
                    match.window = candidate;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&found));
            window = found.window;
            return window && child(window, 50);
        }, "Ventana y visor nativos disponibles");
    }
    void close() {
        PostMessageW(window, WM_CLOSE, 0, 0);
        check(WaitForSingleObject(process_, 10000) == WAIT_OBJECT_0, "Cierre GUI");
        DWORD exit{};
        GetExitCodeProcess(process_, &exit);
        check(exit == 0, "Salida GUI");
    }
    ~Gui() {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            if (WaitForSingleObject(process_, 10000) != WAIT_OBJECT_0) TerminateProcess(process_, 99);
        }
        CloseHandle(process_);
    }
};
std::string id_for(const Json& snapshot, const fs::path& path) {
    for (const auto& file : snapshot.at("files"))
        if (file.at("absolute_path") == path_text(path)) return file.at("id");
    throw std::runtime_error("Archivo de fixture sin ID");
}
void select_first_origin(HWND window) {
    IUIAutomation* automation{};
    IUIAutomationElement* list{};
    IUIAutomationCondition* condition{};
    IUIAutomationElement* row{};
    IUIAutomationSelectionItemPattern* selection{};
    auto result = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&automation));
    if (SUCCEEDED(result)) result = automation->ElementFromHandle(child(window, 50), &list);
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = UIA_ListItemControlTypeId;
    if (SUCCEEDED(result)) result = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition);
    if (SUCCEEDED(result)) result = list->FindFirst(TreeScope_Children, condition, &row);
    if (SUCCEEDED(result) && row) result = row->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection));
    if (SUCCEEDED(result) && selection) result = selection->Select();
    const bool selected = SUCCEEDED(result) && selection;
    if (selection) selection->Release();
    if (row) row->Release();
    if (condition) condition->Release();
    if (list) list->Release();
    if (automation) automation->Release();
    check(selected, "Selección de origen mediante UI Automation");
}
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        check(argc == 2, "Indique workspace");
        qa = fs::path(argv[1]) / L"build/live-qa" / wide(unique_id()) / L"Proyecto con espacios";
        fs::create_directories(qa / L"source");
        fs::create_directories(qa / L"target");
        const auto registry = qa / L"projects.sqlite";
        SetEnvironmentVariableW(L"MIGRATOOL_REGISTRY_PATH", registry.c_str());
        write_text(qa / L"source/origin.py", "value = 1\n");
        write_text(qa / L"source/second.py", "other = 2\n");
        write_text(qa / L"source/bin/generated.py", "generated = 1\n");
        write_text(qa / L"source/vendor/library.py", "external = 1\n");
        write_text(qa / L"source/web/index.html", "<h1>Source UI</h1>\n");
        write_text(qa / L"source/guide.md", "Documentación indexada para búsqueda FTS.\n");
        write_text(qa / L"target/migrated.py", "value = 1\n");
        write_text(qa / L"target/unlinked.py", "unlinked = 3\n");
        auto project = Project::create(qa / L"migration.json", "Proyecto CLI visible en GUI",
            Json::array({{{"id", "source"}, {"role", "source"}, {"path", path_text(qa / L"source")}},
                         {{"id", "target"}, {"role", "target"}, {"path", path_text(qa / L"target")}}}));
        index_sync(project, false);
        const auto full_snapshot=index_snapshot(project),compact_snapshot=index_snapshot(project,false);
        check(full_snapshot["files"].size()==compact_snapshot["files"].size(),"Inventario compacto conserva archivos");
        check(full_snapshot["symbols"]==compact_snapshot["symbols"]&&full_snapshot["roots"]==compact_snapshot["roots"],"Inventario compacto conserva unidades y huellas");
        bool removed_text=false;for(size_t i=0;i<full_snapshot["files"].size();++i)if(full_snapshot["files"][i].value("text_content","").size()>0)
            removed_text=!compact_snapshot["files"][i].contains("text_content");
        check(removed_text,"GUI y porcentajes omiten cuerpos documentales del catálogo");
        check(indexed_status(project)["source_files"] == 2, "Política de código relevante predeterminada");
        project.config["roots"][0]["include_all_code"] = true;
        project.save();
        index_sync(project, false);
        check(indexed_status(project)["source_files"] == 5, "Alcance total incluye generados, dependencias y UI");
        project.config["roots"][0]["include_all_code"] = false;
        project.config["code_extensions"] = {".nfx"};
        project.config["code_filenames"] = {"ProjectHooks"};
        project.save();
        index_sync(project, false);
        const auto snapshot = index_snapshot(project);
        const auto source = id_for(snapshot, qa / L"source/origin.py");
        const auto target = id_for(snapshot, qa / L"target/migrated.py");
        check(registered_projects()["projects"].empty(), "Fixture sin registro previo a CLI");
        Gui gui;
        cli({"agent", "context", "--project", "migration.json", "--budget-tokens", "2000"});
        wait_until([&] { return ListView_GetItemCount(child(gui.window, 50)) == 2; },
                   "La GUI descubre el proyecto CLI sin apertura manual");
        check(registered_projects()["projects"].size() == 1, "Rutas relativas y barras distintas conservan un solo proyecto");
        bool cli_activity = false, gui_activity = false;
        for (const auto& activity : project_activity(project.file)["items"]) {
            cli_activity = cli_activity || activity["operation"] == "agent/context";
            gui_activity = gui_activity || activity["operation"] == "Proyecto abierto";
        }
        check(cli_activity && gui_activity, "CLI relativa y GUI absoluta comparten actividad");
        check(ListView_GetItemCount(child(gui.window, 51)) == 2, "Visor de destino incluye archivos sin vínculo");
        const auto record = qa / L"mark.json";
        Database db(project);
        write_json(record, {{"actor", "Agente de prueba"}, {"expected_revision", db.revision()},
            {"operations", Json::array({{{"op", "mark"}, {"source", source}, {"targets", {target}},
                {"complete", true}, {"note", "Cambio CLI visible en tiempo real"}}})}});
        cli({"agent", "batch", "--project", path_text(project.file), "--input", path_text(record)});
        choose_filter(gui.window, 3);
        wait_until([&] { return ListView_GetItemCount(child(gui.window, 50)) == 1; },
                   "La marca CLI aparece en el filtro de migrados");
        select_first_origin(gui.window);
        const auto linked = child(gui.window, 53);
        SendMessageW(linked, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(GetParent(linked), WM_COMMAND, 53, reinterpret_cast<LPARAM>(linked));
        check(ListView_GetItemCount(child(gui.window, 51)) == 1, "Selección origen filtra su destino vinculado");
        const auto model = migration_comparison(index_snapshot(project), indexed_status(project));
        check(model["origins"][0]["comparison_state"] == "COMPLETE", "Estado declarado calculado por el modelo");
        check(model["targets"][0]["origins"].size() == 1 && !model["targets"][1]["linked"].get<bool>(),
              "Modelo conserva relaciones y destinos sin correspondencia");
        write_text(qa / L"source/origin.py", "value = 5\n");
        choose_filter(gui.window, 5);
        wait_until([&] { return ListView_GetItemCount(child(gui.window, 50)) == 1; },
                   "Watcher invalida la marca y actualiza el visor");
        const auto second = Project::create(qa / L"second.json", "Segundo proyecto CLI", project.config["roots"]);
        cli({"agent", "context", "--project", path_text(second.file), "--budget-tokens", "1600"});
        wait_until([&] { return SendMessageW(child(gui.window, 14), CB_GETCOUNT, 0, 0) == 2; },
                   "Segundo proyecto aparece en selector");
        check(ListView_GetItemCount(child(gui.window, 50)) == 1, "El proyecto activo permanece seleccionado");
        project = Project::load(project.file);
        project.config["builds"] = Json::array({{{"id", "sleep-build"}, {"root", "target"},
            {"argv", {path_text(executable_dir() / L"test_worker.exe"), "sleep"}}, {"timeout_ms", 6000},
            {"artifacts", {path_text(executable_dir() / L"test_worker.exe")}}}});
        project.save();
        const auto verify = qa / L"verify.json";
        write_json(verify, {{"actor", "Agente en ejecución"}, {"action", "build"}, {"execute", true}, {"token_budget", 10000}});
        auto running = std::async(std::launch::async, [&] {
            return cli({"agent", "verify", "--project", path_text(project.file), "--input", path_text(verify)});
        });
        wait_until([&] {
            for (const auto& session : registered_projects()["sessions"])
                if (session["actor"] == "Agente en ejecución" && session["live"] == true &&
                    session["message"] == "Compilación: sleep-build") return true;
            return false;
        }, "Operación y log visibles mientras el agente está ejecutando");
        command(gui.window, 1013);
        wait_until([&] { return ListView_GetItemCount(child(gui.window, 40)) >= 3; }, "Vista de agentes recibe sesiones");
        const auto result = running.get();
        check(result["result"]["builds"][0]["state"] == "PASS", "Ejecución CLI completada");
        bool finished = false;
        for (const auto& session : registered_projects()["sessions"])
            if (session["actor"] == "Agente en ejecución") finished = session["state"] == "SUCCEEDED" && session["live"] == false;
        check(finished, "Estado final persiste al cerrar la CLI");
        project.config["builds"][0]["argv"] = {path_text(executable_dir() / L"test_worker.exe"), "different"};
        project.config["builds"][0]["artifacts"] = {path_text(qa / L"missing-artifact.exe")};
        project.save();
        write_json(verify, {{"actor", "Agente con error"}, {"action", "build"}, {"execute", true}, {"token_budget", 10000}});
        const auto failed = cli({"agent", "verify", "--project", path_text(project.file), "--input", path_text(verify)});
        check(failed["result"]["builds"][0]["state"] == "ERROR", "verify informa el fallo aunque la CLI entregue JSON con salida cero");
        bool failed_session = false;
        for (const auto& session : registered_projects()["sessions"])
            if (session["actor"] == "Agente con error") failed_session = session["state"] == "FAILED";
        check(failed_session, "La actividad distingue el fallo funcional del código de salida CLI");

        const auto guide=qa/L"source/guide.md";
        const auto guide_id=id_for(index_snapshot(project,false),guide);
        enable_source_only(gui.window);
        wait_until([&] {
            const auto config=Project::load(project.file);
            return config.config.value("inventory_mode", "universal")=="source_only" &&
                   db.integer("SELECT COUNT(*) FROM files WHERE absolute_path=? AND deleted=1 AND json_extract(data,'$.filtered_by_inventory_mode')=1",{path_text(guide)})==1;
        }, "La GUI guarda y aplica Solo código fuente al inventario existente");
        check(fs::is_regular_file(guide),"Aplicar el filtro conserva el documento físico");
        check(db.integer("SELECT COUNT(*) FROM history WHERE action='filtered_out' AND entity=?",{guide_id})==1,
              "La exclusión conserva el ID y se registra como cambio de alcance");
        const auto before_new=db.revision();
        write_text(qa/L"source/nueva/after.py","print('creado con GUI abierta')\n");
        write_text(qa/L"source/nueva/custom.NFX","custom command\n");
        write_text(qa/L"source/nueva/ProjectHooks","project hook\n");
        write_text(qa/L"source/nueva/run-tool","#!/usr/bin/env python3\nprint(1)\n");
        write_text(qa/L"source/nueva/data.json","{}\n");
        write_text(qa/L"source/nueva/binary.py",std::string("\0\1\2",3));
        write_text(qa/L"source/nueva/unsupported.odd","plain data\n");
        command(gui.window,1001);
        choose_filter(gui.window,0);
        wait_until([&]{return ListView_GetItemCount(child(gui.window,50))==6;},
                   "GUI incorpora fuentes nuevas, extensión propia, nombre propio y shebang");
        const auto query=cli({"agent","query","--project",path_text(project.file),"--kind","new","--since-revision",
            std::to_string(before_new),"--fields","path","--budget-tokens","5000"});
        std::set<std::string> new_paths;
        for(const auto& row:query["items"])new_paths.insert(row.at("path"));
        check(new_paths==std::set<std::string>{"nueva/after.py","nueva/custom.NFX","nueva/ProjectHooks","nueva/run-tool"},
              "Consulta de nuevos omite datos, binarios y formatos sin reconocer");
        const auto after_new=cli({"agent","context","--project",path_text(project.file),"--budget-tokens","2000"});
        check(after_new.at("total_units")==6,"Las fuentes nuevas actualizan el total de unidades");
        gui.close();

        write_text(qa/L"source/nueva/offline.py","print('creado con GUI cerrada')\n");
        check(db.integer("SELECT COUNT(*) FROM files WHERE path='nueva/offline.py'")==0,"La fuente offline aún no estaba inventariada");
        {
            Gui reopened;
            wait_until([&]{return ListView_GetItemCount(child(reopened.window,50))==7;},
                       "La reconciliación al reabrir descubre la fuente creada con la GUI cerrada");
            reopened.close();
        }
        project=Project::load(project.file);
        const auto filtered=index_sync(project,true);
        check(filtered["inventory_mode"]=="source_only"&&filtered["filtered_non_code"]==4&&filtered["hashed_files"]==12,
              "Escaneo completo calcula huellas de código y omite los cuatro archivos ajenos");
        project.config["inventory_mode"]="universal";
        project.save();
        index_sync(project,false);
        check(id_for(index_snapshot(project),guide)==guide_id,"Volver al inventario universal restaura el mismo ID");

        const auto archive_input=qa/L"zip-fixture/nested";
        const auto original=qa/L"source/duplicate.py";
        const auto duplicate=qa/L"target/copy.py";
        const auto zip=qa/L"source/respaldo código.zip";
        const std::string source_bytes=std::string(130*1024,'#')+"\nprint('duplicado exacto')\n";
        write_text(original,source_bytes);
        write_text(duplicate,source_bytes);
        write_text(archive_input/L"clone.py",source_bytes);
        write_text(archive_input/L"near.py",source_bytes+"\n");
        write_text(archive_input/L"notes.txt",source_bytes);
        const auto tar=fs::path(L"C:/Windows/System32/tar.exe");
        const auto zipped=run_process({path_text(tar),"-a","-cf",path_text(zip),"-C",path_text(archive_input.parent_path()),"nested"},
                                     qa,"",qa/L"zip-creation",10000);
        check(zipped.at("exit_code")==0&&fs::is_regular_file(zip),"El fixture ZIP nativo se creó con éxito");
        const auto original_zip_hash=hash_file(zip);
        write_text(qa/L"source/invalido.zip","No es un archivo ZIP\n");
        auto get_duplicates=[&](const char* kind){
            return cli({"agent","query","--project",path_text(project.file),"--kind",kind,"--budget-tokens","12000"});
        };
        const auto inactive=get_duplicates("code_duplicates");
        check(inactive.at("report").at("mode")=="off"&&inactive.at("items").empty(),"Duplicados exactos es optativo");
        project.config["code_duplicate_mode"]="files";
        project.save();
        auto only_files=index_sync(project,false);
        check(only_files["code_duplicates"]["archive_count"]==0&&get_duplicates("code_duplicates").at("items").size()==2,
              "El modo carpetas encuentra ambas copias sin abrir ZIP");
        Json copies,zip_errors,after_added_zip;
        {
            Gui duplicate_gui;
            wait_until([&]{return ListView_GetItemCount(child(duplicate_gui.window,50))>=8;},"Proyecto de duplicados abierto");
            enable_source_only(duplicate_gui.window,2);
            command(duplicate_gui.window,1015);
            wait_until([&]{return ListView_GetItemCount(child(duplicate_gui.window,40))==5;},
                       "La GUI muestra tres copias exactas y el ZIP que no se pudo leer");
            copies=get_duplicates("code_duplicates");
            zip_errors=get_duplicates("code_duplicate_errors");
            const auto new_zip=qa/L"source/nueva/otro respaldo.zip";
            fs::copy_file(zip,new_zip);
            wait_until([&]{return ListView_GetItemCount(child(duplicate_gui.window,40))==8;},
                       "Un ZIP nuevo amplía el primer grupo y crea otro para near.py");
            after_added_zip=get_duplicates("code_duplicates");
            bool new_archive_copy=false;
            for(const auto& copy:after_added_zip.at("items"))
                if(copy.at("kind")=="zip"&&copy.at("absolute_path")==path_text(new_zip)&&copy.at("entry_path")=="nested/clone.py")
                    new_archive_copy=copy.at("copy_count")==4;
            check(new_archive_copy,"La nueva copia ZIP aparece con la ruta interna y cuatro coincidencias");
            check(after_added_zip.at("report").at("group_count")==2&&after_added_zip.at("items").size()==6,
                  "Los dos ZIP comparten también near.py, que difiere de clone.py");
            duplicate_gui.close();
        }
        check(copies.at("report").at("mode")=="files_and_zip"&&copies.at("report").at("fresh")==true&&
              copies.at("report").at("complete")==false&&copies.at("report").at("group_count")==1,
              "Informe ZIP distingue resultados actuales y revisión parcial");
        check(copies.at("items").size()==3,"Se excluyen el código parecido y la entrada de documentación");
        bool archived_copy=false;
        for(const auto& copy:copies.at("items")){
            check(copy.at("sha256")==sha256(source_bytes)&&copy.at("bytes")==source_bytes.size(),
                  "La coincidencia usa los bytes completos, incluso por encima del bloque de lectura");
            if(copy.at("kind")=="zip")archived_copy=copy.at("absolute_path")==path_text(zip)&&copy.at("entry_path")=="nested/clone.py";
        }
        check(archived_copy,"El resultado conserva la ruta Unicode y la entrada exacta dentro del ZIP");
        check(zip_errors.at("items").size()==1&&zip_errors.at("items")[0]["absolute_path"]==path_text(qa/L"source/invalido.zip"),
              "Un ZIP ilegible se informa con su ruta");
        check(hash_file(zip)==original_zip_hash&&db.integer("SELECT COUNT(*) FROM files WHERE path LIKE '%.zip' AND deleted=0")==0,
              "La inspección conserva el ZIP y separa sus entradas del inventario de migración");
        project=Project::load(project.file);
        write_json(qa / L"receipt.json", {{"passed", true}, {"checks", checks.load()}, {"at", now()},
            {"project", path_text(project.file)}, {"registry", path_text(registry)}, {"operations", operations},
            {"source_only_sync",filtered},{"new_source_paths",new_paths},{"duplicates",copies},
            {"duplicate_errors",zip_errors},{"duplicates_after_new_zip",after_added_zip}});
        std::cout << "PASS " << checks.load() << " comprobaciones. Evidencia " << path_text(qa) << '\n';
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        if (!qa.empty()) write_json(qa / L"failure.json", {{"error", error.what()}, {"checks", checks.load()}, {"operations", operations}});
        std::cerr << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
