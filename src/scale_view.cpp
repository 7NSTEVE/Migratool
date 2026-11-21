#include "migratool/scale_view.hpp"
#include "migratool/scale.hpp"
#include "migratool/ui.hpp"
#include <commctrl.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <deque>
#include <shellapi.h>
#include <map>
#include <cstdio>
#include <windowsx.h>

namespace mt {
namespace {
constexpr UINT WM_ROWS=WM_APP+171;
std::wstring control_text(HWND w){std::wstring t(GetWindowTextLengthW(w)+1,L'\0');GetWindowTextW(w,t.data(),static_cast<int>(t.size()));t.resize(wcslen(t.c_str()));return t;}
std::string str_or(const Json& obj,const char* key,const std::string& fallback=""){
    if(!obj.is_object()||!obj.contains(key))return fallback;
    const auto& v=obj[key];
    return v.is_string()?v.get<std::string>():fallback;
}
std::string display(const Json& v){return v.is_null()?"—":v.is_string()?v.get<std::string>():v.dump();}
std::string human(const std::string& key,const Json& value){if(value.is_null())return key.find("percent")!=std::string::npos?"Por calcular":"Sin datos";if(value.is_boolean())return value.get<bool>()?"Sí":"No";if(value.is_array()){if(value.empty())return key=="target_paths"?"Sin destino":"—";std::string result;for(const auto& entry:value){if(!result.empty())result+="; ";result+=human(key,entry);}return result;}
    if(key=="kind"&&value=="export_inventory")return "Exportación";
    if(key=="kind"&&value=="inventory")return "Inventario";
    if(key=="phase"&&value=="export")return "Escribiendo archivos";
    if(key=="phase"&&value=="export_publish")return "Guardando resultado";
    if(key=="phase"&&value=="export_finished")return "Resultado guardado";
    if(key=="bytes"&&value.is_number_integer())return utf8(ui::number(std::to_wstring(value.get<int64_t>()),0))+" B";
    if(key=="observed_files_per_second"){if(!value.is_number()||value.get<double>()<=0.0)return "—";char b[32]{};snprintf(b,sizeof(b),"%.0f",value.get<double>());return utf8(ui::number(wide(b),0))+ " /s";}
    if(key=="eta_seconds"){if(!value.is_number_integer())return "Faltan datos";auto s=value.get<int64_t>();if(s<60)return std::to_string(s)+" s";if(s<3600)return std::to_string(s/60)+" m "+std::to_string(s%60)+" s";return std::to_string(s/3600)+" h "+std::to_string((s%3600)/60)+" m";}
    if(key.find("percent")!=std::string::npos&&value.is_number()){char buffer[32]{};snprintf(buffer,sizeof(buffer),"%.1f",value.get<double>());return utf8(ui::number(wide(buffer),1)+L" %");}
    if(value.is_string()){static const std::map<std::string,std::string> states={{"UNMAPPED","Sin destino"},{"MAPPED_UNVERIFIED","Avance parcial"},{"DECLARED_COMPLETE_UNVERIFIED","Migración registrada"},{"VERIFIED_IN_CASES","Verificado en casos"},{"STALE","Requiere revisión"},{"NON_SOURCE","No código fuente"},{"PENDING","Pendiente"},{"PARTIAL","Análisis parcial"},{"ANALYZED","Analizado"},{"NOT_APPLICABLE","No requiere análisis"},{"UNSUPPORTED","Sin analizador"},{"ERROR","Error"},{"UNKNOWN","Sin determinar"},{"queued","En espera"},{"running","En curso"},{"completed","Finalizado"},{"completed_with_errors","Finalizado con errores"},{"failed","Error"},{"paused","En pausa"},{"cancelled","Cancelado"},{"census","Buscando archivos"},{"analysis","Analizando código"},{"analysis_deferred","Análisis pendiente"},{"finished","Terminado"},{"directory","Carpeta"},{"file","Archivo"},{"directory_link","Enlace de carpeta"},{"file_link","Enlace de archivo"},{"MISSING_TARGET","Falta registrar un destino"},{"STALE_EVIDENCE","El contenido cambió desde la última comprobación"},{"ANALYSIS_PARTIAL","Falta completar el análisis"},{"COMPLETION_DECLARATION","Falta confirmar la migración"},{"LINKED_PASSING_CASES","Faltan pruebas vinculadas vigentes"},{"CURRENT_BUILDS","Faltan compilaciones vigentes"},{"TASK_CLAIMED","Otro agente reservó este archivo"},{"CANDIDATE","Candidato sin comprobar"},{"INPUT_OUTPUT_HASH_VERIFIED","Entrada y resultado comprobados por hash"},{"INPUT_HASH_VERIFIED","Entrada comprobada por hash"},{"OUTPUT_NOT_VERIFIED","Resultado sin comprobar"},{"INPUT_NOT_VERIFIED","Entrada sin comprobar"},{"INPUT_PROVENANCE_HASH_MISSING","Falta el hash de procedencia"},
        {"CUBIERTA","Cubierta"},{"CARPETA_VACIA","Carpeta vacía"},{"CARPETA_PENDIENTE","Carpeta pendiente"},{"ERROR_DE_ACCESO","Error de acceso"},{"ENLACE_NO_SEGUIDO","Enlace simbólico no seguido"},{"EXCLUIDA_POR_FILTRO","Excluida por exclude_dirs"},{"exclude_dirs","Excluida por exclude_dirs"},{"filtered_by_inventory_mode","Fuera de modo solo código fuente"},{"protected_configuration","Configuración protegida"},{"ANALISIS_PARCIAL","Análisis parcial o con error"},{"ANALISIS_PENDIENTE","Análisis pendiente"},{"SIN_EXT_DESCONOCIDO","Sin extensión / tipo desconocido"},{"SIN_ANALIZADOR","Formato sin analizador profundo"},{"CARPETA_OCULTA","Carpeta oculta contabilizada"},{"ARCHIVO_OCULTO","Archivo oculto contabilizado"},
        {"planned","Planificado"},{"half_done","A medias"},{"ported","Portado"},{"integrated","Integrado"},{"verified","Verificado"},{"unmapped","Sin mapear"},{"stale","Obsoleto"},
        {"migrated","Migrado al destino"},{"merged_into","Fusionado en otro destino"},{"intentionally_removed","Eliminado intencionalmente"},{"replaced_by_external","Reemplazado por externo"},{"unassigned","Sin asignar"},
        {"MIGRATE","Migrar"},{"DO_NOT_MIGRATE","No migrar"},{"EXTRACT_CONTRACT_ONLY","Extraer solo contrato"},{"REFERENCE_ONLY","Solo referencia"},{"DUPLICATE_OR_GENERATED","Duplicado o generado"},
        {"reference_only","Solo referencia"},{"tracked_tasks","Tareas extraídas"},{"obsolete","Obsoleto"},{"contract_extracted","Contrato extraído"},{"no_action_needed","Sin acción requerida"},{"unreviewed","Sin revisar"},
        {"PLANNED","Planificada"},{"IN_PROGRESS","En curso"},{"HALF_DONE","A medias"},{"COMPLETED","Completada"},{"VERIFIED","Verificada"},{"BLOCKED","Bloqueada"},{"TODO","Pendiente"},{"PORTED","Portada"},{"INTEGRATED","Integrada"},{"UNREVIEWED","Sin revisar"},{"REVIEWED","Revisado"},{"ACTION_REQUIRED","Acción requerida"}};auto raw=value.get<std::string>();auto it=states.find(raw);if(it!=states.end()&&(key=="state"||key=="stage"||key=="disposition"||key=="analysis"||key=="phase"||key=="kind"||key=="missing"||key=="coverage_cause"))return it->second;return raw;}return value.dump();}
std::wstring label(const std::string& key){
    static const std::map<std::string,std::wstring> names={
        {"id",L"ID"},{"name",L"Nombre"},{"value",L"Valor"},{"description",L"Detalle"},{"path",L"Archivo"},
        {"source_path",L"Ruta de origen"},{"absolute_path",L"Ruta completa"},{"target_paths",L"Destinos"},
        {"migration_percent",L"Migrado %"},{"state",L"Estado"},{"stage",L"Madurez"},{"disposition",L"Disposición"},{"external_replacement",L"Reemplazo externo"},{"phase",L"Etapa"},{"files",L"Archivos"},
        {"source_module",L"Módulo de origen"},{"target_summary",L"Módulos de destino"},{"target_modules",L"Detalle de módulos destino"},{"mapped_files",L"Mapeados"},{"half_done_files",L"A medias"},{"ported_files",L"Portados"},{"declared_files",L"Declarados"},{"verified_files",L"Verificados"},{"verified_units",L"Unidades verificadas"},{"non_source_files",L"No código"},{"dispositions",L"Disposiciones"},
        {"record_type",L"Categoría"},{"item_id",L"Identificador"},{"title",L"Título"},{"detail_summary",L"Resumen / evidencia"},{"updated_at",L"Actualizado (UTC)"},
        {"phase_id",L"Fase"},{"task_kind",L"Tipo de tarea"},{"priority",L"Prioridad"},{"assignee",L"Responsable"},{"ordinal",L"Orden"},
        {"gate_builds",L"Compuerta builds"},{"gate_cases",L"Compuerta casos"},{"source_entities",L"Entidades origen"},{"target_entities",L"Entidades destino"},
        {"required_cases",L"Casos requeridos"},{"depends_on",L"Depende de"},{"linked_targets",L"Destinos vinculados"},{"extracted_tasks",L"Tareas extraídas"},{"root",L"Raíz"},
        {"dirs",L"Carpetas"},{"processed",L"Procesados"},{"pending_analysis",L"Análisis pendientes"},
        {"failed",L"Fallos"},{"error",L"Error"},{"kind",L"Tipo"},{"bytes",L"Bytes"},{"actor",L"Autor"},
        {"at",L"Fecha y hora UTC"},{"note",L"Nota"},{"source_sha256",L"SHA-256 de origen"},
        {"output_sha256",L"SHA-256 del resultado"},{"output_path",L"Resultado localizado"},
        {"checked",L"Última comprobación"},{"copy_count",L"Copias iguales"},{"entry_path",L"Ruta dentro del ZIP"},
        {"analysis",L"Análisis"},{"language",L"Lenguaje"},{"total_units",L"Unidades"},
        {"migrated_units",L"Unidades migradas"},{"units_with_passing_cases",L"Unidades con pruebas vigentes"},
        {"missing",L"Qué falta"},{"declared_complete",L"Declarado completo"},{"source_files",L"Archivos de origen"},
        {"physical_files",L"Archivos físicos"},{"inventory_complete",L"Censo completo"},
        {"analysis_pending",L"Análisis pendientes"},{"last_sync",L"Última sincronización"},
        {"known_subset_migration_percent",L"Avance del alcance conocido %"},{"git_state",L"Estado en Git"},
        {"build_state",L"Compilación"},{"request_bytes",L"Bytes enviados"},{"response_bytes",L"Bytes recibidos"},
        {"duration_us",L"Duración (µs)"},{"entity_type",L"Tipo"},{"entity_id",L"Elemento"},{"excerpt",L"Coincidencia"},
        {"action",L"Operación"},{"operation",L"Operación"},{"entity",L"Elemento"},{"detail",L"Detalle"},
        {"message",L"Mensaje"},{"tool",L"Herramienta"},{"pending_directories",L"Carpetas pendientes"},
        {"scope",L"Alcance"},{"generation",L"Generación"},{"created",L"Creado (UTC)"},{"started",L"Inicio (UTC)"},
        {"finished",L"Fin (UTC)"},{"updated",L"Actualizado (UTC)"},{"code",L"Código del error"},
        {"origin_catalog",L"Catálogo de procedencia"},{"origin_id",L"Registro de procedencia"},{"classification",L"Clasificación"},
        {"lines",L"Líneas"},{"lines_covered",L"Líneas cubiertas"},{"branches",L"Ramas"},{"branches_covered",L"Ramas cubiertas"},
        {"observed_files_per_second",L"Velocidad"},{"eta_seconds",L"Tiempo estimado"},{"discovered_entries",L"Entradas"},
        {"errors_count",L"Errores"},{"excluded_entries",L"Excluidas"},{"coverage_cause",L"Estado y causa"},
        {"last_scanned",L"Último recorrido"},{"link_target",L"Destino del enlace"}
    };
    const auto found=names.find(key);return found==names.end()?wide(key):found->second;
}
struct Payload {uint64_t sequence{},navigation{};HTREEITEM tree_parent{};bool refresh_after=false;Json left=Json::object(),right=Json::object(),tree,summary;std::string error,caption;};
}
struct ScaleView::Impl {
    HWND hwnd{},list{},targets{},detail{},caption{},tree{},previous{},next{},next_target{},pause{},resume{},cancel{},retry{},rescan{},up{},linked{},pending_filter{},copy{},refresh_button{},source_title{},target_title{},details_button{},filter_button{},pane_choice{},empty{},progress{},depth_choice{},root_choice{},dev_kind_choice{},dev_state_choice{},include_non_source_chk{};HFONT font{};int dpi=96,page=0;
    ui::Palette colors=ui::system_palette();HBRUSH background=CreateSolidBrush(colors.background),surface=CreateSolidBrush(colors.surface);int font_pixels=0;
    bool filters_visible=false,reading=false,replacing_rows=false,include_non_source=false,keep_context=false;double split=.5;RECT split_bounds{};bool dragging=false,stacked=false,single_pane=false;
    int depth=2,dev_kind=0;
    std::optional<Project> project;Json rows=Json::array(),target_rows=Json::array();std::vector<std::string> columns,target_columns,root_options;
    std::vector<std::string> cursors;std::string cursor,next_cursor,target_cursor,next_target_cursor,filter,parent,root,selected_source,pending_reason,dev_state;bool linked_only=false,target_selected=false;
    struct Folder {std::string root,path,cursor;bool loaded=false;};std::map<HTREEITEM,Folder> folders;uint64_t navigation=0;
    struct Task{uint64_t ticket;std::function<Payload()> run;};
    std::mutex mutex;std::condition_variable_any ready;std::optional<Task> pending;std::deque<Task> actions;std::jthread worker;uint64_t sequence=0,settled_sequence=0;
    Impl():worker([this](std::stop_token stop){while(!stop.stop_requested()){Task task{};{std::unique_lock lock(mutex);ready.wait(lock,stop,[&]{return pending.has_value()||!actions.empty();});if(stop.stop_requested())break;if(!actions.empty()){task=std::move(actions.front());actions.pop_front();}else{task=std::move(*pending);pending.reset();}}
        auto result=std::make_unique<Payload>();try{*result=task.run();}catch(const std::exception& e){result->sequence=task.ticket;result->error=e.what();}if(hwnd&&PostMessageW(hwnd,WM_ROWS,0,reinterpret_cast<LPARAM>(result.get())))result.release();}}){}
    ~Impl(){worker.request_stop();ready.notify_all();if(worker.joinable())worker.join();MSG message{};while(hwnd&&PeekMessageW(&message,hwnd,WM_ROWS,WM_ROWS,PM_REMOVE))delete reinterpret_cast<Payload*>(message.lParam);DeleteObject(background);DeleteObject(surface);}
    int px(int n)const{return MulDiv(n,dpi,96);}
    HWND make(const wchar_t* cls,const wchar_t* title,DWORD style,int id){auto h=CreateWindowExW(0,cls,title,WS_CHILD|WS_VISIBLE|style,0,0,10,10,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);if(wcscmp(cls,L"STATIC")!=0&&wcscmp(cls,L"BUTTON")!=0)ui::name(h,title);return h;}
    void sync_root_options(){
        if(!root_choice)return;
        SendMessageW(root_choice,CB_RESETCONTENT,0,0);
        root_options.clear();
        SendMessageW(root_choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Todas las raíces"));
        root_options.emplace_back("");
        int sel=0;
        if(project&&project->config.contains("roots")&&project->config["roots"].is_array()){
            for(const auto& r:project->config["roots"]){
                if(!r.is_object())continue;
                if(str_or(r,"role")!="source")continue;
                const auto rid=str_or(r,"id");
                if(rid.empty())continue;
                const auto label_w=wide(rid+" ("+str_or(r,"path")+")");
                SendMessageW(root_choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label_w.c_str()));
                root_options.push_back(rid);
                if(!root.empty()&&root==rid)sel=static_cast<int>(root_options.size())-1;
            }
        }
        SendMessageW(root_choice,CB_SETCURSEL,sel,0);
    }
    void update_controls(){
        int at=ListView_GetNextItem(list,-1,LVNI_SELECTED);const bool selected=at>=0&&static_cast<size_t>(at)<rows.size();
        auto state=selected&&page==16?str_or(rows[at],"state"):"";
        const bool deferred=selected&&page==16&&str_or(rows[at],"phase")=="analysis_deferred";
        EnableWindow(pause,selected&&(state=="queued"||state=="running"));EnableWindow(resume,selected&&(state=="paused"||deferred));
        EnableWindow(cancel,selected&&(state=="queued"||state=="running"||state=="paused"));EnableWindow(retry,selected&&(state=="failed"||state=="cancelled"||state=="completed_with_errors"));
        const bool target=page==1&&ListView_GetNextItem(targets,-1,LVNI_SELECTED)>=0;
        const bool can_rescan_row=page==17&&selected&&str_or(rows[at],"kind")=="directory";
        EnableWindow(up,!parent.empty());EnableWindow(rescan,!root.empty()||can_rescan_row);EnableWindow(copy,selected||target);EnableWindow(details_button,selected||target);
        EnableWindow(next_target,!next_target_cursor.empty());EnableWindow(linked,!selected_source.empty());
        PostMessageW(GetParent(hwnd),WM_SCALE_SELECTION,0,0);
    }
    void empty_state(){
        const bool no_rows=rows.empty()&&page!=0;ShowWindow(empty,no_rows?SW_SHOW:SW_HIDE);if(!no_rows)return;
        const wchar_t* message=page==16?L"No hay trabajos registrados. Use Inicio > Escanear proyecto para iniciar uno.":page==10?L"Escriba un nombre, una ruta o un símbolo en el campo de búsqueda.":page==15?L"No hay coincidencias en los datos consultados. Revise la opción de duplicados en Proyecto > Carpetas y filtros.":page==18?L"No hay trabajo previo registrado. Consulte F1 para conocer las opciones de los agentes.":page==21?L"Aún no hay miembros ZIP registrados. Active el análisis de ZIP en Carpetas y filtros.":page==22?L"No hay problemas de escaneo en esta consulta.":page==4?L"No hay resultados de pruebas registrados. Configure sus casos antes de ejecutarlos.":L"No hay resultados en esta vista. Borre la búsqueda o cambie los filtros para ampliar la consulta.";
        SetWindowTextW(empty,message);
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,empty,OBJID_CLIENT,CHILDID_SELF);
    }
    void layout(){
        if(!list)return;RECT rc{};GetClientRect(hwnd,&rc);const int width=rc.right,height=rc.bottom,gap=ui::text_percent>=150?px(6):px(10),control=ui::control_height(dpi),line=ui::type_size(18,dpi)+px(8);split_bounds={};
        const bool compare=page==1,folder_view=page==17,mod_view=page==23,dev_view=page==24;
        const bool show_root_filter=(page==1&&filters_visible)||page==7||page==8||mod_view||(dev_view&&dev_kind==3);
        const bool show_pending_filter=(compare&&filters_visible)||page==7||page==8;
        const bool inspector=page!=0&&height>px(400)&&(ListView_GetNextItem(list,-1,LVNI_SELECTED)>=0||(compare&&ListView_GetNextItem(targets,-1,LVNI_SELECTED)>=0));
        for(auto item:{pause,resume,cancel,retry})ui::show_if(item,page==16);
        for(auto item:{rescan,up})ui::show_if(item,folder_view);
        for(auto item:{refresh_button,copy,details_button})ui::show_if(item,page!=0);
        const bool paginate=page!=0&&(!cursors.empty()||!next_cursor.empty());
        for(auto item:{previous,next})ui::show_if(item,paginate);
        ui::show_if(filter_button,compare);ui::show_if(detail,inspector);
        ui::show_if(linked,compare&&filters_visible);
        ui::show_if(pending_filter,show_pending_filter);
        ui::show_if(depth_choice,mod_view);
        ui::show_if(include_non_source_chk,mod_view);
        ui::show_if(root_choice,show_root_filter);
        ui::show_if(dev_kind_choice,dev_view);
        ui::show_if(dev_state_choice,dev_view);
        ui::show_if(tree,folder_view);ui::show_if(next_target,compare&&!next_target_cursor.empty());
        int x=0,y=0;
        const int btn_pad=ui::text_percent>=150?px(10):px(24);
        const int btn_min=ui::text_percent>=150?px(64):px(90);
        for(auto item:{refresh_button,filter_button,copy,details_button,pause,resume,cancel,retry,rescan,up})if(GetWindowLongPtrW(item,GWL_STYLE)&WS_VISIBLE){
            auto text=control_text(item);HDC dc=GetDC(item);auto old=SelectObject(dc,font);SIZE size{};GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&size);SelectObject(dc,old);ReleaseDC(item,dc);
            const int button_width=std::min(width,std::max(btn_min,static_cast<int>(size.cx)+btn_pad));if(x&&x+button_width>width){x=0;y+=control+gap;}ui::move_if(item,x,y,button_width,control);x+=button_width+gap;
        }
        if(page!=0)y+=control+gap;
        if(show_pending_filter||show_root_filter||mod_view||dev_view||(compare&&filters_visible)){
            std::vector<std::pair<HWND,std::pair<int,int>>> vis_filters;
            auto add_f=[&](HWND ctrl,int w,int drop){
                if(ctrl&&(GetWindowLongPtrW(ctrl,GWL_STYLE)&WS_VISIBLE))vis_filters.push_back({ctrl,{w,drop}});
            };
            add_f(depth_choice,std::max(px(150),ui::type_size(18,dpi)*10),px(240));
            add_f(dev_kind_choice,std::max(px(220),ui::type_size(18,dpi)*16),px(240));
            add_f(dev_state_choice,std::max(px(170),ui::type_size(18,dpi)*11),px(320));
            add_f(root_choice,std::max(px(180),ui::type_size(18,dpi)*12),px(260));
            add_f(pending_filter,std::max(px(200),ui::type_size(18,dpi)*14),px(300));
            add_f(linked,std::max(px(200),ui::type_size(18,dpi)*14),0);
            add_f(include_non_source_chk,std::max(px(160),ui::type_size(18,dpi)*11),0);
            if(!vis_filters.empty()){
                const int n=static_cast<int>(vis_filters.size());
                const int equal_share=n>1&&width>=px(480)?std::max(px(130),(width-gap*(n-1))/n):width;
                int fx=0,fy=y;
                for(const auto& [ctrl,spec]:vis_filters){
                    const int cw=std::min({width,spec.first,equal_share});
                    if(fx&&fx+cw>width){fx=0;fy+=control+gap;}
                    ui::move_if(ctrl,fx,fy,cw,control+spec.second);
                    fx+=cw+gap;
                }
                y=fy+control+gap;
            }
        }
        const int caption_height=page==0?line*2:line;
        ui::move_if(progress,0,y,px(70),line);ui::move_if(caption,reading?px(80):0,y,std::max(px(50),width-(reading?px(80):0)),caption_height);y+=caption_height+gap;
        const int detail_height=inspector?std::min(ui::type_size(18,dpi)*6,std::max(line*2,height/4)):0;
        const int pager=(paginate||(compare&&!next_target_cursor.empty()))?control+gap:0;
        const int available=std::max(px(24),height-y-pager-detail_height-(inspector?gap:0));
        const int tree_width=folder_view?std::min(width/3,std::max(px(180),ui::type_size(18,dpi)*13)):0;
        const int left=folder_view?tree_width+gap:0,body_width=std::max(px(50),width-left);
        stacked=compare&&body_width<ui::type_size(18,dpi)*52;single_pane=stacked&&available<line*5;
        ui::show_if(pane_choice,single_pane);
        if(single_pane){ui::move_if(pane_choice,0,y,width,control+px(200));y+=control+gap;}
        const int actual_available=std::max(px(24),available-(single_pane?control+gap:0));
        const int first_width=compare&&!stacked?static_cast<int>((body_width-gap)*split):body_width;
        const bool target_only=single_pane&&SendMessageW(pane_choice,CB_GETCURSEL,0,0)==1;
        ui::show_if(source_title,compare&&!single_pane);ui::show_if(target_title,compare&&!single_pane);
        ui::show_if(list,!target_only);ui::show_if(targets,compare&&(!single_pane||target_only));
        if(compare&&!single_pane){
            const int total_tables=std::max(px(40),actual_available-gap-line*2);
            const int top_table_h=stacked?std::max(px(24),(total_tables*58)/100):std::max(px(20),actual_available-line);
            const int bot_table_h=stacked?std::max(px(20),total_tables-top_table_h):top_table_h;
            const int second_x=stacked?left:left+first_width+gap,second_y=stacked?y+line+top_table_h+gap:y;
            ui::move_if(source_title,left,y,first_width,line);ui::move_if(list,left,y+line,first_width,top_table_h);
            ui::move_if(target_title,second_x,second_y,stacked?body_width:body_width-first_width-gap,line);ui::move_if(targets,second_x,second_y+line,stacked?body_width:body_width-first_width-gap,bot_table_h);
            if(!stacked)split_bounds={left+first_width,y,left+first_width+gap,y+actual_available};
        }else{ui::move_if(list,left,y,body_width,actual_available);ui::move_if(targets,left,y,body_width,actual_available);}
        if(folder_view)ui::move_if(tree,0,y,tree_width,actual_available);
        ui::move_if(empty,left+gap,y+line,body_width-gap*2,std::max(line*2,actual_available-line*2));
        int pager_y=y+actual_available+gap;int pager_x=left;
        for(auto item:{previous,next,next_target})if(GetWindowLongPtrW(item,GWL_STYLE)&WS_VISIBLE){int button_width=std::min((width-gap*2)/3,ui::type_size(18,dpi)*10);ui::move_if(item,pager_x,pager_y,button_width,control);pager_x+=button_width+gap;}
        if(inspector)ui::move_if(detail,0,height-detail_height,width,detail_height);
        update_controls();InvalidateRect(hwnd,nullptr,FALSE);
    }
    HTREEITEM add_folder(HTREEITEM parent_item,const std::wstring& name,const Folder& folder,bool children=true){TVINSERTSTRUCTW insert{};insert.hParent=parent_item;insert.hInsertAfter=TVI_LAST;insert.item.mask=TVIF_TEXT|TVIF_CHILDREN;insert.item.cChildren=children?1:0;insert.item.pszText=const_cast<wchar_t*>(name.c_str());auto item=TreeView_InsertItem(tree,&insert);folders[item]=folder;return item;}
    void request_tree(HTREEITEM item){if(!project||!folders.contains(item)||folders[item].loaded)return;auto folder=folders[item];folders[item].loaded=true;const auto p=*project;const auto nav=navigation;
        {std::lock_guard lock(mutex);actions.push_back({0,[=](){Payload out;out.tree_parent=item;out.navigation=nav;Database db(p);Json args={{"kind","directories"},{"root",folder.root},{"parent",folder.path},{"limit",200},{"include_total",false}};if(!folder.cursor.empty())args["cursor"]=folder.cursor;out.tree=*scale_query(p,db,args);return out;}});}ready.notify_one();}
    void setup_columns(HWND table,const Json& values,std::vector<std::string>& names){
        if(values.empty()){ListView_SetItemCountEx(table,0,LVSICF_NOSCROLL);return;}
        const auto& row=values[0];std::vector<std::string> preferred;
        if(row.contains("output_path"))preferred={"origin_id","source_path","output_path","state","checked"};
        else if(row.contains("source_module")){
            preferred={"source_module","source_files","mapped_files","half_done_files","ported_files","declared_files","verified_files","total_units","migrated_units","verified_units","migration_percent","target_summary","state"};
            if(include_non_source)preferred.insert(preferred.begin()+2,"non_source_files");
        }
        else if(row.contains("record_type"))preferred={"record_type","item_id","title","state","detail_summary","updated_at"};
        else if(row.contains("migration_percent"))preferred={"path","stage","disposition","migration_percent","state"};
        else if(row.contains("source_path"))preferred={"source_path","target_paths","stage","disposition","declared_complete","actor","at"};
        else if(row.contains("coverage_cause"))preferred={"path","kind","discovered_entries","pending_directories","errors_count","excluded_entries","coverage_cause","last_scanned"};
        else if(row.contains("phase"))preferred={"kind","state","phase","files","bytes","dirs","pending_analysis","failed","observed_files_per_second","eta_seconds"};
        else if(row.contains("entry_path")&&row.contains("archive_id"))preferred={"entry_path","absolute_path","state","bytes","error"};
        else if(row.contains("job_id")&&row.contains("message"))preferred={"path","operation","message","at"};
        else if(row.contains("copy_count"))preferred={"absolute_path","entry_path","copy_count","bytes"};
        else if(row.contains("name")&&row.contains("value"))preferred={"name","value"};
        else if(row.contains("name")&&row.contains("description"))preferred={"name","state","description"};
        else if(row.contains("name")&&row.contains("kind"))preferred={"name","kind","path","bytes"};
        else if(row.contains("path"))preferred=table==targets?std::vector<std::string>{"path","language","analysis"}:std::vector<std::string>{"path","language","analysis","git_state","build_state"};
        else if(row.contains("excerpt"))preferred={"entity_type","entity_id","excerpt"};
        else if(row.contains("action"))preferred={"at","actor","action","entity","detail"};
        else preferred={"name","absolute_path","state","kind","lines_covered","lines","branches_covered","branches","tool","at","request_bytes","response_bytes","duration_us"};
        std::vector<std::string> desired;for(const auto& key:preferred)if(values[0].contains(key))desired.push_back(key);
        if(desired.empty())for(auto it=row.begin();it!=row.end()&&desired.size()<4;++it)if(!it.value().is_object()&&!it.value().is_array())desired.push_back(it.key());
        if(desired!=names){
            ListView_SetItemCountEx(table,0,0);while(ListView_DeleteColumn(table,0)){}names=std::move(desired);
            RECT bounds{};GetClientRect(table,&bounds);int i=0;
            for(const auto& key:names){
                auto name=label(key);
                if(page==23){
                    if(key=="source_files")name=L"Archivos";
                    else if(key=="declared_files")name=L"Completos";
                    else if(key=="migrated_units")name=L"U. migr.";
                    else if(key=="verified_units")name=L"U. casos";
                }else if(page==17&&key=="pending_directories"){
                    name=L"Pendientes";
                }
                LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;
                int width=150;
                if(key=="path")width=page==17?215:230;
                else if(key=="name")width=page==0?275:225;
                else if(key=="source_module")width=175;
                else if(key=="target_summary")width=240;
                else if(key=="title"||key=="detail_summary")width=260;
                else if(key=="record_type"||key=="stage")width=115;
                else if(key=="disposition")width=165;
                else if(key=="item_id"||key=="updated_at")width=160;
                else if(key=="source_files"||key=="non_source_files"||key=="mapped_files"||key=="half_done_files"||key=="ported_files"||key=="declared_files"||key=="verified_files"||key=="total_units"||key=="migrated_units"||key=="verified_units")width=86;
                else if(key.find("path")!=std::string::npos)width=330;
                else if(key=="migration_percent")width=page==23?96:110;
                else if(key=="language")width=110;
                else if(key=="state")width=page==0?205:200;
                else if(key=="phase")width=240;
                else if(key=="coverage_cause")width=230;
                else if(key=="observed_files_per_second"||key=="eta_seconds")width=140;
                else if(key=="last_scanned")width=185;
                else if(key=="discovered_entries"||key=="errors_count"||key=="excluded_entries"||key=="pending_directories")width=105;
                else if(key=="kind"||key=="bytes")width=115;
                column.cx=key=="description"||key=="value"||key=="excerpt"?std::max<int>(px(320),bounds.right-ui::type_size(485,dpi)):ui::type_size(width,dpi);
                column.pszText=name.data();ListView_InsertColumn(table,i++,&column);
            }
        }
        ListView_SetItemCountEx(table,static_cast<int>(values.size()),LVSICF_NOSCROLL);InvalidateRect(table,nullptr,TRUE);
    }
    void request(){if(!project)return;reading=true;SetTimer(hwnd,2,150,nullptr);const auto p=*project;const auto view=page;const auto query_cursor=cursor,target_after=target_cursor;const auto query_filter=filter,query_parent=parent,query_root=root,source=selected_source,reason=pending_reason,query_dev_state=dev_state;const int query_depth=depth,query_dev_kind=dev_kind;const bool linked_targets=linked_only,query_non_source=include_non_source;const auto ticket=++sequence;
        {std::lock_guard lock(mutex);pending=Task{ticket,[=](){Payload result;result.sequence=ticket;Database db(p);Json args={{"limit",200},{"include_total",false}};if(!query_cursor.empty())args["cursor"]=query_cursor;if(!query_filter.empty())args["text"]=query_filter;
            if(view==16)result.left=scale_jobs(p,{{"limit",100}});
            else if(view==17){args["kind"]="census_entries";args["with_coverage"]=true;if(!query_root.empty())args["root"]=query_root;args["parent"]=query_parent;result.left=*scale_query(p,db,args);Json roots=Json::array();for(const auto& r:p.config["roots"])roots.push_back({{"id",r["id"]},{"path",path_text(p.resolve(r["path"]))}});result.tree=roots;
                if(result.left.contains("coverage_summary")&&result.left["coverage_summary"].is_object()){const auto& cs=result.left["coverage_summary"];result.caption="Cobertura · "+std::to_string(cs.value("discovered_entries",int64_t(0)))+" entradas · "+std::to_string(cs.value("pending_directories",int64_t(0)))+" carpetas pendientes · "+std::to_string(cs.value("errors_count",int64_t(0)))+" errores · "+std::to_string(cs.value("excluded_entries",int64_t(0)))+" excluidas";}}
            else if(view==18){args["kind"]="reuse_candidates";result.left=*scale_query(p,db,args);}
            else if(view==21){args["kind"]="archive_members";result.left=*scale_query(p,db,args);}
            else if(view==22){args["kind"]="scan_errors";result.left=*scale_query(p,db,args);}
            else if(view==19){auto doctor=scale_doctor(p);Json data=Json::array();for(auto it=doctor.begin();it!=doctor.end();++it)data.push_back({{"name",it.key()},{"value",it.value()}});result.left={{"items",data}};}
            else if(view==0){auto summary=scale_summary(p,db);result.summary=summary;Json data=Json::array();size_t sources=0,destinations=0;for(const auto& r:p.config["roots"])(r["role"]=="source"?sources:destinations)++;
                data.push_back({{"name","Carpetas del proyecto"},{"state","Configuradas"},{"description",std::to_string(sources)+" de origen y "+std::to_string(destinations)+" de destino."}});
                data.push_back({{"name","Búsqueda de archivos"},{"state",summary["inventory_complete"]==true?"Terminada":"Pendiente"},{"description",display(summary["physical_files"])+" archivos encontrados en las carpetas configuradas."}});
                data.push_back({{"name","Análisis del código"},{"state",summary["analysis_pending"]==0?"Al día":"Pendiente"},{"description",summary["analysis_pending"]==0?"El código reconocido ya tiene un resultado de análisis.":display(summary["analysis_pending"])+" archivos esperan análisis; consulte Actividad."}});
                const auto completed=summary["declared_complete_files"].get<int64_t>();
                data.push_back({{"name","Migración registrada"},{"state",completed==0?"Por iniciar":"En curso"},{"description",completed==1?"1 archivo de origen marcado como terminado.":std::to_string(completed)+" archivos de origen marcados como terminados."}});
                const bool no_cases=summary["configured_cases"]==0;
                data.push_back({{"name","Pruebas de comportamiento"},{"state",no_cases?"Sin casos":summary["passing_current_cases"]==0?"Sin resultados vigentes":"Con resultados"},{"description",no_cases?"Configure casos para comprobar el comportamiento del código migrado.":display(summary["passing_current_cases"])+" de "+display(summary["configured_cases"])+" casos configurados tienen resultados vigentes."}});
                const auto dc=summary.value("development_control",Json::object());
                const auto ph_tot=dc.value("phases_total",int64_t(0)),ph_done=dc.value("phases_completed",int64_t(0));
                const auto tk_tot=dc.value("tasks_total",int64_t(0)),tk_done=dc.value("tasks_completed",int64_t(0)),tk_half=dc.value("tasks_half_done",int64_t(0)),tk_act=dc.value("tasks_in_progress",int64_t(0));
                const auto doc_tot=dc.value("documentation_files",int64_t(0)),doc_rev=dc.value("documentation_reviewed",int64_t(0)),doc_stale=dc.value("documentation_stale",int64_t(0));
                data.push_back({{"name","Fases de control"},{"state",ph_tot==0?"Sin fases":(ph_done==ph_tot?"Al día":"En curso")},{"description",std::to_string(ph_done)+" de "+std::to_string(ph_tot)+" fases completadas."}});
                data.push_back({{"name","Tareas de desarrollo"},{"state",tk_tot==0?"Sin tareas":(tk_done==tk_tot?"Completadas":"Activas")},{"description",std::to_string(tk_done)+" completadas, "+std::to_string(tk_half)+" a medias, "+std::to_string(tk_act)+" en curso (de "+std::to_string(tk_tot)+")."}});
                data.push_back({{"name","Auditoría documental"},{"state",doc_stale>0?"Requiere revisión":(doc_tot>0&&doc_rev==doc_tot?"Revisada":"Pendiente")},{"description",std::to_string(doc_rev)+" de "+std::to_string(doc_tot)+" documentos revisados ("+std::to_string(doc_stale)+" obsoletos)."}});
                std::map<std::string,std::pair<int64_t,int64_t>> root_counts;
                for(const auto& rc:db.query("SELECT root,COUNT(*) AS total,SUM(CASE WHEN relevant=1 THEN 1 ELSE 0 END) AS rel FROM files WHERE deleted=0 GROUP BY root")){
                    root_counts[str_or(rc,"root")]={rc.value("rel",int64_t(0)),rc.value("total",int64_t(0))};
                }
                for(const auto& r:p.config.value("roots",Json::array())){
                    const auto rid=str_or(r,"id"),role=str_or(r,"role");
                    const auto cnt=root_counts[rid];
                    data.push_back({{"name","Raíz: "+rid+" ("+(role=="source"?"origen":"destino")+")"},{"state","Configurada"},{"description",path_text(p.resolve(str_or(r,"path")))+" · "+std::to_string(cnt.first)+" fuente / "+std::to_string(cnt.second)+" totales."}});
                }
                const auto non_src=summary.value("non_source_files",std::max<int64_t>(0,summary.value("known_files",int64_t(0))-summary.value("source_files",int64_t(0))));
                data.push_back({{"name","Fuera de alcance / no código fuente"},{"state",non_src>0?"Inventariado":"Sin excluidos"},{"description",std::to_string(non_src)+" archivos no relevantes o auxiliares (documentos, artefactos, dependencias y configuración)."}});
                result.left={{"items",data}};
                result.caption=(summary["migration_percent"].is_null()?std::string("El porcentaje global se calculará cuando el alcance y el análisis estén al día."):
                    (display(summary["migrated_units"])+" de "+display(summary["total_units"])+" unidades de origen con migración registrada."))+
                    " · Fases "+std::to_string(ph_done)+"/"+std::to_string(ph_tot)+" · Docs "+std::to_string(doc_rev)+"/"+std::to_string(doc_tot);
            }
            else if(view==1){
                if(!reason.empty())args["pending_reason"]=reason;
                if(!query_root.empty())args["root"]=query_root;
                result.left=scale_file_status(p,db,args);
                if(!linked_targets||!source.empty()){
                    Json tq={{"kind","files"},{"role","target"},{"limit",200},{"include_total",false},{"fields",{"id","path","absolute_path","language","analysis"}}};
                    if(!query_filter.empty()&&!linked_targets)tq["text"]=query_filter;
                    if(!target_after.empty())tq["cursor"]=target_after;
                    if(linked_targets)tq["linked_source"]=source;
                    result.right=query_page(p,db,tq);
                }
            }
            else if(view==2){args["kind"]="symbols";result.left=query_page(p,db,args);}
            else if(view==3){args["kind"]="mappings";result.left=*scale_query(p,db,args);}
            else if(view==4){auto evidence=scale_evidence(p,db,db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'")==0);result.left={{"items",evidence["cases"]}};}
            else if(view==5){args["kind"]="coverage";result.left=*scale_query(p,db,args);}
            else if(view==6){args["kind"]="history";result.left=*scale_query(p,db,args);}
            else if(view==7){
                if(!query_root.empty())args["root"]=query_root;
                if(!reason.empty()){
                    args["pending_reason"]=reason;
                    if(reason=="non_relevant"||reason=="with_disposition")args["include_non_relevant"]=true;
                    result.left=scale_file_status(p,db,args);
                }else{
                    args["kind"]="census_entries";result.left=*scale_query(p,db,args);
                }
            }
            else if(view==8){
                args["pending_reason"]=reason.empty()?std::string("missing_target"):reason;
                if(!query_root.empty())args["root"]=query_root;
                if(args["pending_reason"]=="non_relevant"||args["pending_reason"]=="with_disposition")args["include_non_relevant"]=true;
                result.left=scale_file_status(p,db,args);
            }
            else if(view==9){args["kind"]="files";args["fields"]={"id","path","absolute_path","git_state","build_state","analysis"};result.left=query_page(p,db,args);}
            else if(view==10){if(query_filter.empty())result.left={{"items",Json::array()}};else{args["kind"]="search";result.left=*scale_query(p,db,args);}}
            else if(view==11){args["kind"]="dependencies";result.left=*scale_query(p,db,args);}
            else if(view==12){result.left={{"items",db.query("SELECT id,at,tool,request_bytes,response_bytes,duration_us FROM requests ORDER BY id DESC LIMIT 200")}};}
            else if(view==15){
                args["kind"]="code_duplicates";auto duplicate=scale_query(p,db,args);
                result.left=duplicate?*duplicate:Json{{"items",Json::array()}};
                const auto report=result.left.value("report",Json::object());
                const auto mode=report.value("mode",std::string("off"));
                if(mode=="off")result.caption="Active la búsqueda de copias en Proyecto > Carpetas y filtros.";
                else if(report.value("complete",false))result.caption="Revisión terminada. Seleccione una copia para ver su ruta.";
                else result.caption="Revisión parcial · "+std::to_string(report.value("error_count",0))+" problemas de ZIP. Consulte Archivos dentro de ZIP.";
            }
            else if(view==23){
                args["depth"]=query_depth;
                if(!query_root.empty())args["root"]=query_root;
                if(query_non_source)args["include_non_source"]=true;
                result.left=scale_module_matrix(p,db,args);
                result.caption="Matriz módulo a módulo (profundidad "+std::to_string(query_depth)+") · "+std::to_string(result.left.value("total",size_t(0)))+" módulos de origen inventariados";
            }
            else if(view==24){
                auto copy_phase_row=[&](const Json& r){
                    Json item={{"id",str_or(r,"phase_id")},{"record_type","Fase"},{"item_id",str_or(r,"phase_id")},{"title",str_or(r,"title")},{"state",str_or(r,"state")},{"detail_summary",str_or(r,"description")},{"updated_at",str_or(r,"updated_at")}};
                    for(const auto* k:{"ordinal","description","gate_builds","gate_cases","target_modules","actor","revision"})if(r.contains(k))item[k]=r[k];
                    return item;
                };
                auto copy_task_row=[&](const Json& r){
                    const auto pid=str_or(r,"phase_id");
                    const std::string phase_tag=pid.empty()?"":"["+pid+"] ";
                    Json item={{"id",str_or(r,"task_id")},{"record_type","Tarea"},{"item_id",str_or(r,"task_id")},{"title",str_or(r,"title")},{"state",str_or(r,"state")},{"detail_summary",phase_tag+str_or(r,"note")},{"updated_at",str_or(r,"updated_at")}};
                    for(const auto* k:{"phase_id","task_kind","priority","assignee","source_entities","target_entities","required_cases","depends_on","note","created_at","actor","revision"})if(r.contains(k))item[k]=r[k];
                    return item;
                };
                auto copy_doc_row=[&](const Json& r){
                    const auto disp=str_or(r,"disposition"),note=str_or(r,"note");
                    Json item={{"id",str_or(r,"doc_id")},{"record_type","Documento"},{"item_id",str_or(r,"doc_id")},{"title",str_or(r,"path")},{"state",str_or(r,"state")},{"detail_summary",disp+(note.empty()?"":(" · "+note))},{"updated_at",str_or(r,"at")}};
                    for(const auto* k:{"root","path","absolute_path","disposition","linked_targets","extracted_tasks","note","actor","at"})if(r.contains(k))item[k]=r[k];
                    return item;
                };
                if(query_dev_kind>=1&&query_dev_kind<=3){
                    Json q_sub={{"limit",200},{"include_total",false}};
                    if(!query_cursor.empty())q_sub["cursor"]=query_cursor;
                    if(!query_filter.empty())q_sub["text"]=query_filter;
                    if(!query_dev_state.empty())q_sub["state"]=query_dev_state;
                    if(query_dev_kind==3&&!query_root.empty())q_sub["root"]=query_root;
                    q_sub["kind"]=query_dev_kind==1?"phases":query_dev_kind==2?"project_tasks":"doc_audits";
                    Json items=Json::array();
                    Json next_cur=Json(nullptr);
                    if(auto res=scale_query(p,db,q_sub)){
                        next_cur=res->value("next_cursor",Json(nullptr));
                        for(const auto& r:res->value("items",Json::array())){
                            items.push_back(query_dev_kind==1?copy_phase_row(r):query_dev_kind==2?copy_task_row(r):copy_doc_row(r));
                        }
                    }
                    result.left={{"items",items},{"next_cursor",next_cur}};
                    result.caption="Control integral · "+std::to_string(items.size())+(query_dev_kind==1?" fases":query_dev_kind==2?" tareas":" documentos");
                }else{
                    Json combined=Json::array();
                    Json q_sub={{"limit",200},{"include_total",false}};
                    if(!query_filter.empty())q_sub["text"]=query_filter;
                    if(!query_dev_state.empty())q_sub["state"]=query_dev_state;
                    q_sub["kind"]="phases";
                    if(auto ph=scale_query(p,db,q_sub))for(const auto& r:ph->value("items",Json::array()))combined.push_back(copy_phase_row(r));
                    q_sub["kind"]="project_tasks";
                    if(auto tk=scale_query(p,db,q_sub))for(const auto& r:tk->value("items",Json::array()))combined.push_back(copy_task_row(r));
                    q_sub["kind"]="doc_audits";
                    bool docs_more=false;
                    if(auto dc=scale_query(p,db,q_sub)){
                        docs_more=dc->contains("next_cursor")&&(*dc)["next_cursor"].is_string();
                        for(const auto& r:dc->value("items",Json::array()))combined.push_back(copy_doc_row(r));
                    }
                    result.left={{"items",combined}};
                    result.caption="Control integral · "+std::to_string(combined.size())+" elementos (fases, tareas y auditoría documental)"+(docs_more?" · Use 'Solo documentos' para paginar todos los documentos":"");
                }
            }
            else {args["kind"]="files";result.left=query_page(p,db,args);}
            if(result.caption.empty()){
                const auto count=result.left.value("items",Json::array()).size();
                result.caption=std::to_string(count)+(count==1?" resultado en esta página":" resultados en esta página");
            }return result;}};}ready.notify_one();
    }
    void selected_detail(){
        int selected=ListView_GetNextItem(target_selected?targets:list,-1,LVNI_SELECTED);
        const auto& data=target_selected?target_rows:rows;
        if(selected<0||static_cast<size_t>(selected)>=data.size()){
            SetWindowTextW(detail,L"Seleccione una fila para consultar rutas completas y evidencia.");
            if(page==1&&!target_selected&&!selected_source.empty()){selected_source.clear();if(linked_only){target_cursor.clear();request();}}
            return;
        }
        const auto& item=data[selected];std::wstring value;
        if(page==16&&str_or(item,"kind")=="export_inventory"){
            if(item.contains("scope")&&item["scope"].is_object()&&item["scope"].contains("out"))value+=L"Destino: "+wide(str_or(item["scope"],"out"))+L"\r\n";
            if(item.contains("scope")&&item["scope"].is_object()&&item["scope"].contains("format"))value+=L"Formato: "+wide(str_or(item["scope"],"format"))+L"\r\n";
            const auto done_f=item.value("files",int64_t(0)),tot_f=item.value("export_total",int64_t(0)),exp_b=item.value("bytes",int64_t(0));
            value+=L"Avance de exportación: "+std::to_wstring(done_f)+(tot_f>0?(L" / "+std::to_wstring(tot_f)):L"")+L" archivos · "+wide(human("bytes",exp_b))+L"\r\n";
            if(item.contains("result")&&item["result"].is_object()&&item["result"].contains("sha256"))value+=L"SHA-256: "+wide(str_or(item["result"],"sha256"))+L"\r\n";
            if(!value.empty())value+=L"\r\n";
        }
        if(!item.contains("absolute_path")&&!item.contains("source_path")&&item.contains("path"))value+=L"Ruta: "+wide(display(item["path"]))+L"\r\n";
        for(const auto* key:{"source_module","root","record_type","item_id","title","phase_id","task_kind","priority","assignee","ordinal","source_path","absolute_path","target_paths","output_path","entry_path","state","stage","disposition","external_replacement","phase","coverage_cause","analysis","source_files","non_source_files","mapped_files","half_done_files","ported_files","declared_files","verified_files","migration_percent","total_units","migrated_units","units_with_passing_cases","target_summary","target_modules","dispositions","description","gate_builds","gate_cases","source_entities","target_entities","required_cases","depends_on","linked_targets","extracted_tasks","missing","note","actor","at","updated_at","last_scanned","discovered_entries","pending_directories","errors_count","excluded_entries","link_target","scope","generation","files","bytes","dirs","pending_analysis","observed_files_per_second","eta_seconds","started","finished","updated","error","message","origin_catalog","origin_id","source_sha256","output_sha256","checked","id"}){
            if(!item.contains(key)||(std::string_view(key)=="absolute_path"&&item.contains("source_path")&&item[key]==item["source_path"]))continue;
            if(str_or(item,"kind")=="export_inventory"&&(std::string_view(key)=="scope"||std::string_view(key)=="generation"||std::string_view(key)=="dirs"||std::string_view(key)=="pending_directories"||std::string_view(key)=="pending_analysis"))continue;
            value+=label(key)+L": ";
            if(item[key].is_array()&&!item[key].empty()){value+=L"\r\n";for(const auto& part:item[key])value+=L"  "+wide(human(key,part))+L"\r\n";}
            else value+=wide(human(key,item[key]))+L"\r\n";
        }
        if(page==16&&item.contains("resources")){
            const auto& resource=item["resources"];
            value+=L"\r\nLímite de memoria: "+wide(display(resource.value("memory_mib",Json())))+L" MiB";
            if(resource.contains("project_allocated_mib"))value+=L" · Cuota asignada: "+wide(display(resource["project_allocated_mib"]))+L" MiB";
            if(resource.contains("shared_budget_mib"))value+=L" (presupuesto global "+wide(display(resource["shared_budget_mib"]))+L" MiB)";
            value+=L"\r\nTrabajadores: "+wide(display(resource.value("workers",Json())))+L" · Analizadores: "+wide(display(resource.value("analyzers",Json())))+L"\r\n";
            if(resource.contains("peak_job_bytes"))value+=L"Pico medido del trabajo: "+wide(display(resource["peak_job_bytes"]))+L" bytes\r\n";
        }
        if(value.empty())for(auto it=item.begin();it!=item.end();++it)value+=label(it.key())+L": "+wide(display(it.value()))+L"\r\n";SetWindowTextW(detail,value.c_str());
        if(page==1&&!target_selected&&item.contains("id")){auto id=display(item["id"]);if(selected_source!=id){selected_source=id;if(linked_only){target_cursor.clear();request();}}}
    }
    static std::string selected_id(HWND table,const Json& items){
        const auto index=ListView_GetNextItem(table,-1,LVNI_SELECTED);
        return index>=0&&static_cast<size_t>(index)<items.size()&&items[index].contains("id")?display(items[index]["id"]):std::string();
    }
    static bool restore_selection(HWND table,const Json& items,const std::string& id){
        if(id.empty())return false;
        for(size_t i=0;i<items.size();++i)if(items[i].contains("id")&&display(items[i]["id"])==id){
            ListView_SetItemState(table,static_cast<int>(i),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);return true;
        }
        return false;
    }
    void accept_rows(const Payload& data){
        const auto selected_origin=selected_id(list,rows),selected_target=selected_id(targets,target_rows);
        replacing_rows=true;
        ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(targets,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        const auto items_of=[](const Json& obj){return obj.is_object()&&obj.contains("items")&&obj["items"].is_array()?obj["items"]:Json::array();};
        rows=items_of(data.left);target_rows=items_of(data.right);
        const auto cursor_of=[](const Json& page){if(!page.is_object())return std::string();const auto value=page.value("next_cursor",Json());return value.is_string()?value.get<std::string>():std::string();};
        next_cursor=cursor_of(data.left);next_target_cursor=cursor_of(data.right);
        settled_sequence=data.sequence;
        reading=false;KillTimer(hwnd,2);ShowWindow(progress,SW_HIDE);
        setup_columns(list,rows,columns);setup_columns(targets,target_rows,target_columns);
        auto caption_value=wide(data.caption);
        if(!cursors.empty()||!next_cursor.empty())caption_value=L"Página "+std::to_wstring(cursors.size()+1)+L" · "+caption_value;
        SetWindowTextW(caption,caption_value.c_str());
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,caption,OBJID_CLIENT,CHILDID_SELF);
        EnableWindow(next,!next_cursor.empty());EnableWindow(previous,!cursors.empty());
        restore_selection(list,rows,selected_origin);
        const bool restored_target=restore_selection(targets,target_rows,selected_target);
        target_selected=target_selected&&restored_target;
        replacing_rows=false;
        selected_detail();empty_state();layout();
    }
    void copy_paths(){int index=ListView_GetNextItem(target_selected?targets:list,-1,LVNI_SELECTED);const auto& data=target_selected?target_rows:rows;if(index<0||static_cast<size_t>(index)>=data.size())return;const auto& item=data[index];std::wstring text;for(const auto* key:{"source_path","absolute_path","output_path"})if(item.contains(key)&&item[key].is_string()){auto path=wide(item[key]);if(!path.empty()&&text.find(path)==std::wstring::npos)text+=path+L"\r\n";}if(item.contains("target_paths"))for(const auto& path:item["target_paths"])text+=wide(path)+L"\r\n";if(text.empty())text=control_text(detail);
        if(OpenClipboard(hwnd)){EmptyClipboard();auto memory=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));if(memory){auto bytes=GlobalLock(memory);memcpy(bytes,text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}CloseClipboard();}}
    void sync_folder(){if(!project)return;std::string target_root=root,target_sub=parent.empty()?".":parent;
        if(page==17){int at=ListView_GetNextItem(list,-1,LVNI_SELECTED);if(at>=0&&static_cast<size_t>(at)<rows.size()&&rows[at].value("kind",std::string())=="directory"){target_root=rows[at].value("root",target_root);target_sub=rows[at].value("path",target_sub);if(target_sub.empty())target_sub=".";}}
        if(target_root.empty())return;
        const auto p=*project;auto scope=Json{{"root",target_root},{"subdirectory",target_sub}};const auto ticket=++sequence;{std::lock_guard lock(mutex);actions.push_back({ticket,[=](){auto job=scale_start(p,{{"scope",scope},{"actor","GUI"}});Payload out;out.sequence=ticket;out.refresh_after=true;out.caption="Recorrido de carpeta iniciado: "+job["job_id"].get<std::string>();out.left=Json::object();return out;}});}ready.notify_one();}
    void control_job(const std::string& action){if(!project||page!=16)return;int at=ListView_GetNextItem(list,-1,LVNI_SELECTED);if(at<0||static_cast<size_t>(at)>=rows.size())return;auto id=display(rows[at]["id"]);if(id.empty())return;auto p=*project;auto ticket=++sequence;
        {std::lock_guard lock(mutex);actions.push_back({ticket,[=](){Payload out;scale_control(p,{{"id",id},{"action",action},{"actor","GUI"}});out.sequence=ticket;out.left=scale_jobs(p,{{"limit",100}});out.caption="Control de trabajo: "+action;return out;}});}ready.notify_one();}
    static LRESULT CALLBACK proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(msg==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->hwnd=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(w,msg,wp,lp);
        try{switch(msg){case WM_SIZE:self->layout();return 0;
          case WM_ERASEBKGND:{RECT rc{};GetClientRect(w,&rc);FillRect(reinterpret_cast<HDC>(wp),&rc,self->background);return 1;}
          case WM_PAINT:{PAINTSTRUCT paint{};auto dc=BeginPaint(w,&paint);if(!IsRectEmpty(&self->split_bounds))ui::draw_splitter_grip(dc,self->split_bounds,self->colors);EndPaint(w,&paint);return 0;}
          case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:return reinterpret_cast<LRESULT>(ui::color_control(reinterpret_cast<HDC>(wp),reinterpret_cast<HWND>(lp),self->colors,self->background,self->surface));
          case WM_SETCURSOR:{POINT pt{};GetCursorPos(&pt);ScreenToClient(w,&pt);if(self->dragging||PtInRect(&self->split_bounds,pt)){SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;}break;}
          case WM_LBUTTONDOWN:{POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(PtInRect(&self->split_bounds,pt)){self->dragging=true;SetCapture(w);return 0;}break;}
          case WM_MOUSEMOVE:if(self->dragging){RECT rc{};GetClientRect(w,&rc);self->split=std::clamp(static_cast<double>(GET_X_LPARAM(lp))/std::max(1L,rc.right),.2,.8);self->layout();return 0;}break;
          case WM_LBUTTONUP:if(self->dragging){self->dragging=false;ReleaseCapture();return 0;}break;
          case WM_CAPTURECHANGED:self->dragging=false;break;
          case WM_TIMER:if(wp==2){KillTimer(w,2);if(self->reading){BOOL animate=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animate,0);ShowWindow(self->progress,SW_SHOW);SendMessageW(self->progress,PBM_SETMARQUEE,animate,40);SetWindowTextW(self->caption,L"Consultando esta página del catálogo…");self->layout();}}return 0;
          case WM_ROWS:{std::unique_ptr<Payload> data(reinterpret_cast<Payload*>(lp));if(data->sequence&&data->sequence!=self->sequence)return 0;if(!data->error.empty()){self->settled_sequence=data->sequence;self->reading=false;KillTimer(w,2);ShowWindow(self->progress,SW_HIDE);SetWindowTextW(self->caption,L"No se pudo completar la consulta. Use Actualizar para reintentar.");SetWindowTextW(self->empty,wide(data->error).c_str());ShowWindow(self->empty,self->rows.empty()?SW_SHOW:SW_HIDE);PostMessageW(GetParent(w),WM_APP+1,0,reinterpret_cast<LPARAM>(new std::string("Consulta no completada: "+data->error)));self->layout();return 0;}
            if(data->summary.is_object())SendMessageW(GetParent(w),WM_APP+172,0,reinterpret_cast<LPARAM>(&data->summary));
            if(data->tree_parent){if(data->navigation!=self->navigation||!self->folders.contains(data->tree_parent))return 0;auto folder=self->folders[data->tree_parent];for(const auto& entry:data->tree["items"])self->add_folder(data->tree_parent,wide(entry["name"]),{folder.root,entry["path"],"",false},entry["kind"]=="directory");if(data->tree["next_cursor"].is_string())self->add_folder(data->tree_parent,L"Más carpetas…",{folder.root,folder.path,data->tree["next_cursor"],false});return 0;}
            if(data->refresh_after){SetWindowTextW(self->caption,wide(data->caption).c_str());self->request();return 0;}
            self->accept_rows(*data);
            if(data->tree.is_array()&&TreeView_GetCount(self->tree)==0)for(const auto& root:data->tree)self->add_folder(TVI_ROOT,wide(root["id"].get<std::string>()+" · "+root["path"].get<std::string>()),{root["id"],"","",false});return 0;}
          case WM_COMMAND:{
            switch(LOWORD(wp)){
              case 1:if(!self->cursors.empty()){self->cursor=self->cursors.back();self->cursors.pop_back();self->request();}break;
              case 2:if(!self->next_cursor.empty()){self->cursors.push_back(self->cursor);self->cursor=self->next_cursor;self->request();}break;
              case 3:if(!self->next_target_cursor.empty()){self->target_cursor=self->next_target_cursor;self->request();}break;
              case 4:self->request();break;
              case 5:self->control_job("pause");break;
              case 6:self->control_job("resume");break;
              case 7:self->control_job("cancel");break;
              case 13:self->control_job("retry");break;
              case 14:self->sync_folder();break;
              case 15:self->parent=path_text(fs::path(wide(self->parent)).parent_path());std::replace(self->parent.begin(),self->parent.end(),'\\','/');self->cursor.clear();self->cursors.clear();self->request();break;
              case 16:self->copy_paths();break;
              case 17:{auto value=control_text(self->detail);SendMessageW(GetParent(w),WM_APP+174,0,reinterpret_cast<LPARAM>(&value));break;}
              case 18:self->filters_visible=!self->filters_visible;SetWindowTextW(self->filter_button,self->filters_visible?L"Ocultar filtros":L"Filtros");self->layout();break;
              case 20:self->linked_only=SendMessageW(self->linked,BM_GETCHECK,0,0)==BST_CHECKED;self->target_cursor.clear();self->request();break;
              case 21:if(HIWORD(wp)==CBN_SELCHANGE){const char* reasons[]={"","missing_target","analysis_partial","declared_complete","tests_missing","stale","claimed","non_relevant","with_disposition"};auto choice=SendMessageW(self->pending_filter,CB_GETCURSEL,0,0);if(choice>=0&&choice<9)self->pending_reason=reasons[choice];self->cursor.clear();self->cursors.clear();self->request();}break;
              case 23:if(HIWORD(wp)==CBN_SELCHANGE){self->target_selected=SendMessageW(self->pane_choice,CB_GETCURSEL,0,0)==1;self->layout();}break;
              case 26:if(HIWORD(wp)==CBN_SELCHANGE){auto choice=SendMessageW(self->depth_choice,CB_GETCURSEL,0,0);if(choice>=0&&choice<4)self->depth=static_cast<int>(choice)+1;self->cursor.clear();self->cursors.clear();self->request();}break;
              case 27:if(HIWORD(wp)==CBN_SELCHANGE){auto choice=SendMessageW(self->root_choice,CB_GETCURSEL,0,0);if(choice>=0&&static_cast<size_t>(choice)<self->root_options.size())self->root=self->root_options[static_cast<size_t>(choice)];self->cursor.clear();self->cursors.clear();self->request();}break;
              case 28:if(HIWORD(wp)==CBN_SELCHANGE){auto choice=SendMessageW(self->dev_kind_choice,CB_GETCURSEL,0,0);if(choice>=0&&choice<4)self->dev_kind=static_cast<int>(choice);self->cursor.clear();self->cursors.clear();self->layout();self->request();}break;
              case 29:if(HIWORD(wp)==CBN_SELCHANGE){const char* states[]={"","PLANNED","IN_PROGRESS","HALF_DONE","PORTED","INTEGRATED","COMPLETED","VERIFIED","BLOCKED","TODO","UNREVIEWED","REVIEWED","ACTION_REQUIRED","STALE"};auto choice=SendMessageW(self->dev_state_choice,CB_GETCURSEL,0,0);if(choice>=0&&choice<14)self->dev_state=states[choice];self->cursor.clear();self->cursors.clear();self->request();}break;
              case 30:self->include_non_source=SendMessageW(self->include_non_source_chk,BM_GETCHECK,0,0)==BST_CHECKED;self->cursor.clear();self->cursors.clear();self->request();break;
            }return 0;}
          case WM_NOTIFY:{auto* hdr=reinterpret_cast<NMHDR*>(lp);
            if(hdr->code==NM_CUSTOMDRAW&&(hdr->hwndFrom==self->list||hdr->hwndFrom==self->targets))return ui::handle_listview_customdraw(lp,self->colors);
            if(hdr->code==LVN_GETDISPINFOW){auto* info=reinterpret_cast<NMLVDISPINFOW*>(lp);auto& rows=hdr->hwndFrom==self->targets?self->target_rows:self->rows;auto& cols=hdr->hwndFrom==self->targets?self->target_columns:self->columns;
                if(info->item.iItem>=0&&static_cast<size_t>(info->item.iItem)<rows.size()&&info->item.iSubItem>=0&&static_cast<size_t>(info->item.iSubItem)<cols.size()&&(info->item.mask&LVIF_TEXT)){auto value=wide(human(cols[info->item.iSubItem],rows[info->item.iItem].value(cols[info->item.iSubItem],Json())));wcsncpy_s(info->item.pszText,info->item.cchTextMax,value.c_str(),_TRUNCATE);}return 0;}
            if(hdr->code==LVN_ITEMCHANGED&&!self->replacing_rows&&(hdr->hwndFrom==self->list||hdr->hwndFrom==self->targets)){
                const auto* change=reinterpret_cast<const NMLISTVIEW*>(lp);
                if((change->uNewState^change->uOldState)&LVIS_SELECTED){self->target_selected=hdr->hwndFrom==self->targets;self->selected_detail();self->layout();}
                return 0;
            }
            if(hdr->code==NM_DBLCLK&&hdr->hwndFrom==self->list){
                int at=ListView_GetNextItem(self->list,-1,LVNI_SELECTED);
                if(at>=0&&static_cast<size_t>(at)<self->rows.size()){
                    if(self->page==17&&str_or(self->rows[at],"kind")=="directory"){self->parent=str_or(self->rows[at],"path");self->root=str_or(self->rows[at],"root");self->cursor.clear();self->cursors.clear();self->request();return 0;}
                    if(self->page==23){PostMessageW(GetParent(w),WM_SCALE_ACTION,69,0);return 0;}
                    if(self->page==24){const auto rt=str_or(self->rows[at],"record_type");const WPARAM cmd=rt=="Fase"?65:rt=="Tarea"?67:68;PostMessageW(GetParent(w),WM_SCALE_ACTION,cmd,0);return 0;}
                }
            }
            if(hdr->code==TVN_ITEMEXPANDINGW&&hdr->hwndFrom==self->tree){auto* change=reinterpret_cast<NMTREEVIEWW*>(lp);if(change->action==TVE_EXPAND)self->request_tree(change->itemNew.hItem);return 0;}
            if(hdr->code==TVN_SELCHANGEDW&&hdr->hwndFrom==self->tree){auto* change=reinterpret_cast<NMTREEVIEWW*>(lp);auto found=self->folders.find(change->itemNew.hItem);if(found!=self->folders.end()){self->root=found->second.root;self->parent=found->second.path;self->cursor.clear();self->cursors.clear();self->request();}return 0;}break;}
        }}catch(const std::exception& e){SetWindowTextW(self->caption,wide(e.what()).c_str());}return DefWindowProcW(w,msg,wp,lp);
    }
};
ScaleView::ScaleView():impl_(std::make_unique<Impl>()){}ScaleView::~ScaleView()=default;
void ScaleView::create(HWND parent,HFONT font,int dpi){auto& s=*impl_;s.font=font;s.dpi=dpi;WNDCLASSW cls{};cls.lpfnWndProc=Impl::proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MigratoolScaleView";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&cls);
    s.hwnd=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"Inventario y trabajos a escala",WS_CHILD|WS_CLIPCHILDREN,0,0,100,100,parent,nullptr,cls.hInstance,&s);
    s.previous=s.make(L"BUTTON",L"Anterior",WS_TABSTOP,1);s.next=s.make(L"BUTTON",L"Siguiente",WS_TABSTOP,2);s.next_target=s.make(L"BUTTON",L"Más destinos",WS_TABSTOP,3);s.refresh_button=s.make(L"BUTTON",L"Actualizar",WS_TABSTOP,4);s.pause=s.make(L"BUTTON",L"Pausar",WS_TABSTOP,5);s.resume=s.make(L"BUTTON",L"Reanudar",WS_TABSTOP,6);s.cancel=s.make(L"BUTTON",L"Cancelar",WS_TABSTOP,7);
    s.retry=s.make(L"BUTTON",L"Reintentar",WS_TABSTOP,13);s.rescan=s.make(L"BUTTON",L"Recorrer aquí",WS_TABSTOP,14);s.up=s.make(L"BUTTON",L"Subir carpeta",WS_TABSTOP,15);s.copy=s.make(L"BUTTON",L"Copiar rutas",WS_TABSTOP,16);s.linked=s.make(L"BUTTON",L"Solo destinos vinculados",WS_TABSTOP|BS_AUTOCHECKBOX,20);s.pending_filter=s.make(L"COMBOBOX",L"Filtrar pendientes",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,21);for(const auto* option:{L"Todos los archivos",L"Sin destino",L"Análisis parcial",L"Declarados completos",L"Sin casos vinculados",L"Evidencia obsoleta",L"Reclamados por un agente",L"No relevantes / excluidos",L"Con disposición (eliminado/externo)"})SendMessageW(s.pending_filter,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(option));SendMessageW(s.pending_filter,CB_SETCURSEL,0,0);
    s.depth_choice=s.make(WC_COMBOBOXW,L"Profundidad de módulos",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,26);for(const auto* dopt:{L"Profundidad 1",L"Profundidad 2",L"Profundidad 3",L"Profundidad 4"})SendMessageW(s.depth_choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(dopt));SendMessageW(s.depth_choice,CB_SETCURSEL,1,0);
    s.root_choice=s.make(WC_COMBOBOXW,L"Raíz de origen",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,27);SendMessageW(s.root_choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Todas las raíces"));SendMessageW(s.root_choice,CB_SETCURSEL,0,0);
    s.dev_kind_choice=s.make(WC_COMBOBOXW,L"Categoría de control",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,28);for(const auto* kopt:{L"Todo (Fases, tareas y documentos)",L"Solo fases",L"Solo tareas",L"Solo documentos"})SendMessageW(s.dev_kind_choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(kopt));SendMessageW(s.dev_kind_choice,CB_SETCURSEL,0,0);
    s.dev_state_choice=s.make(WC_COMBOBOXW,L"Estado de control",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,29);for(const auto* sopt:{L"Todos los estados",L"PLANNED",L"IN_PROGRESS",L"HALF_DONE",L"PORTED",L"INTEGRATED",L"COMPLETED",L"VERIFIED",L"BLOCKED",L"TODO",L"UNREVIEWED",L"REVIEWED",L"ACTION_REQUIRED",L"STALE"})SendMessageW(s.dev_state_choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(sopt));SendMessageW(s.dev_state_choice,CB_SETCURSEL,0,0);
    s.include_non_source_chk=s.make(L"BUTTON",L"Incluir no código",WS_TABSTOP|BS_AUTOCHECKBOX,30);
    s.caption=s.make(L"STATIC",L"Consultando catálogo en segundo plano…",0,8);s.list=s.make(WC_LISTVIEWW,L"Origen e inventario",WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS,9);s.targets=s.make(WC_LISTVIEWW,L"Destinos",WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL,10);
    auto split_key_proc=[](HWND ctrl,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)->LRESULT{
        if(msg==WM_KEYDOWN&&(wp==VK_LEFT||wp==VK_RIGHT)&&(GetKeyState(VK_CONTROL)&0x8000)){
            auto* impl=reinterpret_cast<Impl*>(data);
            if(impl&&impl->page==1&&!impl->stacked){impl->split=std::clamp(impl->split+(wp==VK_RIGHT?.05:-.05),.2,.8);impl->layout();return 0;}
        }
        return DefSubclassProc(ctrl,msg,wp,lp);
    };
    SetWindowSubclass(s.list,split_key_proc,1,reinterpret_cast<DWORD_PTR>(&s));
    SetWindowSubclass(s.targets,split_key_proc,1,reinterpret_cast<DWORD_PTR>(&s));
    s.source_title=s.make(L"STATIC",L"Archivos de origen",0,33);s.target_title=s.make(L"STATIC",L"Archivos de destino",0,34);
    s.detail=s.make(L"EDIT",L"Seleccione una fila para consultar rutas completas y evidencia.",WS_TABSTOP|WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL,11);s.tree=s.make(WC_TREEVIEWW,L"Raíces y cobertura",WS_TABSTOP|WS_BORDER|TVS_HASLINES|TVS_HASBUTTONS|TVS_LINESATROOT|TVS_SHOWSELALWAYS,12);
    ui::apply_control_polish(s.detail,true,s.px(8));
    ui::apply_control_polish(s.tree,false);
    ui::name(s.detail,L"Rutas completas y evidencia del archivo seleccionado");
    s.details_button=s.make(L"BUTTON",L"Ver detalle…",WS_TABSTOP,17);s.filter_button=s.make(L"BUTTON",L"Filtros",WS_TABSTOP,18);
    s.pane_choice=s.make(WC_COMBOBOXW,L"Lista visible: origen o destino",WS_TABSTOP|CBS_DROPDOWNLIST,23);
    for(const auto* name:{L"Archivos de origen",L"Archivos de destino"})SendMessageW(s.pane_choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
    SendMessageW(s.pane_choice,CB_SETCURSEL,0,0);
    s.empty=s.make(L"STATIC",L"",SS_CENTER,24);
    s.progress=s.make(PROGRESS_CLASSW,L"Consulta del catálogo en curso",PBS_MARQUEE,25);
    ShowWindow(s.progress,SW_HIDE);ShowWindow(s.empty,SW_HIDE);
    const HWND keyboard[]={s.refresh_button,s.filter_button,s.copy,s.details_button,s.pause,s.resume,s.cancel,s.retry,s.rescan,s.up,s.depth_choice,s.dev_kind_choice,s.dev_state_choice,s.root_choice,s.pending_filter,s.linked,s.include_non_source_chk,s.tree,s.pane_choice,s.list,s.targets,s.previous,s.next,s.next_target,s.detail};
    ui::tab_order(keyboard);
    for(auto list:{s.list,s.targets}){
        ListView_SetExtendedListViewStyle(list,LVS_EX_DOUBLEBUFFER|LVS_EX_FULLROWSELECT|LVS_EX_LABELTIP);
        ui::apply_control_polish(list,false);
    }
    s.layout();
}
HWND ScaleView::window()const{return impl_->hwnd;}
void ScaleView::appearance(HFONT font,int dpi){
    auto& s=*impl_;s.font=font;s.dpi=dpi;
    if(s.hwnd)SendMessageW(s.hwnd,WM_SETREDRAW,FALSE,0);
    s.colors=ui::system_palette();DeleteObject(s.background);DeleteObject(s.surface);
    const auto pixels=ui::type_size(18,dpi);
    if(s.font_pixels>0&&s.font_pixels!=pixels){
        for(auto table:{s.list,s.targets})for(int column=0;column<Header_GetItemCount(ListView_GetHeader(table));++column){
            ListView_SetColumnWidth(table,column,MulDiv(ListView_GetColumnWidth(table,column),pixels,s.font_pixels));
        }
    }
    s.font_pixels=pixels;
    s.background=CreateSolidBrush(s.colors.background);s.surface=CreateSolidBrush(s.colors.surface);
    ui::list_colors(s.list,s.colors);ui::list_colors(s.targets,s.colors);TreeView_SetBkColor(s.tree,s.colors.surface);
    TreeView_SetTextColor(s.tree,s.colors.text);TreeView_SetItemHeight(s.tree,ui::type_size(18,dpi)+ui::dip(10,dpi));
    s.layout();
    if(s.hwnd){SendMessageW(s.hwnd,WM_SETREDRAW,TRUE,0);InvalidateRect(s.hwnd,nullptr,FALSE);}
}
void ScaleView::show(const Project& p,int page,const std::string& filter){
    auto& s=*impl_;
    const bool changed_proj=!s.project||s.project->file!=p.file||s.project->revision!=p.revision;
    const bool changed_view=changed_proj||s.page!=page;
    if(changed_view||s.filter!=filter){s.cursor.clear();s.cursors.clear();s.target_cursor.clear();s.next_cursor.clear();s.next_target_cursor.clear();}
    if(changed_view){
        if(s.hwnd)SendMessageW(s.hwnd,WM_SETREDRAW,FALSE,0);
        s.parent.clear();if(page!=1&&!s.keep_context)s.root.clear();s.keep_context=false;s.selected_source.clear();s.target_selected=false;++s.navigation;s.folders.clear();
        if(TreeView_GetCount(s.tree)>0){SendMessageW(s.tree,WM_SETREDRAW,FALSE,0);TreeView_DeleteAllItems(s.tree);SendMessageW(s.tree,WM_SETREDRAW,TRUE,0);}
        s.replacing_rows=true;ListView_SetItemCountEx(s.list,0,0);ListView_SetItemCountEx(s.targets,0,0);s.replacing_rows=false;
        s.rows=Json::array();s.target_rows=Json::array();ui::show_if(s.empty,false);
        SetWindowTextW(s.caption,L"Preparando esta vista…");
    }
    s.project=p;s.page=page;s.filter=filter;
    if(changed_view)s.sync_root_options();
    ui::show_if(s.hwnd,true);s.layout();
    if(changed_view&&s.hwnd){SendMessageW(s.hwnd,WM_SETREDRAW,TRUE,0);InvalidateRect(s.hwnd,nullptr,FALSE);}
    s.request();
}
void ScaleView::refresh(){impl_->request();}
std::string ScaleView::selected()const{auto& s=*impl_;int at=ListView_GetNextItem(s.list,-1,LVNI_SELECTED);return at>=0&&static_cast<size_t>(at)<s.rows.size()&&s.rows[at].contains("id")?display(s.rows[at]["id"]):"";}
Json ScaleView::selected_row()const{auto& s=*impl_;int at=ListView_GetNextItem(s.list,-1,LVNI_SELECTED);return at>=0&&static_cast<size_t>(at)<s.rows.size()?s.rows[at]:Json();}
std::string ScaleView::active_root()const{return impl_->root;}
bool ScaleView::busy()const{return impl_->reading;}
bool ScaleView::settled()const{return !impl_->reading&&impl_->sequence==impl_->settled_sequence;}
void ScaleView::set_filter_context(const std::string& root_id,const std::string& filter_text){
    auto& s=*impl_;s.keep_context=true;s.root=root_id;s.filter=filter_text;s.cursor.clear();s.cursors.clear();s.sync_root_options();
}
}
