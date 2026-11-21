#include "migratool/analysis_context.hpp"
#include "migratool/scale.hpp"
#include <iostream>

namespace {
using namespace mt;
int checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
}
int wmain(int argc,wchar_t** argv)try{
    using namespace mt;if(argc!=2)throw std::runtime_error("Requires persistent workspace");
    const auto qa=fs::path(argv[1])/L"build"/L"qa-scale"/L"acceptance"/wide("roslyn-context-"+unique_id());
    const auto source=qa/L"source",target=qa/L"target";
    fs::create_directories(source/L"aplicación");fs::create_directories(source/L"shared");
    fs::create_directories(source/L"unrelated");fs::create_directories(source/L"oversized");fs::create_directories(target);
    write_text(source/L"aplicación"/L"App.csproj","<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup><ItemGroup><ProjectReference Include=\"../shared/Shared.csproj\" /></ItemGroup></Project>");
    write_text(source/L"aplicación"/L"Program.cs","public class Program { public int Run() => Helper.Value(); }\n");
    write_text(source/L"aplicación"/L"Sibling.cs","public class Sibling { public int Count = 3; }\n");
    write_text(source/L"shared"/L"Shared.csproj","<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>");
    write_text(source/L"shared"/L"Helper.cs","public static class Helper { public static int Value() => 7; }\n");
    write_text(source/L"unrelated"/L"Other.csproj","<Project Sdk=\"Microsoft.NET.Sdk\" />");
    write_text(source/L"unrelated"/L"Broken.cs","This is intentionally invalid C# and must not belong to App's semantic component.");
    write_text(source/L"oversized"/L"Big.csproj","<Project Sdk=\"Microsoft.NET.Sdk\" />");
    write_text(source/L"oversized"/L"Big.cs","/*"+std::string(5*1024*1024,'x')+"*/ public class Big {}\n");
    auto project=Project::create(qa/L"migration.json","Indexed Roslyn context",Json::array({
        {{"id","source"},{"role","source"},{"path",path_text(source)}},{{"id","target"},{"role","target"},{"path",path_text(target)}}}));
    project.config["inventory_engine"]="durable";
    project.config["inventory_mode"]="source_only";
    project.config["resources"]={{"memory_mib",128},{"workers",1},{"analyzers",1}};
    project.save();
    const auto queued=scale_start(project,{{"actor","context-test"},{"defer_analysis",true}},false);scale_coordinator(project,true);
    const auto initial=scale_jobs(project,{{"id",queued["job_id"]}})["items"][0];require(initial["state"]=="completed","fixture census failed");
    const auto program=path_text(source/L"aplicación"/L"Program.cs");
    const auto context=roslyn_indexed_context(project,Json::array({program}));
    require(context["context_authoritative"]==true&&context["context_file_count"]==3&&context["context_project_count"]==2,"context did not select the exact project and reference component");
    require(context["context_budget_bytes"]==4*1024*1024,"profile did not bound semantic context memory");
    for(const auto& path:context["context_files"])require(path.get<std::string>().find("unrelated")==std::string::npos&&path.get<std::string>().find("oversized")==std::string::npos,"unrelated source entered the context");
    Cancel cancelled;cancelled.requested=true;bool stopped=false;try{roslyn_indexed_context(project,Json::array({program}),&cancelled);}catch(const std::exception& error){stopped=std::string(error.what())=="CONTEXT_CANCELLED";}require(stopped,"context preparation ignored cancellation");
    Database database(project);
    const auto source_id=database.integer("SELECT id FROM files WHERE absolute_path=?",{program});
    auto input=Json{{"files",Json::array({{{"id","F"+std::to_string(source_id)},{"root","source"},{"path","aplicación/Program.cs"},
        {"absolute_path",program},{"language","C#"},{"analysis","PENDING"},{"sha256",hash_file(source/L"aplicación"/L"Program.cs")}}})},
        {"roots",project.config["roots"]},{"symbols",Json::array()},{"dependencies",Json::array()},{"diagnostics",Json::array()},{"indexed_context",true}};
    const auto analyzed=analyze(project,input);
    require(analyzed["files"][0]["analysis"]=="ANALYZED","indexed semantic analysis failed");
    require(analyzed["roslyn"]["projects_loaded"]==2&&analyzed["roslyn"]["context_file_count"]==3,"adapter recursively loaded an unrelated project");
    bool call=false;
    for(const auto& dependency:analyzed["dependencies"]){
        if(dependency.value("kind",std::string())=="call"&&dependency.value("resolution",std::string())=="SEMANTIC_EXACT"&&
           dependency.value("target_key",std::string())=="M:Helper.Value")call=true;
    }
    require(call,"ProjectReference call did not retain semantic resolution");
    bool limited=false;try{roslyn_indexed_context(project,Json::array({path_text(source/L"oversized"/L"Big.cs")}));}catch(const std::exception& error){limited=std::string(error.what()).starts_with("RESOURCE_CONTEXT_LIMIT");}require(limited,"oversized component did not produce a resource pending reason");
    const auto scoped=scale_start(project,{{"actor","context-test"},{"scope",{{"root","source"},{"file","oversized/Big.cs"}}}},false);scale_coordinator(project,true);
    const auto failed=scale_jobs(project,{{"id",scoped["job_id"]}})["items"][0];
    require(failed["state"]=="completed_with_errors","context limit was reported as a successful complete analysis");
    require(database.integer("SELECT COUNT(*) FROM inventory_tasks WHERE job_id=? AND state='failed' AND error LIKE 'RESOURCE_CONTEXT_LIMIT%'",{scoped["job_id"]})==1,"durable task lost its context resource pending reason");
    require(database.integer("SELECT COUNT(*) FROM scan_errors WHERE job_id=? AND operation='analysis' AND message LIKE 'RESOURCE_CONTEXT_LIMIT%'",{scoped["job_id"]})==1,"GUI/CLI error query cannot explain the context limit");
    const auto headers = qa / L"includes con espacios";
    const auto native = target / L"operacion con espacios.cpp";
    const auto compdb = qa / L"compile_commands.json";
    write_text(headers / L"datos.hpp", "#define VALOR_QA 23\n");
    write_text(native, "#include \"datos.hpp\"\n#ifdef PERFILQA\nint operacionContextual() { return VALOR_QA; }\n#endif\n");
    project.config["analyze_cpp"] = true;
    project.config["roots"][1]["compile_commands"] = path_text(compdb);
    const Json cpp_input = {
        {"files", Json::array({{{"id", "cpp-qa"}, {"root", "target"}, {"path", path_text(native.filename())},
            {"absolute_path", path_text(native)}, {"language", "C++"}, {"analysis", "PENDING"},
            {"analysis_error", "Diagnóstico anterior"}, {"sha256", hash_file(native)}}})},
        {"roots", project.config["roots"]}, {"symbols", Json::array()},
        {"dependencies", Json::array()}, {"diagnostics", Json::array()}
    };
    Json entry = {{"directory", path_text(target)}, {"file", path_text(native)}};
    const auto native_argument = utf8(fs::path(native).make_preferred().wstring());
    const auto module_map = target / L"operacion con espacios.cpp.obj.modmap";
    write_text(module_map, "");
    entry["command"] = "\"C:/Compilador con espacios/cl.exe\" /nologo /TP /std:c++latest /DPERFILQA /I\"" +
        path_text(headers) + "\" /Yu\"datos.hpp\" /Fp\"salida de PCH.pch\" /Fo\"salida objeto.obj\" " +
        "@\"operacion con espacios.cpp.obj.modmap\" /c \"" + native_argument + "\"";
    const auto check_context = [&] {
        write_json(compdb, Json::array({entry}));
        const auto result = analyze(project, cpp_input);
        require(result["files"][0]["analysis"]=="ANALYZED" &&
            result["files"][0]["analysis_config"]=="compile_commands", "el contexto MSVC no se analizó");
        require(result["files"][0]["compile_commands_path"]==path_text(fs::weakly_canonical(compdb)),
            "el análisis omitió la ruta de su base de compilación");
        require(!result["files"][0].contains("analysis_error"), "un análisis correcto conservó el error anterior");
        bool symbol = false;
        for (const auto& row : result["symbols"])
            if (row.value("name", std::string()) == "operacionContextual") symbol = true;
        require(symbol, "faltó el símbolo que depende de /D y de la ruta de /I");
    };
    check_context();
    write_text(module_map, "/reference operacion=operacion.ifc\n");
    const auto module_result = analyze(project, cpp_input);
    require(module_result["files"][0]["analysis"]=="ERROR" &&
        module_result["files"][0].value("analysis_error", std::string()).find("módulos IFC de MSVC")!=std::string::npos,
        "se perdió el diagnóstico del mapa IFC de MSVC");
    entry["arguments"] = {"cl.exe", "/TP", "/std:c++latest", "/DPERFILQA",
                          "/I" + path_text(headers), "/c", path_text(native)};
    entry["command"] = "ignored && ignored";
    check_context();
    const auto valid_entry = entry;
    entry.erase("arguments");
    write_json(compdb, Json::array({entry}));
    const auto invalid = analyze(project, cpp_input);
    require(invalid["files"][0]["analysis"]=="ERROR" &&
        invalid["files"][0].value("analysis_error", std::string()).find("operadores de shell")!=std::string::npos,
        "se aceptó un command con operadores de shell");

    const auto view = source / L"Detalle.xaml";
    const auto code_behind = source / L"Detalle.xaml.cs";
    write_text(code_behind, "partial class Detalle { void OnDetalle() {} }\n");
    write_text(view, R"XAML(<Page xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:ui="http://schemas.microsoft.com/winfx/2006/xaml" ui:Class="Ejemplo.Detalle">
      <Page.Resources><SolidColorBrush ui:Key="Fondo" Color="Red"/></Page.Resources>
      <Button ui:Name="Capturar" Click="OnDetalle" Command="{ui:Bind ViewModel.Capturar}"
        Content="{Binding Label}" Background="{StaticResource Fondo}" />
    </Page>)XAML");
    const auto view_input = Json{
        {"files", Json::array({{{"id", "xaml-qa"}, {"root", "source"}, {"path", "Detalle.xaml"},
           {"absolute_path", path_text(view)}, {"language", "XAML"}, {"analysis", "PENDING"}, {"sha256", hash_file(view)}}})},
        {"roots", project.config["roots"]}, {"symbols", Json::array()}, {"dependencies", Json::array()}, {"diagnostics", Json::array()}
    };
    const auto analyzed_view = analyze(project, view_input);
    require(analyzed_view["files"][0]["analysis"]=="ANALYZED" &&
            analyzed_view["files"][0]["analysis_config"]=="xaml_structure", "la vista XAML quedó sin contexto");
    require(analyzed_view["symbols"].size()==3, "faltan la clase, el control nombrado o el recurso XAML");
    const auto& refs = analyzed_view["dependencies"];
    require(refs.size()==4, "faltan el evento, los enlaces o la referencia de recurso XAML");
    bool event_context = false, command = false;
    for (const auto& ref : refs) {
        require(ref["resolution"]=="XAML_DECLARED" && ref["resolved"]==false, "se presentó una referencia XAML como resuelta");
        if (ref["kind"]=="xaml_event") {
            event_context = ref["name"]=="OnDetalle" && ref["candidate_path"]==path_text(code_behind) &&
                            ref["line"].get<unsigned>()>1;
        }
        if (ref["attribute"]=="Command") command = ref["name"]=="{ui:Bind ViewModel.Capturar}";
    }
    require(event_context && command, "se perdió el origen del evento o la expresión del comando XAML");

    write_json(compdb, Json::array({valid_entry}));
    project.config["resources"]["memory_mib"] = 384;
    project.save();
    const auto native_job = scale_start(project, {{"actor", "context-test"},
        {"scope", {{"root", "target"}, {"file", path_text(native.filename())}}}}, false);
    scale_coordinator(project, true);
    require(scale_jobs(project, {{"id", native_job["job_id"]}})["items"][0]["state"]=="completed",
            "la sincronización no publicó el análisis nativo");
    require(database.integer(R"SQL(
        SELECT COUNT(*) FROM files f JOIN symbols s ON s.file_id=f.id
        WHERE f.root='target' AND f.path=? AND f.analysis='ANALYZED' AND s.active=1
        AND s.name='operacionContextual' AND json_extract(f.data,'$.compile_commands_path')=?
    )SQL", {path_text(native.filename()),path_text(fs::weakly_canonical(compdb))})==1,
            "el catálogo perdió la base de compilación del símbolo nativo");
    const auto view_job = scale_start(project, {{"actor", "context-test"},
        {"scope", {{"root", "source"}, {"file", "Detalle.xaml"}}}}, false);
    scale_coordinator(project, true);
    require(scale_jobs(project, {{"id", view_job["job_id"]}})["items"][0]["state"]=="completed",
            "la sincronización no publicó el análisis XAML");
    require(database.integer(R"SQL(
        SELECT COUNT(*) FROM dependencies d JOIN files f ON f.id=d.file_id WHERE f.path='Detalle.xaml'
        AND d.target_id IS NULL AND json_extract(d.data,'$.resolution')='XAML_DECLARED'
        AND json_extract(f.data,'$.analysis_config')='xaml_structure'
    )SQL")==4, "el catálogo alteró el contexto o la resolución de una referencia XAML");

    write_text(view, "<Page><Button/></Grid>");
    const auto invalid_view = analyze(project, view_input);
    require(invalid_view["files"][0]["analysis"]=="ERROR" && invalid_view["symbols"].empty(),
            "una vista XML inválida quedó marcada como analizada");
    write_json(qa/L"receipt.json",{{"passed",true},{"checks",checks},{"context",context},{"roslyn",analyzed["roslyn"]},{"limited_job",failed},{"build",build_identity()}});
    std::cout<<"PASS "<<checks<<" checks "<<path_text(qa/L"receipt.json")<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
