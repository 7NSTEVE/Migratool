#include "migratool/core.hpp"
#include "migratool/gateway.hpp"
#include "migratool/scale.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <iostream>
#include <cmath>

namespace {
using namespace mt;
int checks=0;
void check(bool ok,const std::string& message){++checks;if(!ok)throw std::runtime_error("FALLÓ: "+message);}
template<class F>void throws(F f,const std::string& message){bool raised=false;try{f();}catch(const std::exception&){raised=true;}check(raised,message);}
void sql(const fs::path& file,const char* statements){sqlite3* db{};if(sqlite3_open(path_text(file).c_str(),&db)!=SQLITE_OK)throw std::runtime_error("test db open");auto rc=sqlite3_exec(db,statements,nullptr,nullptr,nullptr);sqlite3_close(db);check(rc==SQLITE_OK,"fixture SQLite");}
}
int wmain(int argc,wchar_t** argv){
    using namespace mt;
    try {
        if(argc!=2)throw std::runtime_error("Requiere raíz del proyecto");
        auto qa=fs::path(argv[1])/L"build/qa"/wide(unique_id());fs::create_directories(qa);
        auto src=qa/L"fuente ñ",dst=qa/L"destino",dst2=qa/L"destino2";fs::create_directories(src);fs::create_directories(dst);fs::create_directories(dst2);
        write_text(src/L"modelo.cs","namespace Original; public class Invoice { public int Add(int a, int b) { if(a<0) return 0; return a+b; } public event System.Action? Changed; public const int Limit=10; }\n");
        write_text(src/L"other.toy","procedure custom_operation()\n");write_text(src/L"readme.txt","resource\n");write_text(src/L"bin/ignored.cs","generated");
        write_text(dst/L"logic.cpp","namespace Native { int Add(int a,int b){return a+b;} }\n");write_text(dst2/L"out.zig","pub fn operation() void {}\n");
        auto roots=Json::array({{{"id","old"},{"role","source"},{"path",path_text(src)}},{{"id","new"},{"role","target"},{"path",path_text(dst)}},{{"id","extra"},{"role","target"},{"path",path_text(dst2)}}});
        auto p=Project::create(qa/L"audit.json","Prueba genérica",roots);auto worker=path_text(executable_dir()/L"test_worker.exe");
        p.config["analyzers"].push_back({{"id","custom"},{"extensions",{".toy",".zig"}},{"argv",{worker,"analyzer","{file}"}}});p.save();
        check(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 conocido");
        check(utf8(wide("niño / 日本語"))=="niño / 日本語","UTF-8");
        check(within(src/L"x",src)&&!within(src/L"../destino",src),"contención de rutas");
        auto inv=scan(p);check(inv["complete"]==true,"inventario completo");check(inv["files"].size()==6,"inventariar artefactos sin contarlos como código a migrar");
        std::string model_id;for(const auto& f:inv["files"])if(f["path"]=="modelo.cs")model_id=f["id"];
        std::string add_source,add_target;bool event_found=false,constant=false,custom=false,branch=false;
        for(const auto& s:inv["symbols"]) {
            if(s["name"]=="Add"&&s["root"]=="old"){add_source=s["id"];branch=s.value("branches",Json::array()).size()==1;}
            if(s["name"]=="Add"&&s["root"]=="new")add_target=s["id"];
            if(s["name"]=="Changed")event_found=true;if(s["name"]=="Limit")constant=true;if(s["name"]=="custom_operation")custom=true;
        }
        check(!add_source.empty()&&!add_target.empty(),"Roslyn y libclang extraen métodos reales");check(event_found&&constant&&branch,"eventos, constantes y rama C#");check(custom,"analizador externo de lenguaje arbitrario");
        for(const auto& f:inv["files"])check(f["analysis"]!="ERROR","analizador sin error: "+f["path"].get<std::string>());
        auto s0=status(p);check(s0["migration_percent"]==0,"progreso inicial 0");
        add_mapping(p,add_source,{add_target},{},"Método revisado",true,"test-agent");auto s1=status(p);
        check(s1["migrated_units"]==1,"unidad completa contada");check(s1["migration_percent"].get<double>()>0&&s1["migration_percent"].get<double>()<100,"progreso parcial ponderado");
        auto marked=mark_file(p,src/L"modelo.cs",{dst/L"logic.cpp",dst2/L"out.zig"},true,"Nota de prueba","test-agent");
        check(marked["target_paths"].size()==2&&fs::path(wide(marked["source_path"])).is_absolute(),"rutas completas y uno a varios destinos");
        auto s2=status(p);bool file100=false;for(const auto& f:s2["files"])if(f["id"]==model_id)file100=f["migration_percent"]==100;
        check(file100&&s2["migration_percent"].get<double>()<100,"porcentaje por archivo y total por unidades");check(s2["files_verified_in_cases"]==0,"marca no inventa evidencia");
        add_note(p,"nota con fecha automática","agente");Database db(p);check(db.integer("SELECT COUNT(*) FROM history WHERE actor='test-agent'")>0,"changelog SQLite con actor");
        auto competing=Project::load(p.file);p.config["name"]="actualizado";p.save();competing.config["name"]="obsoleto";throws([&]{competing.save();},"no sobrescribir modificaciones concurrentes");
        auto other=p;other.file=qa/L"other.json";check(other.state_dir()!=p.state_dir(),"estado por proyecto, sin colisiones");
        auto work=qa/L"work";fs::create_directories(work);
        auto binary=std::string("datos\0bin",9);auto echoed=run_process({worker},work,binary,qa/L"echo",2000);
        check(echoed["exit_code"]==0&&read_text(fs::path(wide(echoed["stdout"])))==binary,"stdin/stdout binario conserva NUL");
        auto quoted=run_process({worker,"args","a b","\"quoted\"","tail\\",""},work,"",qa/L"args",2000);
        check(read_text(fs::path(wide(quoted["stdout"])))=="a b\r\n\"quoted\"\r\ntail\\\r\n\r\n","quoting Windows, espacios y argumentos vacíos");
        auto timed=run_process({worker,"sleep"},work,"",qa/L"timeout",80);check(timed["timed_out"]==true,"timeout de proceso");
        auto child=run_process({worker,"child"},work,"",qa/L"child",150);check(child["timed_out"]==true,"timeout incluye descendientes aunque padre termine");
        auto pid=std::stoul(read_text(fs::path(wide(child["stdout"]))));auto handle=OpenProcess(SYNCHRONIZE,FALSE,pid);
        if(handle){check(WaitForSingleObject(handle,1000)==WAIT_OBJECT_0,"descendiente terminado por Job Object");CloseHandle(handle);}else check(true,"descendiente ya salió");
        Cancel cancelled;cancelled.requested=true;auto stopped=run_process({worker,"sleep"},work,"",qa/L"cancel",2000,&cancelled);check(stopped["cancelled"]==true,"cancelación de trabajo propio");
        auto pair=[&](std::string id,std::vector<std::string> left,std::vector<std::string> right,std::string comparator="bytes") {
            return Json{{"id",id},{"stdin","same"},{"comparator",comparator},{"left",{{"root","old"},{"argv",left}}},{"right",{{"root","new"},{"argv",right}}}};
        };
        auto equal=pair("equal",{worker},{worker});equal["observers"]=Json::array({{{"kind","files"},{"path","effect.txt"}}});
        auto unequal=pair("unequal",{worker},{worker,"different"});
        auto json=pair("json-order",{worker,"json-left"},{worker,"json-right"},"json");
        auto bad=pair("both-bad",{worker},{worker});bad["expected_exit"]=2;
        p.config["cases"]=Json::array({equal,unequal,json,bad});p.save();
        auto runs=run_cases(p);check(runs["stable"]==true,"fuentes estables durante pruebas");check(runs["cases"][0]["state"]=="PASS","salidas y efectos iguales");
        check(runs["cases"][1]["state"]=="DIFFERENT","diferencia real detectada");check(runs["cases"][2]["state"]=="PASS","objetos JSON sin depender del orden de claves");
        check(runs["cases"][3]["state"]=="ERROR","dos fallos iguales no aprueban caso con salida no esperada");
        throws([&]{run_cases(p, {}, nullptr, {"inexistente"});},"rechazar caso inexistente antes de ejecutar");
        const auto selected = agent_request(p, "verify",
            {{"action","test"},{"execute",true},{"case_ids",{"json-order"}},{"token_budget",24000}}).at("result");
        check(selected.at("cases").size()==1 && selected.at("cases")[0].at("id")=="json-order",
              "verify ejecuta solo el caso solicitado");
        check(selected.at("reused_cases")==Json::array({"equal","unequal","both-bad"}),
              "conservar recibos vigentes de casos omitidos");
        const auto saved = read_json(p.state_dir()/L"latest-tests.json");
        const auto unchanged = std::find_if(saved["cases"].begin(), saved["cases"].end(),
            [](const Json& c){ return c.at("id")=="equal"; });
        check(unchanged!=saved["cases"].end() && unchanged->at("receipt_path")==runs["cases"][0]["receipt_path"],
              "el recibo omitido conserva su origen");
        auto observer=equal;observer["observers"]=Json::array({{{"kind","files"},{"path","../escape"}}});
        throws([&]{compare_outputs(observer,runs["cases"][0]["left"],runs["cases"][0]["right"],work,work);},"observadores no salen del directorio de trabajo");
        sql(qa/L"a.db","CREATE TABLE values_test(id INTEGER, value TEXT); INSERT INTO values_test VALUES(1,'a'),(2,'b');");
        sql(qa/L"b.db","CREATE TABLE values_test(id INTEGER, value TEXT); INSERT INTO values_test VALUES(2,'b'),(1,'a');");
        check(sqlite_snapshot(qa/L"a.db")==sqlite_snapshot(qa/L"b.db"),"comparación lógica SQLite independiente de orden físico");
        sql(qa/L"b.db","UPDATE values_test SET value='different' WHERE id=1;");check(sqlite_snapshot(qa/L"a.db")!=sqlite_snapshot(qa/L"b.db"),"diferencia SQLite detectada");
        p.config["builds"]=Json::array({{{"id","old-build"},{"root","old"},{"argv",{worker}},{"cwd",path_text(work)},{"artifacts",{worker}}},
            {{"id","new-build"},{"root","new"},{"argv",{worker}},{"cwd",path_text(work)},{"artifacts",{worker}}}});p.save();
        auto builds=run_builds(p);check(builds["stable"]==true&&builds["builds"][0]["state"]=="PASS","comando y artefacto registrado");
        mark_file(p,src/L"modelo.cs",{dst/L"logic.cpp"},true,"Vinculado a casos finitos","test-agent",{"equal"});
        auto evidence=status(p);check(evidence["files_verified_in_cases"]==1,"compilación y caso vinculado vigentes");check(evidence["functional_parity_percent"].is_null(),"no inventa porcentaje de equivalencia funcional");
        Json cover={{"schema","migratool.coverage/1"},{"files",Json::array({{{"path","modelo.cs"},{"lines",{{"count",1},{"covered",1}}},{"branches",{{"count",2},{"covered",1}}}}})}};
        write_json(qa/L"coverage.json",cover);auto cv=import_coverage(p,qa/L"coverage.json","old","generic");check(cv["files"][0]["branches"]["percent"]==50,"porcentaje real de cobertura con denominador");
        write_text(qa/L"cobertura.xml","<?xml version=\"1.0\"?><coverage><packages><package><classes><class filename=\"modelo.cs\"><lines><line number=\"1\" hits=\"1\" branch=\"true\" condition-coverage=\"50% (1/2)\"/></lines></class></classes></package></packages></coverage>");
        auto cob=import_coverage(p,qa/L"cobertura.xml","old","cobertura");check(cob["files"][0]["branches"]["covered"]==1,"importar Cobertura XML sin DTD");check(cob["files"][0]["branch_gaps"][0]["line"]==1,"señalar línea de rama sin cubrir");
        write_text(qa/L"unsafe.xml","<!DOCTYPE coverage [<!ENTITY secret SYSTEM 'file:///C:/private'>]><coverage>&secret;</coverage>");
        throws([&]{import_coverage(p,qa/L"unsafe.xml","old","cobertura");},"XML DTD prohibido");
        cover["files"][0]["lines"]["covered"]=3;write_json(qa/L"bad-coverage.json",cover);throws([&]{import_coverage(p,qa/L"bad-coverage.json","old","generic");},"rechazar cobertura imposible");
        p.config["name"]="<script>nota</script>";p.save();report(p,qa/L"report.html");auto html=read_text(qa/L"report.html");check(html.find("&lt;script&gt;nota&lt;/script&gt;")!=std::string::npos,"escapar contenido del informe");
        check(fs::exists(qa/L"report.json"),"informe JSON junto a HTML");
        auto leftlog=fs::path(wide(runs["cases"][0]["left"]["stdout"]));write_text(leftlog,"tampered");auto tampered=status(p);check(tampered["cases"][0]["state"]=="STALE","modificar log invalida prueba");
        p.config["cases"][3]["expected_exit"]=0;
        p.save();
        auto retry=run_cases(p, {}, nullptr, {"json-order"});
        check(retry["reused_cases"]==Json::array({"unequal"}),"omitir recibos con salida o configuración cambiadas");
        write_text(src/L"modelo.cs",read_text(src/L"modelo.cs")+"// cambiado\n");auto stale=status(p);check(stale["migration_percent"]==0&&stale["files_verified_in_cases"]==0,"fuente cambiada invalida marcas y evidencia");
        check(stale["declared_complete_files"]==0,"marca histórica no permanece vigente tras cambio");
        auto after_change=run_cases(p, {}, nullptr, {"json-order"});
        check(after_change["reused_cases"].empty(),"un cambio en las raíces invalida todos los recibos omitidos");
        auto unchecked=status(p,false);check(unchecked["freshness"]["checked"]==false&&unchecked["freshness"]["basis"]=="indexed_at_last_sync_with_watcher_invalidations","consulta caché declara base de vigencia");
        // Production hardening regressions (v3.1.0: CI-001, P2-015, DEEP-001, DEEP-003)
        check(build_identity().value("version", std::string{}) == "3.1.0", "build_identity version unificada == 3.1.0");
        // Repeated libclang invocations in the same process must never crash on FreeLibrary/TLS (CI-001)
        write_text(dst/L"logic.cpp","namespace Native { int Add(int a,int b){return a+b+1;} }\n");
        auto inv2 = scan(p);
        check(inv2["complete"]==true, "segunda invocación de libclang en el mismo proceso sin crash");
        write_text(dst/L"logic.cpp","namespace Native { int Add(int a,int b){return a+b+2;} }\n");
        auto inv3 = scan(p);
        check(inv3["complete"]==true, "tercera invocación de libclang en el mismo proceso sin crash");
        // is_path_within_dir edge cases including trailing '.' segments (DEEP-001)
        check(is_path_within_dir(src / L"." / L"modelo.cs", src / L".", false, true), "is_path_within_dir tolera segmentos '.' sin UB");
        check(!is_path_within_dir(src, src, false, true), "is_path_within_dir allow_equal=false rechaza igualdad");
        check(is_path_within_dir(src, src, true, true), "is_path_within_dir allow_equal=true acepta igualdad");
        check(!is_path_within_dir(src / L".." / L"destino" / L"logic.cpp", src, false, true), "is_path_within_dir rechaza traversal '..'");
        // file_toolkit create_file must reject traversal and outside-root writes (DEEP-003)
        throws([&]{ agent_request(p, "create_file", {{"path", "../escape_out.txt"}, {"content", "bad"}}); }, "create_file rechaza path traversal");
        std::cout<<"PASS: "<<checks<<" comprobaciones. Evidencia: "<<path_text(qa)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
