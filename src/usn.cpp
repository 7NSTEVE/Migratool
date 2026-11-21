#include "usn.hpp"
#include <array>
#include <cstring>

namespace mt {
namespace {
bool excluded(const fs::path& path){for(const auto& part:path){auto text=part.wstring();for(auto& c:text)c=towlower(c);if(text==L".migratool")return true;}return false;}
}
void UsnReplay::status(Database& db,const std::string& state,DWORD error){db.exec("INSERT INTO meta VALUES(?,?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{"journal_status:"+root_,Json{{"available",volume_!=INVALID_HANDLE_VALUE},{"state",state},{"win32_error",error},{"fallback","metadata_reconciliation"},{"at",now()}}.dump()});}
UsnReplay::UsnReplay(const Project& p,const Json& root,Database& db):base_(p.resolve(root["path"])),root_(root["id"]),key_("journal_checkpoint:"+root_){
    std::wstring volume(32768,L'\0'),device(32768,L'\0');if(!GetVolumePathNameW(base_.c_str(),volume.data(),static_cast<DWORD>(volume.size()))||!GetVolumeNameForVolumeMountPointW(volume.c_str(),device.data(),static_cast<DWORD>(device.size()))){status(db,"UNSUPPORTED_VOLUME",GetLastError());return;}
    GetVolumeInformationW(volume.c_str(),nullptr,0,&serial_,nullptr,nullptr,nullptr,0);device.resize(wcslen(device.c_str()));if(device.ends_with(L"\\"))device.pop_back();
    volume_=CreateFileW(device.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(volume_==INVALID_HANDLE_VALUE){status(db,"UNAVAILABLE",GetLastError());return;}
    USN_JOURNAL_DATA_V0 current{};DWORD size{};if(!DeviceIoControl(volume_,FSCTL_QUERY_USN_JOURNAL,nullptr,0,&current,sizeof(current),&size,nullptr)){auto error=GetLastError();CloseHandle(volume_);volume_=INVALID_HANDLE_VALUE;status(db,"UNAVAILABLE",error);return;}
    journal_=current.UsnJournalID;next_=current.NextUsn;
    const bool has_published=db.integer("SELECT EXISTS(SELECT 1 FROM inventory_generations WHERE state='published')")!=0;
    baseline_=has_published;
    auto rows=db.query("SELECT value FROM meta WHERE key=?",{key_});
    if(!rows.empty()){auto checkpoint=Json::parse(rows[0]["value"].get<std::string>());if(checkpoint.value("journal_id",DWORDLONG(0))==journal_&&checkpoint.value("next_usn",USN(0))>=std::max(current.FirstUsn,current.LowestValidUsn)&&checkpoint.value("next_usn",USN(0))<=current.NextUsn){next_=checkpoint["next_usn"];baseline_=false;}}
    if(rows.empty()||baseline_)db.exec("INSERT INTO meta VALUES(?,?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{key_,Json{{"journal_id",journal_},{"next_usn",next_},{"at",now()}}.dump()});
    status(db,baseline_?"BASELINE_REQUIRED":"REPLAY_READY");
}
UsnReplay::~UsnReplay(){if(volume_!=INVALID_HANDLE_VALUE)CloseHandle(volume_);}
fs::path UsnReplay::parent_path(DWORDLONG id){auto cached=parents_.find(id);if(cached!=parents_.end())return cached->second;FILE_ID_DESCRIPTOR identity{};identity.dwSize=sizeof(identity);identity.Type=FileIdType;identity.FileId.QuadPart=static_cast<LONGLONG>(id);
    auto file=OpenFileById(volume_,&identity,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT);if(file==INVALID_HANDLE_VALUE)return {};
    std::wstring name(32768,L'\0');auto n=GetFinalPathNameByHandleW(file,name.data(),static_cast<DWORD>(name.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);CloseHandle(file);if(!n||n>=name.size())return {};name.resize(n);if(name.starts_with(L"\\\\?\\UNC\\"))name=L"\\\\"+name.substr(8);else if(name.starts_with(L"\\\\?\\"))name.erase(0,4);
    if(parents_.size()>=2048)parents_.clear();auto path=fs::path(name);parents_[id]=path;return path;
}
void UsnReplay::poll(Database& db,bool& reconcile,bool& changed){
    if(volume_==INVALID_HANDLE_VALUE)return;
    if(baseline_){db.exec("INSERT INTO meta VALUES('dirty','1')ON CONFLICT(key)DO UPDATE SET value='1'");reconcile=changed=true;baseline_=false;}
    constexpr DWORD reasons=USN_REASON_DATA_OVERWRITE|USN_REASON_DATA_EXTEND|USN_REASON_DATA_TRUNCATION|USN_REASON_FILE_CREATE|USN_REASON_FILE_DELETE|USN_REASON_RENAME_OLD_NAME|USN_REASON_RENAME_NEW_NAME|USN_REASON_REPARSE_POINT_CHANGE;
    alignas(USN_RECORD_V2)std::array<unsigned char,64*1024> buffer{};
    for(int batch=0;batch<4;++batch){READ_USN_JOURNAL_DATA_V0 request{};request.StartUsn=next_;request.ReasonMask=reasons;request.UsnJournalID=journal_;DWORD size{};
        if(!DeviceIoControl(volume_,FSCTL_READ_USN_JOURNAL,&request,sizeof(request),buffer.data(),static_cast<DWORD>(buffer.size()),&size,nullptr)){auto error=GetLastError();status(db,"REPLAY_FAILED",error);reconcile=changed=true;CloseHandle(volume_);volume_=INVALID_HANDLE_VALUE;return;}
        if(size<sizeof(USN))return;USN after{};std::memcpy(&after,buffer.data(),sizeof(after));if(after==next_)return;bool any=false,batch_reconcile=false;
        std::vector<std::pair<std::string,DWORD>> pending_dirty;
        for(size_t offset=sizeof(USN);offset+sizeof(DWORD)+2*sizeof(WORD)<=static_cast<size_t>(size);){auto* record=reinterpret_cast<USN_RECORD_V2*>(buffer.data()+offset);
            if(record->RecordLength<8||offset+record->RecordLength>static_cast<size_t>(size))throw std::runtime_error("USN_RECORD_TRUNCATED");
            if(record->MajorVersion!=2){batch_reconcile=true;offset+=record->RecordLength;continue;}
            if(record->RecordLength<offsetof(USN_RECORD_V2,FileName)||static_cast<DWORD>(record->FileNameOffset)+static_cast<DWORD>(record->FileNameLength)>record->RecordLength)throw std::runtime_error("USN_RECORD_INVALID");
            if(record->FileAttributes&FILE_ATTRIBUTE_DIRECTORY)parents_.clear();auto parent=parent_path(record->ParentFileReferenceNumber);fs::path absolute;
            if(!parent.empty())absolute=parent/fs::path(std::wstring(reinterpret_cast<wchar_t*>(reinterpret_cast<unsigned char*>(record)+record->FileNameOffset),record->FileNameLength/sizeof(wchar_t)));
            else {const auto prefix=std::to_string(serial_)+":"+std::to_string(record->FileReferenceNumber)+":";auto rows=db.query("SELECT absolute_path FROM files INDEXED BY file_identity WHERE winid>=? AND winid<? AND root=? LIMIT 1",{prefix,prefix+"~",root_});if(!rows.empty())absolute=fs::path(wide(rows[0]["absolute_path"]));else {auto known=db.query("SELECT path FROM directory_ids WHERE root=? AND volume_serial=? AND frn=?",{root_,serial_,static_cast<int64_t>(record->ParentFileReferenceNumber)});if(!known.empty()){absolute=base_/wide(known[0]["path"])/fs::path(std::wstring(record->FileName,record->FileNameLength/sizeof(wchar_t)));batch_reconcile=true;}}}
            if(!absolute.empty()&&within(absolute,base_)){auto relative=absolute.lexically_relative(base_);if(!relative.empty()&&!excluded(relative)){DWORD action=FILE_ACTION_MODIFIED;if(record->Reason&(USN_REASON_FILE_DELETE|USN_REASON_RENAME_OLD_NAME))action=FILE_ACTION_REMOVED;else if(record->Reason&(USN_REASON_FILE_CREATE|USN_REASON_RENAME_NEW_NAME))action=FILE_ACTION_ADDED;
                    pending_dirty.emplace_back(path_text(relative),action);any=true;}}
            offset+=record->RecordLength;}
        if(any||batch_reconcile){
            db.begin();
            try{
                const auto stamp=now();
                for(const auto& [rel,action]:pending_dirty)db.exec("INSERT INTO dirty_paths(root,path,action,at)VALUES(?,?,?,?)ON CONFLICT(root,path)DO UPDATE SET action=excluded.action,at=excluded.at",{root_,rel,action,stamp});
                db.exec("INSERT INTO meta VALUES(?,?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{key_,Json{{"journal_id",journal_},{"next_usn",after},{"at",stamp}}.dump()});
                db.exec("INSERT INTO meta VALUES('dirty','1')ON CONFLICT(key)DO UPDATE SET value='1'");
                db.commit();next_=after;
            }catch(...){db.rollback();throw;}
        }else{
            db.try_aux_write("INSERT INTO meta VALUES(?,?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{key_,Json{{"journal_id",journal_},{"next_usn",after},{"at",now()}}.dump()});
            next_=after;
        }
        reconcile=reconcile||batch_reconcile;changed=changed||any||batch_reconcile;if(size==sizeof(USN))break;
    }
}
}
