#include "migratool/scale.hpp"
#include "digest.hpp"
#include <array>
#include <limits>
#include <system_error>

namespace mt {
namespace {
struct FileHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~FileHandle(){close();}
    void close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
};

[[noreturn]] void io_error(const char* operation){
    throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),operation);
}

uint64_t size_of(HANDLE file){
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file,&size))io_error("EXPORT_SIZE");
    return static_cast<uint64_t>(size.QuadPart);
}

void seek_to(HANDLE file,uint64_t offset){
    if(offset>static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))throw std::runtime_error("EXPORT_OFFSET_RANGE");
    LARGE_INTEGER at{};at.QuadPart=static_cast<int64_t>(offset);
    if(!SetFilePointerEx(file,at,nullptr,FILE_BEGIN))io_error("EXPORT_SEEK");
}

void append(HANDLE file,std::string_view value){
    DWORD written{};
    if(value.size()>std::numeric_limits<DWORD>::max())throw std::runtime_error("EXPORT_ROW_TOO_LARGE");
    if(!WriteFile(file,value.data(),static_cast<DWORD>(value.size()),&written,nullptr))io_error("EXPORT_WRITE");
    if(written!=value.size())throw std::runtime_error("EXPORT_SHORT_WRITE");
}

std::string digest_file(HANDLE file,const std::function<void()>& check){
    seek_to(file,0);
    Digest digest;
    std::array<char,256*1024> buffer{};
    unsigned chunks=0;
    for(;;){
        if(chunks++%16==0)check();
        DWORD read{};
        if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr))io_error("EXPORT_HASH_READ");
        if(!read)break;
        digest.add(buffer.data(),read);
    }
    return digest.finish();
}

int64_t progress_epoch(Database& db){
    return db.integer("SELECT CAST(value AS INTEGER) FROM meta WHERE key='progress_epoch'");
}

std::string roots_identity(Database& db){
    return sha256(db.query("SELECT id,role,path FROM roots ORDER BY id").dump());
}

void verify_epoch(Database& db,const Json& checkpoint){
    if(progress_epoch(db)!=checkpoint.at("epoch").get<int64_t>()||roots_identity(db)!=checkpoint.at("roots").get<std::string>())
        throw std::runtime_error("EXPORT_CATALOG_CHANGED: reintente para exportar el catálogo actual");
}

void save_checkpoint(Database& db,const Json& id,const Json& state,const char* phase){
    db.exec("UPDATE inventory_jobs SET resources=json_set(resources,'$.export_checkpoint',json(?)), phase=?, files=?, bytes=?, updated=? WHERE id=?",
        {state.dump(),phase,state.at("files"),state.at("bytes"),now(),id});
}

void finish_export(Database& db,const Json& id,const Json& scope,const Json& checkpoint){
    const Json result={{"path",scope.at("out")},{"format",scope.at("format")},{"files",checkpoint.at("files")},
        {"bytes",checkpoint.at("bytes")},{"sha256",checkpoint.at("sha256")},{"revision",checkpoint.at("revision")},
        {"generation",checkpoint.at("generation")}};
    db.begin();
    try{
        db.exec("UPDATE inventory_jobs SET state='completed',phase='export_finished',control='',pid=NULL,finished=?,updated=?,resources=json_set(resources,'$.export_result',json(?)) WHERE id=?",
            {now(),now(),result.dump(),id});
        db.change("export","inventory_export_finished",{{"job_id",id},{"result",result}});
        db.commit();
    }catch(...){db.rollback();throw;}
}
}

