#pragma once
#include "migratool/core.hpp"
#include "migratool/ui.hpp"

namespace mt {
class ComparisonView {
    HWND window_{}, origins_{}, targets_{}, state_filter_{}, linked_only_{};
    HWND origin_label_{}, target_label_{}, detail_{}, open_origin_{}, open_target_{};
    HFONT font_{};
    ui::Palette colors_{};
    HBRUSH background_{}, surface_{};
    int dpi_ = 96;
    bool updating_ = false;
    Json model_;
    std::string text_filter_;
    std::vector<std::string> origin_ids_, target_ids_;
    std::string selected_origin_;
    int px(int value) const;
    HWND make(const wchar_t* cls, const wchar_t* label, DWORD style, int id);
    void layout();
    void populate();
    void select_origin();
    void describe();
    void open_selected(bool origin);
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
public:
    void create(HWND parent, HFONT font, int dpi);
    void appearance(HFONT,int);
    HWND window() const { return window_; }
    void update(const Json& snapshot, const Json& status, const std::string& filter);
    std::string selected() const { return selected_origin_; }
};
}
