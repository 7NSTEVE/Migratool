#include "migratool/core.hpp"
#include "migratool/gateway.hpp"
#include "migratool/scale.hpp"
#include "migratool/scale_view.hpp"
#include "migratool/navigation.hpp"
#include <initguid.h>
#include "migratool/ui.hpp"
#include "migratool/live.hpp"
#include "migratool/comparison_view.hpp"
#include "migratool/duplicates.hpp"
#include "migratool/fs_organize.hpp"
#include "migratool/multiagent_coordinator.hpp"
#include "migratool/code_audit.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <memory>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <functional>
#include <set>
#include <chrono>

namespace {
using namespace mt;
constexpr UINT WM_LOG=WM_APP+1, WM_DONE=WM_APP+2,WM_OPEN_PROJECT=WM_APP+3;
constexpr UINT WM_PROJECT_POLL=WM_APP+5;
constexpr UINT WM_REUSE_RESULT=WM_APP+6;
constexpr int ID_NEW=10,ID_OPEN=11,ID_CONFIG=12,ID_NOTE=13,ID_CONFIG_ADVANCED=15,ID_PROJECT=14,ID_SCAN=20,ID_BUILD=21,ID_TEST=22,ID_REFRESH=23,ID_REPORT=24,ID_MARK=25,ID_COVERAGE=26,ID_CANCEL=27,ID_FILTER=30;
mt::ui::Palette palette=mt::ui::system_palette();
#define BG palette.background
#define INK palette.text
#define MUTED palette.secondary
#define BLUE palette.accent
#define NAV palette.sidebar
const wchar_t* pages[]={L"Estado del proyecto",L"Origen y destino",L"Símbolos del código",L"Correspondencias registradas",L"Pruebas de comportamiento",L"Cobertura de pruebas",L"Historial de cambios",L"Todos los archivos",L"Archivos pendientes",L"Git y compilación",L"Buscar en el proyecto",L"Dependencias del código",L"Integración con agentes",L"Agentes en vivo",L"Proyectos recientes",L"Código duplicado",L"Trabajos del proyecto",L"Cobertura por carpeta",L"Trabajo existente",L"Diagnóstico local",L"Carpetas y configuración",L"Archivos dentro de ZIP",L"Problemas del escaneo",L"Matriz de módulos",L"Fases, tareas y documentos",L"Sesiones y Handoffs",L"Linaje de rutas y árbol",L"Conocimiento y métodos probados",L"Documentos del proyecto",L"Lenguajes y herramientas del host",L"Coordinación y worktrees",L"Compilaciones y almacenamiento",L"Requisitos maestros",L"Auditoría técnica continua"};
const wchar_t* page_subtitle(int p){
    switch(p){
        case 30:return L"Inventario persistente de worktrees, avances por agente y evaluación de fusión sin tocar el árbol.";
        case 31:return L"Manifiestos de compilación, trazabilidad inversa de binarios y auditoría de almacenamiento en cuatro niveles.";
        case 32:return L"Especificación maestra del producto y matriz de trazabilidad entre requisitos, código, pruebas y builds.";
        case 33:return L"Auditoría incremental del código, complejidad sintáctica, seguridad y verificación de entregas.";
        default:return navigation_groups()[navigation_group(p)].description;
    }
}
constexpr int ID_SECTION_VIEW=31,ID_SECTION_COMPACT=32,ID_PROJECT_MENU=16,ID_LOG_TOGGLE=17,ID_NEXT_STEP=19;
constexpr int ID_HELP=50,ID_EXIT=51,ID_ZOOM_IN=52,ID_ZOOM_OUT=53,ID_ZOOM_RESET=54,ID_FOCUS_SEARCH=55,ID_RELOAD_VIEW=56;
constexpr int ID_SIDEBAR=57,ID_CODE_SETTINGS=58,ID_RESOURCES=59;
constexpr int ID_IMPORT_WORK=60,ID_REVALIDATE_WORK=61,ID_CONTINUE_IMPORT=62;
constexpr int ID_EXPORT_INVENTORY=63,ID_VERIFY_CONFIG=64;
constexpr int ID_PHASE_UPSERT=65,ID_TASK_UPSERT=66,ID_TASK_TRANSITION=67,ID_DOC_AUDIT=68,ID_MODULE_FILTER_FILES=69,ID_TASK_DELETE=70,ID_CYCLE_PANE=71;
constexpr int ID_HOUSEKEEP=72,ID_TOPOLOGY_AUDIT=73,ID_INCREASE_CONTRAST=74,ID_TOGGLE_THEME=75;
constexpr const char* duplicate_modes[]={"off","files","files_and_zip"};
struct FormCombo {
    int id=0;
    std::wstring label;
    std::vector<std::wstring> options;
    int selected=0;
    HWND control{};
};
struct Form {
    HWND window{},parent{};bool done=false,accepted=false;int width=800,height=610;
    std::vector<std::wstring> labels,values;std::vector<HWND> edits;std::wstring title;
    std::optional<bool> source_only;
    int duplicate_choice=-1;
    int completion_choice=-1,disposition_choice=-1;bool browse_folders=false,browse_files=false,append_first=true;
    int expanded_code=-1,single_folder=-1;HWND expanded_code_mode{};
    int analyze_cpp_choice=-1,nested_zip_choice=-1;HWND analyze_cpp_mode{},nested_zip_mode{};
    int reuse_kind=-1,reuse_verify=-1;HWND reuse_kind_mode{},reuse_verify_mode{};
    std::vector<FormCombo> extra_combos;
    std::set<size_t> single_fields,browse_fields;
    std::wstring explanation,confirm_label=L"Guardar";HWND explanation_control{};
    HWND inventory_mode{},duplicate_mode{},completion_mode{},disposition_mode{};
    HWND viewport{},save{},cancel{},validation{};HFONT dialog_font{};int dpi=96,scroll=0,content_height=0;
    ui::Palette colors=ui::system_palette();HBRUSH background{},surface{};
    std::vector<HWND> field_labels,browse,mode_labels;
    std::function<std::wstring(const Form&)> validate;
    bool read_only=false;
};
std::optional<fs::path> open_file(HWND,const wchar_t*,bool folder=false);
std::vector<std::string> lines(const std::wstring& value);
std::wstring text(HWND w) {int n=GetWindowTextLengthW(w);std::wstring s(n+1,L'\0');GetWindowTextW(w,s.data(),n+1);s.resize(n);return s;}
bool same_path(fs::path left,fs::path right){left=left.lexically_normal().make_preferred();right=right.lexically_normal().make_preferred();return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE)==CSTR_EQUAL;}
std::string percent(const Json& value){if(value.is_null())return "sin verificar";std::wostringstream out;out<<std::fixed<<std::setprecision(1)<<value.get<double>();return utf8(ui::number(out.str(),1)+L" %");}
void font(HWND w,HFONT f) {SendMessageW(w,WM_SETFONT,reinterpret_cast<WPARAM>(f),FALSE);}
HWND control(HWND parent,const wchar_t* cls,const std::wstring& title,DWORD style,int id,HFONT f) {
    auto w=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,title.c_str(),WS_CHILD|WS_VISIBLE|style,0,0,10,10,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    font(w,f);if(wcscmp(cls,WC_LISTVIEWW)==0||wcscmp(cls,WC_COMBOBOXW)==0)ui::name(w,title);return w;
}
HFONT body{},heading{},small_font{},cardfont{};
struct ViewGuard { bool& flag; explicit ViewGuard(bool& value):flag(value){flag=true;} ~ViewGuard(){flag=false;} };
void form_appearance(Form& form){
    form.colors=ui::system_palette();
    if(form.background)DeleteObject(form.background);
    if(form.surface)DeleteObject(form.surface);
    form.background=CreateSolidBrush(form.colors.background);
    form.surface=CreateSolidBrush(form.colors.surface);
    auto previous=form.dialog_font;
    form.dialog_font=CreateFontW(-ui::type_size(18,form.dpi),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    ui::font_tree(form.window,form.dialog_font);
    if(previous)DeleteObject(previous);
}
void form_layout(Form& f){
    if(!f.viewport)return;RECT rc{};GetClientRect(f.window,&rc);const auto p=[&](int n){return ui::dip(n,f.dpi);};
    const int gap=p(12),line=ui::type_size(18,f.dpi)+p(7),button=ui::control_height(f.dpi);
    const int footer=button+line*2+gap*3,width=std::max(p(220),static_cast<int>(rc.right)-p(46));
    MoveWindow(f.viewport,0,0,rc.right,std::max(p(80),static_cast<int>(rc.bottom)-footer),TRUE);
    int y=p(20);size_t mode=0;
    if(f.explanation_control){
        RECT bounds{0,0,width,0};auto dc=GetDC(f.window);auto old=SelectObject(dc,f.dialog_font);
        DrawTextW(dc,f.explanation.c_str(),-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,old);ReleaseDC(f.window,dc);
        MoveWindow(f.explanation_control,p(20),y,width,bounds.bottom,TRUE);y+=bounds.bottom+gap;
    }
    for(auto combo:{f.inventory_mode,f.duplicate_mode,f.completion_mode,f.disposition_mode,f.expanded_code_mode,f.analyze_cpp_mode,f.nested_zip_mode,f.reuse_kind_mode,f.reuse_verify_mode})if(combo){
        MoveWindow(f.mode_labels[mode++],p(20),y,width,line,TRUE);y+=line+p(4);
        MoveWindow(combo,p(20),y,width,button+p(240),TRUE);y+=button+gap;
    }
    for(const auto& ec:f.extra_combos)if(ec.control){
        MoveWindow(f.mode_labels[mode++],p(20),y,width,line,TRUE);y+=line+p(4);
        MoveWindow(ec.control,p(20),y,width,button+p(240),TRUE);y+=button+gap;
    }
    for(size_t i=0;i<f.edits.size();++i){
        MoveWindow(f.field_labels[i],p(20),y,width,line,TRUE);y+=line+p(4);
        const bool single_line=(GetWindowLongPtrW(f.edits[i],GWL_STYLE)&ES_MULTILINE)==0;
        const int browse_width=f.browse[i]?std::min(width/3,std::max(p(150),ui::type_size(18,f.dpi)*9)):0;
        if(f.browse[i])MoveWindow(f.browse[i],p(20)+width-browse_width,y,browse_width,button,TRUE);
        int edit_height=single_line?button:f.labels.size()==1?std::max(p(160),static_cast<int>(rc.bottom)-footer-y-p(25)):std::max(line*2,p(f.browse_folders?86:62));
        MoveWindow(f.edits[i],p(20),y,width-(browse_width?browse_width+gap:0),edit_height,TRUE);y+=edit_height+gap;
    }
    f.content_height=y;RECT view{};GetClientRect(f.viewport,&view);
    f.scroll=std::clamp(f.scroll,0,std::max(0,y-static_cast<int>(view.bottom)));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,y-1),static_cast<UINT>(view.bottom),f.scroll,0};SetScrollInfo(f.viewport,SB_VERT,&info,TRUE);
    for(auto child=GetWindow(f.viewport,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,f.viewport,reinterpret_cast<POINT*>(&bounds),2);
        SetWindowPos(child,nullptr,bounds.left,bounds.top-f.scroll,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    const int footer_top=std::max(p(80),static_cast<int>(rc.bottom)-footer);
    MoveWindow(f.validation,p(20),footer_top+gap,width,line*2,TRUE);
    const int button_width=std::max(p(120),ui::type_size(18,f.dpi)*8);
    MoveWindow(f.save,rc.right-p(20)-button_width,rc.bottom-gap-button,button_width,button,TRUE);
    MoveWindow(f.cancel,rc.right-p(32)-button_width*2,rc.bottom-gap-button,button_width,button,TRUE);
}
void form_scroll(Form& f,int requested){
    RECT rc{};GetClientRect(f.viewport,&rc);const auto position=std::clamp(requested,0,std::max(0,f.content_height-static_cast<int>(rc.bottom)));
    if(position==f.scroll)return;const auto delta=f.scroll-position;f.scroll=position;
    for(auto child=GetWindow(f.viewport,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,f.viewport,reinterpret_cast<POINT*>(&bounds),2);
        SetWindowPos(child,nullptr,bounds.left,bounds.top+delta,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    RedrawWindow(f.viewport,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    SCROLLINFO info{sizeof(info),SIF_POS};info.nPos=position;SetScrollInfo(f.viewport,SB_VERT,&info,TRUE);
}
LRESULT CALLBACK form_body_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    auto f=reinterpret_cast<Form*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(msg==WM_NCCREATE){f=static_cast<Form*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(f));}
    if(f)switch(msg){
        case WM_COMMAND:return SendMessageW(f->window,msg,wp,lp);
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:return SendMessageW(f->window,msg,wp,lp);
        case WM_ERASEBKGND:{RECT area{};GetClientRect(w,&area);FillRect(reinterpret_cast<HDC>(wp),&area,f->background);return 1;}
        case WM_VSCROLL:{SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(w,SB_VERT,&info);int position=f->scroll;switch(LOWORD(wp)){
            case SB_LINEUP:position-=ui::dip(36,f->dpi);break;case SB_LINEDOWN:position+=ui::dip(36,f->dpi);break;
            case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;case SB_THUMBTRACK:position=info.nTrackPos;break;}
            form_scroll(*f,position);return 0;}
        case WM_MOUSEWHEEL:form_scroll(*f,f->scroll-GET_WHEEL_DELTA_WPARAM(wp)*ui::dip(90,f->dpi)/WHEEL_DELTA);return 0;
    }return DefWindowProcW(w,msg,wp,lp);
}
LRESULT CALLBACK form_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    auto f=reinterpret_cast<Form*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(msg==WM_NCCREATE){f=static_cast<Form*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(f));f->window=w;}
    if(!f)return DefWindowProcW(w,msg,wp,lp);
    switch(msg) {
      case WM_CREATE: {
        f->dpi=GetDpiForWindow(w);form_appearance(*f);
        f->viewport=CreateWindowExW(WS_EX_CONTROLPARENT,L"MigratoolFormBody",L"Campos del formulario",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_CLIPCHILDREN,0,0,10,10,w,nullptr,GetModuleHandleW(nullptr),f);
        ui::apply_control_polish(f->viewport,false);
        const auto mode_combo=[&](const wchar_t* name,int id,std::initializer_list<const wchar_t*> choices,int current){
            f->mode_labels.push_back(control(f->viewport,L"STATIC",name,0,0,f->dialog_font));
            auto combo=control(f->viewport,WC_COMBOBOXW,name,CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,id,f->dialog_font);
            for(const auto* option:choices)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(option));
            SendMessageW(combo,CB_SETCURSEL,current,0);return combo;
        };
        if(f->source_only.has_value())f->inventory_mode=mode_combo(L"Archivos que desea analizar",90,{L"Todos los archivos",L"Solo código fuente"},*f->source_only?1:0);
        if(f->duplicate_choice>=0)f->duplicate_mode=mode_combo(L"Buscar copias de código exactamente iguales",91,{L"Desactivado",L"En carpetas",L"En carpetas y dentro de ZIP"},f->duplicate_choice);
        if(f->completion_choice>=0)f->completion_mode=mode_combo(L"Etapa de madurez (stage)",92,{L"Planificado (planned)",L"A medias (half_done)",L"Portado (ported)",L"Integrado / completo (integrated)",L"Verificado en casos (verified — automático)"},f->completion_choice);
        if(f->disposition_choice>=0)f->disposition_mode=mode_combo(L"Disposición del archivo",98,{L"Migrado al destino (migrated)",L"Fusionado en otro destino (merged_into)",L"Eliminado intencionalmente (intentionally_removed)",L"Reemplazado por externo/tercero (replaced_by_external)"},f->disposition_choice);
        if(f->expanded_code>=0)f->expanded_code_mode=mode_combo(L"Código generado y dependencias",93,{L"Conservar el ajuste de cada carpeta",L"Alcance habitual",L"Incluir también código generado y de dependencias"},f->expanded_code);
        if(f->analyze_cpp_choice>=0)f->analyze_cpp_mode=mode_combo(L"Analizar C y C++ con libclang",96,{L"Desactivado",L"Activado"},f->analyze_cpp_choice);
        if(f->nested_zip_choice>=0)f->nested_zip_mode=mode_combo(L"Profundidad máxima en ZIP anidados",97,{L"0 (sin ZIP anidados)",L"1 nivel",L"2 niveles (por defecto)",L"3 niveles",L"4 niveles"},f->nested_zip_choice);
        if(!f->explanation.empty())f->explanation_control=control(f->viewport,L"STATIC",f->explanation,0,0,f->dialog_font);
        if(f->reuse_kind>=0)f->reuse_kind_mode=mode_combo(L"Procedencia del trabajo",94,{L"Detectar automáticamente",L"Catálogo de archivos",L"Registro de recuperaciones y resultados"},f->reuse_kind);
        if(f->reuse_verify>=0)f->reuse_verify_mode=mode_combo(L"Comprobación del lote",95,{L"Comprobar el contenido por SHA-256",L"Registrar candidatos para comprobar después"},f->reuse_verify);
        for(size_t idx=0;idx<f->extra_combos.size();++idx){
            auto& ec=f->extra_combos[idx];
            const int cid=ec.id?ec.id:(99+static_cast<int>(idx));
            f->mode_labels.push_back(control(f->viewport,L"STATIC",ec.label,0,0,f->dialog_font));
            ec.control=control(f->viewport,WC_COMBOBOXW,ec.label,CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,cid,f->dialog_font);
            for(const auto& opt:ec.options)SendMessageW(ec.control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(opt.c_str()));
            SendMessageW(ec.control,CB_SETCURSEL,std::max(0,ec.selected),0);
        }
        for(size_t i=0;i<f->labels.size();++i) {
            f->field_labels.push_back(control(f->viewport,L"STATIC",f->labels[i],0,0,f->dialog_font));
            HWND browse{};
            if(f->browse_fields.contains(i))browse=control(f->viewport,L"BUTTON",L"Elegir archivo…",WS_TABSTOP,300+static_cast<int>(i),f->dialog_font);
            else if(static_cast<int>(i)==f->single_folder)browse=control(f->viewport,L"BUTTON",L"Elegir carpeta…",WS_TABSTOP,300+static_cast<int>(i),f->dialog_font);
            else if(i<2&&(f->browse_folders||f->browse_files))browse=control(f->viewport,L"BUTTON",f->browse_folders?L"Añadir carpeta…":L"Elegir archivo…",WS_TABSTOP,300+static_cast<int>(i),f->dialog_font);
            f->browse.push_back(browse);
            const bool single_line=f->single_fields.contains(i)||(i==0&&f->browse_files)||static_cast<int>(i)==f->single_folder||f->labels[i].starts_with(L"Nombre del proyecto")||f->labels[i].starts_with(L"Autor")||f->labels[i].starts_with(L"Memoria")||f->labels[i].starts_with(L"Tareas")||f->labels[i].starts_with(L"Analizadores simultáneos");
            const DWORD style=single_line?ES_AUTOHSCROLL:ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL;
            auto edit=control(f->viewport,L"EDIT",f->values[i],WS_TABSTOP|style|(f->read_only?ES_READONLY:0),100+static_cast<int>(i),f->dialog_font);
            ui::apply_control_polish(edit,true,ui::dip(8,f->dpi));
            ui::name(edit,f->labels[i]);SendMessageW(edit,EM_SETLIMITTEXT,32*1024*1024,0);f->edits.push_back(edit);
        }
        f->validation=control(w,L"STATIC",L"",0,0,f->dialog_font);
        f->save=control(w,L"BUTTON",f->read_only?L"Cerrar":f->confirm_label,WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,f->dialog_font);
        f->cancel=control(w,L"BUTTON",L"Cancelar",WS_TABSTOP,IDCANCEL,f->dialog_font);
        if(f->read_only)ShowWindow(f->cancel,SW_HIDE);
        std::vector<HWND> keyboard;
        for(auto combo:{f->inventory_mode,f->duplicate_mode,f->completion_mode,f->disposition_mode,f->expanded_code_mode,f->analyze_cpp_mode,f->nested_zip_mode,f->reuse_kind_mode,f->reuse_verify_mode})if(combo)keyboard.push_back(combo);
        for(const auto& ec:f->extra_combos)if(ec.control)keyboard.push_back(ec.control);
        for(size_t i=0;i<f->edits.size();++i){keyboard.push_back(f->edits[i]);if(f->browse[i])keyboard.push_back(f->browse[i]);}
        ui::tab_order(keyboard);
        form_layout(*f);SetFocus(f->inventory_mode?f->inventory_mode:f->completion_mode?f->completion_mode:(!f->extra_combos.empty()&&f->extra_combos[0].control)?f->extra_combos[0].control:f->edits.empty()?f->save:f->edits[0]);return 0;
      }
      case WM_SIZE:form_layout(*f);return 0;
      case DM_GETDEFID:return MAKELRESULT(IDOK,DC_HASDEFID);
      case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={ui::dip(520,f->dpi),ui::dip(400,f->dpi)};return 0;
      case WM_DPICHANGED:{f->dpi=HIWORD(wp);auto r=reinterpret_cast<RECT*>(lp);form_appearance(*f);SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);form_layout(*f);return 0;}
      case WM_SETTINGCHANGE:case WM_THEMECHANGED:case WM_SYSCOLORCHANGE:form_appearance(*f);form_layout(*f);RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);return 0;
      case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:{
        auto child=reinterpret_cast<HWND>(lp);auto dc=reinterpret_cast<HDC>(wp);
        if(child==f->validation){SetTextColor(dc,f->colors.danger_text);SetBkColor(dc,f->colors.background);return reinterpret_cast<LRESULT>(f->background);}
        return reinterpret_cast<LRESULT>(ui::color_control(dc,child,f->colors,f->background,f->surface));
      }
      case WM_ERASEBKGND:{
        auto dc=reinterpret_cast<HDC>(wp);
        RECT area{};GetClientRect(w,&area);FillRect(dc,&area,f->background);
        const auto p=[&](int n){return ui::dip(n,f->dpi);};
        const int gap=p(12),line=ui::type_size(18,f->dpi)+p(7),button=ui::control_height(f->dpi);
        const int footer_top=std::max(p(80),static_cast<int>(area.bottom)-(button+line*2+gap*3));
        RECT div_rc{0,footer_top,area.right,footer_top+1};
        auto div_br=CreateSolidBrush(f->colors.border);FillRect(dc,&div_rc,div_br);DeleteObject(div_br);
        return 1;
      }
      case WM_COMMAND:
        if(f->validation&&(HIWORD(wp)==EN_CHANGE||HIWORD(wp)==CBN_SELCHANGE))SetWindowTextW(f->validation,L"");
        if((HIWORD(wp)==EN_SETFOCUS||HIWORD(wp)==CBN_SETFOCUS)&&lp&&GetParent(reinterpret_cast<HWND>(lp))==f->viewport){
            RECT child{},view{};GetWindowRect(reinterpret_cast<HWND>(lp),&child);MapWindowPoints(nullptr,f->viewport,reinterpret_cast<POINT*>(&child),2);GetClientRect(f->viewport,&view);
            if(child.top<0)form_scroll(*f,f->scroll+child.top-ui::dip(35,f->dpi));else if(child.bottom>view.bottom)form_scroll(*f,f->scroll+child.bottom-view.bottom+ui::dip(12,f->dpi));
        }
        if(LOWORD(wp)>=300&&LOWORD(wp)<300+f->edits.size()){
            const auto index=LOWORD(wp)-300;
            const bool folder=f->browse_folders||static_cast<int>(index)==f->single_folder;
            try{
                auto chosen=open_file(w,folder?L"Seleccione una carpeta":L"Seleccione un archivo",folder);
                if(chosen){
                    auto current=text(f->edits[index]);const auto path=chosen->wstring();
                    if(f->browse_fields.contains(index)||(index==0&&!f->append_first)||index==f->single_folder)current=path;
                    else{
                        const auto paths=lines(current);
                        if(std::find(paths.begin(),paths.end(),utf8(path))==paths.end()){if(!current.empty()&&current.back()!=L'\n')current+=L"\r\n";current+=path;}
                    }
                    SetWindowTextW(f->edits[index],current.c_str());
                }
            }catch(const std::exception& e){SetWindowTextW(f->validation,wide(e.what()).c_str());}
            return 0;
        }
        if(LOWORD(wp)==IDOK){
            for(size_t i=0;i<f->edits.size();++i)f->values[i]=text(f->edits[i]);
            if(f->inventory_mode)f->source_only=SendMessageW(f->inventory_mode,CB_GETCURSEL,0,0)==1;
            if(f->duplicate_mode)f->duplicate_choice=static_cast<int>(SendMessageW(f->duplicate_mode,CB_GETCURSEL,0,0));
            if(f->completion_mode)f->completion_choice=static_cast<int>(SendMessageW(f->completion_mode,CB_GETCURSEL,0,0));
            if(f->disposition_mode)f->disposition_choice=static_cast<int>(SendMessageW(f->disposition_mode,CB_GETCURSEL,0,0));
            if(f->expanded_code_mode)f->expanded_code=static_cast<int>(SendMessageW(f->expanded_code_mode,CB_GETCURSEL,0,0));
            if(f->analyze_cpp_mode)f->analyze_cpp_choice=static_cast<int>(SendMessageW(f->analyze_cpp_mode,CB_GETCURSEL,0,0));
            if(f->nested_zip_mode)f->nested_zip_choice=static_cast<int>(SendMessageW(f->nested_zip_mode,CB_GETCURSEL,0,0));
            if(f->reuse_kind_mode)f->reuse_kind=static_cast<int>(SendMessageW(f->reuse_kind_mode,CB_GETCURSEL,0,0));
            if(f->reuse_verify_mode)f->reuse_verify=static_cast<int>(SendMessageW(f->reuse_verify_mode,CB_GETCURSEL,0,0));
            for(auto& ec:f->extra_combos)if(ec.control)ec.selected=static_cast<int>(SendMessageW(ec.control,CB_GETCURSEL,0,0));
            if(f->validate){auto error=f->validate(*f);if(!error.empty()){SetWindowTextW(f->validation,error.c_str());NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,f->validation,OBJID_CLIENT,CHILDID_SELF);return 0;}}
            f->accepted=true;f->done=true;return 0;
        }
        if(LOWORD(wp)==IDCANCEL){f->done=true;return 0;}break;
      case WM_CLOSE:f->done=true;return 0;
      case WM_DESTROY:if(f->dialog_font){DeleteObject(f->dialog_font);f->dialog_font=nullptr;}if(f->background){DeleteObject(f->background);f->background=nullptr;}if(f->surface){DeleteObject(f->surface);f->surface=nullptr;}return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
