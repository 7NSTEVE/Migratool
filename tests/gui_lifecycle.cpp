#include "migratool/core.hpp"
#include "migratool/scale.hpp"
#include <objbase.h>
#include <uiautomation.h>
#include <tlhelp32.h>
#include <wct.h>
#include <commctrl.h>
#include <iostream>
#include <algorithm>
#include <set>
#include <wrl/client.h>

namespace {
using namespace mt;
Json waits(DWORD pid){
    Json result=Json::array();auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snapshot==INVALID_HANDLE_VALUE)return result;
    THREADENTRY32 entry{sizeof(entry)};auto session=OpenThreadWaitChainSession(0,nullptr);
    if(session&&Thread32First(snapshot,&entry))do{if(entry.th32OwnerProcessID!=pid)continue;WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT]{};DWORD count=WCT_MAX_NODE_COUNT;BOOL cycle=FALSE;
        Json thread={{"tid",entry.th32ThreadID}};
        if(GetThreadWaitChain(session,0,WCTP_GETINFO_ALL_FLAGS,entry.th32ThreadID,&count,nodes,&cycle)){thread["cycle"]=cycle!=FALSE;thread["nodes"]=Json::array();for(DWORD i=0;i<count;++i){Json node={{"type",nodes[i].ObjectType},{"status",nodes[i].ObjectStatus}};if(nodes[i].ObjectType==WctThreadType){node["pid"]=nodes[i].ThreadObject.ProcessId;node["tid"]=nodes[i].ThreadObject.ThreadId;}thread["nodes"].push_back(node);}}
        else thread["error"]=GetLastError();result.push_back(thread);
    }while(Thread32Next(snapshot,&entry));if(session)CloseThreadWaitChainSession(session);CloseHandle(snapshot);return result;
}
struct Find {DWORD pid;HWND window{};};
BOOL CALLBACK find_window(HWND hwnd,LPARAM data){auto& match=*reinterpret_cast<Find*>(data);DWORD pid{};GetWindowThreadProcessId(hwnd,&pid);wchar_t cls[64]{};GetClassNameW(hwnd,cls,64);if(pid==match.pid&&wcscmp(cls,L"MigratoolWindow")==0){match.window=hwnd;return FALSE;}return TRUE;}
HWND owned_preview(DWORD pid){
    auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)throw std::runtime_error("No se pudo identificar el proceso de pruebas");
    std::wstring image(32768,L'\0');auto length=static_cast<DWORD>(image.size());
    const bool read=QueryFullProcessImageNameW(process,0,image.data(),&length)!=FALSE;CloseHandle(process);image.resize(length);
    std::error_code error;
    if(!read||!fs::equivalent(fs::path(image),executable_dir()/MIGRATOOL_GUI_NAME,error))throw std::runtime_error("El proceso no corresponde a la GUI de esta compilación");
    Find match{pid};EnumWindows(find_window,reinterpret_cast<LPARAM>(&match));
    if(!match.window)throw std::runtime_error("No hay una ventana principal en el proceso indicado");
    return match.window;
}
void window_command(HWND hwnd,int id){
    DWORD_PTR result{};
    if(!SendMessageTimeoutW(hwnd,WM_COMMAND,id,0,SMTO_ABORTIFHUNG,3000,&result))throw std::runtime_error("La ventana no respondió al comando "+std::to_string(id));
}
Json preview_layout(HWND hwnd,int zoom,int width,int height){
    window_command(hwnd,54);
    for(int percent=100;percent<zoom;percent+=25)window_command(hwnd,52);
    SetWindowPos(hwnd,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    window_command(hwnd,2001);
    RECT bounds{};GetClientRect(hwnd,&bounds);
    const auto compact=GetDlgItem(hwnd,32),search=GetDlgItem(hwnd,30),mark=GetDlgItem(hwnd,25);
    const auto check=[&](HWND item){RECT rect{};GetWindowRect(item,&rect);MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&rect),2);return IsWindowVisible(item)&&rect.left>=0&&rect.right<=bounds.right&&rect.top>=0&&rect.bottom<=bounds.bottom;};
    return {{"dpi",GetDpiForWindow(hwnd)},{"requested_text_percent",zoom},{"client_width",bounds.right},{"client_height",bounds.bottom},
        {"compact_navigation",IsWindowVisible(compact)!=FALSE},{"search_within_window",check(search)},{"register_action_within_window",check(mark)},
        {"responding",!IsHungAppWindow(hwnd)}};
}
HWND native_child(HWND parent,const wchar_t* class_name){
    for(auto child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        wchar_t cls[64]{};GetClassNameW(child,cls,64);if(wcscmp(cls,class_name)==0)return child;
    }
    return nullptr;
}
Json preview_inventory(HWND hwnd,const Project& project){
    using Microsoft::WRL::ComPtr;
    auto pane=native_child(hwnd,L"MigratoolScaleView");
    if(!pane)throw std::runtime_error("La GUI no ofrece la vista paginada");
    Database db(project);
    const auto start=GetTickCount64();auto quiet=start;
    while(GetTickCount64()-quiet<1400){
        if(GetTickCount64()-start>25000)throw std::runtime_error("El catálogo sigue cambiando");
        if(db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE state IN ('queued','running'))")!=0)quiet=GetTickCount64();
        Sleep(50);
    }
    window_command(hwnd,1007);
    ComPtr<IUIAutomation> automation;ComPtr<IUIAutomationElement> table;ComPtr<IUIAutomationCondition> row_condition;
    auto status=CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
    if(SUCCEEDED(status))status=automation->ElementFromHandle(GetDlgItem(pane,9),&table);
    VARIANT type{};type.vt=VT_I4;type.lVal=UIA_ListItemControlTypeId;
    if(SUCCEEDED(status))status=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&row_condition);
    if(FAILED(status))throw std::runtime_error("UI Automation no expuso las filas del inventario");
    Json pages=Json::array(),args={{"kind","census_entries"},{"limit",200},{"include_total",false}};
    std::set<int64_t> unique;
    for(int page=0;page<=50;++page){
        const auto requested=GetTickCount64();
        if(page>0)window_command(pane,2);
        const auto expected=scale_query(project,db,args).value();
        if(expected.at("items").empty())throw std::runtime_error("El catálogo de prueba tiene menos de 10.001 filas");
        const auto& first=expected.at("items").front();
        const auto expected_label=wide(first.at("name"));
        if(!unique.insert(first.at("id").get<int64_t>()).second)throw std::runtime_error("Cursor de inventario repetido");
        bool found=false;ComPtr<IUIAutomationElement> first_row;
        while(GetTickCount64()-requested<4000){
            first_row.Reset();
            if(SUCCEEDED(table->FindFirst(TreeScope_Children,row_condition.Get(),&first_row))&&first_row){
                BSTR name{};if(SUCCEEDED(first_row->get_CurrentName(&name))&&name){found=expected_label==std::wstring_view(name,SysStringLen(name));SysFreeString(name);}
                if(found)break;
            }
            Sleep(30);
        }
        if(!found)throw std::runtime_error("La GUI no mostró la primera fila esperada de la página "+std::to_string(page+1)+": "+utf8(expected_label));
        pages.push_back({{"page",page+1},{"first_index",page*200+1},{"id",first.at("id")},{"path",first.at("absolute_path")},{"first_row_name",utf8(expected_label)},{"elapsed_ms",GetTickCount64()-requested}});
        if(page==50){
            ComPtr<IUIAutomationSelectionItemPattern> selection;
            if(FAILED(first_row->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(&selection)))||FAILED(selection->Select()))throw std::runtime_error("No se pudo seleccionar la fila 10.001");
            std::wstring description(32768,L'\0');DWORD_PTR copied{};
            if(!SendMessageTimeoutW(GetDlgItem(pane,11),WM_GETTEXT,description.size(),reinterpret_cast<LPARAM>(description.data()),SMTO_ABORTIFHUNG,3000,&copied))throw std::runtime_error("No respondió el detalle de la fila 10.001");
            description.resize(static_cast<size_t>(copied));
            if(description.find(wide(first.at("absolute_path")))==std::wstring::npos)throw std::runtime_error("Ruta de la fila 10.001 distinta en el detalle");
        }
        if(page<50){if(!expected.at("next_cursor").is_string())throw std::runtime_error("Falta una continuación antes de 10.001");args["cursor"]=expected.at("next_cursor");}
    }
    return {{"schema","migratool.gui-inventory-test/1"},{"project",path_text(project.file)},{"first_index_after_10000",10001},{"exact_final_path_checked",true},{"pages",pages},{"build",build_identity()}};
}
}
int wmain(int argc,wchar_t** argv){using namespace mt;
    if(argc==4&&std::wstring_view(argv[1])==L"--preview-command"){
        try{
            const auto id=std::stoi(argv[3]);
            if((id<10||id>75)&&(id<1000||id>1024)&&(id<2000||id>2004))return 2;
            const auto window=owned_preview(std::stoul(argv[2]));
            if(!PostMessageW(window,WM_COMMAND,id,0))throw std::runtime_error("No se pudo solicitar la acción de pruebas");
            std::cout<<"Comando solicitado a la GUI de esta compilación.\n";return 0;
        }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    }
    if(argc==6&&std::wstring_view(argv[1])==L"--preview-layout"){
        try{const auto zoom=std::stoi(argv[3]);if(zoom<100||zoom>200||zoom%25!=0)return 2;
            const auto result=preview_layout(owned_preview(std::stoul(argv[2])),zoom,std::stoi(argv[4]),std::stoi(argv[5]));
            std::cout<<result.dump(2)<<'\n';return result["search_within_window"]==true&&result["register_action_within_window"]==true?0:1;
        }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    }
    if(argc==5&&std::wstring_view(argv[1])==L"--preview-inventory"){
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        try{const auto result=preview_inventory(owned_preview(std::stoul(argv[2])),Project::load(fs::path(argv[3])));write_json(fs::path(argv[4]),result);std::cout<<"51 páginas comprobadas, incluida la ruta de la fila 10.001.\n";CoUninitialize();return 0;}
        catch(const std::exception& error){std::cerr<<error.what()<<'\n';CoUninitialize();return 1;}
    }
    if(argc==4&&std::wstring(argv[1])==L"--manifest"){
        try{const auto root=fs::canonical(fs::path(argv[2]));std::vector<std::string> paths;
            for(auto it=fs::recursive_directory_iterator(root);it!=fs::recursive_directory_iterator();++it){auto attrs=GetFileAttributesW(it->path().c_str());if(attrs&FILE_ATTRIBUTE_REPARSE_POINT){if(it->is_directory())it.disable_recursion_pending();continue;}if(it->is_regular_file())paths.push_back(utf8(it->path().wstring()));}
            auto destination=utf8(fs::absolute(fs::path(argv[3])).wstring());if(std::find(paths.begin(),paths.end(),destination)==paths.end())paths.push_back(destination);
            std::sort(paths.begin(),paths.end());std::string output;for(const auto& p:paths)output+=p+"\n";write_text(fs::path(argv[3]),output);std::cout<<"files="<<paths.size()<<" sha256="<<hash_file(fs::path(argv[3]))<<'\n';return 0;
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    }
    if(argc==3&&std::wstring(argv[1])==L"--inspect"){std::cout<<waits(std::stoul(argv[2])).dump(2)<<'\n';return 0;}
    if(argc!=2&&!(argc==3&&std::wstring(argv[2])==L"--busy"))return 2;
    try{
        auto qa=fs::path(argv[1])/L"build/gui-lifecycle"/wide(unique_id());fs::create_directories(qa/L"source");fs::create_directories(qa/L"target");write_text(qa/L"source/test.py","print(1)\n");write_text(qa/L"target/test.py","print(1)\n");
        auto p=Project::create(qa/L"project.json","Prueba de cierre GUI",Json::array({{{"id","s"},{"role","source"},{"path",path_text(qa/L"source")}},{{"id","t"},{"role","target"},{"path",path_text(qa/L"target")}}}));
        auto exe=executable_dir()/MIGRATOOL_GUI_NAME;auto cmd=quote_arg(exe.wstring())+L" "+quote_arg(p.file.wstring());STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_SHOWNOACTIVATE;PROCESS_INFORMATION pi{};
        if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,qa.c_str(),&si,&pi))throw std::runtime_error("No se pudo abrir GUI");CloseHandle(pi.hThread);
        Find match{pi.dwProcessId};auto start=GetTickCount64();while(GetTickCount64()-start<10000){EnumWindows(find_window,reinterpret_cast<LPARAM>(&match));if(match.window)break;Sleep(50);}
        if(!match.window){TerminateProcess(pi.hProcess,99);CloseHandle(pi.hProcess);throw std::runtime_error("GUI no creó ventana");}
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);IUIAutomation* automation{};IUIAutomationElement* element{};IUIAutomationCondition* condition{};IUIAutomationElementArray* children{};
        auto hr=CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
        if(SUCCEEDED(hr)){automation->ElementFromHandle(match.window,&element);automation->CreateTrueCondition(&condition);if(element&&condition)element->FindAll(TreeScope_Children,condition,&children);}
        // Conservar las referencias UIA durante WM_CLOSE reproduce la conexión de un inspector real.
        if(argc==3)PostMessageW(match.window,WM_COMMAND,20,0);
        PostMessageW(match.window,WM_CLOSE,0,0);auto closed=WaitForSingleObject(pi.hProcess,8000)==WAIT_OBJECT_0;Json receipt={{"pid",pi.dwProcessId},{"uia_connected",SUCCEEDED(hr)},{"busy_close",argc==3},{"closed",closed},{"at",now()}};
        if(!closed){receipt["wait_chain"]=waits(pi.dwProcessId);TerminateProcess(pi.hProcess,99);WaitForSingleObject(pi.hProcess,3000);}
        DWORD code{};GetExitCodeProcess(pi.hProcess,&code);receipt["exit_code"]=code;CloseHandle(pi.hProcess);
        if(children)children->Release();if(condition)condition->Release();if(element)element->Release();if(automation)automation->Release();CoUninitialize();write_json(qa/L"receipt.json",receipt);
        std::cout<<receipt.dump(2)<<"\nEvidencia: "<<path_text(qa)<<'\n';return closed&&code==0?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
