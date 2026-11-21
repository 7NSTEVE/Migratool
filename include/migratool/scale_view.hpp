#pragma once
#include "migratool/core.hpp"
#include <memory>

namespace mt {
inline constexpr UINT WM_SCALE_SELECTION=WM_APP+175;
inline constexpr UINT WM_SCALE_ACTION=WM_APP+176;
class ScaleView {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    ScaleView();~ScaleView();
    void create(HWND,HFONT,int);
    void appearance(HFONT,int);
    HWND window() const;
    void show(const Project&,int,const std::string&);
    void refresh();
    std::string selected() const;
    Json selected_row() const;
    std::string active_root() const;
    bool busy() const;
    bool settled() const;
    void set_filter_context(const std::string& root,const std::string& filter_text);
};
}
