#pragma once
#include <windows.h>
#include <commctrl.h>
#include <oleacc.h>
#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <unordered_map>

// Native desktop equivalents of the HIG's semantic colors, type roles and
// accessible names. No Apple framework or decorative material is emulated.
namespace mt::ui {
struct Palette {
    COLORREF background=RGB(247,249,252), surface=RGB(255,255,255),
        text=RGB(29,38,51), secondary=RGB(79,96,116), accent=RGB(28,91,180),
        sidebar=RGB(235,240,247), selection=RGB(215,229,249),
        selection_text=RGB(20,65,130), hover=RGB(224,232,243), border=RGB(174,187,203);
    bool high_contrast=false;
};
inline double luminance(COLORREF c){const auto linear=[](double v){v/=255.;return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);};return .2126*linear(GetRValue(c))+.7152*linear(GetGValue(c))+.0722*linear(GetBValue(c));}
inline double contrast(COLORREF a,COLORREF b){auto x=luminance(a),y=luminance(b);return (std::max(x,y)+.05)/(std::min(x,y)+.05);}
inline Palette system_palette(){
    Palette p;HIGHCONTRASTW hc{sizeof(hc)};
    if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON)){
        p.high_contrast=true;p.background=p.surface=p.sidebar=GetSysColor(COLOR_WINDOW);
        p.text=p.secondary=GetSysColor(COLOR_WINDOWTEXT);p.accent=GetSysColor(COLOR_HOTLIGHT);
        if(contrast(p.accent,p.surface)<4.5)p.accent=p.text;
        p.selection=p.hover=GetSysColor(COLOR_HIGHLIGHT);p.selection_text=GetSysColor(COLOR_HIGHLIGHTTEXT);
        p.border=GetSysColor(COLOR_WINDOWTEXT);
    }return p;
}
inline int text_percent=100;
inline int dip(int value,int dpi){return MulDiv(value,dpi,96);}
inline int type_size(int value,int dpi){return MulDiv(dip(value,dpi),text_percent,100);}
inline int control_height(int dpi){return std::max(dip(36,dpi),type_size(18,dpi)+dip(16,dpi));}
inline void tab_order(std::span<const HWND> controls){
    HWND previous=HWND_TOP;
    for(auto control:controls)if(control){
        SetWindowPos(control,previous,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        previous=control;
    }
}
inline std::wstring number(const std::wstring& value,UINT decimals){
    wchar_t decimal[8]=L".",separator[8]=L",",out[128]{};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT,LOCALE_SDECIMAL,decimal,8);
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT,LOCALE_STHOUSAND,separator,8);
    NUMBERFMTW format{decimals,1,3,decimal,separator,1};
    return GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT,0,value.c_str(),&format,out,128)?std::wstring(out):value;
}
inline void name(HWND hwnd,const std::wstring& value){
    if(!hwnd||value.empty())return;
    thread_local std::unordered_map<HWND,std::wstring> last_names;
    if(auto it=last_names.find(hwnd);it!=last_names.end()&&it->second==value)return;
    IAccPropServices* service{};
    if(SUCCEEDED(CoCreateInstance(CLSID_AccPropServices,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service)))&&service){
        service->SetHwndPropStr(hwnd,static_cast<DWORD>(OBJID_CLIENT),CHILDID_SELF,PROPID_ACC_NAME,value.c_str());
        service->Release();
        last_names[hwnd]=value;
    }
}
inline void list_colors(HWND list,const Palette& p){ListView_SetBkColor(list,p.surface);ListView_SetTextBkColor(list,p.surface);ListView_SetTextColor(list,p.text);}
inline void font_tree(HWND window,HFONT font){SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);EnumChildWindows(window,[](HWND child,LPARAM data)->BOOL{SendMessageW(child,WM_SETFONT,static_cast<WPARAM>(data),FALSE);return TRUE;},reinterpret_cast<LPARAM>(font));}
inline HBRUSH color_control(HDC dc,HWND child,const Palette& p,HBRUSH background,HBRUSH surface){
    wchar_t cls[64]{};GetClassNameW(child,cls,64);const bool edit=wcscmp(cls,L"Edit")==0||wcscmp(cls,L"ComboBox")==0;
    SetTextColor(dc,p.text);SetBkColor(dc,edit?p.surface:p.background);return edit?surface:background;
}
inline void show_if(HWND h,bool vis){
    if(!h)return;
    const bool cur=(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)!=0;
    if(cur!=vis)ShowWindow(h,vis?SW_SHOW:SW_HIDE);
}
inline void move_if(HWND h,int x,int y,int w,int ht){
    if(!h)return;
    RECT r{};
    if(GetWindowRect(h,&r)){
        MapWindowPoints(HWND_DESKTOP,GetParent(h),reinterpret_cast<LPPOINT>(&r),2);
        if(r.left==x&&r.top==y&&(r.right-r.left)==w&&(r.bottom-r.top)==ht)return;
    }
    MoveWindow(h,x,y,w,ht,FALSE);
    InvalidateRect(h,nullptr,FALSE);
}
}
