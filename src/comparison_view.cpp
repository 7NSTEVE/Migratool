#include "migratool/comparison_view.hpp"
#include "migratool/live.hpp"
#include "migratool/ui.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <set>

namespace mt {
namespace {
std::string percent_text(const Json& value) {
    if (value.is_null()) return "—";
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << value.get<double>() << " %";
    return text.str();
}
std::string state_text(const std::string& state) {
    if (state == "COMPLETE") return "MIGRADO DECLARADO";
    if (state == "IN_PROGRESS") return "EN CURSO";
    if (state == "STALE") return "REVISAR VIGENCIA";
    return "PENDIENTE";
}
void add_row(HWND list, const std::vector<std::string>& cells) {
    auto first = wide(cells[0]);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(list);
    item.pszText = first.data();
    const auto index = ListView_InsertItem(list, &item);
    for (size_t i = 1; i < cells.size(); ++i) {
        auto value = wide(cells[i]);
        ListView_SetItemText(list, index, static_cast<int>(i), value.data());
    }
}
std::string selected_id(HWND list, const std::vector<std::string>& ids) {
    const auto index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    return index >= 0 && static_cast<size_t>(index) < ids.size() ? ids[index] : "";
}
bool matches(const std::string& value, const std::string& needle) {
    auto text = wide(value), filter = wide(needle);
    std::transform(text.begin(), text.end(), text.begin(), towlower);
    std::transform(filter.begin(), filter.end(), filter.begin(), towlower);
    return text.find(filter) != std::wstring::npos;
}
}

int ComparisonView::px(int value) const { return MulDiv(value, dpi_, 96); }

HWND ComparisonView::make(const wchar_t* cls, const wchar_t* label, DWORD style, int id) {
    const auto child = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style,
        0, 0, 10, 10, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ui::name(child,label);
    return child;
}

void ComparisonView::create(HWND parent, HFONT font, int dpi) {
    font_ = font;
    dpi_ = dpi;
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = L"MigratoolComparison";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"Comparación de origen y destino",
        WS_CHILD | WS_CLIPCHILDREN, 0, 0, 100, 100, parent, nullptr, cls.hInstance, this);
    state_filter_ = make(WC_COMBOBOXW, L"Estado de migración", CBS_DROPDOWNLIST | WS_TABSTOP, 52);
    for (const auto* label : {L"Todos los orígenes", L"Pendientes", L"En curso", L"Migrados declarados",
                               L"Con casos aprobados", L"Revisar vigencia"})
        SendMessageW(state_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(state_filter_, CB_SETCURSEL, 0, 0);
    linked_only_ = make(L"BUTTON", L"Solo destinos del origen seleccionado", BS_AUTOCHECKBOX | WS_TABSTOP, 53);
    origin_label_ = make(L"STATIC", L"Código de origen", 0, 54);
    target_label_ = make(L"STATIC", L"Código de destino", 0, 55);
    origins_ = make(WC_LISTVIEWW, L"Archivos de origen", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 50);
    targets_ = make(WC_LISTVIEWW, L"Archivos de destino", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 51);
    for (const auto list : {origins_, targets_})
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    int column = 0;
    for (const auto* label : {L"% migrado", L"Estado", L"Ruta absoluta de origen"}) {
        LVCOLUMNW definition{};
        definition.mask = LVCF_TEXT | LVCF_WIDTH;
        definition.pszText = const_cast<wchar_t*>(label);
        definition.cx = px(column == 0 ? 90 : column == 1 ? 160 : 500);
        ListView_InsertColumn(origins_, column++, &definition);
    }
    column = 0;
    for (const auto* label : {L"Vínculo", L"Ruta absoluta de destino", L"Orígenes"}) {
        LVCOLUMNW definition{};
        definition.mask = LVCF_TEXT | LVCF_WIDTH;
        definition.pszText = const_cast<wchar_t*>(label);
        definition.cx = px(column == 0 ? 110 : column == 1 ? 500 : 100);
        ListView_InsertColumn(targets_, column++, &definition);
    }
    detail_ = make(L"EDIT", L"Seleccione un origen para ver sus rutas y evidencia.", ES_MULTILINE | ES_READONLY | WS_VSCROLL, 56);
    open_origin_ = make(L"BUTTON", L"Abrir origen", WS_TABSTOP, 57);
    open_target_ = make(L"BUTTON", L"Abrir destino", WS_TABSTOP, 58);
    colors_ = ui::system_palette();
    background_ = CreateSolidBrush(colors_.background);
    surface_ = CreateSolidBrush(colors_.surface);
    ui::list_colors(origins_, colors_);
    ui::list_colors(targets_, colors_);
    layout();
}

void ComparisonView::layout() {
    RECT rect{};
    GetClientRect(window_, &rect);
    const int width = rect.right;
    const int bottom = rect.bottom;
    const int gap = px(12),control=ui::control_height(dpi_),line=ui::type_size(18,dpi_)+px(8);
    const bool stacked=width<ui::type_size(18,dpi_)*42;const int half=(width-gap)/2;
    int x=0,y=0;for(const auto item:{state_filter_,linked_only_,open_origin_,open_target_}){const int item_width=std::min(width,item==linked_only_?ui::type_size(18,dpi_)*17:item==state_filter_?ui::type_size(18,dpi_)*15:ui::type_size(18,dpi_)*9);if(x&&x+item_width>width){x=0;y+=control+gap;}MoveWindow(item,x,y,item_width,item==state_filter_?control+px(260):control,TRUE);x+=item_width+gap;}
    y+=control+gap;const int detail_height=std::min(std::max(line*2,px(65)),std::max(px(30),(bottom-y)/3));
    const int available=std::max(px(30),bottom-y-detail_height-gap);
    const int table_height=std::max(px(20),stacked?(available-line*2-gap)/2:available-line);
    MoveWindow(origin_label_,0,y,stacked?width:half,line,TRUE);MoveWindow(origins_,0,y+line,stacked?width:half,table_height,TRUE);
    const int target_y=stacked?y+line+table_height+gap:y;
    MoveWindow(target_label_,stacked?0:half+gap,target_y,stacked?width:half,line,TRUE);MoveWindow(targets_,stacked?0:half+gap,target_y+line,stacked?width:half,table_height,TRUE);
    MoveWindow(detail_,0,bottom-detail_height,width,detail_height,TRUE);
}
void ComparisonView::appearance(HFONT font,int dpi){
    const int prev_dpi = dpi_ > 0 ? dpi_ : 96;
    font_=font;dpi_=dpi;ui::font_tree(window_,font);
    colors_=ui::system_palette();
    if(background_)DeleteObject(background_);
    if(surface_)DeleteObject(surface_);
    background_=CreateSolidBrush(colors_.background);
    surface_=CreateSolidBrush(colors_.surface);
    if(prev_dpi!=dpi){
        for(const auto table:{origins_,targets_}){
            const int cols=Header_GetItemCount(ListView_GetHeader(table));
            for(int c=0;c<cols;++c)ListView_SetColumnWidth(table,c,MulDiv(ListView_GetColumnWidth(table,c),dpi,prev_dpi));
        }
    }
    ui::list_colors(origins_,colors_);ui::list_colors(targets_,colors_);layout();
    if(window_)InvalidateRect(window_,nullptr,TRUE);
}

void ComparisonView::update(const Json& snapshot, const Json& status, const std::string& filter) {
    model_ = migration_comparison(snapshot, status);
    text_filter_ = filter;
    populate();
}

void ComparisonView::populate() {
    if (updating_ || !model_.is_object()) return;
    updating_ = true;
    SendMessageW(origins_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(targets_, WM_SETREDRAW, FALSE, 0);
    const auto old_origin = selected_origin_;
    const auto old_target = selected_id(targets_, target_ids_);
    const auto top_origin = ListView_GetTopIndex(origins_);
    const auto top_target = ListView_GetTopIndex(targets_);
    ListView_DeleteAllItems(origins_);
    ListView_DeleteAllItems(targets_);
    origin_ids_.clear();
    target_ids_.clear();
    const auto selection = SendMessageW(state_filter_, CB_GETCURSEL, 0, 0);
    for (const auto& origin : model_["origins"]) {
        const auto state = origin.at("comparison_state").get<std::string>();
        if (selection == 1 && state != "PENDING") continue;
        if (selection == 2 && state != "IN_PROGRESS") continue;
        if (selection == 3 && state != "COMPLETE") continue;
        if (selection == 4 && origin.at("units_with_passing_cases_percent") != 100) continue;
        if (selection == 5 && state != "STALE") continue;
        if (!matches(origin.at("source_path").get<std::string>() + " " + origin.at("id").get<std::string>() +
                     " " + state_text(state) + " " + origin.value("note", ""), text_filter_)) continue;
        add_row(origins_, {percent_text(origin.at("migration_percent")), state_text(state), origin.at("source_path")});
        const auto id = origin.at("id").get<std::string>();
        origin_ids_.push_back(id);
        if (id == old_origin) ListView_SetItemState(origins_, static_cast<int>(origin_ids_.size()) - 1, LVIS_SELECTED, LVIS_SELECTED);
    }
    selected_origin_ = selected_id(origins_, origin_ids_);
    const bool linked_only = SendMessageW(linked_only_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (const auto& target : model_["targets"]) {
        const bool selected_link = std::find(target["origins"].begin(), target["origins"].end(), Json(selected_origin_)) != target["origins"].end();
        if (linked_only && !selected_link) continue;
        add_row(targets_, {target["linked"] == true ? "VINCULADO" : "SIN VÍNCULO", target["absolute_path"], target["origins"].dump()});
        const auto id = target.at("id").get<std::string>();
        target_ids_.push_back(id);
        if (selected_link || id == old_target)
            ListView_SetItemState(targets_, static_cast<int>(target_ids_.size()) - 1, LVIS_SELECTED, LVIS_SELECTED);
    }
    SetWindowTextW(origin_label_, wide("Origen · " + std::to_string(origin_ids_.size()) + " archivos de código").c_str());
    SetWindowTextW(target_label_, wide("Destino · " + std::to_string(target_ids_.size()) + " archivos · vínculos registrados").c_str());
    if (top_origin > 0) ListView_EnsureVisible(origins_, std::min(top_origin, ListView_GetItemCount(origins_) - 1), FALSE);
    if (top_target > 0) ListView_EnsureVisible(targets_, std::min(top_target, ListView_GetItemCount(targets_) - 1), FALSE);
    SendMessageW(origins_, WM_SETREDRAW, TRUE, 0);
    SendMessageW(targets_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(origins_, nullptr, TRUE);
    InvalidateRect(targets_, nullptr, TRUE);
    updating_ = false;
    describe();
}

void ComparisonView::select_origin() {
    if (updating_) return;
    selected_origin_ = selected_id(origins_, origin_ids_);
    if (SendMessageW(linked_only_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        populate();
        return;
    }
    updating_ = true;
    ListView_SetItemState(targets_, -1, 0, LVIS_SELECTED);
    std::set<std::string> linked;
    for (const auto& target : model_["targets"])
        if (std::find(target["origins"].begin(), target["origins"].end(), Json(selected_origin_)) != target["origins"].end())
            linked.insert(target.at("id"));
    for (size_t index = 0; index < target_ids_.size(); ++index)
        if (linked.contains(target_ids_[index]))
            ListView_SetItemState(targets_, static_cast<int>(index), LVIS_SELECTED, LVIS_SELECTED);
    updating_ = false;
    describe();
}

void ComparisonView::describe() {
    std::string description = "Seleccione un origen. Los destinos vinculados se resaltan a la derecha.";
    if (!model_.value("inventory_complete", false))
        description = "Inventario pendiente o incompleto. Los archivos aparecerán al terminar la sincronización del proyecto. Consulte Agentes en vivo o pulse Analizar para iniciar el inventario.";
    for (const auto& origin : model_.value("origins", Json::array())) {
        if (origin["id"] != selected_origin_) continue;
        description = origin["id"].get<std::string>() + " · " + origin["source_path"].get<std::string>() +
            "\r\nMigración declarada " + percent_text(origin["migration_percent"]) +
            " · Con casos aprobados " + percent_text(origin["units_with_passing_cases_percent"]) +
            " · " + origin["migrated_units"].dump() + "/" + origin["total_units"].dump() + " unidades";
        for (const auto& target : origin["target_paths"]) description += "\r\nDestino " + target.get<std::string>();
        description += "\r\n" + origin.value("actor", "") + " · " + origin.value("at", "") + " · " + origin.value("note", "");
        description += "\r\nPendientes " + origin["missing"].dump();
    }
    SetWindowTextW(detail_, wide(description).c_str());
}

void ComparisonView::open_selected(bool origin) {
    const auto id = origin ? selected_origin_ : selected_id(targets_, target_ids_);
    for (const auto& item : model_.value(origin ? "origins" : "targets", Json::array())) {
        if (item["id"] != id) continue;
        const auto path = wide(item[origin ? "source_path" : "absolute_path"]);
        ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
}

LRESULT CALLBACK ComparisonView::procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto view = reinterpret_cast<ComparisonView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        view = static_cast<ComparisonView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        view->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    }
    if (!view) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_SIZE: if (view->origins_) view->layout(); return 0;
    case WM_ERASEBKGND: { RECT rc{}; GetClientRect(window, &rc); FillRect(reinterpret_cast<HDC>(wp), &rc, view->background_ ? view->background_ : GetSysColorBrush(COLOR_WINDOW)); return 1; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(ui::color_control(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), view->colors_, view->background_, view->surface_));
    case WM_DESTROY:
        if (view->background_) { DeleteObject(view->background_); view->background_ = nullptr; }
        if (view->surface_) { DeleteObject(view->surface_); view->surface_ = nullptr; }
        return 0;
    case WM_COMMAND:
        if ((LOWORD(wp) == 52 && HIWORD(wp) == CBN_SELCHANGE) || LOWORD(wp) == 53) view->populate();
        if (LOWORD(wp) == 57 || LOWORD(wp) == 58) view->open_selected(LOWORD(wp) == 57);
        return 0;
    case WM_NOTIFY: {
        const auto notification = reinterpret_cast<NMHDR*>(lp);
        if (notification->hwndFrom == view->origins_ && notification->code == LVN_ITEMCHANGED)
            view->select_origin();
        if (notification->code == NM_DBLCLK)
            view->open_selected(notification->hwndFrom == view->origins_);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