bool show_form(Form& f) {
    static const bool registered=[](){WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=form_body_proc;cls.lpszClassName=L"MigratoolFormBody";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);return RegisterClassW(&cls)!=0;}();(void)registered;
    RECT parent{},work{};GetWindowRect(f.parent,&parent);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(f.parent,MONITOR_DEFAULTTONEAREST),&monitor);work=monitor.rcWork;
    const auto dpi=GetDpiForWindow(f.parent);const int width=std::min(ui::dip(f.width,dpi),static_cast<int>(work.right-work.left)-ui::dip(32,dpi)),height=std::min(ui::dip(f.height,dpi),static_cast<int>(work.bottom-work.top)-ui::dip(32,dpi));
    const auto x=std::clamp(static_cast<int>(parent.left)+(static_cast<int>(parent.right-parent.left)-width)/2,static_cast<int>(work.left),static_cast<int>(work.right)-width),y=std::clamp(static_cast<int>(parent.top)+24,static_cast<int>(work.top),static_cast<int>(work.bottom)-height);
    f.window=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,L"MigratoolForm",f.title.c_str(),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_CLIPCHILDREN,x,y,width,height,f.parent,nullptr,GetModuleHandleW(nullptr),&f);
    if(!f.window)throw std::runtime_error("No se pudo abrir el formulario");
    EnableWindow(f.parent,FALSE);ShowWindow(f.window,SW_SHOW);
    RedrawWindow(f.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    MSG msg{};
    while(!f.done&&GetMessageW(&msg,nullptr,0,0)>0){
        if(msg.message==WM_KEYDOWN&&msg.wParam==VK_RETURN&&(GetKeyState(VK_CONTROL)&0x8000)){SendMessageW(f.window,WM_COMMAND,IDOK,0);continue;}
        if(!IsDialogMessageW(f.window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    DestroyWindow(f.window);EnableWindow(f.parent,TRUE);SetActiveWindow(f.parent);return f.accepted;
}
std::optional<fs::path> open_file(HWND parent,const wchar_t* title,bool folder) {
    IFileOpenDialog* dlg{};if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dlg))))throw std::runtime_error("Selector de archivos no disponible");
    DWORD options{};dlg->GetOptions(&options);dlg->SetOptions(options|(folder?FOS_PICKFOLDERS:0)|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);std::optional<fs::path> result;
    if(SUCCEEDED(dlg->Show(parent))) {IShellItem* item{};if(SUCCEEDED(dlg->GetResult(&item))){PWSTR path{};if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){result=fs::path(path);CoTaskMemFree(path);}item->Release();}}
    dlg->Release();return result;
}
std::optional<std::pair<fs::path,std::string>> export_destination(HWND parent){
    IFileSaveDialog* dialog{};
    if(FAILED(CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("No se pudo abrir el selector de destino.");
    struct Release{IFileSaveDialog* dialog;~Release(){dialog->Release();}} release{dialog};
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions((options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST)&~FOS_OVERWRITEPROMPT);
    dialog->SetTitle(L"Exportar inventario de archivos");
    dialog->SetOkButtonLabel(L"Exportar");
    const COMDLG_FILTERSPEC formats[]={{L"JSON por línea (*.jsonl)",L"*.jsonl"},{L"Documento JSON (*.json)",L"*.json"}};
    dialog->SetFileTypes(2,formats);
    dialog->SetFileTypeIndex(1);
    dialog->SetFileName(L"inventario");
    dialog->SetDefaultExtension(L"jsonl");
    const auto shown=dialog->Show(parent);
    if(shown==HRESULT_FROM_WIN32(ERROR_CANCELLED))return std::nullopt;
    if(FAILED(shown))throw std::runtime_error("No se pudo seleccionar el destino de la exportación.");
    IShellItem* item{};
    if(FAILED(dialog->GetResult(&item)))throw std::runtime_error("Falta seleccionar el archivo de destino.");
    PWSTR filename{};
    const auto resolved=item->GetDisplayName(SIGDN_FILESYSPATH,&filename);
    item->Release();
    if(FAILED(resolved))throw std::runtime_error("No se pudo obtener la ruta completa del destino.");
    const fs::path path(filename);
    CoTaskMemFree(filename);
    UINT kind=1;
    dialog->GetFileTypeIndex(&kind);
    return std::pair{path,kind==2?std::string("json"):std::string("jsonl")};
}
std::vector<std::string> lines(const std::wstring& value) {
    std::wistringstream input(value);std::wstring line;std::vector<std::string> out;
    while(std::getline(input,line)){if(!line.empty()&&line.back()==L'\r')line.pop_back();if(!line.empty())out.push_back(utf8(line));}return out;
}
std::wstring validate_directories(const Form& form,size_t count){
    for(size_t i=0;i<count;++i){
        const auto paths=lines(form.values[i]);
        if(paths.empty())return i==0?L"Añada una carpeta de origen.":i==1?L"Añada una carpeta de destino.":L"Elija dónde guardar el proyecto.";
        if(i>=2&&paths.size()!=1)return L"Elija una sola carpeta donde guardar el proyecto.";
        for(const auto& path:paths){std::error_code error;const auto p=fs::path(wide(path));if(!p.is_absolute()||!fs::is_directory(native_path(p),error))return L"No se puede abrir esta carpeta: "+wide(path);}
    }
    return {};
}
Json form_roots(const Form& form,const Project* previous){
    Json roots=Json::array();std::set<std::string> ids;
    if(previous)for(const auto& old:previous->config.at("roots"))ids.insert(old.at("id"));
    for(int i=0;i<2;++i){
        const std::string role=i==0?"source":"target";
        int index=0;
        std::vector<fs::path> included;
        for(const auto& path:lines(form.values[i])){
            const auto absolute=fs::path(wide(path)).lexically_normal();
            if(std::any_of(included.begin(),included.end(),[&](const auto& existing){return same_path(absolute,existing);}))continue;
            included.push_back(absolute);
            Json root;
            if(previous)for(const auto& old:previous->config.at("roots"))if(old.at("role")==role&&same_path(previous->resolve(old.at("path")),absolute)){root=old;break;}
            if(root.is_null()){
                std::string id;
                do{id=role+std::to_string(++index);}while(ids.contains(id));
                ids.insert(id);root={{"id",id},{"role",role},{"include_all_code",true}};
            }
            root["path"]=path_text(absolute);roots.push_back(std::move(root));
        }
    }
    return roots;
}
struct OpenProjectResult {std::optional<Project> project;Json scan,summary;int64_t revision=-1;bool durable=false;std::string error;std::unique_ptr<LiveSession> session;};
struct ProjectPoll {
    uint64_t epoch=0;
    Json registry,activity,scan,summary,export_jobs=Json::array();
    std::optional<Project> updated_project;
    int64_t revision=-1;
    std::string jobs_signature,projects_signature,registry_signature;
    bool durable=false,update_catalog=false,active_jobs=false;
    std::string error;
};
struct ReuseResult {fs::path project;Json request,result;};
bool save_window_png(HWND hwnd,const fs::path& output){
    if(!hwnd||!IsWindow(hwnd))return false;
    RECT rc{};
    if(!GetWindowRect(hwnd,&rc))return false;
    const int width=rc.right-rc.left,height=rc.bottom-rc.top;
    if(width<=0||height<=0)return false;
    HDC screen=GetDC(nullptr);
    if(!screen)return false;
    HDC mem=CreateCompatibleDC(screen);
    if(!mem){ReleaseDC(nullptr,screen);return false;}
    BITMAPINFO bi{};
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=width;
    bi.bmiHeader.biHeight=-height;
    bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    HBITMAP bmp=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    ReleaseDC(nullptr,screen);
    if(!bmp||!bits){if(bmp)DeleteObject(bmp);DeleteDC(mem);return false;}
    HGDIOBJ prev=SelectObject(mem,bmp);
    BOOL printed=PrintWindow(hwnd,mem,PW_RENDERFULLCONTENT);
    if(!printed)printed=PrintWindow(hwnd,mem,0);
    GdiFlush();
    SelectObject(mem,prev);
    DeleteDC(mem);
    if(!printed){DeleteObject(bmp);return false;}
    std::error_code ec;
    if(!output.parent_path().empty())fs::create_directories(native_path(output.parent_path()),ec);
    IWICImagingFactory* factory=nullptr;
    IWICBitmap* wic_bmp=nullptr;
    IWICStream* stream=nullptr;
    IWICBitmapEncoder* encoder=nullptr;
    IWICBitmapFrameEncode* frame=nullptr;
    bool ok=false;
    if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))&&factory){
        if(SUCCEEDED(factory->CreateBitmapFromHBITMAP(bmp,nullptr,WICBitmapIgnoreAlpha,&wic_bmp))&&wic_bmp){
            if(SUCCEEDED(factory->CreateStream(&stream))&&stream&&SUCCEEDED(stream->InitializeFromFilename(native_path(output).wstring().c_str(),GENERIC_WRITE))){
                if(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&encoder&&SUCCEEDED(encoder->Initialize(stream,WICBitmapEncoderNoCache))){
                    if(SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&frame&&SUCCEEDED(frame->Initialize(nullptr))){
                        WICPixelFormatGUID fmt=GUID_WICPixelFormat24bppBGR;
                        if(SUCCEEDED(frame->SetSize(static_cast<UINT>(width),static_cast<UINT>(height)))&&SUCCEEDED(frame->SetPixelFormat(&fmt))){
                            if(SUCCEEDED(frame->WriteSource(wic_bmp,nullptr))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit()))ok=true;
                        }
                    }
                }
            }
        }
    }
    if(frame)frame->Release();
    if(encoder)encoder->Release();
    if(stream)stream->Release();
    if(wic_bmp)wic_bmp->Release();
    if(factory)factory->Release();
    DeleteObject(bmp);
    return ok;
}
std::vector<std::string> split_tokens(const std::wstring& value){
    std::vector<std::string> out;
    std::string raw=utf8(value),cur;
    auto push=[&](){
        size_t s=0,e=cur.size();
        while(s<e&&std::isspace(static_cast<unsigned char>(cur[s])))++s;
        while(e>s&&std::isspace(static_cast<unsigned char>(cur[e-1])))--e;
        if(e>s)out.push_back(cur.substr(s,e-s));
        cur.clear();
    };
    for(char ch:raw){
        if(ch==','||ch==';'||ch=='\r'||ch=='\n')push();
        else cur.push_back(ch);
    }
    push();
    return out;
}
Json run_agent_batch_with_retry(const fs::path& project_file,const std::string& actor,const Json& ops){
    for(int attempt=0;attempt<3;++attempt){
        auto fresh=Project::load(project_file);
        int64_t rev=0;
        {Database db(fresh);rev=db.revision();}
        try{
            return agent_batch(fresh,{{"actor",actor},{"expected_revision",rev},{"operations",ops}});
        }catch(const std::exception& e){
            const std::string msg=e.what();
            if(attempt<2&&(msg.find("REVISION_CONFLICT")!=std::string::npos||msg.find("ENTITY_REVISION_CONFLICT")!=std::string::npos))continue;
            throw;
        }
    }
    throw std::runtime_error("REVISION_CONFLICT al reintentar operación en lote");
}
struct App {
    HWND window{},list{},filter{},logbox{},statusbar{},subtitle{},welcome{},project_selector{},section_selector{},section_compact{},project_menu{},log_toggle{};std::vector<HWND> buttons,nav,section_nav,card_values,card_labels;std::vector<int> section_pages;bool logs_visible=false,sidebar_hidden=false,compact=false,cards_visible=false;
    HBRUSH background_brush{},surface_brush{};
    int hovered=-1,content_left=252,content_width=900,title_y=24,title_width=600,title_height=40,cards_y=145,cards_height=88,cards_columns=4;
    std::optional<Project> project;Json scan_data,summary;std::vector<std::string> selected_ids;
    std::jthread worker,poll_worker;std::mutex poll_mutex;std::condition_variable_any poll_cv;std::optional<std::function<void(std::stop_token)>> poll_pending;std::unique_ptr<IndexWatcher> watcher;Cancel cancel;bool busy=false,closing=false,nav_hidden_once=false;int page=0,dpi=96;
    ComparisonView comparison;
    ScaleView scalable_view;
    bool durable=false;
    Json registry={{"projects",Json::array()},{"sessions",Json::array()}};
    std::string registry_signature,projects_signature;
    bool loading=false,refreshing=false,polling=false;
    uint64_t project_epoch=0;
    std::vector<fs::path> project_paths;
    std::unique_ptr<LiveSession> gui_session;
    int64_t last_activity_id=0;
    std::string history_entity,displayed_jobs_signature;int64_t displayed_revision=-1;bool jobs_were_active=false;
    int selector_group=-1,requested_page=-1;
    std::array<int,5> last_views{0,1,16,4,20};fs::path navigation_project;
    Json reuse_request=Json::object();fs::path reuse_project;
    std::wstring status_text=L"Abra un proyecto o cree una auditoría para comenzar.";
    std::array<std::wstring,4> last_card_texts{};
    std::vector<HWND> last_keyboard_order;
    std::wstring last_cue_banner;
    int last_combo_height=-1;
    bool prefs_dirty=false;
    std::jthread prefs_worker;
    std::map<std::string,std::string> last_export_states;
    std::vector<int> capture_pages;
    size_t capture_index=0;
    fs::path capture_frame_path,capture_dir_path;
    bool headless_mode=false,capture_active=false,expect_project=false;
    ULONGLONG capture_settled_since=0,capture_wait_started=0;
    int px(int x)const{return MulDiv(x,dpi,96);}
    int type_px(int x)const{return ui::type_size(x,dpi);}
    fs::path preferences_path()const{wchar_t value[32768]{};auto length=GetEnvironmentVariableW(L"MIGRATOOL_REGISTRY_PATH",value,32768);if(length&&length<32768)return fs::path(value).parent_path()/L"ui-preferences.json";length=GetEnvironmentVariableW(L"LOCALAPPDATA",value,32768);return (length&&length<32768?fs::path(value)/L"Migratool":executable_dir())/L"ui-preferences.json";}
    void read_preferences(){try{auto path=preferences_path();if(fs::exists(native_path(path))){auto p=read_json(path);ui::text_percent=std::clamp(p.value("text_percent",100),100,200);sidebar_hidden=p.value("sidebar_hidden",false);ui::g_theme_mode=std::clamp(p.value("theme_mode",ui::g_theme_mode),0,1);ui::g_increased_contrast=p.value("increased_contrast",ui::g_increased_contrast);}}catch(...){}}
    void flush_preferences_sync(){try{auto path=preferences_path();fs::create_directories(native_path(path.parent_path()));write_json(path,{{"text_percent",ui::text_percent},{"sidebar_hidden",sidebar_hidden},{"theme_mode",ui::g_theme_mode},{"increased_contrast",ui::g_increased_contrast}});}catch(const std::exception& e){log("No se pudo guardar la presentación: "+std::string(e.what()));}}
    void save_preferences(){prefs_dirty=true;if(window)SetTimer(window,4,250,nullptr);else flush_preferences_sync();}
    void apply_appearance(bool relayout=true){
        if(window)SendMessageW(window,WM_SETREDRAW,FALSE,0);
        if(scalable_view.window())SendMessageW(scalable_view.window(),WM_SETREDRAW,FALSE,0);
        palette=ui::system_palette();if(background_brush)DeleteObject(background_brush);if(surface_brush)DeleteObject(surface_brush);background_brush=CreateSolidBrush(BG);surface_brush=CreateSolidBrush(palette.surface);
        auto make=[&](int size,int weight){return CreateFontW(-type_px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");};
        const std::array<HFONT,4> old={body,heading,small_font,cardfont};body=make(18,FW_NORMAL);heading=make(28,FW_SEMIBOLD);small_font=make(14,FW_NORMAL);cardfont=make(30,FW_SEMIBOLD);
        last_card_texts={};
        ui::font_tree(window,body);for(auto child:{project_selector,statusbar,logbox,log_toggle})if(child)font(child,small_font);
        for(auto label:card_labels)font(label,small_font);
        for(size_t i=0;i<card_values.size();++i){const auto cur=text(card_values[i]);font(card_values[i],cur==L"Por calcular"||cur==L"Sin casos"?body:cardfont);}
        if(list)ui::list_colors(list,palette);if(scalable_view.window())scalable_view.appearance(body,dpi);if(comparison.window())comparison.appearance(body,dpi);
        if(project_selector)SendMessageW(project_selector,CB_SETDROPPEDWIDTH,px(900),0);
        for(auto previous:old)if(previous)DeleteObject(previous);
        if(relayout){
            RECT before_sv{};
            if(scalable_view.window())GetWindowRect(scalable_view.window(),&before_sv);
            layout();
            if(scalable_view.window()&&IsWindowVisible(scalable_view.window())){
                RECT after_sv{};GetWindowRect(scalable_view.window(),&after_sv);
                if(EqualRect(&before_sv,&after_sv)){
                    RECT cr{};GetClientRect(scalable_view.window(),&cr);
                    SendMessageW(scalable_view.window(),WM_SIZE,SIZE_RESTORED,MAKELPARAM(cr.right,cr.bottom));
                }
            }
        }
        if(scalable_view.window())SendMessageW(scalable_view.window(),WM_SETREDRAW,TRUE,0);
        if(window&&relayout){SendMessageW(window,WM_SETREDRAW,TRUE,0);InvalidateRect(window,nullptr,FALSE);if(scalable_view.window()&&IsWindowVisible(scalable_view.window()))InvalidateRect(scalable_view.window(),nullptr,FALSE);}
    }
    void zoom(int change){const auto next=change==0?100:std::clamp(ui::text_percent+change,100,200);if(next==ui::text_percent)return;ui::text_percent=next;apply_appearance();save_preferences();}
    void menus(){
        auto bar=CreateMenu(),project_items=CreatePopupMenu(),view=CreatePopupMenu(),navigate=CreatePopupMenu(),help=CreatePopupMenu();
        AppendMenuW(project_items,MF_STRING,ID_NEW,L"&Crear proyecto…\tCtrl+N");AppendMenuW(project_items,MF_STRING,ID_OPEN,L"&Abrir proyecto…\tCtrl+O");AppendMenuW(project_items,MF_STRING,1014,L"Proyectos &recientes");AppendMenuW(project_items,MF_SEPARATOR,0,nullptr);
        for(auto item:std::vector<std::pair<int,const wchar_t*>>{{ID_CONFIG,L"Carpetas y filtros…\tCtrl+,"},{ID_CODE_SETTINGS,L"Tipos de código y analizadores…"},{ID_RESOURCES,L"Recursos…"},{ID_VERIFY_CONFIG,L"Compilación y pruebas…"},{ID_CONFIG_ADVANCED,L"Configuración JSON…"},{ID_SCAN,L"Escanear proyecto"},{ID_MARK,L"Registrar avance…"},{ID_NOTE,L"Añadir nota…"},{ID_PHASE_UPSERT,L"Registrar fase…"},{ID_TASK_UPSERT,L"Registrar tarea…"},{ID_TASK_TRANSITION,L"Cambiar estado de tarea…"},{ID_TASK_DELETE,L"Eliminar tarea seleccionada"},{ID_DOC_AUDIT,L"Auditar documento…"},{ID_BUILD,L"Compilar"},{ID_TEST,L"Ejecutar pruebas"},{ID_COVERAGE,L"Importar cobertura…"},{ID_IMPORT_WORK,L"Importar trabajo existente…"},{ID_REVALIDATE_WORK,L"Revalidar resultado seleccionado"},{ID_CONTINUE_IMPORT,L"Continuar importación"},{ID_REPORT,L"Generar informe"},{ID_EXPORT_INVENTORY,L"Exportar inventario…"},{ID_HOUSEKEEP,L"Autoaseo del catálogo (Housekeep)…"},{ID_TOPOLOGY_AUDIT,L"Topología y seguridad del workspace…"}})AppendMenuW(project_items,MF_STRING,item.first,item.second);
        AppendMenuW(project_items,MF_SEPARATOR,0,nullptr);AppendMenuW(project_items,MF_STRING,ID_EXIT,L"&Cerrar\tAlt+F4");
        AppendMenuW(view,MF_STRING,ID_FOCUS_SEARCH,L"&Buscar en esta vista\tCtrl+F");AppendMenuW(view,MF_STRING,ID_RELOAD_VIEW,L"&Actualizar vista\tF5");AppendMenuW(view,MF_SEPARATOR,0,nullptr);
        AppendMenuW(view,MF_STRING,ID_ZOOM_IN,L"&Aumentar texto\tCtrl++");AppendMenuW(view,MF_STRING,ID_ZOOM_OUT,L"&Reducir texto\tCtrl+-");AppendMenuW(view,MF_STRING,ID_ZOOM_RESET,L"&Tamaño original\tCtrl+0");AppendMenuW(view,MF_SEPARATOR,0,nullptr);
        AppendMenuW(view,MF_STRING,ID_INCREASE_CONTRAST,L"&Contraste reforzado (Bordes nítidos)\tCtrl+Alt+C");AppendMenuW(view,MF_STRING,ID_TOGGLE_THEME,L"Alternar &tema claro / oscuro\tCtrl+Alt+T");AppendMenuW(view,MF_SEPARATOR,0,nullptr);
        AppendMenuW(view,MF_STRING,ID_SIDEBAR,L"Barra &lateral\tCtrl+B");AppendMenuW(view,MF_STRING,ID_LOG_TOGGLE,L"Detalles &técnicos\tCtrl+Mayús+D");
        for(const auto& group:navigation_groups()){auto submenu=CreatePopupMenu();for(auto view_page:group.pages)AppendMenuW(submenu,MF_STRING,1000+view_page,pages[view_page]);AppendMenuW(navigate,MF_POPUP,reinterpret_cast<UINT_PTR>(submenu),group.title);}
        AppendMenuW(help,MF_STRING,ID_HELP,L"&Cómo usar esta pantalla\tF1");AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(project_items),L"&Proyecto");AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(view),L"&Ver");AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(navigate),L"&Ir a");AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(help),L"A&yuda");SetMenu(window,bar);
    }
    void update_navigation(){const auto group=navigation_group(page);last_views[group]=page;if(section_selector&&selector_group!=group){selector_group=group;section_pages=navigation_groups()[group].pages;SendMessageW(section_selector,CB_RESETCONTENT,0,0);for(const auto index:section_pages)SendMessageW(section_selector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(pages[index]));}
        for(size_t i=0;i<section_pages.size();++i)if(section_pages[i]==page)SendMessageW(section_selector,CB_SETCURSEL,i,0);for(auto button:section_nav)InvalidateRect(button,nullptr,FALSE);
        if(section_compact)SendMessageW(section_compact,CB_SETCURSEL,group,0);
        if(window){
            auto title=std::wstring(project?pages[page]:page==0?L"Empiece una migración":pages[page]);
            if(project)title+=L" · "+wide(project->config.value("name","Proyecto"));
            title+=L" · Migratool";
            if(text(window)!=title)SetWindowTextW(window,title.c_str());
        }
    }
    void show_section(int group){if(group<0||group>=static_cast<int>(navigation_groups().size()))return;last_views[navigation_group(page)]=page;page=last_views[group];if(loading)requested_page=page;if(GetWindowTextLengthW(filter)>0)SetWindowTextW(filter,L"");refresh_view();layout();InvalidateRect(window,nullptr,FALSE);}
    void log(const std::string& message) {
        auto s=wide(message+"\r\n");auto len=GetWindowTextLengthW(logbox);
        if(len>200000)SetWindowTextW(logbox,L"");
        SendMessageW(logbox,EM_SETSEL,GetWindowTextLengthW(logbox),GetWindowTextLengthW(logbox));SendMessageW(logbox,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(s.c_str()));
        status_text=message.size()>20&&message[4]=='-'&&message[7]=='-'?L"Actividad compartida actualizada. Consulte Actividad para ver los detalles.":wide(message);SetWindowTextW(statusbar,status_text.c_str());
    }
    void set_busy(bool b) {
        busy=b;for(auto w:buttons)EnableWindow(w,(!b)||GetDlgCtrlID(w)==ID_CANCEL);
        EnableWindow(GetDlgItem(window,ID_CANCEL),b);
        if(project_menu)EnableWindow(project_menu,!b);if(project_selector)EnableWindow(project_selector,!b);
        update_reuse_actions();
    }
    void update_reuse_actions(){
        EnableWindow(GetDlgItem(window,ID_REVALIDATE_WORK),project&&!busy&&page==18&&!selected().empty());
        EnableWindow(GetDlgItem(window,ID_CONTINUE_IMPORT),!busy&&can_continue_import());
    }
    void start(std::function<void(Project&,Log,Cancel*)> fn,const std::string& operation="Operación GUI") {
        if(busy||!project)return;++project_epoch;watcher.reset();project=Project::load(project->file);set_busy(true);cancel.requested=false;
        auto copy=*project;auto hwnd=window;
        worker=std::jthread([this,copy,fn,hwnd,operation]() mutable {
            std::string error;
            try{
                LiveSession session(copy,"GUI",operation,"GUI");
                auto notify=[hwnd,&session](const std::string& s){session.log(s);auto msg=std::make_unique<std::string>(s);if(PostMessageW(hwnd,WM_LOG,0,reinterpret_cast<LPARAM>(msg.get())))msg.release();};
                fn(copy,notify,&cancel);session.finish(cancel.requested?1:0);
            }catch(const std::exception& e){error=e.what();}
            auto done_msg=std::make_unique<std::string>(error);
            if(PostMessageW(hwnd,WM_DONE,error.empty()?0:1,reinterpret_cast<LPARAM>(done_msg.get())))done_msg.release();
        });
    }
    bool can_continue_import()const {
        return project && same_path(project->file,reuse_project) && reuse_request.contains("cursor");
    }
    void import_work(Json request) {
        const auto target=window;
        start([target,request](Project& current,Log notify,Cancel* cancellation){
            auto outcome=std::make_unique<ReuseResult>();outcome->project=current.file;outcome->request=request;
            outcome->result=scale_import(current,request,cancellation);
            const auto& result=outcome->result;
            const auto count=result.value("revalidated",result.value("imported",size_t(0)));
            notify(std::to_string(count)+" registros procesados · "+std::to_string(result.value("input_output_pairs_verified",0))+" pares con hashes vigentes.");
            if(result.contains("next_cursor")&&result["next_cursor"].is_string())
                notify("Quedan más registros. Use Continuar importación para procesar el siguiente lote.");
            if(PostMessageW(target,WM_REUSE_RESULT,0,reinterpret_cast<LPARAM>(outcome.get())))outcome.release();
        },request.value("action","import")=="revalidate"?"Revalidar trabajo existente":"Importar trabajo existente");
        layout();
    }
    void finish_reuse(ReuseResult& outcome) {
        if(!project||!same_path(project->file,outcome.project))return;
        if(outcome.request.value("action","import")!="revalidate"){
            reuse_project=outcome.project;reuse_request=std::move(outcome.request);
            const auto cursor=outcome.result.value("next_cursor",Json());
            if(cursor.is_string())reuse_request["cursor"]=cursor;else reuse_request.erase("cursor");
        }
        page=18;refresh_view();layout();
    }
    void load(const fs::path& path,bool discovered=false,int destination_view=-1) {
        if(loading||busy)return;++project_epoch;loading=true;requested_page=destination_view;set_busy(true);cancel.requested=false;watcher.reset();history_entity.clear();displayed_revision=-1;
        if(gui_session){gui_session->finish(0);gui_session.reset();}project.reset();scan_data=Json();summary=Json();log("Abriendo el proyecto. Puede seguir usando la ventana mientras se prepara el catálogo.");
        const auto hwnd=window;worker=std::jthread([this,path,discovered,hwnd](){
            auto result=std::make_unique<OpenProjectResult>();
            try{auto p=Project::load(path);result->session=std::make_unique<LiveSession>(p,"GUI","Proyecto abierto","GUI");
                if(fs::exists(native_path(p.state_dir()/L"catalog.sqlite"))){Database db(p);result->revision=db.revision();result->durable=scale_enabled(p,db);if(!result->durable){result->scan=index_snapshot(p,false);result->summary=mt::status(p,false);}}
                if(cancel.requested)throw std::runtime_error("Apertura cancelada");result->project=std::move(p);
            }catch(const std::exception& e){result->error=e.what();}
            if(PostMessageW(hwnd,WM_OPEN_PROJECT,discovered?1:0,reinterpret_cast<LPARAM>(result.get())))result.release();
        });InvalidateRect(window,nullptr,TRUE);
    }
    void finish_open(OpenProjectResult& result,bool discovered){
        loading=false;set_busy(false);if(closing){if(IsWindow(window))DestroyWindow(window);return;}
        if(!result.error.empty()){log("No se pudo abrir el proyecto: "+result.error);return;}
        project=std::move(result.project);scan_data=std::move(result.scan);summary=std::move(result.summary);durable=result.durable;displayed_revision=result.revision;gui_session=std::move(result.session);last_activity_id=0;
        for(size_t i=0;i<project_paths.size();++i)if(same_path(project->file,project_paths[i])){SendMessageW(project_selector,CB_SETCURSEL,i,0);break;}
        if(!same_path(navigation_project,project->file))last_views={0,1,16,4,20};navigation_project=project->file;
        page=requested_page>=0?requested_page:discovered?1:0;requested_page=-1;if(capture_active&&!capture_pages.empty())page=capture_pages[capture_index];if(!capture_active)watcher=std::make_unique<IndexWatcher>(*project,900000,Log{},true);
        log("Proyecto abierto: "+project->config.value("name",""));refresh_view();layout();InvalidateRect(window,nullptr,TRUE);poll_live();
    }
    void poll_live() {
        if(polling||loading||closing)return;
        polling=true;
        const auto target=window;
        const auto current_project=project;
        const auto epoch=project_epoch;
        const auto previous_revision=displayed_revision;
        const bool jobs_visible=page==16||page==17;
        const auto after=last_activity_id;
        const bool read_catalog=!busy;
        if(!poll_worker.joinable()){
            poll_worker=std::jthread([this](std::stop_token stop){
                while(!stop.stop_requested()){
                    std::function<void(std::stop_token)> fn;
                    {std::unique_lock lk(poll_mutex);poll_cv.wait(lk,stop,[&]{return poll_pending.has_value();});if(stop.stop_requested())break;fn=std::move(*poll_pending);poll_pending.reset();}
                    if(fn&&!stop.stop_requested())fn(stop);
                }
            });
        }
        {
            std::lock_guard lk(poll_mutex);
            poll_pending=[target,current_project,epoch,previous_revision,after,read_catalog,jobs_visible](std::stop_token stop){
                if(stop.stop_requested())return;
                auto result=std::make_unique<ProjectPoll>();result->epoch=epoch;
                try{
                    result->registry=registered_projects();
                    if(stop.stop_requested())return;
                    result->registry_signature=result->registry.dump();
                    result->projects_signature=result->registry.value("projects",Json::array()).dump();
                    if(current_project){
                        result->activity=project_activity_delta(current_project->file,after);
                        if(stop.stop_requested())return;
                        if(read_catalog){
                            auto current=Project::load(current_project->file);
                            if(stop.stop_requested())return;
                            Database db(current);
                            if(stop.stop_requested())return;
                            result->revision=db.revision();
                            const bool config_changed=current.revision!=current_project->revision;
                            result->update_catalog=config_changed||result->revision!=previous_revision;
                            result->active_jobs=jobs_visible&&db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE state IN('queued','running'))")!=0;
                            if(jobs_visible){
                                for(const auto& j:db.query("SELECT id,state,phase,processed,files,dirs,failed FROM inventory_jobs ORDER BY created DESC LIMIT 15")){
                                    result->jobs_signature+=j["id"].get<std::string>()+":"+j["state"].get<std::string>()+":"+j["phase"].get<std::string>()+":"+j["processed"].dump()+":"+j["files"].dump()+":"+j["dirs"].dump()+":"+j["failed"].dump()+";";
                                }
                            }
                            result->export_jobs=db.query("SELECT id,state,phase,files,error,scope FROM inventory_jobs WHERE generation=0 AND json_extract(scope,'$.kind')='export_inventory' ORDER BY created DESC LIMIT 10");
                            if(result->update_catalog&&!stop.stop_requested()){
                                result->durable=scale_enabled(current,db);
                                if(!result->durable){result->scan=index_snapshot(current,false);result->summary=mt::status(current,false);}
                            }
                            if(config_changed)result->updated_project=std::move(current);
                        }
                    }
                }catch(const std::exception& e){result->error=e.what();}
                if(!stop.stop_requested()&&PostMessageW(target,WM_PROJECT_POLL,0,reinterpret_cast<LPARAM>(result.get())))result.release();
            };
        }
        poll_cv.notify_one();
    }
    void finish_poll(ProjectPoll& update){
        polling=false;
        if(closing)return;
        if(update.epoch!=project_epoch||loading){if(!loading&&!closing)poll_live();return;}
        if(!update.error.empty())log("No se pudo actualizar la vista. Se reintentará: "+update.error);
        if(!update.registry.is_object())return;
        const bool changed=update.registry_signature!=registry_signature;
        const bool projects_changed=update.projects_signature!=projects_signature||project_paths.size()!=update.registry.value("projects",Json::array()).size();
        registry=std::move(update.registry);
        registry_signature=std::move(update.registry_signature);
        if(projects_changed&&SendMessageW(project_selector,CB_GETDROPPEDSTATE,0,0)==FALSE){
            SendMessageW(project_selector,CB_RESETCONTENT,0,0);project_paths.clear();
            for(const auto& item:registry["projects"]){
                const auto label=wide(item["name"].get<std::string>());
                SendMessageW(project_selector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
                project_paths.emplace_back(wide(item["path"]));
                if(project&&same_path(project->file,project_paths.back()))
                    SendMessageW(project_selector,CB_SETCURSEL,project_paths.size()-1,0);
            }
            projects_signature=std::move(update.projects_signature);
        }
        if(!project){
            for(const auto& item:registry["projects"])if(item["available"]==true){load(fs::path(wide(item["path"])),true);break;}
        }
        if(project&&update.activity.is_array()&&!update.activity.empty()){
            std::string batch;
            for(const auto& item:update.activity){
                const auto id=item["id"].get<int64_t>();
                if(id<=last_activity_id)continue;
                if(!batch.empty())batch+="\r\n";
                batch+=item["at"].get<std::string>()+" · "+item["actor"].get<std::string>()+" · "+
                    item["operation"].get<std::string>()+" · "+item["state"].get<std::string>()+" · "+item["message"].get<std::string>();
                last_activity_id=id;
            }
            if(!batch.empty())log(batch);
        }
        const bool refresh_jobs=update.active_jobs||jobs_were_active||(!update.jobs_signature.empty()&&update.jobs_signature!=displayed_jobs_signature);
        if(page==16||page==17)displayed_jobs_signature=update.jobs_signature;
        jobs_were_active=update.active_jobs;
        if(project&&update.update_catalog&&!busy&&update.error.empty()){
            const bool proj_cfg_updated=update.updated_project.has_value();
            const bool was_durable=durable;
            if(update.updated_project)project=std::move(update.updated_project);
            displayed_revision=update.revision;durable=update.durable;
            if(!durable){scan_data=std::move(update.scan);summary=std::move(update.summary);}
            const bool scaled_now=(durable||page>=16)&&page!=13&&page!=14&&page!=20;
            if(scaled_now&&was_durable&&!proj_cfg_updated){
                if(page!=29)scalable_view.refresh();
            }else{
                refresh_view();
            }
        }else if(refresh_jobs&&(page==16||page==17)&&!busy)scalable_view.refresh();
        if(project&&update.export_jobs.is_array()){
            for(const auto& ej:update.export_jobs){
                const auto jid=ej.value("id",std::string());
                const auto jstate=ej.value("state",std::string());
                if(jid.empty())continue;
                auto it=last_export_states.find(jid);
                if(it!=last_export_states.end()&&it->second!=jstate){
                    if(jstate=="completed"){
                        std::string out_p;
                        try{auto sc=Json::parse(ej.value("scope","{}"));if(sc.is_object())out_p=sc.value("out","");}catch(...){}
                        log("Exportación de inventario completada ("+jid+")"+(out_p.empty()?"":": "+out_p));
                    }else if(jstate=="failed"){
                        log("Exportación de inventario fallida ("+jid+"): "+(ej.contains("error")&&ej["error"].is_string()?ej["error"].get<std::string>():"error desconocido"));
                    }else if(jstate=="running"){
                        log("Exportación de inventario en curso ("+jid+")…");
                    }
                }
                last_export_states[jid]=jstate;
            }
        }
        if(!refresh_jobs&&changed&&(page==13||page==14))refresh_view();
    }
    void columns(const std::vector<std::pair<std::wstring,int>>& defs) {
        ListView_DeleteAllItems(list);while(ListView_DeleteColumn(list,0)){}
        int index=0;for(const auto& [title,width]:defs){LVCOLUMNW c{};c.mask=LVCF_TEXT|LVCF_WIDTH;c.cx=px(width);c.pszText=const_cast<wchar_t*>(title.c_str());ListView_InsertColumn(list,index++,&c);}selected_ids.clear();
    }
    void row(const std::vector<std::string>& cells,const std::string& id="") {
        auto q=wide(utf8(text(filter)));std::wstring hay;
        for(const auto& c:cells)hay+=wide(c)+L" ";for(auto& c:hay)c=towlower(c);for(auto& c:q)c=towlower(c);
        if(!q.empty()&&hay.find(q)==std::wstring::npos)return;
        auto index=ListView_GetItemCount(list);auto first=wide(cells.empty()?"":cells[0]);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=index;item.pszText=first.data();ListView_InsertItem(list,&item);
        for(size_t i=1;i<cells.size();++i){auto cell=wide(cells[i]);ListView_SetItemText(list,index,static_cast<int>(i),cell.data());}selected_ids.push_back(id);
    }
    std::string selected()const{if((durable||page>=16)&&page!=13&&page!=14&&page!=20)return scalable_view.selected();if(page==1)return comparison.selected();int n=ListView_GetNextItem(list,-1,LVNI_SELECTED);return n>=0&&static_cast<size_t>(n)<selected_ids.size()?selected_ids[n]:"";}
    void refresh_view() {
        if(refreshing)return;ViewGuard guard(refreshing);
        update_cards();
        update_navigation();
        if(!project&&page==0){ui::show_if(scalable_view.window(),false);ui::show_if(comparison.window(),false);ui::show_if(list,false);SetWindowTextW(subtitle,loading?L"Preparando el catálogo en segundo plano…":L"Empiece creando un proyecto o abra uno que ya estén usando sus agentes.");return;}
        if(page==20){ui::show_if(scalable_view.window(),false);ui::show_if(comparison.window(),false);ui::show_if(list,true);SetWindowTextW(subtitle,L"Elija qué carpetas contienen el código original y dónde se guarda el resultado.");columns({{L"Uso",140},{L"Carpeta",250},{L"Ruta completa",780}});if(project){for(const auto& root:project->config["roots"]){auto path=project->resolve(root["path"]);row({root["role"]=="source"?"Origen":"Destino",path_text(path.filename()),path_text(path)});}row({"Inventario",project->config.value("inventory_mode","universal")=="source_only"?"Solo código fuente":"Todos los archivos","El censo conserva qué se encontró y qué quedó pendiente."});}return;}
        const bool scaled=project&&(durable||page>=16)&&page!=13&&page!=14&&page!=20;
        ui::show_if(scalable_view.window(),scaled);
        if(scaled){ui::show_if(list,false);ui::show_if(comparison.window(),false);SetWindowTextW(subtitle,page_subtitle(page));scalable_view.show(*project,page,utf8(text(filter)));return;}
        const wchar_t* hints[]={L"Estado de evidencia · los conteos no equivalen a paridad funcional",L"Rutas exactas y avance ponderado por unidades declaradas",L"Declaraciones extraídas por analizadores; filtre por nombre o ruta",L"Declaraciones del agente y evidencia de los casos configurados",L"Entradas, salidas, errores y efectos observables",L"Informes importados; los valores desconocidos permanecen sin verificar",L"Historial inmutable · seleccione un archivo antes de entrar para filtrarlo",L"Todos los archivos conocidos, incluidos recursos, documentos y generados",L"Archivos nuevos, huérfanos y eliminados; el índice conserva su identidad",L"Estado Git independiente del inventario y pertenencia a la compilación",L"SQLite FTS5 · busca documentación, rutas, símbolos, notas y relaciones",L"Dependencias inventariadas; resolución por nombre único cuando es posible",L"Procesos MCP observados y volumen medido de consultas compactas"};
        SetWindowTextW(subtitle,page==13?L"Operaciones CLI, MCP y GUI · mensajes y estados actualizados automáticamente":
            page==14?L"Proyectos usados por agentes · doble clic para abrir el catálogo compartido":
            page==15?L"Copias de código con el mismo tamaño y SHA-256 · doble clic para ver la ruta completa":page<13?hints[page]:page_subtitle(page));
        ui::show_if(list,page!=1);
        ui::show_if(comparison.window(),page==1);
        if(page==14){
            columns({{L"Proyecto",300},{L"Sesiones activas",130},{L"Última operación / agente",300},{L"Ruta completa del proyecto",650},{L"Última apertura UTC",210}});
            if(registry.value("projects",Json::array()).empty())row({"No hay proyectos recientes","","Cree uno nuevo o abra un proyecto existente.","",""});
            for(const auto& item:registry.value("projects",Json::array()))
                row({item["name"],item["active_sessions"].dump(),item["operation"].get<std::string>()+" · "+item["actor"].get<std::string>(),item["path"],item["last_opened"]},item["path"]);
            return;
        }
        if(!project){columns({{L"Comenzar",900}});row({"Cree un proyecto con sus carpetas de origen y destino, o abra un archivo de proyecto JSON."});return;}
        if(page==0) {
            columns({{L"Raíz / requisito",230},{L"Rol / estado",170},{L"Ruta absoluta / detalle",730}});
            for(const auto& r:project->config["roots"])row({r["id"],r["role"],path_text(project->resolve(r["path"]))});
            row({"Ámbito del inventario",project->config.value("inventory_mode","universal")=="source_only"?"SOLO CÓDIGO FUENTE":"UNIVERSAL","Se puede cambiar en Configuración."});
            if(summary.is_object()) {
                row({"Paridad funcional global","SIN DETERMINAR","Ningún porcentaje se deduce de archivos, símbolos o nombres coincidentes."});
                row({"Unidades con casos aprobados",percent(summary["units_with_passing_cases_percent"]),summary["units_with_passing_cases"].dump()+" / "+summary["total_units"].dump()+" unidades con compilación y casos vinculados vigentes."});
                row({"Declarados completos",summary["declared_complete_files"].dump(),"Declaraciones completas de archivo; las marcas por símbolo se muestran como progreso parcial."});
                row({"Vigencia del inventario",summary["freshness"].value("checked",false)?(summary["freshness"]["fresh"]==true?"VIGENTE":"DESACTUALIZADO"):"SIN COMPROBAR","Use Actualizar para contrastar hashes con los archivos actuales."});
                if(summary.contains("non_source_files"))row({"Archivos no fuente",summary["non_source_files"].dump(),"Archivos excluidos de métricas de código fuente; conserve disposición y trazabilidad."});
                if(summary.contains("development_control")&&summary["development_control"].is_object()){
                    const auto& dc=summary["development_control"];
                    row({"Fases del proyecto",std::to_string(dc.value("phases_total",0)),"Completadas: "+std::to_string(dc.value("phases_completed",0))});
                    row({"Tareas del plan",std::to_string(dc.value("tasks_total",0)),"Completadas: "+std::to_string(dc.value("tasks_completed",0))+" · A medias: "+std::to_string(dc.value("tasks_half_done",0))+" · En curso: "+std::to_string(dc.value("tasks_in_progress",0))});
                    row({"Auditoría documental",std::to_string(dc.value("documentation_files",0)),"Revisados: "+std::to_string(dc.value("documentation_reviewed",0))+" · Obsoletos: "+std::to_string(dc.value("documentation_stale",0))});
                }
                for(const auto& limit:summary["limits"])row({"Alcance de evidencia","",limit});
                for(const auto& d:summary["diagnostics"])row({"Diagnóstico","",d.dump()});
            }else row({"Inventario","PENDIENTE","Pulse Analizar para inventariar archivos y extraer símbolos."});
        } else if(page==1) {
            comparison.update(scan_data,summary,utf8(text(filter)));
        } else if(page==2) {
            columns({{L"Símbolo / nombre",360},{L"Tipo",190},{L"Raíz",110},{L"Archivo absoluto",560},{L"Línea",70},{L"Identificador",550}});
            if(scan_data.is_object())for(const auto& s:scan_data["symbols"])row({s.value("qualified_name",s["name"].get<std::string>()),s["kind"],s["root"],s["absolute_path"],s["line"].dump(),s["id"]},s["id"]);
        } else if(page==3) {
            columns({{L"Origen absoluto",490},{L"Destino absoluto",490},{L"Estado",240},{L"Autor / fecha UTC",240},{L"Nota",350}});
            if(summary.is_object())for(const auto& m:summary["mappings"])row({m["source_path"],m["target_paths"].dump(),m["state"],m.value("actor","")+" "+m["at"].get<std::string>(),m.value("note","")},m["source"]);
        } else if(page==4) {
            columns({{L"Caso",200},{L"Estado",120},{L"Diferencias / motivo",400},{L"Comprobante absoluto",650}});
            if(summary.is_object())for(const auto& t:summary["cases"])row({t["id"],t["state"],t.value("differences",Json::array()).dump()+" "+t.value("reason",""),t.value("receipt_path","")},t["id"]);
        } else if(page==5) {
            columns({{L"Archivo absoluto",550},{L"Líneas cubiertas / total",180},{L"Ramas cubiertas / total",180},{L"Vigencia",150}});
            if(summary.is_object())for(const auto& c:summary["coverage"])for(const auto& f:c["files"]){auto metric=[&](const char* k){return f[k].is_null()?"sin datos":f[k]["covered"].dump()+" / "+f[k]["count"].dump();};row({f["path"],metric("lines"),metric("branches"),c["fresh"]==true?"VIGENTE":"SIN VERIFICAR"});}
        } else if(page==6) {
            columns({{L"Fecha y hora UTC",220},{L"Autor",150},{L"Acción",220},{L"Detalle",1400}});
            Database db(*project);auto records=history_entity.empty()?db.query("SELECT at,actor,action,entity,detail FROM history ORDER BY id DESC LIMIT 2000"):db.query("SELECT at,actor,action,entity,detail FROM history WHERE entity=? ORDER BY id DESC LIMIT 2000",{history_entity});
            for(const auto& e:records)row({e["at"],e["actor"],e["action"],(e["entity"].is_string()?e["entity"].get<std::string>():std::string())+" "+(e["detail"].is_string()?e["detail"].get<std::string>():e["detail"].dump())});
        } else if(page>=7&&page<=9){
            columns({{L"ID",70},{L"Ruta absoluta",660},{L"Clasificación",170},{L"Lenguaje",120},{L"Git",100},{L"Compilación",240},{L"SHA-256",520}});
            Database db(*project);std::string where=page==8?" WHERE f.deleted=1 OR f.first_revision=f.last_revision OR (f.relevant=1 AND NOT EXISTS(SELECT 1 FROM mappings m WHERE m.source='F'||f.id))":page==9?" WHERE f.deleted=0 AND(f.git_state IN('!!','??')OR f.build_state='NOT_IN_COMPILATION_DATABASE')":" WHERE f.deleted=0";
            const auto* order=page==8?" ORDER BY f.first_revision DESC,f.id DESC LIMIT 10000":" ORDER BY f.id LIMIT 10000";
            for(const auto& f:db.query("SELECT f.id,f.absolute_path,f.category,f.language,f.git_state,f.build_state,f.sha256,f.deleted,f.first_revision,f.last_revision,COALESCE(json_extract(f.data,'$.filtered_by_inventory_mode'),0) AS filtered FROM files f"+where+order)){
                auto state=f["category"].get<std::string>();
                if(f["deleted"]==1)state=f["filtered"]==1?"Fuera del filtro":"Eliminado";
                else if(page==8&&f["first_revision"]==f["last_revision"])state="Nuevo · "+state;
                row({"F"+f["id"].dump(),f["absolute_path"],state,f["language"],f["git_state"],f["build_state"],f["sha256"].is_null()?"":f["sha256"].get<std::string>()},"F"+f["id"].dump());
            }
        } else if(page==10){
            columns({{L"Tipo",100},{L"ID",80},{L"Coincidencia",1500}});auto query=utf8(text(filter));
            if(!query.empty()){auto result=agent_request(*project,"query",{{"kind","search"},{"text",query},{"limit",100},{"token_budget",100000}});for(const auto& r:result.value("items",Json::array()))row({r["entity_type"],r["entity_id"],r["excerpt"]},r["entity_id"]);}
        } else if(page==11){
            columns({{L"Archivo ID",90},{L"Referencia",390},{L"Tipo",190},{L"Destino ID",100},{L"Ruta absoluta",700}});Database db(*project);
            for(const auto& d:db.query("SELECT 'F'||d.file_id AS source,'F'||d.target_id AS target,d.name,d.kind,f.absolute_path FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0 ORDER BY d.id LIMIT 10000"))row({d["source"],d["name"],d["kind"],d["target"].is_null()?"sin resolver":d["target"].get<std::string>(),d["absolute_path"]},d["source"]);
        } else if(page==12){
            columns({{L"Conexión / herramienta",260},{L"Estado / invocaciones",200},{L"Bytes de respuesta",180},{L"Tiempo medio µs / detalle",720}});
            auto connections=agent_request(*project,"connections",{{"limit",100},{"token_budget",100000}});
            for(const auto& c:connections.value("items",Json::array()))row({c["client"],c["live"]==true?"ACTIVA":"CERRADA", "",c["opened"].get<std::string>()+" · PID "+c["pid"].dump()});
            auto stats=agent_request(*project,"statistics",{{"token_budget",100000}});for(const auto& s:stats.value("queries",Json::array()))row({s["tool"],s["invocations"].dump(),s["response_bytes"].dump(),s["mean_us"].dump()});
            Database db(*project);for(const auto& w:db.query("SELECT root,pid,opened,closed,error FROM watchers ORDER BY opened DESC LIMIT 15"))row({"Watcher "+w["root"].get<std::string>(),w["closed"].is_null()?"ABIERTO":"CERRADO","",w["error"].is_null()?"":w["error"].get<std::string>()});
        } else if(page==13){
            columns({{L"Estado",145},{L"Agente / usuario",220},{L"Interfaz",90},{L"Operación",200},{L"Último mensaje",550},{L"Actualización UTC",215},{L"PID",90}});
            for(const auto& session:registry.value("sessions",Json::array())){
                if(!same_path(fs::path(wide(session["project_path"])),project->file))continue;
                row({session["live"]==true?"EN EJECUCIÓN":session["state"].get<std::string>(),session["actor"],session["interface"],session["operation"],session["message"],session["updated"],session["pid"].dump()},session["id"]);
            }
        } else if(page==15){
            columns({{L"Copias / estado",130},{L"Tipo / rol",120},{L"Ruta absoluta",400},
                     {L"Entrada dentro del ZIP",260},{L"Bytes",100},{L"SHA-256",500}});
            Database db(*project);
            const auto report=duplicate_code_report(*project,db);
            if(report.at("mode")=="off")row({"Desactivado","","Active Duplicados exactos en Configuración."});
            else if(!report.at("available").get<bool>())row({"Pendiente","","Pulse Analizar para calcular las coincidencias."});
            else{
                const auto state=!report.at("fresh").get<bool>()?"Revisión pendiente":
                    report.at("complete").get<bool>()?"Revisión completa":"Revisión parcial";
                const auto error_count=report.at("errors").size();
                row({state,"",report.at("group_count").dump()+" grupos · "+report.at("archive_count").dump()+
                    " ZIP · "+std::to_string(error_count)+(error_count==1?" error":" errores")});
                for(const auto& group:report.at("groups"))for(const auto& copy:group.at("copies")){
                    const auto path=copy.at("absolute_path").get<std::string>();
                    const auto entry=copy.value("entry_path","");
                    const auto hash=group.at("sha256").get<std::string>();
                    const auto kind=copy.at("kind")=="zip"?"ZIP":"Archivo";
                    const auto role=copy.at("role")=="source"?"Origen":"Destino";
                    auto detail=path;
                    if(!entry.empty())detail+="\r\nEntrada "+copy.at("entry_index").dump()+" · "+entry;
                    row({group.at("copy_count").dump(),kind+std::string(" / ")+role,path,
                         entry,group.at("bytes").dump(),hash},detail+"\r\nSHA-256 "+hash);
                }
                for(const auto& issue:report.at("errors"))
                    row({"Error ZIP","ZIP",issue.at("absolute_path"),issue.value("entry_path",""),"",issue.at("error")});
            }
        }
        InvalidateRect(window,nullptr,FALSE);
    }
    void update_cards(){
        const char* keys[]={"source_files","migration_percent","units_with_passing_cases_percent","analysis_pending"};
        const wchar_t* labels[]={L"Archivos de origen",L"Migración registrada",L"Con pruebas vigentes",L"Análisis pendientes"};
        for(size_t i=0;i<card_values.size();++i){
            auto value=summary.is_object()?summary.value(keys[i],Json()):Json();
            auto formatted=value.is_null()?L"Por calcular":i==1||i==2?wide(percent(value)):ui::number(wide(value.dump()),0);
            if(i==2&&summary.is_object()&&summary.value("configured_cases",-1)==0)formatted=L"Sin casos";
            if(last_card_texts[i]==formatted)continue;
            last_card_texts[i]=formatted;
            SetWindowTextW(card_values[i],formatted.c_str());ui::name(card_values[i],std::wstring(labels[i])+L": "+formatted);
            NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,card_values[i],OBJID_CLIENT,CHILDID_SELF);
            font(card_values[i],formatted==L"Por calcular"||formatted==L"Sin casos"?body:cardfont);
        }
    }
    void layout() {
        if(!window||!filter)return;RECT rect{};GetClientRect(window,&rect);const int width=rect.right,height=rect.bottom;
        const bool condensed=ui::text_percent>=150||(ui::text_percent>125&&height<px(850));
        const int gap=condensed?px(6):px(12),margin=condensed?px(16):px(24),control=condensed?std::max(px(32),type_px(18)+px(8)):ui::control_height(dpi),label_height=type_px(18)+px(8);
        compact=sidebar_hidden||ui::text_percent>=150||width<px(1100)||((ui::text_percent>125)&&width<px(1600));
        const int sidebar=compact?0:std::max(px(224),type_px(18)*13),left=sidebar+margin,right=margin,content=std::max(px(240),width-left-right);
        content_left=left;content_width=content;title_y=condensed?px(8):px(20);title_height=type_px(28)+px(6);
        if(!nav_hidden_once){for(auto item:nav)ui::show_if(item,false);nav_hidden_once=true;}
        const int nav_top=px(112),nav_height=std::max(px(44),label_height+px(14)),nav_gap=px(6);
        for(size_t i=0;i<section_nav.size();++i){ui::show_if(section_nav[i],!compact);if(!compact)ui::move_if(section_nav[i],px(16),nav_top+static_cast<int>(i)*(nav_height+nav_gap),sidebar-px(32),nav_height);}
        int y=title_y+title_height+gap;
        const bool show_project=project.has_value()&&!condensed;
        const int project_width=std::min(content/3,std::max(px(210),type_px(14)*18));
        title_width=content-(show_project?project_width+gap:0);
        ui::show_if(project_selector,show_project);ui::show_if(project_menu,false);
        if(show_project){ui::move_if(project_selector,width-right-project_width,title_y+px(4),project_width,control+px(320));const int want_h=control-px(8);if(last_combo_height!=want_h){last_combo_height=want_h;SendMessageW(project_selector,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),want_h);}}
        ui::show_if(section_compact,compact);
        const bool section_visible=(project&&page!=0)||page==14;
        const bool has_search=navigation_search(page)&&project.has_value();
        const bool combined=section_visible&&has_search&&!compact&&content>=type_px(18)*38;
        ui::show_if(section_selector,section_visible);
        if(compact){int group_width=section_visible?std::min(content/3,std::max(px(150),type_px(18)*8)):content;
            ui::move_if(section_compact,left,y,group_width,control+px(320));
            if(section_visible)ui::move_if(section_selector,left+group_width+gap,y,content-group_width-gap,control+px(420));y+=control+gap;
        }else if(section_visible){const int section_width=combined?std::min(content/2,std::max(px(240),type_px(18)*15)):content;ui::move_if(section_selector,left,y,section_width,control+px(420));if(combined)ui::move_if(filter,left+section_width+gap,y,content-section_width-gap,control);y+=control+gap;}
        const int hint_height=label_height*(content<type_px(18)*55?2:1)+px(2);
        ui::show_if(subtitle,!condensed);if(!condensed){ui::move_if(subtitle,left,y,content,hint_height);y+=hint_height+gap;}
        int filter_row_y=y;
        ui::show_if(filter,has_search);if(has_search&&!combined){ui::move_if(filter,left,y,content,control);y+=control+gap;}
        const wchar_t* cue=page==1?L"Buscar un archivo de origen o destino…":page==17?L"Buscar en esta carpeta…":L"Buscar en esta vista…";
        if(last_cue_banner!=cue){last_cue_banner=cue;SendMessageW(filter,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(cue));}
        cards_columns=content>=type_px(18)*60?4:2;cards_y=y;cards_height=type_px(30)+type_px(14)*2+px(32);
        const int card_rows=4/cards_columns;
        cards_visible=page==0&&project&&y+card_rows*(cards_height+gap)+control+px(160)<height;
        const int card_width=(content-(cards_columns-1)*gap)/cards_columns;
        for(size_t i=0;i<card_values.size();++i){for(auto item:{card_values[i],card_labels[i]})ui::show_if(item,cards_visible);
            if(cards_visible){const int x=left+static_cast<int>(i%cards_columns)*(card_width+gap),top=y+static_cast<int>(i/cards_columns)*(cards_height+gap);
            ui::move_if(card_values[i],x+px(16),top+px(14),card_width-px(32),type_px(30)+px(8));
            ui::move_if(card_labels[i],x+px(16),top+px(14)+type_px(30)+px(8),card_width-px(32),type_px(14)*2+px(4));}
        }
        if(cards_visible)y+=card_rows*(cards_height+gap);
        ui::show_if(welcome,!project&&page==0);
        std::vector<int> actions;
        if(!project)actions={ID_NEW,ID_OPEN};
        else switch(page){case 0:actions={ID_SCAN,ID_NEXT_STEP,ID_CONFIG};break;case 1:case 8:actions={ID_MARK,ID_NOTE};break;case 2:actions={ID_NOTE};break;case 3:actions={ID_MARK};break;
            case 4:actions={ID_BUILD,ID_TEST,ID_VERIFY_CONFIG};break;case 5:actions={ID_COVERAGE};break;case 7:actions={ID_SCAN,ID_EXPORT_INVENTORY};break;case 9:actions={ID_BUILD};break;case 12:actions={ID_CONFIG_ADVANCED};break;case 14:actions={ID_NEW,ID_OPEN};break;case 16:actions={ID_EXPORT_INVENTORY};break;
            case 15:actions={ID_CONFIG};break;case 18:actions={ID_IMPORT_WORK,ID_REVALIDATE_WORK};if(can_continue_import())actions.push_back(ID_CONTINUE_IMPORT);break;case 20:actions={ID_CONFIG,ID_CODE_SETTINGS,ID_VERIFY_CONFIG,ID_RESOURCES,ID_CONFIG_ADVANCED};break;case 23:actions={ID_MODULE_FILTER_FILES,ID_MARK};break;case 24:actions={ID_PHASE_UPSERT,ID_TASK_UPSERT,ID_TASK_TRANSITION,ID_DOC_AUDIT,ID_TASK_DELETE};break;default:break;}
        if(busy)actions.push_back(ID_CANCEL);
        if(condensed&&actions.size()>1&&!busy)actions.resize(1);
        for(auto button:buttons){const int bid=GetDlgCtrlID(button);if(std::find(actions.begin(),actions.end(),bid)==actions.end())ui::show_if(button,false);}
        int x=left,action_y=y;bool first=true;
        HDC measure_dc=GetDC(window);
        HGDIOBJ prev_font=measure_dc?SelectObject(measure_dc,body):nullptr;
        const bool inline_condensed_action=condensed&&has_search&&!combined&&actions.size()==1&&content>=px(480);
        for(const auto id:actions){auto button=GetDlgItem(window,id);if(!button)continue;
            const wchar_t* title=id==ID_EXPORT_INVENTORY?L"Exportar inventario…":id==ID_NEW?L"Crear proyecto…":id==ID_OPEN?L"Abrir proyecto…":id==ID_CONFIG?(page==15?L"Configurar duplicados…":L"Carpetas y filtros…"):id==ID_SCAN?L"Escanear proyecto":id==ID_MARK?L"Registrar avance…":id==ID_NOTE?L"Añadir nota…":id==ID_TEST?L"Ejecutar pruebas":id==ID_BUILD?L"Compilar":id==ID_COVERAGE?L"Importar cobertura…":id==ID_NEXT_STEP?L"Ver pendientes":id==ID_CONFIG_ADVANCED?L"JSON avanzado…":id==ID_CODE_SETTINGS?L"Tipos de código y analizadores…":id==ID_VERIFY_CONFIG?L"Compilación y pruebas…":id==ID_RESOURCES?L"Recursos…":id==ID_IMPORT_WORK?L"Importar trabajo…":id==ID_REVALIDATE_WORK?L"Revalidar selección":id==ID_CONTINUE_IMPORT?L"Continuar importación":id==ID_PHASE_UPSERT?L"Nueva / editar fase…":id==ID_TASK_UPSERT?L"Nueva / editar tarea…":id==ID_TASK_TRANSITION?L"Cambiar estado…":id==ID_DOC_AUDIT?L"Auditar documento…":id==ID_MODULE_FILTER_FILES?L"Ver archivos":id==ID_TASK_DELETE?L"Eliminar tarea":L"Cancelar";
            if(text(button)!=title)SetWindowTextW(button,title);
            SIZE size{};if(measure_dc)GetTextExtentPoint32W(measure_dc,title,static_cast<int>(wcslen(title)),&size);
            const int item_width=std::min(content,std::max(px(105),static_cast<int>(size.cx)+px(28)));
            const DWORD want_style=(first&&id!=ID_CANCEL)?BS_DEFPUSHBUTTON:BS_PUSHBUTTON;
            if(first&&id!=ID_CANCEL)first=false;
            if((GetWindowLongPtrW(button,GWL_STYLE)&0x0F)!=want_style)SendMessageW(button,BM_SETSTYLE,want_style,FALSE);
            if(inline_condensed_action&&content>item_width+px(200)){
                const int filter_w=content-item_width-gap;
                ui::move_if(filter,left,filter_row_y,filter_w,control);
                ui::move_if(button,left+filter_w+gap,filter_row_y,item_width,control);
                ui::show_if(button,true);
                action_y=filter_row_y;
            }else{
                if(x>left&&x+item_width>width-right){x=left;action_y+=control+gap;}
                ui::move_if(button,x,action_y,item_width,control);ui::show_if(button,true);x+=item_width+gap;
            }
        }
        if(!actions.empty())y=action_y+control+gap;
        if(!project&&page==0)ui::move_if(welcome,left,y+gap,content,std::max(px(80),height-y-px(75)));
        const int footer=std::max(px(36),type_px(14)+px(16)),logs=logs_visible?std::min(px(140),std::max(px(50),(height-y)/3)):0;
        const int body_height=std::max(px(30),height-y-logs-footer-gap);
        for(auto view:{list,comparison.window(),scalable_view.window()})if(view)ui::move_if(view,left,y,content,body_height);
        ui::show_if(logbox,logs_visible);if(logs_visible)ui::move_if(logbox,left,height-footer-logs,content,logs);
        const wchar_t* lt_txt=logs_visible?L"Ocultar detalles":L"Detalles técnicos";if(text(log_toggle)!=lt_txt)SetWindowTextW(log_toggle,lt_txt);
        SIZE lt_size{};
        if(measure_dc){SelectObject(measure_dc,small_font);GetTextExtentPoint32W(measure_dc,lt_txt,static_cast<int>(wcslen(lt_txt)),&lt_size);SelectObject(measure_dc,prev_font);ReleaseDC(window,measure_dc);}
        const int details_width=std::min(content/3,std::max(px(145),static_cast<int>(lt_size.cx)+px(28)));
        ui::move_if(log_toggle,width-right-details_width,height-footer+px(2),details_width,footer-px(6));
        ui::move_if(statusbar,left,height-footer+px(6),std::max(px(50),content-details_width-gap),type_px(14)+px(8));
        std::vector<HWND> keyboard=section_nav;
        keyboard.insert(keyboard.end(),{project_selector,section_compact,section_selector,filter});
        for(const auto id:actions)keyboard.push_back(GetDlgItem(window,id));
        keyboard.insert(keyboard.end(),{list,comparison.window(),scalable_view.window(),log_toggle,logbox});
        if(keyboard!=last_keyboard_order){ui::tab_order(keyboard);last_keyboard_order=keyboard;}
        update_reuse_actions();
        InvalidateRect(window,nullptr,FALSE);
    }
    void paint() {
        PAINTSTRUCT ps{};auto dc=BeginPaint(window,&ps);RECT area{};GetClientRect(window,&area);FillRect(dc,&area,background_brush?background_brush:GetSysColorBrush(COLOR_WINDOW));SetBkMode(dc,TRANSPARENT);
        auto draw=[&](const std::wstring& value,RECT bounds,HFONT selected_font,COLORREF color,UINT flags=DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS){auto old=SelectObject(dc,selected_font);SetTextColor(dc,color);DrawTextW(dc,value.c_str(),-1,&bounds,flags);SelectObject(dc,old);};
        if(!compact){
            const int side_right=content_left-px(24);
            RECT sidebar{0,0,side_right,area.bottom};auto brush=CreateSolidBrush(NAV);FillRect(dc,&sidebar,brush);DeleteObject(brush);
            RECT side_border{side_right-1,0,side_right,area.bottom};
            auto border_br=CreateSolidBrush(palette.border);FillRect(dc,&side_border,border_br);DeleteObject(border_br);
            draw(L"Migratool",{px(22),px(22),sidebar.right-px(14),px(22)+type_px(28)+px(8)},heading,INK);
            draw(L"Auditoría y organización",{px(24),px(26)+type_px(28),sidebar.right-px(14),px(32)+type_px(28)+type_px(14)},small_font,MUTED);
            RECT side_div{px(18),px(98),side_right-px(18),px(99)};
            auto div_br=CreateSolidBrush(palette.divider);FillRect(dc,&side_div,div_br);DeleteObject(div_br);
        }
        draw(!project&&page==0?L"Empiece una migración":pages[page],{content_left,title_y,content_left+title_width,title_y+title_height},heading,INK);
        if(cards_visible){
            const int gap=px(12),card_width=(content_width-(cards_columns-1)*gap)/cards_columns;
            for(size_t i=0;i<card_values.size();++i){
                const int x=content_left+static_cast<int>(i%cards_columns)*(card_width+gap),top=cards_y+static_cast<int>(i/cards_columns)*(cards_height+gap);
                RECT card{x,top,x+card_width,top+cards_height};
                ui::draw_card(dc,card,palette,px(10),true,palette.accent);
            }
        }
        const int footer=std::max(px(36),type_px(14)+px(16));
        RECT footer_div{content_left,area.bottom-footer-px(3),area.right-px(24),area.bottom-footer-px(3)+1};
        auto fdiv_br=CreateSolidBrush(palette.divider);FillRect(dc,&footer_div,fdiv_br);DeleteObject(fdiv_br);
        EndPaint(window,&ps);
    }
    void action(int id) {
        if(id==ID_HELP){Form f;f.parent=window;f.title=std::wstring(L"Ayuda · ")+pages[page];f.width=900;f.height=750;f.read_only=true;f.labels={pages[page]};
            std::wstring help=L"Migratool conserva el control de una migración entre lenguajes.\r\n\r\n1. Proyecto: elija las carpetas del código original y del resultado. Los originales se conservan.\r\n2. Inicio: escanee el proyecto y revise qué falta. El escaneo puede continuar en segundo plano.\r\n3. Migración: compare los archivos de origen y destino. Seleccione un archivo y use Registrar avance para guardar sus destinos exactos y una nota.\r\n4. Actividad: consulte los trabajos y las operaciones de sus agentes; puede pausar o continuar los trabajos que lo permitan.\r\n5. Revisión: compruebe pruebas, duplicados, cobertura y evidencia.\r\n\r\nLos porcentajes distinguen el avance declarado y las pruebas vigentes. «Por calcular» significa que todavía falta conocer parte del alcance; no equivale a cero. Un archivo detectado no se considera migrado automáticamente. Las notas y los cambios guardan autor y fecha.\r\n\r\nPuede cambiar de sección con Ctrl+1 a Ctrl+5 o el menú Ir a. Cada sección tiene un selector de pantallas. Ctrl+F busca en la pantalla actual, F5 actualiza, Ctrl++ amplía el texto hasta 200 %, Ctrl+0 restablece el tamaño y Ctrl+B muestra u oculta la barra lateral.\r\n\r\nPara agentes: abra el mismo archivo de proyecto desde CLI o MCP. La GUI comparte el catálogo y recibe sus cambios. Consulte Proyecto > Integración con agentes para comprobar las conexiones.\r\n\r\nLas funciones avanzadas siguen disponibles en los menús Proyecto e Ir a. «Detalles técnicos» muestra los mensajes del motor cuando necesite investigarlos.";
            help+=L"\r\n\r\nEsta pantalla: ";help+=page_subtitle(page);f.values={help};show_form(f);return;}
        if(id==ID_EXIT){SendMessageW(window,WM_CLOSE,0,0);return;}
        if(id==ID_ZOOM_IN||id==ID_ZOOM_OUT||id==ID_ZOOM_RESET){zoom(id==ID_ZOOM_IN?25:id==ID_ZOOM_OUT?-25:0);return;}
        if(id==ID_INCREASE_CONTRAST){ui::g_increased_contrast=!ui::g_increased_contrast;apply_appearance();save_preferences();log(ui::g_increased_contrast?"Contraste reforzado y bordes nítidos activados (WCAG AAA).":"Contraste estándar restaurado.");return;}
        if(id==ID_TOGGLE_THEME){ui::g_theme_mode=(ui::g_theme_mode==0)?1:0;apply_appearance();save_preferences();log(ui::g_theme_mode==1?"Modo oscuro calibrado activado.":"Tema Porcelana Claro activado.");return;}
        if(id==ID_SIDEBAR){sidebar_hidden=!sidebar_hidden;save_preferences();layout();return;}
        if(id==ID_FOCUS_SEARCH){if(IsWindowVisible(filter))SetFocus(filter);else log("Esta pantalla no tiene búsqueda. Elija una lista desde el menú Ir a.");return;}
        if(id==ID_RELOAD_VIEW){if(!loading)refresh_view();return;}
        if(id==ID_LOG_TOGGLE){logs_visible=!logs_visible;layout();return;}
        if(id==ID_SECTION_COMPACT){auto index=SendMessageW(section_compact,CB_GETCURSEL,0,0);if(index>=0&&index<5)show_section(static_cast<int>(index));return;}
        if(id>=2000&&id<2005){show_section(id-2000);return;}
        if(id==ID_SECTION_VIEW){const auto index=SendMessageW(section_selector,CB_GETCURSEL,0,0);if(index>=0&&static_cast<size_t>(index)<section_pages.size()){page=section_pages[index];if(GetWindowTextLengthW(filter)>0)SetWindowTextW(filter,L"");refresh_view();layout();}return;}
        if(id==ID_PROJECT_MENU){auto menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,ID_NEW,L"Crear proyecto…");AppendMenuW(menu,MF_STRING,ID_OPEN,L"Abrir proyecto…");AppendMenuW(menu,MF_STRING,1014,L"Proyectos recientes");if(project){AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,2004,L"Carpetas y configuración");AppendMenuW(menu,MF_STRING,ID_REPORT,L"Generar informe…");}RECT bounds{};GetWindowRect(project_menu,&bounds);auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTALIGN,bounds.right,bounds.bottom,0,window,nullptr);DestroyMenu(menu);if(command)action(command);return;}
        if(id==ID_NEXT_STEP){page=8;if(GetWindowTextLengthW(filter)>0)SetWindowTextW(filter,L"");refresh_view();layout();return;}
        if(id>=1000&&id<1000+static_cast<int>(std::size(pages))){SendMessageW(window,WM_SETREDRAW,FALSE,0);if(scalable_view.window())SendMessageW(scalable_view.window(),WM_SETREDRAW,FALSE,0);if(id==1006)history_entity=selected();page=id-1000;if(loading)requested_page=page;if(GetWindowTextLengthW(filter)>0)SetWindowTextW(filter,L"");layout();refresh_view();if(scalable_view.window())SendMessageW(scalable_view.window(),WM_SETREDRAW,TRUE,0);SendMessageW(window,WM_SETREDRAW,TRUE,0);InvalidateRect(window,nullptr,FALSE);if(scalable_view.window()&&IsWindowVisible(scalable_view.window()))InvalidateRect(scalable_view.window(),nullptr,FALSE);return;}
        if(busy){if(id==ID_CANCEL){cancel.requested=true;log("Cancelación solicitada; esperando cierre de procesos propios.");}return;}
        if(id==ID_PROJECT){const auto index=SendMessageW(project_selector,CB_GETCURSEL,0,0);if(index>=0&&static_cast<size_t>(index)<project_paths.size()&&(!project||!same_path(project_paths[index],project->file))){load(project_paths[index]);layout();}return;}
        if(id==ID_OPEN){auto path=open_file(window,L"Abrir proyecto Migratool (.json)");if(path)load(*path);return;}
        if(id==ID_NEW){
            Form f;f.parent=window;f.title=L"Crear un proyecto de migración";f.width=900;f.height=670;f.browse_folders=true;f.single_folder=2;
            f.labels={L"Carpetas del código original",L"Carpetas del resultado",L"Carpeta donde guardar el proyecto",L"Nombre del proyecto"};f.values={L"",L"",L"",L"Nueva migración"};
            f.validate=[](const Form& form){auto problem=validate_directories(form,3);if(!problem.empty())return problem;if(form.values[3].find_first_not_of(L" \r\n\t")==std::wstring::npos)return std::wstring(L"Escriba un nombre para el proyecto.");if(fs::exists(native_path(fs::path(form.values[2])/L"migration.json")))return std::wstring(L"Esta carpeta ya contiene un proyecto. Elija otra carpeta o abra el existente.");return std::wstring();};
            if(show_form(f)){auto p=Project::create(fs::path(f.values[2])/L"migration.json",utf8(f.values[3]),form_roots(f,nullptr));load(p.file);}
            return;
        }
        if(!project)throw std::runtime_error("Abra o cree un proyecto primero");
        if(id==ID_EXPORT_INVENTORY){
            const auto destination=export_destination(window);
            if(!destination)return;
            const auto queued=scale_export(*project,{{"out",path_text(destination->first)},{"format",destination->second},{"wait",false},{"actor","GUI"}});
            last_export_states[queued.at("job_id").get<std::string>()]=queued.value("state","queued");
            page=16;
            SetWindowTextW(filter,L"");
            refresh_view();layout();
            log("Exportación en cola. Consulte Trabajos del proyecto. ID "+queued.at("job_id").get<std::string>());
            return;
        }
        if(id==ID_IMPORT_WORK){
            Form f;f.parent=window;f.title=L"Importar trabajo existente";f.width=900;f.height=780;
            f.confirm_label=L"Importar lote";f.reuse_kind=0;f.reuse_verify=0;
            f.explanation=L"Abra un registro de trabajos o un catálogo que ya exista. Migratool puede identificar su formato y comprobar los archivos por SHA-256. Conserva la procedencia y abre el registro externo en modo de solo lectura. Cada lote procesa hasta 1000 registros.";
            f.labels={L"Registro o catálogo existente",L"Carpeta dentro del catálogo (opcional)",L"Registros por lote (1 a 1000)",L"Autor"};
            f.values={L"",L"",L"100",L"GUI"};f.single_fields={0,1,2,3};f.browse_fields={0};
            f.validate=[](const Form& form){
                const fs::path path(form.values[0]);std::error_code error;
                if(!path.is_absolute()||!fs::is_regular_file(native_path(path),error))return std::wstring(L"Elija la ruta completa del registro o catálogo existente.");
                try{size_t used{};const auto count=std::stoi(form.values[2],&used);if(used!=form.values[2].size()||count<1||count>1000)throw std::out_of_range("lote");}
                catch(...){return std::wstring(L"El tamaño del lote debe estar entre 1 y 1000 registros.");}
                if(form.values[3].find_first_not_of(L" \r\n\t")==std::wstring::npos)return std::wstring(L"Escriba el autor de la importación.");
                if(form.reuse_kind==2&&!form.values[1].empty())return std::wstring(L"El filtro de carpeta se utiliza con catálogos de archivos. Déjelo vacío para un registro de recuperaciones.");
                return std::wstring();
            };
            if(show_form(f)){
                Json request={{"actor",utf8(f.values[3])},{"limit",std::stoi(f.values[2])},{"verify",f.reuse_verify==0}};
                request[f.reuse_kind==0?"external_path":f.reuse_kind==2?"registry_path":"catalog_path"]=path_text(fs::path(f.values[0]));
                auto prefix=utf8(f.values[1]);std::replace(prefix.begin(),prefix.end(),'\\','/');
                if(!prefix.empty()){if(prefix.back()!='/')prefix+='/';request["prefix"]=prefix;}
                import_work(std::move(request));
            }
            return;
        }
        if(id==ID_CONTINUE_IMPORT){
            if(can_continue_import())import_work(reuse_request);else log("No hay un lote pendiente de importación en este proyecto.");
            return;
        }
        if(id==ID_REVALIDATE_WORK){
            const auto selected_id=page==18?selected():std::string();
            if(selected_id.empty()){log("Seleccione un resultado en Revisión > Trabajo existente para comprobar de nuevo sus archivos.");return;}
            int64_t parsed_id=0;
            try{parsed_id=std::stoll(selected_id);}catch(const std::exception&){log("El identificador seleccionado no es válido para revalidación.");return;}
            import_work({{"action","revalidate"},{"ids",Json::array({parsed_id})},{"actor","GUI"}});
            return;
        }
        if(id==ID_CODE_SETTINGS||id==ID_RESOURCES){
            auto fresh=Project::load(project->file);Form f;f.parent=window;f.width=820;f.height=520;
            Json settings;
            if(id==ID_RESOURCES){
                auto resource=fresh.config.value("resources",Json::object());f.title=L"Recursos del proyecto";
                f.labels={L"Memoria en MiB (128 a 4096)",L"Tareas simultáneas (1 a 8)",L"Analizadores simultáneos (1 a 2)"};
                f.values={std::to_wstring(resource.value("memory_mib",4096)),std::to_wstring(resource.value("workers",8)),std::to_wstring(resource.value("analyzers",2))};
                f.validate=[&](const Form& form){
                    const int minima[]={128,1,1},maxima[]={4096,8,2};int values[3]{};
                    for(size_t i=0;i<3;++i){try{size_t used{};values[i]=std::stoi(form.values[i],&used);if(used!=form.values[i].size()||values[i]<minima[i]||values[i]>maxima[i])throw std::out_of_range("recursos");}catch(...){return L"Revise el valor de «"+form.labels[i]+L"».";}}
                    if(values[2]>values[1])return std::wstring(L"Los analizadores simultáneos deben caber dentro del número de tareas.");
                    settings={{"resources",{{"memory_mib",values[0]},{"workers",values[1]},{"analyzers",values[2]}}}};return std::wstring();
                };
            }else{
                f.title=L"Tipos de código y analizadores";f.width=940;f.height=740;f.expanded_code=0;f.analyze_cpp_choice=fresh.config.value("analyze_cpp",true)?1:0;
                const auto joined=[](const Json& values){std::wstring result;for(const auto& item:values){if(!result.empty())result+=L"\r\n";result+=wide(item);}return result;};
                auto trim=[](std::string s){while(!s.empty()&&std::isspace(static_cast<unsigned char>(s.front())))s.erase(s.begin());while(!s.empty()&&std::isspace(static_cast<unsigned char>(s.back())))s.pop_back();return s;};
                std::wstring def_lines,ana_lines;
                const auto defs=fresh.config.value("defines",Json::array());
                if(defs.is_array()){
                    for(const auto& d:defs){if(!d.is_string())continue;if(!def_lines.empty())def_lines+=L"\r\n";def_lines+=wide(d.get<std::string>());}
                }else if(defs.is_object()){
                    for(auto it=defs.begin();it!=defs.end();++it){if(!def_lines.empty())def_lines+=L"\r\n";def_lines+=wide(it.key()+"="+(it.value().is_string()?it.value().get<std::string>():it.value().dump()));}
                }
                const auto anas=fresh.config.value("analyzers",Json::array());
                if(anas.is_array()){
                    for(const auto& item:anas){if(!ana_lines.empty())ana_lines+=L"\r\n";ana_lines+=wide(item.dump());}
                }else if(anas.is_object()){
                    for(auto it=anas.begin();it!=anas.end();++it){if(!ana_lines.empty())ana_lines+=L"\r\n";ana_lines+=wide(it.key()+"="+it.value().dump());}
                }
                f.labels={L"Extensiones adicionales (una por línea, con punto)",L"Nombres de archivo adicionales (uno por línea)",L"Definiciones C/C++/C# (CLAVE o CLAVE=VALOR, una por línea)",L"Argumentos extra para Clang (uno por línea, ej. -std=c++20)",L"Analizadores externos (JSON por línea: {\"id\":\"...\",\"extensions\":[\".ext\"],\"argv\":[...]})"};
                f.values={joined(fresh.config.value("code_extensions",Json::array())),joined(fresh.config.value("code_filenames",Json::array())),def_lines,joined(fresh.config.value("clang_args",Json::array())),ana_lines};
                f.validate=[&](const Form& form){
                    const auto extensions=lines(form.values[0]),filenames=lines(form.values[1]),def_rows=lines(form.values[2]),cargs=lines(form.values[3]),ana_rows=lines(form.values[4]);
                    for(const auto& extension:extensions)if(extension.size()<2||extension[0]!='.'||extension.find_first_of("/\\ \t")!=std::string::npos)return L"Escriba una extensión con punto y sin espacios: "+wide(extension);
                    for(const auto& filename:filenames)if(filename.find_first_of("/\\")!=std::string::npos)return L"Escriba solo el nombre de archivo: "+wide(filename);
                    Json new_defs=Json::array();
                    for(const auto& row:def_rows){auto t=trim(row);if(t.empty())continue;if(t.front()=='=')return std::wstring(L"Definición inválida: especifique CLAVE o CLAVE=VALOR.");new_defs.push_back(t);}
                    Json new_anas=Json::array();
                    for(const auto& row:ana_rows){
                        auto raw_line=trim(row);if(raw_line.empty())continue;
                        if(raw_line.front()=='{'){
                            try{
                                auto item=Json::parse(raw_line);
                                if(!item.is_object()||item.value("id","").empty()||!item.value("extensions",Json::array()).is_array()||!item.value("argv",Json::array()).is_array()||item["argv"].empty())
                                    return std::wstring(L"Cada analizador JSON requiere id, extensions y argv no vacío.");
                                new_anas.push_back(std::move(item));
                            }catch(const std::exception& e){return L"JSON de analizador inválido: "+wide(e.what());}
                        }else{
                            auto p1=raw_line.find('|'),p2=p1==std::string::npos?std::string::npos:raw_line.find('|',p1+1);
                            if(p1==std::string::npos||p2==std::string::npos)return L"Use JSON o formato: id | .ext1,.ext2 | comando arg: "+wide(row);
                            auto aid=trim(raw_line.substr(0,p1)),exts_s=trim(raw_line.substr(p1+1,p2-p1-1)),cmd_s=trim(raw_line.substr(p2+1));
                            if(aid.empty()||exts_s.empty()||cmd_s.empty())return L"Falta id, extensiones o comando en: "+wide(row);
                            Json exts=Json::array(),argv=Json::array();
                            std::istringstream eiss(exts_s);std::string ext_tok;while(std::getline(eiss,ext_tok,',')){auto et=trim(ext_tok);if(!et.empty())exts.push_back(et);}
                            std::istringstream ciss(cmd_s);std::string arg_tok;while(ciss>>arg_tok)argv.push_back(arg_tok);
                            if(exts.empty()||argv.empty())return L"Especifique al menos una extensión y un comando en: "+wide(row);
                            new_anas.push_back({{"id",aid},{"extensions",exts},{"argv",argv}});
                        }
                    }
                    auto probe=fresh.config;probe["analyzers"]=new_anas;
                    try{validate(probe);}catch(const std::exception& e){return L"Analizadores inválidos: "+wide(e.what());}
                    settings={{"code_extensions",extensions},{"code_filenames",filenames},{"analyze_cpp",form.analyze_cpp_choice==1},{"defines",new_defs},{"clang_args",cargs},{"analyzers",new_anas}};
                    if(form.expanded_code>0){auto roots=fresh.config.at("roots");for(auto& root:roots)root["include_all_code"]=form.expanded_code==2;settings["roots"]=std::move(roots);}
                    return std::wstring();
                };
            }
            if(show_form(f)){scale_settings(fresh,{{"action","set"},{"actor","GUI"},{"expected_config_revision",fresh.revision},{"settings",settings}});load(fresh.file,false,page);}
            return;
        }
        if(id==ID_VERIFY_CONFIG){
            auto fresh=Project::load(project->file);
            auto split_pipe=[](const std::string& line){std::vector<std::string> out;size_t s=0;while(s<=line.size()){auto p=line.find('|',s);auto part=p==std::string::npos?line.substr(s):line.substr(s,p-s);while(!part.empty()&&std::isspace(static_cast<unsigned char>(part.front())))part.erase(part.begin());while(!part.empty()&&std::isspace(static_cast<unsigned char>(part.back())))part.pop_back();out.push_back(part);if(p==std::string::npos)break;s=p+1;}return out;};
            auto split_ws=[](const std::string& s){std::istringstream iss(s);Json a=Json::array();std::string w;while(iss>>w)a.push_back(w);return a;};
            auto fmt_lines=[](const Json& arr,bool is_build){
                bool simple=true;
                for(const auto& x:arr){
                    if(!x.is_object()){simple=false;break;}
                    if(!is_build){
                        const auto& l=x.contains("left")&&x["left"].is_object()?x["left"]:Json::object();
                        const auto& r=x.contains("right")&&x["right"].is_object()?x["right"]:Json::object();
                        const auto& t=x.contains("target")&&x["target"].is_object()?x["target"]:Json::object();
                        if(x.contains("fixtures")||x.contains("observers")||x.contains("expected_stdout")||x.contains("expected_stdout_contains")||x.contains("forbidden_stdout_contains")||x.contains("expected_exit")||x.contains("expected_stderr_contains")||x.contains("required_artifacts")||x.contains("cwd")||x.contains("timeout_ms")||x.contains("normalize_crlf")||x.contains("stdin")||l.contains("env")||l.contains("cwd")||r.contains("env")||r.contains("cwd")||t.contains("env")||t.contains("cwd")||x.contains("env"))simple=false;
                    }else{
                        if(x.contains("env")||x.contains("cwd")||x.contains("timeout_ms")||x.contains("expected_exit"))simple=false;
                    }
                }
                if(!simple){std::wstring w;for(auto ch:wide(arr.dump(2))){if(ch==L'\n')w+=L'\r';w+=ch;}return w;}
                std::wstring out;
                for(const auto& x:arr){
                    if(!out.empty())out+=L"\r\n";
                    if(is_build){
                        std::string cmd;for(const auto& a:x.value("argv",Json::array())){if(!cmd.empty())cmd+=' ';cmd+=a.get<std::string>();}
                        std::string arts;for(const auto& a:x.value("artifacts",Json::array())){if(!arts.empty())arts+=',';arts+=a.get<std::string>();}
                        out+=wide(x.value("id","")+" | "+x.value("root","")+" | "+cmd+(arts.empty()?"":" | "+arts));
                    }else if(x.value("kind","")=="target_test"||(!(x.contains("left")&&x["left"].is_object())&&!(x.contains("right")&&x["right"].is_object()))){
                        const Json& runner=(x.contains("target")&&x["target"].is_object())?x["target"]:x;
                        std::string cmd;for(const auto& a:runner.value("argv",Json::array())){if(!cmd.empty())cmd+=' ';cmd+=a.get<std::string>();}
                        out+=wide(x.value("id","")+" | target_test | "+runner.value("root","")+" | "+cmd);
                    }else{
                        auto side_str=[](const Json& s){std::string cmd;for(const auto& a:s.value("argv",Json::array())){if(!cmd.empty())cmd+=' ';cmd+=a.get<std::string>();}return s.value("root","")+" | "+cmd;};
                        out+=wide(x.value("id","")+" | "+x.value("comparator","bytes")+" | "+side_str(x["left"])+" | "+side_str(x["right"]));
                    }
                }
                return out;
            };
            Form f;f.parent=window;f.title=L"Compilaciones y pruebas de comportamiento";f.width=980;f.height=720;
            f.explanation=L"Escriba una compilación por línea: id | raíz | comando args | artefacto1,artefacto2.\r\nCaso diferencial: id | comparador (bytes, json, numeric) | raíz_izq | comando izq | raíz_der | comando der.\r\nCaso directo (target_test): id | target_test | raíz_destino | comando args.\r\nTambién puede pegar un arreglo JSON completo cuando utilice fixtures u observadores.";
            f.labels={L"Compilaciones configuradas",L"Casos de prueba (diferenciales o target_test)"};
            f.values={fmt_lines(fresh.config.value("builds",Json::array()),true),fmt_lines(fresh.config.value("cases",Json::array()),false)};
            Json builds=Json::array(),cases=Json::array();
            f.validate=[&](const Form& form){
                auto parse_builds=[&](const std::wstring& w)->std::wstring{
                    std::string raw=utf8(w);size_t first=raw.find_first_not_of(" \r\n\t");
                    if(first==std::string::npos){builds=Json::array();return L"";}
                    if(raw[first]=='['){try{builds=Json::parse(raw);if(!builds.is_array())return L"Las compilaciones deben ser arreglo JSON.";return L"";}catch(const std::exception& e){return L"JSON de compilaciones inválido: "+wide(e.what());}}
                    builds=Json::array();
                    for(const auto& line:lines(w)){
                        auto p=split_pipe(line);if(p.size()<3||p[0].empty()||p[1].empty()||p[2].empty())return L"Formato esperado en compilación: id | raíz | comando ... | artefactos(opcional): "+wide(line);
                        Json arts=Json::array();
                        if(p.size()>=4&&!p[3].empty()){std::istringstream ss(p[3]);std::string tok;while(std::getline(ss,tok,',')){while(!tok.empty()&&std::isspace(static_cast<unsigned char>(tok.front())))tok.erase(tok.begin());while(!tok.empty()&&std::isspace(static_cast<unsigned char>(tok.back())))tok.pop_back();if(!tok.empty())arts.push_back(tok);}}
                        builds.push_back({{"id",p[0]},{"root",p[1]},{"argv",split_ws(p[2])},{"artifacts",arts}});
                    }
                    return L"";
                };
                auto parse_cases=[&](const std::wstring& w)->std::wstring{
                    std::string raw=utf8(w);size_t first=raw.find_first_not_of(" \r\n\t");
                    if(first==std::string::npos){cases=Json::array();return L"";}
                    if(raw[first]=='['){try{cases=Json::parse(raw);if(!cases.is_array())return L"Los casos deben ser arreglo JSON.";return L"";}catch(const std::exception& e){return L"JSON de casos inválido: "+wide(e.what());}}
                    cases=Json::array();
                    for(const auto& line:lines(w)){
                        auto p=split_pipe(line);
                        if(p.size()>=4&&p[1]=="target_test"&&!p[0].empty()&&!p[2].empty()&&!p[3].empty()){
                            cases.push_back({{"id",p[0]},{"kind","target_test"},{"root",p[2]},{"argv",split_ws(p[3])}});
                            continue;
                        }
                        if(p.size()<6||p[0].empty()||p[2].empty()||p[3].empty()||p[4].empty()||p[5].empty())return L"Formato esperado: id | comparador | raíz_izq | cmd_izq | raíz_der | cmd_der (o id | target_test | raíz | cmd): "+wide(line);
                        cases.push_back({{"id",p[0]},{"comparator",p[1].empty()?"bytes":p[1]},{"left",{{"root",p[2]},{"argv",split_ws(p[3])}}},{"right",{{"root",p[4]},{"argv",split_ws(p[5])}}}});
                    }
                    return L"";
                };
                if(auto err=parse_builds(form.values[0]);!err.empty())return err;
                if(auto err=parse_cases(form.values[1]);!err.empty())return err;
                auto probe=fresh.config;probe["builds"]=builds;probe["cases"]=cases;
                try{validate(probe);}catch(const std::exception& e){return L"Configuración inválida: "+wide(e.what());}
                return std::wstring();
            };
            if(show_form(f)){scale_settings(fresh,{{"action","set"},{"actor","GUI"},{"expected_config_revision",fresh.revision},{"settings",{{"builds",builds},{"cases",cases}}}});load(fresh.file,false,page);}
            return;
        }
        if(id==ID_CONFIG&&!(GetKeyState(VK_SHIFT)&0x8000)){
            auto fresh=Project::load(project->file);Form f;f.parent=window;f.title=L"Carpetas, exclusiones y ZIP";f.width=980;f.height=740;f.browse_folders=true;
            f.labels={L"Código original · carpetas que desea migrar",L"Resultado · carpetas donde guarda el código migrado",L"Carpetas excluidas del análisis profundo (un nombre por línea, ej. node_modules, .git)",L"Nombre del proyecto"};
            std::wstring source,target,excluded;
            for(const auto& root:fresh.config["roots"]){auto& value=root["role"]=="source"?source:target;value+=wide(path_text(fresh.resolve(root["path"])))+L"\r\n";}
            for(const auto& ex:fresh.config.value("exclude_dirs",Json::array()))if(ex.is_string()){if(!excluded.empty())excluded+=L"\r\n";excluded+=wide(ex);}
            f.values={source,target,excluded,wide(fresh.config.value("name","Proyecto"))};
            f.source_only=fresh.config.value("inventory_mode","universal")=="source_only";auto duplicates=fresh.config.value("code_duplicate_mode","off");f.duplicate_choice=duplicates=="files_and_zip"?2:duplicates=="files"?1:0;
            f.nested_zip_choice=std::clamp(fresh.config.value("nested_zip_depth",2),0,4);
            f.validate=[](const Form& form){
                if(auto err=validate_directories(form,2);!err.empty())return err;
                for(const auto& ex:lines(form.values[2]))if(ex.find_first_of("/\\")!=std::string::npos)return L"Escriba solo nombres de carpeta (sin barras): "+wide(ex);
                if(form.values[3].find_first_not_of(L" \r\n\t")==std::wstring::npos)return std::wstring(L"Escriba un nombre para el proyecto.");
                return std::wstring();
            };
            if(show_form(f)){auto roots=form_roots(f,&fresh);
                auto applied=scale_settings(fresh,{{"action","set"},{"actor","GUI"},{"expected_config_revision",fresh.revision},{"settings",{{"name",utf8(f.values[3])},{"roots",roots},{"exclude_dirs",lines(f.values[2])},{"inventory_mode",*f.source_only?"source_only":"universal"},{"inventory_engine",fresh.config.value("inventory_engine","auto")},{"code_duplicate_mode",duplicate_modes[f.duplicate_choice]},{"nested_zip_depth",f.nested_zip_choice}}}});
                if(!applied["warnings"].empty()){
                    std::wstring msg=L"Se guardaron los ajustes con las siguientes advertencias de alcance:\r\n";
                    for(const auto& w:applied["warnings"])msg+=L"• "+wide(w.value("code","ADVERTENCIA"))+L": "+wide(w.dump())+L"\r\n";
                    MessageBoxW(window,msg.c_str(),L"Revise el alcance del proyecto",MB_OK|MB_ICONINFORMATION);
                }
                load(fresh.file,false,page);
            }return;
        }
        if(id==ID_CONFIG||id==ID_CONFIG_ADVANCED){
            auto fresh=Project::load(project->file);
            const bool source_only=fresh.config.value("inventory_mode","universal")=="source_only";
            const auto current_duplicates=fresh.config.value("code_duplicate_mode","off");
            const int duplicate_choice=current_duplicates=="files_and_zip"?2:current_duplicates=="files"?1:0;
            Form f;f.parent=window;f.title=L"Configuración del proyecto";
            f.labels={L"JSON avanzado · raíces, extensiones de código, analizadores, compilaciones y casos"};
            std::wstring formatted;
            for(const auto ch:wide(fresh.config.dump(2))){if(ch==L'\n')formatted+=L'\r';formatted+=ch;}
            f.values={std::move(formatted)};f.width=1050;f.height=760;
            f.validate=[](const Form& form){try{auto value=Json::parse(utf8(form.values[0]));if(!value.is_object())return std::wstring(L"La configuración debe ser un objeto JSON.");validate(value);}catch(const std::exception& e){return L"Revise la configuración JSON: "+wide(e.what());}return std::wstring();};
            f.source_only=source_only;f.duplicate_choice=duplicate_choice;
            if(show_form(f)){
                auto edited=Json::parse(utf8(f.values[0]));
                if(f.source_only!=source_only)edited["inventory_mode"]=*f.source_only?"source_only":"universal";
                if(f.duplicate_choice>=0&&f.duplicate_choice!=duplicate_choice)
                    edited["code_duplicate_mode"]=duplicate_modes[f.duplicate_choice];
                Json patch=Json::object();
                for(auto it=edited.begin();it!=edited.end();++it){
                    if(it.key()=="schema"||it.key()=="ignored")continue;
                    if(!fresh.config.contains(it.key())||fresh.config[it.key()]!=it.value())patch[it.key()]=it.value();
                }
                if(!patch.empty()){
                    auto applied=scale_settings(fresh,{{"action","set"},{"actor","GUI"},{"expected_config_revision",fresh.revision},{"settings",patch}});
                    if(!applied["warnings"].empty()){
                        std::wstring msg=L"Advertencias de alcance:\r\n";
                        for(const auto& w:applied["warnings"])msg+=L"• "+wide(w.dump())+L"\r\n";
                        MessageBoxW(window,msg.c_str(),L"Revise las carpetas",MB_OK|MB_ICONINFORMATION);
                    }
                }
                load(fresh.file,false,page);
            }
            return;
        }
        if(id==ID_NOTE){Form f;f.parent=window;f.title=L"Agregar nota al registro automático";f.labels={L"Nota",L"Autor / agente"};f.values={L"",L"GUI"};f.height=480;f.validate=[](const Form& form){return form.values[0].find_first_not_of(L" \r\n\t")==std::wstring::npos?std::wstring(L"Escriba la nota antes de guardarla."):std::wstring();};if(show_form(f)){add_note(*project,utf8(f.values[0]),utf8(f.values[1]));refresh_view();log("Nota registrada con fecha y hora UTC.");}return;}
        if(id==ID_MARK){
            auto source=selected();std::wstring path,prefill_targets,prefill_note,prefill_actor=L"GUI",prefill_cases,prefill_ext;
            int stage_idx=0,disp_idx=0;bool was_verified=false;std::string mark_entity;
            static constexpr const char* stage_names[]={"planned","half_done","ported","integrated","verified"};
            static constexpr const char* disp_names[]={"migrated","merged_into","intentionally_removed","replaced_by_external"};
            auto resolve_stage_idx=[&](const std::string& st,bool decl){
                std::string eff=st.empty()?(decl?"integrated":"planned"):st;
                for(int i=0;i<5;++i)if(eff==stage_names[i])return i;
                return decl?3:0;
            };
            auto resolve_disp_idx=[&](const std::string& dp){
                for(int i=0;i<4;++i)if(dp==disp_names[i])return i;
                return 0;
            };
            if(!source.empty()&&(source[0]=='F'||source[0]=='S')&&fs::exists(native_path(project->state_dir()/L"catalog.sqlite"))){
                Database db(*project);
                try{
                    auto rows=db.query(source[0]=='F'?"SELECT absolute_path FROM files WHERE id=? AND deleted=0":"SELECT f.absolute_path FROM symbols s JOIN files f ON f.id=s.file_id WHERE s.id=? AND s.active=1",{std::stoll(source.substr(1))});
                    if(!rows.empty()){path=wide(rows[0]["absolute_path"]);if(source[0]=='S')mark_entity=source;}
                }catch(const std::exception&){}
                auto mrows=db.query("SELECT targets,cases,complete,note,actor,hashes FROM mappings WHERE source=? LIMIT 1",{source});
                if(!mrows.empty()){
                    const auto& mr=mrows[0];
                    try{
                        auto tgts=Json::parse(mr["targets"].get<std::string>());
                        for(const auto& tid:tgts){
                            auto tstr=tid.get<std::string>();
                            std::wstring tpath;
                            if(tstr.size()>1&&(tstr[0]=='F'||tstr[0]=='S')){
                                try{
                                    auto trows=db.query(tstr[0]=='F'?"SELECT absolute_path FROM files WHERE id=? AND deleted=0":"SELECT f.absolute_path FROM symbols s JOIN files f ON f.id=s.file_id WHERE s.id=? AND s.active=1",{std::stoll(tstr.substr(1))});
                                    if(!trows.empty())tpath=wide(trows[0]["absolute_path"]);
                                }catch(const std::exception&){}
                            }
                            if(tpath.empty())tpath=wide(tstr);
                            if(!prefill_targets.empty())prefill_targets+=L"\r\n";
                            prefill_targets+=tpath;
                        }
                    }catch(...){}
                    try{
                        auto cs=Json::parse(mr["cases"].get<std::string>());
                        for(const auto& c:cs){if(!prefill_cases.empty())prefill_cases+=L"\r\n";prefill_cases+=wide(c.get<std::string>());}
                    }catch(...){}
                    prefill_note=wide(mr["note"].is_string()?mr["note"].get<std::string>():"");
                    if(mr["actor"].is_string()&&!mr["actor"].get<std::string>().empty())prefill_actor=wide(mr["actor"].get<std::string>());
                    Json mh=Json::object();
                    try{if(mr["hashes"].is_string()&&!mr["hashes"].get<std::string>().empty())mh=Json::parse(mr["hashes"].get<std::string>());}catch(...){}
                    const bool decl=mr["complete"]==1;
                    const std::string st=mh.value("_stage","");
                    stage_idx=resolve_stage_idx(st,decl);
                    was_verified=(stage_idx==4);
                    disp_idx=resolve_disp_idx(mh.value("_disposition","migrated"));
                    prefill_ext=wide(mh.value("_external_replacement",""));
                }
            }
            if(scan_data.is_object())for(const auto& s:scan_data["symbols"])if(s["id"]==source){path=wide(s["absolute_path"]);mark_entity=source;}
            if(scan_data.is_object())for(const auto& file:scan_data.value("files",Json::array()))if(file["id"]==source)path=wide(file["absolute_path"]);
            Form f;f.parent=window;f.title=L"Registrar avance de migración";f.completion_choice=stage_idx;f.disposition_choice=disp_idx;f.browse_files=true;f.append_first=false;
            f.labels={L"Archivo de origen",L"Archivos de destino · puede elegir más de uno",L"Nota · qué cambió o qué falta",L"Autor",L"Pruebas vinculadas (opcional, un nombre por línea)",L"Reemplazo externo / librería (opcional)"};
            f.values={path,prefill_targets,prefill_note,prefill_actor,prefill_cases,prefill_ext};f.single_fields={0,3,5};f.height=780;f.width=960;
            f.validate=[was_verified](const Form& form){
                const int st=std::clamp(form.completion_choice,0,4);
                const int dp=std::clamp(form.disposition_choice,0,3);
                if(st==4&&!was_verified)return std::wstring(L"La etapa 'Verificado en casos (verified)' se asigna automáticamente al pasar las pruebas vinculadas vigentes. Elija otra etapa manual.");
                const auto src_paths=lines(form.values[0]);
                if(src_paths.empty())return std::wstring(L"Elija el archivo de origen.");
                if(src_paths.size()!=1)return std::wstring(L"Elija un solo archivo de origen.");
                std::error_code error;
                const auto sp=fs::path(wide(src_paths[0]));
                if(!sp.is_absolute()||!fs::is_regular_file(native_path(sp),error))return L"Se necesita la ruta completa de un archivo existente: "+wide(src_paths[0]);
                const auto tgt_paths=lines(form.values[1]);
                if(dp<=1&&tgt_paths.empty())return std::wstring(L"Elija al menos un archivo de destino o cambie la disposición.");
                if(dp>=2&&form.values[2].find_first_not_of(L" \r\n\t")==std::wstring::npos)return std::wstring(L"Escriba una nota explicativa para eliminación intencional o reemplazo externo.");
                for(const auto& pth:tgt_paths){
                    const auto tp=fs::path(wide(pth));
                    if(!tp.is_absolute()||!fs::is_regular_file(native_path(tp),error))return L"Se necesita la ruta completa de un archivo existente: "+wide(pth);
                }
                return std::wstring();
            };
            if(show_form(f)){
                std::vector<fs::path> targets;for(const auto& target:lines(f.values[1]))targets.emplace_back(wide(target));
                const int st=std::clamp(f.completion_choice,0,4),dp=std::clamp(f.disposition_choice,0,3);
                const std::string stage=stage_names[st],disp=disp_names[dp],ext_rep=utf8(f.values[5]);
                const bool complete=(st>=3);
                const bool keep_sym=!mark_entity.empty()&&mark_entity[0]=='S'&&!path.empty()&&same_path(fs::path(f.values[0]),fs::path(path));
                start([source_path=fs::path(f.values[0]),mark_entity,keep_sym,targets=std::move(targets),complete,note=utf8(f.values[2]),actor=utf8(f.values[3]),cases=lines(f.values[4]),stage,disp,ext_rep](Project& p,Log message,Cancel* cancellation){
                    if(keep_sym&&fs::exists(native_path(p.state_dir()/L"catalog.sqlite"))){
                        std::vector<std::string> target_ids;
                        {
                            Database db(p);
                            for(const auto& t:targets){
                                std::string tid;
                                const auto norm_fwd=path_text(t.lexically_normal());
                                auto t_pref=t.lexically_normal();t_pref.make_preferred();
                                const auto norm_back=utf8(t_pref.wstring());
                                auto rows=db.query("SELECT id FROM files WHERE deleted=0 AND sha256 IS NOT NULL AND (absolute_path=? OR absolute_path=?) LIMIT 1",{norm_fwd,norm_back});
                                if(!rows.empty()){
                                    tid="F"+std::to_string(rows[0]["id"].get<int64_t>());
                                }
                                target_ids.push_back(tid.empty()?path_text(t):tid);
                            }
                        }
                        indexed_mapping(p,mark_entity,target_ids,cases,complete,note,actor,stage,disp,ext_rep);
                    }else{
                        mark_file(p,source_path,targets,complete,note,actor,cases,cancellation,stage,disp,ext_rep);
                    }
                    message("Avance guardado con etapa, disposición, autor y fecha.");
                },"Registrar avance");layout();
            }return;
        }
        if(id==ID_MODULE_FILTER_FILES){
            auto row_data=scalable_view.selected_row();
            std::string root_id=row_data.is_object()?row_data.value("root",std::string()):std::string();
            std::string mod=row_data.is_object()?row_data.value("source_module",std::string()):std::string();
            if(root_id.empty())root_id=scalable_view.active_root();
            std::string qtext=mod=="<raíz>"?std::string():mod;
            page=1;
            if(GetWindowTextLengthW(filter)>0||!qtext.empty()){SetWindowTextW(filter,wide(qtext).c_str());KillTimer(window,3);}
            scalable_view.set_filter_context(root_id,qtext);
            refresh_view();layout();
            return;
        }
        if(id==ID_PHASE_UPSERT){
            auto sel=scalable_view.selected_row();
            const bool is_phase=sel.is_object()&&sel.value("record_type","")=="phase";
            Form f;f.parent=window;f.title=L"Registrar o actualizar fase del plan";f.width=920;f.height=720;
            f.labels={L"ID de fase (ej. FASE_1)",L"Título",L"Orden numérico",L"Compilaciones requeridas (IDs separados por coma)",L"Casos de prueba requeridos (IDs separados por coma)",L"Módulos destino (separados por coma)",L"Descripción",L"Autor"};
            auto join_arr=[&](const char* key){
                std::wstring s;
                if(is_phase&&sel.contains(key)&&sel[key].is_array())for(const auto& it:sel[key]){if(!s.empty())s+=L", ";if(it.is_string())s+=wide(it.get<std::string>());}
                return s;
            };
            static constexpr const char* phase_states[]={"PLANNED","IN_PROGRESS","HALF_DONE","COMPLETED","VERIFIED","BLOCKED"};
            int st_idx=0;
            if(is_phase){const auto cs=sel.value("state",std::string());for(int i=0;i<6;++i)if(cs==phase_states[i])st_idx=i;}
            f.extra_combos={{100,L"Estado de la fase",{L"PLANNED",L"IN_PROGRESS",L"HALF_DONE",L"COMPLETED",L"VERIFIED",L"BLOCKED"},st_idx}};
            f.values={is_phase?wide(sel.value("item_id","")) : L"",is_phase?wide(sel.value("title","")) : L"",is_phase?std::to_wstring(sel.value("ordinal",1)):L"1",join_arr("gate_builds"),join_arr("gate_cases"),join_arr("target_modules"),is_phase?wide(sel.value("description","")) : L"",L"GUI"};
            f.single_fields={0,1,2,7};
            f.validate=[](const Form& form){
                if(form.values[0].find_first_not_of(L" \r\n\t")==std::wstring::npos)return std::wstring(L"Escriba un ID de fase.");
                try{size_t u{};(void)std::stoi(form.values[2],&u);if(u!=form.values[2].size())return std::wstring(L"El orden debe ser entero.");}catch(...){return std::wstring(L"El orden debe ser entero.");}
                return std::wstring();
            };
            if(show_form(f)){
                Json op={{"op","upsert_phase"},{"id",utf8(f.values[0])},{"title",utf8(f.values[1])},{"ordinal",std::stoi(f.values[2])},{"state",phase_states[std::clamp(f.extra_combos[0].selected,0,5)]},{"gate_builds",split_tokens(f.values[3])},{"gate_cases",split_tokens(f.values[4])},{"target_modules",split_tokens(f.values[5])},{"description",utf8(f.values[6])}};
                auto actor=utf8(f.values[7]);if(actor.empty())actor="GUI";
                start([op,actor](Project& p,Log msg,Cancel*){
                    run_agent_batch_with_retry(p.file,actor,Json::array({op}));
                    msg("Fase guardada en control integral.");
                },"Registrar fase");layout();
            }return;
        }
        if(id==ID_TASK_UPSERT){
            auto sel=scalable_view.selected_row();
            const bool is_task=sel.is_object()&&sel.value("record_type","")=="task";
            const bool is_phase=sel.is_object()&&sel.value("record_type","")=="phase";
            Form f;f.parent=window;f.title=L"Registrar o actualizar tarea del plan";f.width=940;f.height=780;
            static constexpr const char* task_states[]={"TODO","IN_PROGRESS","HALF_DONE","PORTED","INTEGRATED","COMPLETED","VERIFIED","BLOCKED"};
            int st_idx=0;
            if(is_task){const auto cs=sel.value("state",std::string());for(int i=0;i<8;++i)if(cs==task_states[i])st_idx=i;}
            f.extra_combos={{101,L"Estado de la tarea",{L"TODO",L"IN_PROGRESS",L"HALF_DONE",L"PORTED",L"INTEGRATED",L"COMPLETED",L"VERIFIED",L"BLOCKED"},st_idx}};
            auto join_arr=[&](const char* key){
                std::wstring s;
                if(is_task&&sel.contains(key)&&sel[key].is_array())for(const auto& it:sel[key]){if(!s.empty())s+=L", ";if(it.is_string())s+=wide(it.get<std::string>());}
                return s;
            };
            f.labels={L"ID de tarea (ej. T-001)",L"ID de fase asociada",L"Título",L"Tipo (migration, refactor, test, docs, integration)",L"Prioridad (1-5)",L"Responsable",L"Entidades origen (F.../S..., coma)",L"Entidades destino (F.../S..., coma)",L"Casos requeridos (coma)",L"Depende de tareas (coma)",L"Nota",L"Autor"};
            f.values={is_task?wide(sel.value("item_id","")) : L"",is_task?wide(sel.value("phase_id","")) : (is_phase?wide(sel.value("item_id","")) : L""),is_task?wide(sel.value("title","")) : L"",is_task?wide(sel.value("task_kind","migration")) : L"migration",is_task?std::to_wstring(sel.value("priority",2)):L"2",is_task?wide(sel.value("assignee","")) : L"",join_arr("source_entities"),join_arr("target_entities"),join_arr("required_cases"),join_arr("depends_on"),is_task?wide(sel.value("note","")) : L"",L"GUI"};
            f.single_fields={0,1,2,3,4,5,11};
            f.validate=[](const Form& form){
                if(form.values[0].find_first_not_of(L" \r\n\t")==std::wstring::npos)return std::wstring(L"Escriba un ID de tarea.");
                try{size_t u{};const int prio=std::stoi(form.values[4],&u);if(u!=form.values[4].size()||prio<0||prio>10)return std::wstring(L"Prioridad inválida (0 a 10).");}catch(...){return std::wstring(L"Prioridad inválida (0 a 10).");}
                return std::wstring();
            };
            if(show_form(f)){
                Json op={{"op","upsert_task"},{"id",utf8(f.values[0])},{"phase_id",utf8(f.values[1])},{"title",utf8(f.values[2])},{"task_kind",utf8(f.values[3])},{"state",task_states[std::clamp(f.extra_combos[0].selected,0,7)]},{"priority",std::stoi(f.values[4])},{"assignee",utf8(f.values[5])},{"source_entities",split_tokens(f.values[6])},{"target_entities",split_tokens(f.values[7])},{"required_cases",split_tokens(f.values[8])},{"depends_on",split_tokens(f.values[9])},{"note",utf8(f.values[10])}};
                auto actor=utf8(f.values[11]);if(actor.empty())actor="GUI";
                start([op,actor](Project& p,Log msg,Cancel*){
                    run_agent_batch_with_retry(p.file,actor,Json::array({op}));
                    msg("Tarea guardada en control integral.");
                },"Registrar tarea");layout();
            }return;
        }
        if(id==ID_TASK_TRANSITION){
            auto sel=scalable_view.selected_row();
            const bool is_task=sel.is_object()&&sel.value("record_type","")=="task";
            Form f;f.parent=window;f.title=L"Cambiar estado de una tarea";f.width=780;f.height=520;
            static constexpr const char* task_states[]={"TODO","IN_PROGRESS","HALF_DONE","PORTED","INTEGRATED","COMPLETED","VERIFIED","BLOCKED"};
            int st_idx=0;
            if(is_task){const auto cs=sel.value("state",std::string());for(int i=0;i<8;++i)if(cs==task_states[i])st_idx=i;}
            f.extra_combos={{102,L"Nuevo estado",{L"TODO",L"IN_PROGRESS",L"HALF_DONE",L"PORTED",L"INTEGRATED",L"COMPLETED",L"VERIFIED",L"BLOCKED"},st_idx}};
            f.labels={L"ID de tarea",L"Nota del cambio de estado",L"Autor"};
            f.values={is_task?wide(sel.value("item_id","")) : L"",is_task?wide(sel.value("note","")) : L"",L"GUI"};
            f.single_fields={0,2};
            f.validate=[](const Form& form){return form.values[0].find_first_not_of(L" \r\n\t")==std::wstring::npos?std::wstring(L"Indique el ID de la tarea."):std::wstring();};
            if(show_form(f)){
                Json op={{"op","transition_task"},{"id",utf8(f.values[0])},{"state",task_states[std::clamp(f.extra_combos[0].selected,0,7)]},{"note",utf8(f.values[1])}};
                auto actor=utf8(f.values[2]);if(actor.empty())actor="GUI";
                start([op,actor](Project& p,Log msg,Cancel*){
                    run_agent_batch_with_retry(p.file,actor,Json::array({op}));
                    msg("Estado de tarea actualizado.");
                },"Transicionar tarea");layout();
            }return;
        }
        if(id==ID_TASK_DELETE){
            auto sel=scalable_view.selected_row();
            if(!sel.is_object()||sel.value("record_type","")!="task"){log("Seleccione una tarea en Fases, tareas y documentos para eliminarla.");return;}
            const auto tid=sel.value("item_id","");
            if(tid.empty())return;
            if(MessageBoxW(window,(L"¿Eliminar la tarea '"+wide(tid)+L"' del plan?").c_str(),L"Confirmar eliminación de tarea",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
            start([tid](Project& p,Log msg,Cancel*){
                run_agent_batch_with_retry(p.file,"GUI",Json::array({{{"op","delete_task"},{"id",tid}}}));
                msg("Tarea eliminada: "+tid);
            },"Eliminar tarea");layout();
            return;
        }
        if(id==ID_DOC_AUDIT){
            auto sel=scalable_view.selected_row();
            const bool is_doc=sel.is_object()&&sel.value("record_type","")=="doc_audit";
            auto sel_id=selected();
            std::wstring doc_prefill=is_doc?wide(sel.value("item_id","")):(!sel_id.empty()&&sel_id[0]=='F'?wide(sel_id):L"");
            Form f;f.parent=window;f.title=L"Auditar documento del proyecto";f.width=920;f.height=680;
            static constexpr const char* doc_states[]={"UNREVIEWED","REVIEWED","ACTION_REQUIRED","VERIFIED"};
            static constexpr const char* doc_disps[]={"reference_only","tracked_tasks","obsolete","contract_extracted","no_action_needed"};
            int st_idx=1,dp_idx=0;
            if(is_doc){
                const auto cs=sel.value("state",std::string());for(int i=0;i<4;++i)if(cs==doc_states[i])st_idx=i;
                const auto cd=sel.value("disposition",std::string());for(int i=0;i<5;++i)if(cd==doc_disps[i])dp_idx=i;
            }
            f.extra_combos={{103,L"Estado documental",{L"UNREVIEWED",L"REVIEWED",L"ACTION_REQUIRED",L"VERIFIED"},st_idx},{104,L"Disposición documental",{L"reference_only",L"tracked_tasks",L"obsolete",L"contract_extracted",L"no_action_needed"},dp_idx}};
            auto join_arr=[&](const char* key){
                std::wstring s;
                if(is_doc&&sel.contains(key)&&sel[key].is_array())for(const auto& it:sel[key]){if(!s.empty())s+=L", ";if(it.is_string())s+=wide(it.get<std::string>());}
                return s;
            };
            f.labels={L"Documento (F<id> o ruta absoluta)",L"Destinos vinculados (F<id>, coma)",L"Tareas extraídas (IDs, coma)",L"Nota",L"Autor"};
            f.values={doc_prefill,join_arr("linked_targets"),join_arr("extracted_tasks"),is_doc?wide(sel.value("note","")) : L"",L"GUI"};
            f.single_fields={0,4};
            f.validate=[](const Form& form){return form.values[0].find_first_not_of(L" \r\n\t")==std::wstring::npos?std::wstring(L"Indique el documento (F<id> o ruta)."):std::wstring();};
            if(show_form(f)){
                Json op={{"op","audit_doc"},{"doc",utf8(f.values[0])},{"state",doc_states[std::clamp(f.extra_combos[0].selected,0,3)]},{"disposition",doc_disps[std::clamp(f.extra_combos[1].selected,0,4)]},{"linked_targets",split_tokens(f.values[1])},{"extracted_tasks",split_tokens(f.values[2])},{"note",utf8(f.values[3])}};
                auto actor=utf8(f.values[4]);if(actor.empty())actor="GUI";
                start([op,actor](Project& p,Log msg,Cancel*){
                    run_agent_batch_with_retry(p.file,actor,Json::array({op}));
                    msg("Auditoría documental guardada.");
                },"Auditar documento");layout();
            }return;
        }
        if(id==ID_CYCLE_PANE){
            std::vector<HWND> panes;
            for(auto button:section_nav)if(IsWindowVisible(button)){panes.push_back(button);break;}
            if(IsWindowVisible(filter))panes.push_back(filter);
            for(auto view:{list,comparison.window(),scalable_view.window()})if(view&&IsWindowVisible(view))panes.push_back(view);
            if(logs_visible&&IsWindowVisible(logbox))panes.push_back(logbox);
            if(!panes.empty()){
                HWND cur=GetFocus();
                size_t idx=0;
                for(size_t i=0;i<panes.size();++i)if(cur==panes[i]||IsChild(panes[i],cur)){idx=(i+1)%panes.size();break;}
                SetFocus(panes[idx]);
            }return;
        }
        if(id==ID_HOUSEKEEP){
            start([](Project& p,Log l,Cancel*){
                auto res=agent_request(p,"housekeep",Json::object());
                l("Autoaseo completado: "+std::to_string(res.value("jobs_pruned",0))+" trabajos antiguos depurados · "+std::to_string(res.value("requests_pruned",0))+" solicitudes liberadas.");
            },"Autoaseo del catálogo");
            return;
        }
        if(id==ID_TOPOLOGY_AUDIT){
            Form f;f.parent=window;f.title=L"Topología y seguridad del workspace";f.width=980;f.height=720;f.read_only=true;
            f.labels={L"Diagnóstico de repositorios Git, worktrees, enlaces NTFS, rutas absolutas y marcadores en la nube"};
            Json report=Json::object();
            try{Database db(*project);report=tool_inspect_workspace_topology(*project,db,Json::object());}
            catch(const std::exception& e){report={{"error",e.what()}};}
            f.values={wide(report.dump(2))};
            show_form(f);
            return;
        }
        if(id==ID_COVERAGE){auto input=open_file(window,L"Importar informe de cobertura");if(!input)return;Form f;f.parent=window;f.title=L"Importar cobertura";f.labels={L"Raíz (identificador exacto en configuración)",L"Formato: llvm, cobertura o generic"};f.values={L"target1",L"llvm"};f.height=400;if(show_form(f)){auto root=utf8(f.values[0]),format=utf8(f.values[1]);start([input,root,format](Project& p,Log l,Cancel*){import_coverage(p,*input,root,format,l);});}return;}
        if(id==ID_SCAN){Database db(*project);if(scale_enabled(*project,db)){auto job=scale_start(*project,{{"actor","GUI"}});log("Trabajo iniciado: "+job["job_id"].get<std::string>());durable=true;page=16;refresh_view();layout();}else start([](Project& p,Log l,Cancel* c){scan(p,l,c);});}
        else if(id==ID_BUILD)start([](Project& p,Log l,Cancel* c){auto r=run_builds(p,l,c);if(r["builds"].empty())l("Sin comandos de compilación configurados.");});
        else if(id==ID_TEST)start([](Project& p,Log l,Cancel* c){auto r=run_cases(p,l,c);if(r["cases"].empty())l("Sin casos configurados.");});
        else if(id==ID_REFRESH)start([](Project& p,Log l,Cancel*){Database db(p);Json st;if(scale_enabled(p,db)){scale_fingerprints(db);st=scale_summary(p,db);}else st=mt::status(p);write_json(p.state_dir()/L"status.json",st);l("Vigencia contrastada con los archivos actuales.");});
        else if(id==ID_REPORT)start([](Project& p,Log l,Cancel*){auto output=p.state_dir()/L"report.html";report(p,output);l("Informe HTML y JSON: "+path_text(output));});
    }
};
LRESULT CALLBACK section_proc(HWND w,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(message==WM_MOUSEMOVE){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,w,0};TrackMouseEvent(&track);SendMessageW(GetParent(w),WM_APP+4,GetDlgCtrlID(w),1);}
    else if(message==WM_MOUSELEAVE)SendMessageW(GetParent(w),WM_APP+4,GetDlgCtrlID(w),0);
    else if(message==WM_KEYDOWN&&(wp==VK_UP||wp==VK_DOWN||wp==VK_HOME||wp==VK_END)){int group=GetDlgCtrlID(w)-2000;group=wp==VK_HOME?0:wp==VK_END?4:std::clamp(group+(wp==VK_DOWN?1:-1),0,4);auto next=GetDlgItem(GetParent(w),2000+group);SetFocus(next);SendMessageW(GetParent(w),WM_COMMAND,2000+group,0);return 0;}
    return DefSubclassProc(w,message,wp,lp);
}
LRESULT CALLBACK proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    auto a=reinterpret_cast<App*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(msg==WM_NCCREATE){a=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);a->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
    if(!a)return DefWindowProcW(w,msg,wp,lp);
    try {
        switch(msg) {
          case WM_CREATE: {
            a->dpi=GetDpiForWindow(w);
            a->read_preferences();auto makefont=[&](int size,int weight){return CreateFontW(-a->type_px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");};
            body=makefont(18,FW_NORMAL);heading=makefont(28,FW_SEMIBOLD);small_font=makefont(14,FW_NORMAL);cardfont=makefont(30,FW_SEMIBOLD);
            for(int i=0;i<5;++i){auto button=control(w,L"BUTTON",navigation_groups()[i].title,WS_TABSTOP|BS_OWNERDRAW,2000+i,body);SetWindowSubclass(button,section_proc,1,0);a->section_nav.push_back(button);}
            a->section_selector=control(w,WC_COMBOBOXW,L"Vista de la sección",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,ID_SECTION_VIEW,body);a->project_menu=control(w,L"BUTTON",L"Proyecto ▾",WS_TABSTOP,ID_PROJECT_MENU,body);a->log_toggle=control(w,L"BUTTON",L"Detalles técnicos",WS_TABSTOP,ID_LOG_TOGGLE,small_font);a->buttons.push_back(control(w,L"BUTTON",L"Ver pendientes",WS_TABSTOP,ID_NEXT_STEP,body));
            a->section_compact=control(w,WC_COMBOBOXW,L"Sección principal",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,ID_SECTION_COMPACT,body);for(const auto& group:navigation_groups())SendMessageW(a->section_compact,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(group.title));SendMessageW(a->section_compact,CB_SETCURSEL,0,0);
            for(const auto& [id,label]:std::vector<std::pair<int,const wchar_t*>>{{ID_NEW,L"Nuevo"},{ID_OPEN,L"Abrir"},{ID_CONFIG,L"Configuración"},{ID_CONFIG_ADVANCED,L"JSON avanzado"},{ID_CODE_SETTINGS,L"Tipos de código"},{ID_RESOURCES,L"Recursos"},{ID_VERIFY_CONFIG,L"Compilación y pruebas"},{ID_IMPORT_WORK,L"Importar trabajo…"},{ID_REVALIDATE_WORK,L"Revalidar selección"},{ID_CONTINUE_IMPORT,L"Continuar importación"},{ID_NOTE,L"Agregar nota"},{ID_SCAN,L"Analizar"},{ID_BUILD,L"Compilar"},{ID_TEST,L"Probar"},{ID_REFRESH,L"Actualizar"},{ID_REPORT,L"Informe"},{ID_MARK,L"Marcar archivo"},{ID_COVERAGE,L"Importar cobertura"},{ID_PHASE_UPSERT,L"Nueva / editar fase…"},{ID_TASK_UPSERT,L"Nueva / editar tarea…"},{ID_TASK_TRANSITION,L"Cambiar estado…"},{ID_DOC_AUDIT,L"Auditar documento…"},{ID_MODULE_FILTER_FILES,L"Ver archivos"},{ID_TASK_DELETE,L"Eliminar tarea"},{ID_CANCEL,L"Cancelar"}})a->buttons.push_back(control(w,L"BUTTON",label,WS_TABSTOP,id,body));
            a->buttons.push_back(control(w,L"BUTTON",L"Exportar inventario…",WS_TABSTOP,ID_EXPORT_INVENTORY,body));
            a->subtitle=control(w,L"STATIC",L"",0,0,body);
            a->welcome=control(w,L"STATIC",L"1. Elija las carpetas del código original y del resultado.\r\n\r\n2. Escanee el proyecto para encontrar archivos y pendientes.\r\n\r\n3. Relacione los archivos migrados con sus destinos y guarde su avance.",0,60,body);
            for(const auto* label:{L"Archivos de origen",L"Migración registrada",L"Con pruebas vigentes",L"Análisis pendientes"}){a->card_values.push_back(control(w,L"STATIC",L"Por calcular",SS_ENDELLIPSIS,0,body));a->card_labels.push_back(control(w,L"STATIC",label,0,0,small_font));}
             a->project_selector=control(w,WC_COMBOBOXW,L"Proyecto visible",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,ID_PROJECT,small_font);
             SendMessageW(a->project_selector,CB_SETDROPPEDWIDTH,a->px(900),0);
            a->filter=control(w,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,ID_FILTER,body);ui::apply_control_polish(a->filter,true,a->px(8));ui::name(a->filter,L"Buscar en esta vista");SendMessageW(a->filter,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Filtrar por nombre, ruta exacta, estado o nota…"));
            a->list=control(w,WC_LISTVIEWW,L"Resultados de la pantalla actual",WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_SHOWSELALWAYS,40,body);ListView_SetExtendedListViewStyle(a->list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
            a->logbox=control(w,L"EDIT",L"",WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL,41,small_font);ui::apply_control_polish(a->logbox,true,a->px(8));ui::name(a->logbox,L"Registro técnico de actividad");
            a->statusbar=control(w,L"STATIC",a->status_text,SS_ENDELLIPSIS,42,small_font);
             a->comparison.create(w,body,a->dpi);a->scalable_view.create(w,body,a->dpi);
             ui::apply_control_polish(a->list,false);a->menus();a->apply_appearance();SetTimer(w,1,1000,nullptr);a->set_busy(false);a->refresh_view();a->layout();return 0;
          }
          case WM_SIZE:a->layout();return 0;
          case WM_OPEN_PROJECT:{std::unique_ptr<OpenProjectResult> result(reinterpret_cast<OpenProjectResult*>(lp));a->finish_open(*result,wp!=0);return 0;}
          case WM_PROJECT_POLL:{std::unique_ptr<ProjectPoll> update(reinterpret_cast<ProjectPoll*>(lp));a->finish_poll(*update);return 0;}
          case WM_REUSE_RESULT:{std::unique_ptr<ReuseResult> outcome(reinterpret_cast<ReuseResult*>(lp));if(!a->closing)a->finish_reuse(*outcome);return 0;}
          case WM_SCALE_SELECTION:a->update_reuse_actions();return 0;
          case WM_SCALE_ACTION:a->action(static_cast<int>(wp));return 0;
          case WM_APP+172:if(lp){a->summary=*reinterpret_cast<const Json*>(lp);a->update_cards();InvalidateRect(w,nullptr,FALSE);}return 0;
          case WM_APP+174:if(lp){Form f;f.parent=w;f.title=L"Detalle del archivo y evidencia";f.read_only=true;f.labels={L"Rutas completas, avance y comprobaciones"};f.values={*reinterpret_cast<const std::wstring*>(lp)};f.width=1050;f.height=700;show_form(f);}return 0;
          case WM_APP+4:{const auto previous=a->hovered;a->hovered=lp?static_cast<int>(wp):-1;if(previous!=a->hovered){if(previous>=0)InvalidateRect(GetDlgItem(w,previous),nullptr,FALSE);if(a->hovered>=0)InvalidateRect(GetDlgItem(w,a->hovered),nullptr,FALSE);}return 0;}
          case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={a->px(860),a->px(620)};return 0;
          case WM_PAINT:a->paint();return 0;
          case WM_ERASEBKGND:return 1;
          case WM_DPICHANGED:{a->dpi=HIWORD(wp);a->apply_appearance(false);auto r=reinterpret_cast<RECT*>(lp);RECT cur{};GetWindowRect(w,&cur);const bool same_size=(cur.right-cur.left)==(r->right-r->left)&&(cur.bottom-cur.top)==(r->bottom-r->top);SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);if(same_size)a->layout();SendMessageW(w,WM_SETREDRAW,TRUE,0);InvalidateRect(w,nullptr,FALSE);if(a->scalable_view.window()&&IsWindowVisible(a->scalable_view.window()))InvalidateRect(a->scalable_view.window(),nullptr,FALSE);return 0;}
          case WM_SETTINGCHANGE:case WM_THEMECHANGED:case WM_SYSCOLORCHANGE:a->apply_appearance();return 0;
          case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:{auto child=reinterpret_cast<HWND>(lp);auto dc=reinterpret_cast<HDC>(wp);for(auto item:a->card_values)if(item==child){SetTextColor(dc,BLUE);SetBkColor(dc,palette.surface);return reinterpret_cast<LRESULT>(a->surface_brush);}for(auto item:a->card_labels)if(item==child){SetTextColor(dc,MUTED);SetBkColor(dc,palette.surface);return reinterpret_cast<LRESULT>(a->surface_brush);}return reinterpret_cast<LRESULT>(ui::color_control(dc,child,palette,a->background_brush,a->surface_brush));}
          case WM_DRAWITEM:{auto d=reinterpret_cast<DRAWITEMSTRUCT*>(lp);if(d->CtlID>=2000&&d->CtlID<2005){const int group=static_cast<int>(d->CtlID)-2000;const bool active=navigation_group(a->page)==group;auto background=CreateSolidBrush(NAV);FillRect(d->hDC,&d->rcItem,background);DeleteObject(background);
                const bool pressed=(d->itemState&ODS_SELECTED)!=0,hover=a->hovered==static_cast<int>(d->CtlID);
                if(active||pressed||hover){
                    auto fill=CreateSolidBrush(pressed?palette.selection:active?palette.selection:palette.hover);
                    auto border_pen=CreatePen(PS_SOLID,1,(active&&palette.increased_contrast)?palette.accent:(active?palette.border:palette.hover));
                    auto old_br=SelectObject(d->hDC,fill);auto old_pen=SelectObject(d->hDC,active?border_pen:GetStockObject(NULL_PEN));
                    RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,a->px(8),a->px(8));
                    SelectObject(d->hDC,old_pen);SelectObject(d->hDC,old_br);DeleteObject(border_pen);DeleteObject(fill);
                }
                const int cy=(d->rcItem.top+d->rcItem.bottom)/2;
                if(active){
                    RECT pill{d->rcItem.left+a->px(4),cy-a->px(9),d->rcItem.left+a->px(7),cy+a->px(9)};
                    auto pill_br=CreateSolidBrush(palette.accent);FillRect(d->hDC,&pill,pill_br);DeleteObject(pill_br);
                }
                const COLORREF fg_text=active||pressed?palette.selection_text:palette.high_contrast&&hover?palette.selection_text:INK;
                const COLORREF fg_icon=active||pressed?(palette.high_contrast?palette.selection_text:palette.accent):MUTED;
                const int icon_sz=std::max<int>(a->px(16),std::min<int>(a->px(20),static_cast<int>(d->rcItem.bottom-d->rcItem.top)/2));
                RECT icon_rc{d->rcItem.left+a->px(14),cy-icon_sz/2,d->rcItem.left+a->px(14)+icon_sz,cy+icon_sz/2};
                ui::draw_nav_icon(d->hDC,group,icon_rc,fg_icon,std::max(1,a->px(2)));
                SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,fg_text);auto old=SelectObject(d->hDC,body);
                auto bounds=d->rcItem;bounds.left+=a->px(14)+icon_sz+a->px(10);
                DrawTextW(d->hDC,navigation_groups()[group].title,-1,&bounds,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
                SelectObject(d->hDC,old);if(d->itemState&ODS_FOCUS){auto focus=d->rcItem;InflateRect(&focus,-a->px(3),-a->px(3));DrawFocusRect(d->hDC,&focus);}return TRUE;}
            if(d->CtlID>=1000&&d->CtlID<1000+std::size(pages))return TRUE;break;}
          case WM_INITMENUPOPUP:{auto menu=reinterpret_cast<HMENU>(wp);for(auto id:{ID_CONFIG,ID_CONFIG_ADVANCED,ID_CODE_SETTINGS,ID_RESOURCES,ID_VERIFY_CONFIG,ID_IMPORT_WORK,ID_SCAN,ID_MARK,ID_NOTE,ID_PHASE_UPSERT,ID_TASK_UPSERT,ID_TASK_TRANSITION,ID_DOC_AUDIT,ID_BUILD,ID_TEST,ID_COVERAGE,ID_REPORT,ID_EXPORT_INVENTORY,ID_HOUSEKEEP,ID_TOPOLOGY_AUDIT})EnableMenuItem(menu,id,MF_BYCOMMAND|(a->project&&!a->busy?MF_ENABLED:MF_GRAYED));const auto sel_row=a->scalable_view.selected_row();EnableMenuItem(menu,ID_TASK_DELETE,MF_BYCOMMAND|(a->project&&!a->busy&&a->page==24&&sel_row.is_object()&&sel_row.value("record_type","")=="task"?MF_ENABLED:MF_GRAYED));EnableMenuItem(menu,ID_REVALIDATE_WORK,MF_BYCOMMAND|(a->project&&!a->busy&&a->page==18&&!a->selected().empty()?MF_ENABLED:MF_GRAYED));EnableMenuItem(menu,ID_CONTINUE_IMPORT,MF_BYCOMMAND|(!a->busy&&a->can_continue_import()?MF_ENABLED:MF_GRAYED));for(auto id:{ID_NEW,ID_OPEN})EnableMenuItem(menu,id,MF_BYCOMMAND|(a->busy?MF_GRAYED:MF_ENABLED));CheckMenuItem(menu,ID_SIDEBAR,MF_BYCOMMAND|(!a->compact?MF_CHECKED:MF_UNCHECKED));CheckMenuItem(menu,ID_LOG_TOGGLE,MF_BYCOMMAND|(a->logs_visible?MF_CHECKED:MF_UNCHECKED));CheckMenuItem(menu,ID_INCREASE_CONTRAST,MF_BYCOMMAND|(ui::g_increased_contrast?MF_CHECKED:MF_UNCHECKED));CheckMenuItem(menu,ID_TOGGLE_THEME,MF_BYCOMMAND|(ui::g_theme_mode==1?MF_CHECKED:MF_UNCHECKED));EnableMenuItem(menu,ID_ZOOM_IN,MF_BYCOMMAND|(ui::text_percent<200?MF_ENABLED:MF_GRAYED));EnableMenuItem(menu,ID_ZOOM_OUT,MF_BYCOMMAND|(ui::text_percent>100?MF_ENABLED:MF_GRAYED));return 0;}
          case WM_COMMAND:if(LOWORD(wp)==ID_FILTER&&HIWORD(wp)==EN_CHANGE){SetTimer(w,3,160,nullptr);return 0;}if((LOWORD(wp)==ID_PROJECT||LOWORD(wp)==ID_SECTION_VIEW||LOWORD(wp)==ID_SECTION_COMPACT)&&HIWORD(wp)!=CBN_SELCHANGE&&HIWORD(wp)!=1)return 0;a->action(LOWORD(wp));return 0;
          case WM_LOG:{std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(lp));a->log(*s);return 0;}
          case WM_DONE:{std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(lp));if(a->worker.joinable())a->worker.join();a->set_busy(false);
            if(a->closing){if(wp)a->log("Operación cancelada al cerrar: "+*s);if(IsWindow(w))DestroyWindow(w);return 0;}
            if(wp){a->log("Error: "+*s);MessageBoxW(w,wide(*s).c_str(),L"Migratool — operación no completada",MB_OK|MB_ICONERROR);}else a->log("Operación terminada; consulte estados y comprobantes.");
            if(a->project){a->displayed_revision=-1;a->watcher=std::make_unique<IndexWatcher>(*a->project,900000);if((a->durable||a->page>=16)&&a->page!=13&&a->page!=14&&a->page!=20)a->scalable_view.refresh();}a->layout();a->poll_live();return 0;}
          case WM_TIMER:if(wp==1){if(!a->capture_active)a->poll_live();}else if(wp==3){KillTimer(w,3);a->refresh_view();}else if(wp==4){KillTimer(w,4);a->prefs_dirty=false;const auto path=a->preferences_path();const int tp=ui::text_percent;const bool sh=a->sidebar_hidden;const int tm=ui::g_theme_mode;const bool ic=ui::g_increased_contrast;if(a->prefs_worker.joinable())a->prefs_worker.join();a->prefs_worker=std::jthread([path,tp,sh,tm,ic]{try{fs::create_directories(native_path(path.parent_path()));write_json(path,{{"text_percent",tp},{"sidebar_hidden",sh},{"theme_mode",tm},{"increased_contrast",ic}});}catch(...){}});}else if(wp==5&&a->capture_active){
              const ULONGLONG now_tick=GetTickCount64();
              if(a->capture_wait_started==0)a->capture_wait_started=now_tick;
              const bool timed_out=(now_tick-a->capture_wait_started)>45000;
              const bool ready_proj=!a->expect_project||a->project.has_value();
              const bool uses_scale=a->project&&(a->durable||a->page>=16)&&a->page!=13&&a->page!=14&&a->page!=20;
              const bool scale_ready=!uses_scale||a->scalable_view.settled();
              const bool idle=ready_proj&&!a->loading&&!a->busy&&!a->polling&&!a->scalable_view.busy()&&scale_ready;
              if(idle||timed_out){
                  if(a->capture_settled_since==0)a->capture_settled_since=now_tick;
                  if((now_tick-a->capture_settled_since)>=250||timed_out){
                      RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
                      fs::path out_png=(!a->capture_frame_path.empty()&&a->capture_index==0)?a->capture_frame_path:(a->capture_dir_path/(L"page-"+std::to_wstring(a->page)+L".png"));
                      save_window_png(w,out_png);
                      ++a->capture_index;
                      a->capture_settled_since=0;
                      a->capture_wait_started=now_tick;
                      if(a->capture_index<a->capture_pages.size()){
                          a->page=a->capture_pages[a->capture_index];
                          a->refresh_view();a->layout();
                      }else{
                          KillTimer(w,5);a->capture_active=false;PostMessageW(w,WM_CLOSE,0,0);
                      }
                  }
              }else a->capture_settled_since=0;
          }return 0;
          case WM_NOTIFY:{auto n=reinterpret_cast<NMHDR*>(lp);if(n->hwndFrom==a->list&&n->code==NM_CUSTOMDRAW)return ui::handle_listview_customdraw(lp,palette);if(n->hwndFrom==a->list&&n->code==NM_DBLCLK){auto id=a->selected();if(a->page==14&&!id.empty()){a->load(fs::path(wide(id)));a->layout();return 0;}if(!id.empty()){Form f;f.parent=w;f.title=L"Identificador exacto · copiar para CLI o configuración";f.labels={L"Unidad seleccionada"};f.values={wide(id)};f.height=300;show_form(f);}return 0;}break;}
          case WM_CLOSE:
            a->closing=true;a->cancel.requested=true;
            KillTimer(w,1);KillTimer(w,3);KillTimer(w,4);KillTimer(w,5);
            a->poll_worker.request_stop();a->poll_cv.notify_all();
            a->scalable_view.request_stop();
            if(a->watcher)a->watcher->request_stop();
            if(a->busy){a->log("Cerrando la ventana. Guardando el estado de la operación…");return 0;}
            DestroyWindow(w);return 0;
          case WM_DESTROY:
            KillTimer(w,1);KillTimer(w,3);KillTimer(w,4);KillTimer(w,5);
            a->closing=true;a->cancel.requested=true;
            a->poll_worker.request_stop();a->poll_cv.notify_all();
            a->scalable_view.request_stop();
            if(a->watcher)a->watcher->request_stop();
            if(a->worker.joinable())a->worker.join();
            if(a->prefs_worker.joinable())a->prefs_worker.join();
            if(a->prefs_dirty){a->prefs_dirty=false;a->flush_preferences_sync();}
            if(a->poll_worker.joinable())a->poll_worker.join();
            a->scalable_view.stop();
            a->watcher.reset();
            if(a->gui_session){a->gui_session->finish(0);a->gui_session.reset();}
            PostQuitMessage(0);return 0;
        }
    }catch(const std::exception& e){if(msg==WM_TIMER){SetWindowTextW(a->statusbar,wide("No se pudo actualizar el índice; se reintentará: "+std::string(e.what())).c_str());return 0;}a->log("Error: "+std::string(e.what()));MessageBoxW(w,wide(e.what()).c_str(),L"Migratool",MB_OK|MB_ICONERROR);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_LISTVIEW_CLASSES|ICC_TREEVIEW_CLASSES|ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS};InitCommonControlsEx(&cc);
    WNDCLASSW wc{};wc.hInstance=instance;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"MigratoolWindow";wc.lpfnWndProc=proc;RegisterClassW(&wc);
    wc.lpszClassName=L"MigratoolForm";wc.lpfnWndProc=form_proc;wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&wc);
    App app;auto w=CreateWindowExW(0,L"MigratoolWindow",L"Migratool · Auditor de migraciones de código",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1500,960,nullptr,nullptr,instance,&app);
    if(!w){CoUninitialize();return 1;}
    {
        const UINT initial_dpi=GetDpiForWindow(w);
        RECT work_area{0,0,1920,1040};
        if(HMONITOR mon=MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST)){
            MONITORINFO mi{sizeof(mi)};
            if(GetMonitorInfoW(mon,&mi))work_area=mi.rcWork;
        }
        const int avail_w=std::max(1,static_cast<int>(work_area.right-work_area.left));
        const int avail_h=std::max(1,static_cast<int>(work_area.bottom-work_area.top));
        const int target_w=std::min(MulDiv(1180,initial_dpi,96),avail_w*94/100);
        const int target_h=std::min(MulDiv(760,initial_dpi,96),avail_h*94/100);
        SetWindowPos(w,nullptr,work_area.left+std::max(0,(avail_w-target_w)/2),work_area.top+std::max(0,(avail_h-target_h)/2),target_w,target_h,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    fs::path project_arg;int initial_page=-1;
    for(int i=1;i<argc;++i){
        const std::wstring arg=argv[i];
        if(arg==L"--headless"){app.headless_mode=true;}
        else if(arg==L"--project"&&i+1<argc){project_arg=argv[++i];}
        else if(arg==L"--page"&&i+1<argc){try{int p=std::clamp(std::stoi(argv[++i]),0,static_cast<int>(std::size(pages))-1);if(initial_page<0)initial_page=p;app.capture_pages.push_back(p);}catch(...){}}
        else if(arg==L"--pages"&&i+1<argc){
            std::wistringstream ss(argv[++i]);std::wstring tok;
            while(std::getline(ss,tok,L',')){try{int p=std::clamp(std::stoi(tok),0,static_cast<int>(std::size(pages))-1);if(initial_page<0)initial_page=p;app.capture_pages.push_back(p);}catch(...){}}
        }
        else if(arg==L"--capture-frame"&&i+1<argc){app.capture_frame_path=argv[++i];}
        else if(arg==L"--capture-dir"&&i+1<argc){app.capture_dir_path=argv[++i];}
        else if(!arg.empty()&&arg[0]!=L'-'&&project_arg.empty()){project_arg=arg;}
    }
    if(argv)LocalFree(argv);
    if(app.headless_mode){BOOL cloak=TRUE;DwmSetWindowAttribute(w,DWMWA_CLOAK,&cloak,sizeof(cloak));ShowWindow(w,SW_SHOWNOACTIVATE);}
    else{ShowWindow(w,show);UpdateWindow(w);}
    if(!project_arg.empty()){
        app.expect_project=true;
        try{app.load(project_arg,false,initial_page>=0?initial_page:-1);}catch(const std::exception& e){app.log(e.what());}
    }else if(initial_page>=0){
        app.page=initial_page;app.refresh_view();app.layout();
    }
    if(!app.capture_frame_path.empty()||!app.capture_dir_path.empty()){
        if(app.capture_pages.empty()){
            if(!app.capture_frame_path.empty())app.capture_pages.push_back(initial_page>=0?initial_page:0);
            else app.capture_pages={0,23,24};
        }
        app.capture_active=true;app.capture_index=0;app.capture_wait_started=GetTickCount64();
        SetTimer(w,5,50,nullptr);
    }
    std::vector<ACCEL> shortcuts={{FCONTROL|FVIRTKEY,'O',ID_OPEN},{FCONTROL|FVIRTKEY,'N',ID_NEW},{FCONTROL|FVIRTKEY,'F',ID_FOCUS_SEARCH},{FVIRTKEY,VK_F5,ID_RELOAD_VIEW},{FVIRTKEY,VK_F6,ID_CYCLE_PANE},{FVIRTKEY,VK_F1,ID_HELP},{FCONTROL|FVIRTKEY,VK_OEM_COMMA,ID_CONFIG},{FCONTROL|FVIRTKEY,VK_OEM_PLUS,ID_ZOOM_IN},{FCONTROL|FSHIFT|FVIRTKEY,VK_OEM_PLUS,ID_ZOOM_IN},{FCONTROL|FVIRTKEY,VK_ADD,ID_ZOOM_IN},{FCONTROL|FVIRTKEY,VK_OEM_MINUS,ID_ZOOM_OUT},{FCONTROL|FVIRTKEY,VK_SUBTRACT,ID_ZOOM_OUT},{FCONTROL|FVIRTKEY,'0',ID_ZOOM_RESET},{FCONTROL|FVIRTKEY,'B',ID_SIDEBAR},{FCONTROL|FSHIFT|FVIRTKEY,'D',ID_LOG_TOGGLE},{FCONTROL|FALT|FVIRTKEY,'C',ID_INCREASE_CONTRAST},{FCONTROL|FALT|FVIRTKEY,'T',ID_TOGGLE_THEME}};
    for(WORD i=0;i<5;++i)shortcuts.push_back({FCONTROL|FVIRTKEY,static_cast<WORD>('1'+i),static_cast<WORD>(2000+i)});
    auto accelerator=CreateAcceleratorTableW(shortcuts.data(),static_cast<int>(shortcuts.size()));MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!TranslateAcceleratorW(w,accelerator,&msg)&&!IsDialogMessageW(w,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    DestroyAcceleratorTable(accelerator);for(auto f:{body,heading,small_font,cardfont})if(f)DeleteObject(f);if(app.background_brush)DeleteObject(app.background_brush);if(app.surface_brush)DeleteObject(app.surface_brush);CoUninitialize();return 0;
}
