#pragma once
#include "migratool/scale.hpp"
#include <winioctl.h>
#include <map>

namespace mt {
// Read-only journal replay. Checkpoints are committed with the dirty paths.
class UsnReplay {
    HANDLE volume_=INVALID_HANDLE_VALUE;
    fs::path base_;
    std::string root_,key_;
    DWORD serial_{};
    USN next_{};
    DWORDLONG journal_{};
    bool baseline_=false;
    std::map<DWORDLONG,fs::path> parents_;
    void status(Database&,const std::string&,DWORD=0);
    fs::path parent_path(DWORDLONG);
public:
    UsnReplay(const Project&,const Json&,Database&);
    ~UsnReplay();
    UsnReplay(const UsnReplay&)=delete;
    bool needs_baseline()const{return baseline_;}
    void poll(Database&,bool& reconcile,bool& changed);
};
}
