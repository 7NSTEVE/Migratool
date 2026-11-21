#include "migratool/core.hpp"
#include "migratool/gateway.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/live.hpp"
#include "migratool/scale.hpp"
#include "migratool/workspace_registry.hpp"
#include "migratool/lsp_client.hpp"
#include "migratool/env_registry.hpp"
#include <iostream>
#include <map>
#include <set>
#include <deque>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>

namespace {
using namespace mt;
struct Args {
    std::string action;
    std::map<std::string,std::vector<std::string>> values;
    Args(int argc,wchar_t** argv) {
        if(argc<2) {action="help";return;} action=utf8(argv[1]);
        int begin=2;if(action=="agent"){if(argc<3)throw std::runtime_error("agent requiere bootstrap, complete, context, task, sync, next, query, batch, edit, report, status, changes, history, verify o watch");action="agent/"+utf8(argv[2]);begin=3;}
        for(int i=begin;i<argc;++i) {
            auto name=utf8(argv[i]);if(!name.starts_with("--"))throw std::runtime_error("Se esperaba opción --: "+name);
            if(name=="--complete"||name=="--partial"||name=="--no-fresh"||name=="--full"||name=="--execute"||name=="--dry-run"||name=="--include-body"||name=="--async"||name=="--outline-only"||name=="--auto-sync"||name=="--queue-only"||
               name=="--json-stdin"||name=="--available-only"||name=="--prune"||name=="--flat"||name=="--sync"||name=="--no-sync"||name=="--resume")values[name].push_back("true");
            else {if(i+1>=argc)throw std::runtime_error("Opción sin valor: "+name);values[name].push_back(utf8(argv[++i]));}
        }
    }
    std::string one(const std::string& key,const std::string& fallback="") const {
        auto it=values.find(key);if(it==values.end())return fallback;
        if(it->second.size()!=1)throw std::runtime_error("Opción repetida: "+key);return it->second[0];
    }
    std::string required(const std::string& key) const {auto v=one(key);if(v.empty())throw std::runtime_error("Falta "+key);return v;}
    std::vector<std::string> all(const std::string& key) const {auto it=values.find(key);return it==values.end()?std::vector<std::string>{}:it->second;}
    bool has(const std::string& key) const {return values.contains(key);}
    void allow(std::initializer_list<const char*> keys) const {
        std::set<std::string> allowed={"--project"};for(auto k:keys)allowed.insert(k);
        for(const auto& [key,v]:values)if(!allowed.contains(key))throw std::runtime_error("Opción desconocida: "+key);
    }
};
// Prioridad: --project explícito > MIGRATOOL_PROJECT > migration.json ascendente.
// No descubre otros archivos arbitrarios: evita seleccionar proyectos ambiguos.
fs::path resolve_project(const Args& args) {
    if(args.has("--project"))return fs::path(wide(args.required("--project")));
    const DWORD length=GetEnvironmentVariableW(L"MIGRATOOL_PROJECT",nullptr,0);
    if(length){
        std::wstring value(length,L'\0');
        const DWORD copied=GetEnvironmentVariableW(L"MIGRATOOL_PROJECT",value.data(),length);
        if(copied>0&&copied<length){value.resize(copied);return fs::path(value);}
        throw std::runtime_error("No se pudo leer MIGRATOOL_PROJECT");
    }
    auto dir=fs::current_path();
    while(true){
        auto candidate=dir/L"migration.json";
        if(fs::is_regular_file(candidate))return candidate;
        auto parent=dir.parent_path();
        if(parent.empty()||parent==dir)break;
        dir=parent;
    }
    throw std::runtime_error("Falta --project. Configure MIGRATOOL_PROJECT o coloque migration.json en el directorio actual o un ancestro.");
}
Json agent_arguments(const Args& args) {
    const int src_cnt = (args.has("--input")?1:0) + (args.has("--json")?1:0) + (args.has("--json-stdin")?1:0);
    if(src_cnt>1)throw std::runtime_error("Use solamente uno de --input, --json o --json-stdin");
    Json request=Json::object();
    if(args.has("--input")) request=read_json(fs::path(wide(args.one("--input"))));
    else if(args.has("--json")) request=Json::parse(args.one("--json"));
    else if(args.has("--json-stdin")) {
        std::ostringstream ss; ss<<std::cin.rdbuf();
        std::string raw=ss.str();
        // Strip optional UTF-8 BOM from PowerShell piping
        if(raw.size()>=3&&static_cast<unsigned char>(raw[0])==0xEF&&static_cast<unsigned char>(raw[1])==0xBB&&static_cast<unsigned char>(raw[2])==0xBF) raw.erase(0,3);
        if(!raw.empty()) request=Json::parse(raw);
    }
    if(!request.is_object())throw std::runtime_error("El JSON de argumentos debe ser un objeto");
    for(const auto* key:{"--cursor","--kind","--text","--role","--file","--id","--level","--actor","--action","--root","--category","--language","--detail","--entity","--symbol","--calls","--disposition","--state","--preset","--extension","--path","--name","--mode","--query","--ext","--sort","--zip"})
        if(args.has(key))request[std::string(key+2)]=args.one(key);
    if(args.has("--phase-id"))request["phase_id"]=args.one("--phase-id");
    if(args.has("--task-id"))request["task_id"]=args.one("--task-id");
    if(args.has("--plan-id"))request["plan_id"]=args.one("--plan-id");
    if(args.has("--target-dir"))request["target_dir"]=args.one("--target-dir");
    if(args.has("--quarantine"))request["quarantine_dir"]=args.one("--quarantine");
    if(args.has("--path-prefix"))request["path_prefix"]=args.one("--path-prefix");
    if(args.has("--path-contains"))request["path_contains"]=args.one("--path-contains");
    if(args.has("--new-name"))request["new_name"]=args.one("--new-name");
    if(args.has("--new-body"))request["new_body"]=args.one("--new-body");
    if(args.has("--signature-contains"))request["signature_contains"]=args.one("--signature-contains");
    if(args.has("--symbol-kind"))request["symbol_kind"]=args.one("--symbol-kind");
    if(args.has("--migration-state"))request["migration_state"]=args.one("--migration-state");
    if(args.has("--missing-target-language"))request["missing_target_language"]=args.one("--missing-target-language");
    if(args.has("--ast-pattern"))request["ast_pattern"]=args.one("--ast-pattern");
    if(args.has("--depth"))request["depth"]=std::stoi(args.one("--depth"));
    if(args.has("--threads"))request["threads"]=std::stoi(args.one("--threads"));
    if(args.has("--min-bytes"))request["min_bytes"]=std::stoll(args.one("--min-bytes"));
    if(args.has("--budget-tokens"))request["token_budget"]=std::stoi(args.one("--budget-tokens"));
    if(args.has("--token-budget"))request["token_budget"]=std::stoi(args.one("--token-budget"));
    if(args.has("--limit"))request["limit"]=std::stoi(args.one("--limit"));
    if(args.has("--since-revision"))request["since_revision"]=std::stoll(args.one("--since-revision"));
    if(args.has("--expected-revision"))request["expected_revision"]=std::stoll(args.one("--expected-revision"));
    if(args.has("--full"))request["full"]=true;
    if(args.has("--execute"))request["execute"]=true;
    if(args.has("--dry-run"))request["dry_run"]=true;
    if(args.has("--include-body"))request["include_body"]=true;
    if(args.has("--outline-only"))request["outline_only"]=true;
    if(args.has("--auto-sync"))request["auto_sync_touched"]=true;
    if(args.has("--async"))request["wait"]=false;
    if(args.has("--available-only"))request["available_only"]=true;
    if(args.has("--prune"))request["action"]="prune";
    if(args.has("--flat"))request["flat"]=true;
    if(args.has("--sync"))request["sync"]=true;
    if(args.has("--no-sync"))request["sync"]=false;
    if(args.has("--resume"))request["resume"]=true;
    if(args.has("--out"))request["out"]=args.one("--out");if(args.has("--format"))request["format"]=args.one("--format");
    if(args.has("--expected-config-revision"))request["expected_config_revision"]=args.one("--expected-config-revision");
    for(const auto* key:{"--job-id","--catalog-path","--registry-path","--external-path"})if(args.has(key)){std::string name=key+2;std::replace(name.begin(),name.end(),'-','_');request[name]=args.one(key);}
    auto parse_csv=[&](const std::string& flag,const std::string& field){
        if(!args.has(flag))return;
        request[field]=Json::array();std::istringstream values(args.one(flag));std::string value;
        while(std::getline(values,value,',')){
            const auto first=value.find_first_not_of(" \t");
            if(first==std::string::npos)continue;
            const auto last=value.find_last_not_of(" \t");
            request[field].push_back(value.substr(first,last-first+1));
        }
    };
    parse_csv("--fields","fields");
    parse_csv("--task-ids","task_ids");
    return request;
}
Json capabilities() {
    return {{"schema","migratool.capabilities/1"},{"version",build_identity()["version"]},{"encoding","UTF-8"},{"default_output","JSON"},
      {"languages","Inventario de cualquier archivo; Roslyn C#, libclang C/C++/Objective-C; analizadores externos por extensión"},
      {"directions","Múltiples raíces source y target; cualquier lenguaje; correspondencias uno a muchos y muchos a uno"},
      {"inventory_modes",{{"universal","Inventario de todos los archivos observados"},{"source_only","Solo código reconocido o declarado en code_extensions o code_filenames"}}},
      {"code_duplicate_modes",{{"off","Desactivado"},{"files","Código idéntico en carpetas"},{"files_and_zip","Código idéntico en carpetas y entradas ZIP"}}},
      {"commands",{
        {"init","--project <nuevo.json> --name <nombre> --source <ruta> [repetible] --target <ruta> [repetible]"},
        {"projects","Proyectos conocidos por CLI, MCP y GUI; registro compartido del usuario"},
        {"activity","--project <archivo>; operaciones y mensajes de agentes"},
        {"scan","--project <archivo>"},{"inventory","--project <archivo> [--kind files|symbols|dependencies]"},
        {"status","--project <archivo> [--no-fresh]"},{"pending","--project <archivo> [--no-fresh]"},
        {"suggest","--project <archivo>"},{"map","--project <archivo> --source <id> --target <id> [repetible] [--complete --actor <agente>] [--case <id>] [--note <texto>]"},
        {"mark","--project <archivo> --source <ruta-absoluta-exacta> --target <ruta-absoluta-exacta> [repetible] --complete|--partial --actor <agente> [--case <id>] [--note <texto>]"},
        {"note","--project <archivo> --text <texto> --actor <agente> [--source <ruta>]"},
        {"history","--project <archivo> [--limit <1..10000>]"},{"build","--project <archivo>"},{"test","--project <archivo> [--case <id>] (repetible)"},
        {"coverage","--project <archivo> --root <id> --format llvm|cobertura|generic --input <informe>"},
        {"backup","--project <archivo> [--out <respaldo.sqlite>]"},
        {"report","--project <archivo> --out <informe.html>"},{"check","--project <archivo>; exit 4 si faltan evidencias"}}},
      {"workspace_commands","agent activate|workspaces|query-ws|onboard|explore|find|feed|memory|read|create|edit-file|grep|diagnostics|impls|decl|lsp-rename|restart-lsp|housekeep [--project <archivo>] [--input <args.json> | --json-stdin | --json <objeto>]"},
      {"organize_commands","agent duplicates|verify-zip|topology|organize-plan|organize-apply|organize-undo|organize-verify [--project <archivo>] [--input <args.json> | --json-stdin] [--execute] [--resume]"},
      {"agent_commands","agent bootstrap|complete|context|task|sync|next|query|batch|edit|report|status|modules|phases|tasks|docs|changes|history|verify|statistics|connections|watch [--project <archivo>] [--input <args.json> | --json-stdin | --json <objeto>] [--role source] [--file F1] [--execute]"},
      {"powershell_note","En PowerShell 5.1, use --input archivo.json, --json-stdin (ej. '{\"depth\":1}' | migratool-cli.exe ... --json-stdin) o flags directos (--path, --query, --ext, --depth, --flat) para evitar que PowerShell elimine comillas internas de --json."},
      {"project_resolution","--project explícito, luego MIGRATOOL_PROJECT, luego migration.json en cwd o ancestros (opcional en mcp)"},
      {"mcp","mcp [--project <archivo>]; arranca sin proyecto si se omite --project y admite activate_workspace o project absoluto por llamada"},
      {"exit_codes",{{"0","Operación realizada; consultas pueden contener pendientes"},{"1","Error de configuración, archivo o ejecución"},{"3","Compilación o prueba no aprobada"},{"4","Inventario incompleto, sin casos o evidencias pendientes"}}},
      {"evidence_rule","Una marca completa es declaración del actor; VERiFIED_IN_CASES solo acredita los casos configurados. No existe porcentaje de paridad funcional inferido."}};
}
}
int wmain(int argc,wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::unique_ptr<LiveSession> live;
    try {
        Args args(argc,argv);Json result;int code=0;
        if(args.action=="help"||args.action=="--help"||args.action=="capabilities"||args.action=="--version"){result=capabilities();result["build"]=build_identity();result["scale_commands"]="agent sync --async | jobs | job | settings | acquire | reuse | doctor | export --out <ruta> [--format jsonl|json] [--async]";}
        else if(args.action=="__coordinator"){args.allow({"--queue-only"});return scale_coordinator(Project::load(resolve_project(args)),false,!args.has("--queue-only"));}
        else if(args.action=="__analyze"){args.allow({"--input","--out"});return scale_analyzer(Project::load(resolve_project(args)),fs::path(wide(args.required("--input"))),fs::path(wide(args.required("--out"))));}
        else if(args.action=="projects") {args.allow({});result=registered_projects();}
        else if(args.action=="agent/activate"||args.action=="agent/activate_workspace") {
            Json req=agent_arguments(args);
            result=tool_activate_workspace(req,nullptr,[](const std::string& m){std::cerr<<m<<'\n';});
        }
        else if(args.action=="agent/workspaces"||args.action=="agent/list_workspaces") {
            result=tool_list_workspaces(agent_arguments(args));
        }
        else if(args.action=="agent/query-ws"||args.action=="agent/query_workspace") {
            result=tool_query_workspace(agent_arguments(args),[](const std::string& m){std::cerr<<m<<'\n';});
        }
        else if((args.action=="agent/env-tools"||args.action=="agent/env_tools") && !args.has("--project") && GetEnvironmentVariableW(L"MIGRATOOL_PROJECT",nullptr,0)==0) {
            result=tool_env_tools(std::nullopt,agent_arguments(args));
        }
        else if(args.action=="mcp") {
            args.allow({"--profile","--actor"});
            std::optional<Project> initial_p;
            if(args.has("--project") || GetEnvironmentVariableW(L"MIGRATOOL_PROJECT",nullptr,0)>0) {
                initial_p=Project::load(resolve_project(args));
                live=std::make_unique<LiveSession>(*initial_p,"MCP","mcp",args.one("--actor"));
            }
            const auto exit=mcp_server(initial_p, args.one("--profile","full"));
            if(live)live->finish(exit);
            return exit;
        }
        else if(args.action=="init") {
            args.allow({"--name","--source","--target"});Json roots=Json::array();
            for(const auto* role:{"source","target"}) {
                size_t n=0;for(const auto& path:args.all(std::string("--")+role)) {
                    auto actual=fs::canonical(fs::path(wide(path)));if(!fs::is_directory(actual))throw std::runtime_error("Raíz no es directorio");
                    roots.push_back({{"id",std::string(role)+std::to_string(++n)},{"role",role},{"path",path_text(actual)}});
                }
            }
            auto p=Project::create(fs::path(wide(args.required("--project"))),args.one("--name","Nueva migración"),roots);
            live=std::make_unique<LiveSession>(p,"CLI","init");
            result={{"ok",true},{"project_path",path_text(p.file)},{"project",p.config}};
        } else {
            auto p=Project::load(resolve_project(args));
            if(args.action!="activity")live=std::make_unique<LiveSession>(p,args.action=="mcp"?"MCP":"CLI",args.action,args.one("--actor"));
            auto log=[&live](const std::string& text){std::cerr<<text<<'\n';if(live)live->log(text);};
            if(args.action=="activity"){args.allow({});result=project_activity(p.file);}
            else if(args.action.starts_with("agent/")){
                args.allow({"--input","--json","--json-stdin","--budget-tokens","--token-budget","--limit","--cursor","--kind","--text","--fields","--preset","--since-revision","--full","--seconds","--reconcile-ms",
                    "--role","--file","--id","--level","--actor","--action","--root","--category","--language","--detail","--entity","--expected-revision","--execute",
                    "--task-id","--task-ids","--symbol","--new-name","--new-body","--calls","--signature-contains","--symbol-kind","--migration-state","--missing-target-language","--ast-pattern","--disposition","--state","--phase-id","--depth","--dry-run","--include-body","--outline-only","--auto-sync","--async","--job-id","--catalog-path","--registry-path","--external-path","--expected-config-revision","--out","--format","--path-prefix","--path-contains","--extension",
                    "--path","--name","--mode","--query","--ext","--sort","--min-bytes","--quarantine","--plan-id","--zip","--target-dir","--threads","--available-only","--prune","--flat","--sync","--no-sync","--resume"});
                auto command=args.action.substr(6);Json request=agent_arguments(args);
                if(live)live->actor(request.value("actor",std::string()));
                if(command=="watch"){
                    auto seconds=std::stoi(args.one("--seconds","60"));if(seconds<1||seconds>86400)throw std::runtime_error("seconds debe estar entre 1 y 86400");index_sync(p,false,log);IndexWatcher watcher(p,static_cast<unsigned>(std::stoi(args.one("--reconcile-ms","900000"))),log);auto start=GetTickCount64();while(GetTickCount64()-start<static_cast<ULONGLONG>(seconds)*1000)Sleep(100);result={{"ok",true},{"seconds",seconds}};
                }else{
                    const std::map<std::string,std::string> routes={{"wake","agent_wake"},{"agent_wake","agent_wake"},{"delta","delta_since"},{"delta_since","delta_since"},{"repo-map","repo_map"},{"repo_map","repo_map"},{"read-bundle","read_bundle"},{"read_bundle","read_bundle"},{"batch-edit","batch_edit"},{"batch_edit","batch_edit"},{"run-verified","run_verified"},{"run_verified","run_verified"},{"impact","impact_analysis"},{"impact_analysis","impact_analysis"},{"workstream","workstream"},{"knowledge","knowledge"},{"docs-hub","docs_hub"},{"docs_hub","docs_hub"},{"path-history","path_history"},{"path_history","path_history"},{"git-intel","git_intel"},{"git_intel","git_intel"},{"env-tools","env_tools"},{"env_tools","env_tools"},{"bootstrap","session_bootstrap"},{"session_bootstrap","session_bootstrap"},{"explore","explore_tree"},{"explore_tree","explore_tree"},{"find","find_files"},{"find_files","find_files"},{"feed","changes_feed"},{"changes_feed","changes_feed"},{"duplicates","find_duplicates"},{"find_duplicates","find_duplicates"},{"verify-zip","verify_archive_vs_dir"},{"verify_archive_vs_dir","verify_archive_vs_dir"},{"topology","inspect_workspace_topology"},{"inspect_workspace_topology","inspect_workspace_topology"},{"organize-plan","organize_plan"},{"organize_plan","organize_plan"},{"organize-apply","organize_apply"},{"organize_apply","organize_apply"},{"organize-undo","organize_undo"},{"organize_undo","organize_undo"},{"organize-verify","organize_verify"},{"organize_verify","organize_verify"},{"onboard","onboard_workspace"},{"onboard_workspace","onboard_workspace"},{"memory","memory"},{"read","read_file"},{"read_file","read_file"},{"create","create_file"},{"create_file","create_file"},{"edit-file","edit_file"},{"edit_file","edit_file"},{"grep","search_pattern"},{"search_pattern","search_pattern"},{"diagnostics","get_diagnostics"},{"get_diagnostics","get_diagnostics"},{"impls","find_implementations"},{"find_implementations","find_implementations"},{"decl","find_declaration"},{"find_declaration","find_declaration"},{"lsp-rename","lsp_rename_symbol"},{"lsp_rename_symbol","lsp_rename_symbol"},{"restart-lsp","restart_language_server"},{"restart_language_server","restart_language_server"},{"housekeep","housekeep"},{"complete","complete_task"},{"complete_task","complete_task"},{"export","export_inventory"},{"jobs","job_status"},{"job","job_control"},{"settings","project_settings"},{"acquire","acquire_task"},{"reuse","import_existing_work"},{"doctor","project_diagnostics"},{"context","project_context"},{"task","get_task_context"},{"get_task_context","get_task_context"},{"edit","edit_symbol"},{"edit_symbol","edit_symbol"},{"sync","inventory_sync"},{"next","next_tasks"},{"batch","batch_update"},{"report","evidence_report"},{"status","migration_status"},{"modules","migration_status"},{"phases","migration_status"},{"tasks","migration_status"},{"docs","migration_status"},{"changes","changes_since"},{"history","history"},{"query","query"},{"verify","verify"},{"statistics","statistics"},{"connections","connections"}};
                    auto tool=routes.find(command);if(tool==routes.end())throw std::runtime_error("agent subcomando desconocido");
                    std::string resolved_tool=tool->second;
                    if(command=="changes"&&project_mode(p)=="archive"&&!request.contains("since_revision"))resolved_tool="changes_feed";
                    if((command=="modules"||command=="phases"||command=="tasks"||command=="docs")&&!request.contains("level"))request["level"]=command;
                    result=agent_request(p,resolved_tool,request,log);
                }
            }
            else if(args.action=="scan") {args.allow({});result=scan(p,log);if(!result.value("complete",false))code=4;}
            else if(args.action=="inventory") {args.allow({"--kind"});auto s=load_scan(p);auto key=args.one("--kind","files");if(key!="files"&&key!="symbols"&&key!="dependencies")throw std::runtime_error("kind inválido");result={{"schema","migratool.inventory/1"},{"kind",key},{"at",s["at"]},{"items",s[key]}};}
            else if(args.action=="status") {args.allow({"--no-fresh"});result=status(p,!args.has("--no-fresh"));}
            else if(args.action=="pending"||args.action=="check") {args.allow({"--no-fresh"});result=pending(p,!args.has("--no-fresh"));if(args.action=="check"&&result["count"]!=0)code=4;}
            else if(args.action=="suggest") {args.allow({});result={{"suggestions",suggestions(p,load_scan(p))},{"verified",false}};}
            else if(args.action=="map") {args.allow({"--source","--target","--case","--note","--complete","--actor"});if(args.has("--complete"))args.required("--actor");auto mapped=indexed_mapping(p,args.required("--source"),args.all("--target"),args.all("--case"),args.has("--complete"),args.one("--note"),args.one("--actor"));result={{"ok",true},{"mapping",mapped}};}
            else if(args.action=="mark") {
                args.allow({"--source","--target","--complete","--partial","--actor","--case","--note"});
                if(args.has("--complete")==args.has("--partial"))throw std::runtime_error("Indique exactamente --complete o --partial");
                auto normalized_unit=[](const std::string& input) {
                    if(input.size()>1 && (input[0]=='F'||input[0]=='S') &&
                       std::all_of(input.begin()+1,input.end(),[](char ch){return ch>='0'&&ch<='9';}))return input;
                    auto file=fs::path(wide(input));
                    if(!file.is_absolute())throw std::runtime_error("mark requiere ID F/S o ruta absoluta: "+input);
                    return path_text(fs::canonical(file));
                };
                const auto source_id=normalized_unit(args.required("--source"));
                std::vector<std::string> target_ids;
                for(const auto& target:args.all("--target"))target_ids.push_back(normalized_unit(target));
                if(target_ids.empty())throw std::runtime_error("Debe proporcionar al menos un --target");
                const auto actor=args.required("--actor");
                const auto path_argument=[](const std::string& value){return fs::path(wide(value)).is_absolute();};
                Json mapped;
                if(path_argument(source_id)&&std::all_of(target_ids.begin(),target_ids.end(),path_argument)){
                    std::vector<fs::path> destinations;
                    for(const auto& target:target_ids)destinations.emplace_back(wide(target));
                    mapped=mark_file(p,fs::path(wide(source_id)),destinations,args.has("--complete"),args.one("--note"),actor,args.all("--case"));
                }else{
                    index_sync(p,false,log);
                    mapped=indexed_mapping(p,source_id,target_ids,args.all("--case"),args.has("--complete"),args.one("--note"),actor);
                }
                result={{"ok",true},{"declaration",mapped},{"universal_equivalence_proven",false}};
            }
            else if(args.action=="note") {args.allow({"--text","--actor","--source"});add_note(p,args.required("--text"),args.required("--actor"),args.one("--source"));result={{"ok",true},{"changelog_path",path_text(p.state_dir()/L"changelog.jsonl")}};}
            else if(args.action=="history") {
                args.allow({"--limit"});auto n=std::stoi(args.one("--limit","100"));if(n<1||n>10000)throw std::runtime_error("Límite fuera de rango");
                std::ifstream in(p.state_dir()/L"changelog.jsonl",std::ios::binary);std::string line;std::deque<Json> lines;
                while(std::getline(in,line))if(!line.empty()){lines.push_back(Json::parse(line));if(lines.size()>static_cast<size_t>(n))lines.pop_front();}
                result={{"events",Json(lines)},{"path",path_text(p.state_dir()/L"changelog.jsonl")}};
            }
            else if(args.action=="build"||args.action=="test") {
                const bool build=args.action=="build";
                if (build) args.allow({});
                else args.allow({"--case"});
                result=build?run_builds(p,log):run_cases(p,log,nullptr,args.all("--case"));auto key=build?"builds":"cases";
                if(result[key].empty())code=4;
                for(const auto& v:result[key])if(v["state"]!="PASS")code=3;
                if(!result.value("stable",false))code=3;
            }
            else if(args.action=="coverage") {args.allow({"--root","--input","--format"});result=import_coverage(p,fs::path(wide(args.required("--input"))),args.required("--root"),args.required("--format"),log);}
            else if(args.action=="backup") {
                args.allow({"--out"});
                auto out_str=args.one("--out");
                fs::path dest=out_str.empty()?(p.state_dir()/L"backups"/L"catalog-backup.sqlite"):fs::path(wide(out_str));
                result=backup_catalog(p,dest);
            }
            else if(args.action=="report") {args.allow({"--out"});auto out=fs::absolute(fs::path(wide(args.required("--out"))));report(p,out);result={{"ok",true},{"html",path_text(out)},{"json",path_text(out.parent_path()/(out.stem().wstring()+L".json"))}};}
            else throw std::runtime_error("Comando desconocido; consulte capabilities");
        }
        lsp_shutdown_all();
        if(live)live->finish(code,result);
        std::cout<<result.dump(args.action.starts_with("agent/")?-1:2,' ',false,Json::error_handler_t::replace)<<'\n';return code;
    }catch(const std::exception& e){
        lsp_shutdown_all();
        const Json result={{"ok",false},{"error",e.what()}};
        try{if(live)live->finish(1,result);}catch(...){}
        std::cout<<result.dump(2,' ',false,Json::error_handler_t::replace)<<'\n';return 1;
    }
}