void run_inventory_export(Database& db,const Json& job,const std::function<void()>& check){
    const auto& id=job.at("id");
    const auto scope=Json::parse(job.at("scope").get<std::string>());
    const auto resources=Json::parse(job.at("resources").get<std::string>());
    const auto destination=native_path(fs::path(wide(scope.at("out"))));
    const auto stage=native_path(fs::path(wide(scope.at("stage"))));
    const bool array=scope.at("format")=="json";
    auto checkpoint=resources.value("export_checkpoint",Json::object());
    if(checkpoint.empty()){
        checkpoint={{"epoch",progress_epoch(db)},{"roots",roots_identity(db)},{"revision",db.revision()},
            {"generation",db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='published_generation'")},
            {"total",db.integer("SELECT COUNT(*) FROM files WHERE deleted=0")},
            {"after_id",0},{"bytes",0},{"files",0}};
        save_checkpoint(db,id,checkpoint,"export");
    }

    check();
    if(checkpoint.contains("sha256")){
        if(!fs::exists(stage)&&fs::exists(destination)){
            FileHandle published{CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
            if(published.value==INVALID_HANDLE_VALUE)io_error("EXPORT_RECOVERY_OPEN");
            if(size_of(published.value)!=checkpoint.at("bytes").get<uint64_t>()||digest_file(published.value,check)!=checkpoint.at("sha256").get<std::string>())
                throw std::runtime_error("EXPORT_RECOVERY_MISMATCH");
            finish_export(db,id,scope,checkpoint);
            return;
        }
    }else verify_epoch(db,checkpoint);

    fs::create_directories(stage.parent_path());
    const auto bytes=checkpoint.at("bytes").get<uint64_t>();
    FileHandle output{CreateFileW(stage.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,
        bytes?OPEN_EXISTING:OPEN_ALWAYS,FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    if(output.value==INVALID_HANDLE_VALUE)io_error("EXPORT_OPEN");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if(!GetFileInformationByHandleEx(output.value,FileAttributeTagInfo,&attributes,sizeof(attributes)))io_error("EXPORT_STAGE_ATTRIBUTES");
    if(attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("EXPORT_STAGE_IS_LINK");
    if(size_of(output.value)<bytes)throw std::runtime_error("EXPORT_CHECKPOINT_TRUNCATED");
    if(checkpoint.contains("sha256")){
        if(size_of(output.value)!=bytes||digest_file(output.value,check)!=checkpoint.at("sha256").get<std::string>())
            throw std::runtime_error("EXPORT_STAGED_HASH_MISMATCH");
    }else{
        seek_to(output.value,bytes);
        if(!SetEndOfFile(output.value))io_error("EXPORT_TRUNCATE_UNCONFIRMED_TAIL");
        uint64_t written=bytes;
        auto write=[&](std::string_view text){if(!text.empty()){append(output.value,text);written+=text.size();}};
        if(array&&written==0)write("{\"schema\":\"migratool.scan-export/1\",\"files\":[");
        for(;;){
            check();
            verify_epoch(db,checkpoint);
            const auto rows=db.query(R"sql(SELECT f.id AS row_id,'F'||f.id AS id,f.root,r.role,f.path,f.absolute_path,
                f.sha256,f.bytes,f.mtime,f.language,f.category,f.relevant,f.analysis,
                COALESCE(json_extract(f.data,'$.disposition'),'') AS disposition FROM files f LEFT JOIN roots r ON r.id=f.root
                WHERE f.id>? AND f.deleted=0 ORDER BY f.id LIMIT 1000)sql",{checkpoint.at("after_id")});
            verify_epoch(db,checkpoint);
            if(rows.empty())break;
            std::string batch;
            for(auto row:rows){
                checkpoint["after_id"]=row.at("row_id");row.erase("row_id");
                if(array&&checkpoint.at("files")!=0)batch+=',';
                batch+=row.dump(-1,' ',false,Json::error_handler_t::replace);
                if(!array)batch+='\n';
                checkpoint["files"]=checkpoint.at("files").get<uint64_t>()+1;
            }
            write(batch);
            if(!FlushFileBuffers(output.value))io_error("EXPORT_FLUSH");
            checkpoint["bytes"]=written;
            save_checkpoint(db,id,checkpoint,"export");
        }
        if(array)write("]}");
        if(!FlushFileBuffers(output.value))io_error("EXPORT_FLUSH");
        checkpoint["bytes"]=written;
        checkpoint["sha256"]=digest_file(output.value,check);
        verify_epoch(db,checkpoint);
        save_checkpoint(db,id,checkpoint,"export_publish");
    }

    check();
    output.close();
    if(!MoveFileExW(stage.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))io_error("EXPORT_PUBLISH");
    finish_export(db,id,scope,checkpoint);
}
}
