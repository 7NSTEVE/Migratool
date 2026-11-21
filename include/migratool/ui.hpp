#pragma once
#include <windows.h>
#include <commctrl.h>
#include <oleacc.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// Native desktop equivalents of the Apple HIG semantic colors, type roles,
// concentric geometry, 1px structural borders, and WCAG AA/AAA accessibility.
namespace mt::ui {
struct Palette {
    COLORREF background=RGB(245,247,250), surface=RGB(255,255,255), surface_alt=RGB(248,250,253),
        text=RGB(17,24,39), secondary=RGB(68,82,101), accent=RGB(0,86,179),
        sidebar=RGB(236,240,246), selection=RGB(214,230,252),
        selection_text=RGB(12,56,122), hover=RGB(224,232,244), border=RGB(162,176,194),
        divider=RGB(203,213,225),
        success_text=RGB(14,98,53), success_bg=RGB(236,253,243),
        warning_text=RGB(140,58,10), warning_bg=RGB(254,244,219),
        danger_text=RGB(176,28,20), danger_bg=RGB(254,240,240),
        info_text=RGB(0,78,162), info_bg=RGB(238,246,255);
    bool high_contrast=false;
    bool increased_contrast=false;
    bool dark_mode=false;
};
inline bool g_increased_contrast=false;
inline int g_theme_mode=0; // 0 = Porcelana Claro (HIG default), 1 = Oscuro Calibrado
inline double luminance(COLORREF c){const auto linear=[](double v){v/=255.;return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);};return .2126*linear(GetRValue(c))+.7152*linear(GetGValue(c))+.0722*linear(GetBValue(c));}
inline double contrast(COLORREF a,COLORREF b){auto x=luminance(a),y=luminance(b);return (std::max(x,y)+.05)/(std::min(x,y)+.05);}
inline Palette system_palette(){
    Palette p;HIGHCONTRASTW hc{sizeof(hc)};
    if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON)){
        p.high_contrast=true;p.increased_contrast=true;
        p.background=p.surface=p.surface_alt=p.sidebar=GetSysColor(COLOR_WINDOW);
        p.text=p.secondary=GetSysColor(COLOR_WINDOWTEXT);p.accent=GetSysColor(COLOR_HOTLIGHT);
        if(contrast(p.accent,p.surface)<4.5)p.accent=p.text;
        p.selection=p.hover=GetSysColor(COLOR_HIGHLIGHT);p.selection_text=GetSysColor(COLOR_HIGHLIGHTTEXT);
        p.border=p.divider=GetSysColor(COLOR_WINDOWTEXT);
        p.success_text=p.warning_text=p.danger_text=p.info_text=p.text;
        p.success_bg=p.warning_bg=p.danger_bg=p.info_bg=p.surface;
        return p;
    }
    if(g_theme_mode==1){
        p.dark_mode=true;
        p.background=RGB(18,22,29);p.surface=RGB(26,32,42);p.surface_alt=RGB(31,38,50);
        p.sidebar=RGB(22,27,36);p.text=RGB(241,245,249);p.secondary=RGB(165,179,198);
        p.accent=RGB(96,165,250);p.selection=RGB(30,58,108);p.selection_text=RGB(239,246,255);
        p.hover=RGB(36,46,62);p.border=RGB(71,85,105);p.divider=RGB(51,65,85);
        p.success_text=RGB(110,231,183);p.success_bg=RGB(19,46,35);
        p.warning_text=RGB(252,211,77);p.warning_bg=RGB(54,38,17);
        p.danger_text=RGB(252,165,165);p.danger_bg=RGB(59,24,27);
        p.info_text=RGB(147,197,253);p.info_bg=RGB(23,37,64);
    }
    if(g_increased_contrast){
        p.increased_contrast=true;
        if(p.dark_mode){
            p.text=RGB(255,255,255);p.secondary=RGB(203,213,225);
            p.border=RGB(148,163,184);p.divider=RGB(100,116,139);
        }else{
            p.text=RGB(10,14,22);p.secondary=RGB(45,55,72);
            p.accent=RGB(0,68,148);p.border=RGB(100,116,139);p.divider=RGB(148,163,184);
        }
    }
    return p;
}
inline int text_percent=100;
inline int dip(int value,int dpi){return MulDiv(value,dpi,96);}
inline int type_size(int value,int dpi){return MulDiv(dip(value,dpi),text_percent,100);}
inline int control_height(int dpi){return std::max(dip(36,dpi),type_size(18,dpi)+dip(16,dpi));}
inline void apply_control_polish(HWND hwnd,bool is_edit=false,int margin_px=8){
    if(!hwnd)return;
    SetWindowTheme(hwnd,L"Explorer",nullptr);
    if(is_edit)SendMessageW(hwnd,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(margin_px,margin_px));
}
inline void draw_card(HDC dc,const RECT& r,const Palette& p,int radius,bool accent_top=false,COLORREF top_color=0){
    if(r.right<=r.left||r.bottom<=r.top)return;
    const int rad=std::max(4,radius);
    auto pen=CreatePen(PS_SOLID,p.increased_contrast?2:1,p.border);
    auto brush=CreateSolidBrush(p.surface);
    auto old_pen=SelectObject(dc,pen);
    auto old_brush=SelectObject(dc,brush);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,rad,rad);
    SelectObject(dc,old_pen);SelectObject(dc,old_brush);
    DeleteObject(pen);DeleteObject(brush);
    if(accent_top&&(r.bottom-r.top)>16){
        RECT stripe{r.left+rad/2,r.top+1,r.right-rad/2,r.top+4};
        auto ab=CreateSolidBrush(top_color?top_color:p.accent);
        FillRect(dc,&stripe,ab);
        DeleteObject(ab);
    }
}
inline void draw_nav_icon(HDC dc,int section,const RECT& rc,COLORREF color,int stroke=2){
    const int w=rc.right-rc.left,h=rc.bottom-rc.top;
    if(w<8||h<8)return;
    LOGBRUSH lb{BS_SOLID,color,0};
    auto pen=ExtCreatePen(PS_GEOMETRIC|PS_SOLID|PS_ENDCAP_ROUND|PS_JOIN_ROUND,std::max(1,stroke),&lb,0,nullptr);
    auto old_pen=SelectObject(dc,pen);
    auto old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    const int x0=rc.left,y0=rc.top,x1=rc.right,y1=rc.bottom,cx=(x0+x1)/2,cy=(y0+y1)/2;
    switch(section){
        case 0:{ // Inicio: 2x2 precision modular grid
            const int g=std::max(1,w/7);
            RoundRect(dc,x0,y0,cx-g/2,cy-g/2,3,3);
            RoundRect(dc,cx+g/2,y0,x1,cy-g/2,3,3);
            RoundRect(dc,x0,cy+g/2,cx-g/2,y1,3,3);
            RoundRect(dc,cx+g/2,cy+g/2,x1,y1,3,3);
            break;
        }
        case 1:{ // Migración: bidirectional source->target transfer arrows
            const int ay1=y0+h/4,ay2=y1-h/4,ah=std::max(2,w/4);
            MoveToEx(dc,x0,ay1,nullptr);LineTo(dc,x1-1,ay1);
            MoveToEx(dc,x1-1-ah,ay1-ah,nullptr);LineTo(dc,x1-1,ay1);LineTo(dc,x1-1-ah,ay1+ah);
            MoveToEx(dc,x1,ay2,nullptr);LineTo(dc,x0+1,ay2);
            MoveToEx(dc,x0+1+ah,ay2-ah,nullptr);LineTo(dc,x0+1,ay2);LineTo(dc,x0+1+ah,ay2+ah);
            break;
        }
        case 2:{ // Actividad: pulse waveform
            MoveToEx(dc,x0,cy,nullptr);
            LineTo(dc,x0+w/4,cy);
            LineTo(dc,x0+(w*2)/5,y0+1);
            LineTo(dc,x0+(w*3)/5,y1-1);
            LineTo(dc,x1-w/4,cy);
            LineTo(dc,x1,cy);
            break;
        }
        case 3:{ // Revisión: verified check inside badge
            RoundRect(dc,x0,y0,x1,y1,4,4);
            MoveToEx(dc,x0+w/4,cy,nullptr);
            LineTo(dc,x0+(w*3)/7,y1-h/4);
            LineTo(dc,x1-w/5,y0+h/4+1);
            break;
        }
        default:{ // Proyecto: structured workspace sliders/folder
            const int y_a=y0+h/4,y_b=cy,y_c=y1-h/4,r=std::max(2,w/7);
            MoveToEx(dc,x0,y_a,nullptr);LineTo(dc,x1,y_a);
            MoveToEx(dc,x0,y_b,nullptr);LineTo(dc,x1,y_b);
            MoveToEx(dc,x0,y_c,nullptr);LineTo(dc,x1,y_c);
            auto fill=CreateSolidBrush(color);
            SelectObject(dc,fill);
            Ellipse(dc,x0+w/4-r,y_a-r,x0+w/4+r,y_a+r);
            Ellipse(dc,x1-w/4-r,y_b-r,x1-w/4+r,y_b+r);
            Ellipse(dc,cx-r,y_c-r,cx+r,y_c+r);
            SelectObject(dc,GetStockObject(NULL_BRUSH));
            DeleteObject(fill);
            break;
        }
    }
    SelectObject(dc,old_pen);SelectObject(dc,old_brush);
    DeleteObject(pen);
}
inline void draw_splitter_grip(HDC dc,const RECT& bounds,const Palette& p){
    if(bounds.right<=bounds.left||bounds.bottom<=bounds.top)return;
    auto bg=CreateSolidBrush(p.background);
    FillRect(dc,&bounds,bg);
    DeleteObject(bg);
    const int cx=(bounds.left+bounds.right)/2,cy=(bounds.top+bounds.bottom)/2;
    RECT line{cx,bounds.top+4,cx+1,bounds.bottom-4};
    auto border_br=CreateSolidBrush(p.divider);
    FillRect(dc,&line,border_br);
    DeleteObject(border_br);
    auto dot_br=CreateSolidBrush(p.secondary);
    for(int dy=-10;dy<=10;dy+=10){
        RECT dot{cx-1,cy+dy-2,cx+2,cy+dy+2};
        FillRect(dc,&dot,dot_br);
    }
    DeleteObject(dot_br);
}
inline bool classify_status_cell(std::wstring_view text,const Palette& p,COLORREF& fg,COLORREF& bg){
    if(text.empty()||p.high_contrast)return false;
    std::wstring low;low.reserve(text.size());
    for(wchar_t ch:text)low.push_back(static_cast<wchar_t>(std::towlower(ch)));
    const auto has=[&](std::wstring_view needle){return low.find(needle)!=std::wstring::npos;};
    if(low==L"pass"||low==L"completed"||low==L"verified"||low==L"done"||low==L"succeeded"||low==L"vigente"||
       low==L"finalizado"||low==L"completada"||low==L"verificado"||has(L"verificado en")||has(L"100,0 %")||has(L"100.0 %")){
        fg=p.success_text;bg=p.success_bg;return true;
    }
    if(low==L"error"||low==L"failed"||low==L"different"||low==L"blocked"||low==L"fallo"||
       low==L"bloqueado"||low==L"sin destino"||has(L"error:")||has(L"ilegible")){
        fg=p.danger_text;bg=p.danger_bg;return true;
    }
    if(low==L"stale"||low==L"half_done"||low==L"in_progress"||low==L"unreviewed"||low==L"running"||low==L"queued"||
       low==L"obsoleto"||low==L"a medias"||low==L"avance parcial"||low==L"en curso"||low==L"sin revisar"||
       low==L"reclamado"||low==L"portado"||has(L"declarado completo")){
        fg=p.warning_text;bg=p.warning_bg;return true;
    }
    return false;
}
inline LRESULT handle_listview_customdraw(LPARAM lparam,const Palette& p){
    auto* cd=reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
    if(!cd)return CDRF_DODEFAULT;
    if(cd->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
    if(cd->nmcd.dwDrawStage==CDDS_ITEMPREPAINT){
        const int row=static_cast<int>(cd->nmcd.dwItemSpec);
        cd->clrText=p.text;
        cd->clrTextBk=(row%2==1&&!p.high_contrast)?p.surface_alt:p.surface;
        return CDRF_NOTIFYSUBITEMDRAW;
    }
    if(cd->nmcd.dwDrawStage==(CDDS_ITEMPREPAINT|CDDS_SUBITEM)){
        const int row=static_cast<int>(cd->nmcd.dwItemSpec);
        const COLORREF base_bg=(row%2==1&&!p.high_contrast)?p.surface_alt:p.surface;
        cd->clrText=p.text;
        cd->clrTextBk=base_bg;
        if(cd->iSubItem>0){
            wchar_t buf[256]{};
            LVITEMW item{};item.iSubItem=cd->iSubItem;item.pszText=buf;item.cchTextMax=255;
            const auto len=static_cast<int>(SendMessageW(cd->nmcd.hdr.hwndFrom,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item)));
            if(len>0&&len<80){
                COLORREF fg{},bg{};
                if(classify_status_cell(std::wstring_view(buf,static_cast<size_t>(len)),p,fg,bg)){
                    cd->clrText=fg;
                    cd->clrTextBk=bg;
                }
            }
        }
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}
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
