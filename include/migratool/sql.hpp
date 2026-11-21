#pragma once
#include "migratool/gateway.hpp"
#include <stdexcept>

namespace mt {
// One prepared statement reused for every row of a batch. Owns no result set.
class Statement {
    sqlite3* db_{}; sqlite3_stmt* stmt_{};
public:
    Statement(sqlite3* db,const std::string& sql):db_(db){
        if(sqlite3_prepare_v2(db,sql.c_str(),-1,&stmt_,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));
    }
    Statement(Database& db,const std::string& sql):Statement(db.native_handle(),sql){}
    ~Statement(){sqlite3_finalize(stmt_);}
    Statement(const Statement&)=delete;
    void reset(){sqlite3_reset(stmt_);sqlite3_clear_bindings(stmt_);}
    void bind(const Json& params){
        reset();
        const auto values=params.is_array()?params:Json::array({params});
        const auto expected=sqlite3_bind_parameter_count(stmt_);
        if(values.size()!=static_cast<size_t>(expected))
            throw std::runtime_error("SQL_PARAMETER_COUNT expected="+std::to_string(expected)+" received="+std::to_string(values.size())+" type="+params.type_name());
        for(size_t i=0;i<values.size();++i){const auto& p=values[i];int rc=SQLITE_OK;const int n=static_cast<int>(i)+1;
            if(p.is_null())rc=sqlite3_bind_null(stmt_,n);
            else if(p.is_boolean())rc=sqlite3_bind_int(stmt_,n,p.get<bool>()?1:0);
            else if(p.is_number_integer()||p.is_number_unsigned())rc=sqlite3_bind_int64(stmt_,n,p.get<int64_t>());
            else if(p.is_number_float())rc=sqlite3_bind_double(stmt_,n,p.get<double>());
            else {auto s=p.is_string()?p.get<std::string>():p.dump();rc=sqlite3_bind_text(stmt_,n,s.data(),static_cast<int>(s.size()),SQLITE_TRANSIENT);}
            if(rc!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }
    bool next(){const auto rc=sqlite3_step(stmt_);if(rc==SQLITE_ROW)return true;if(rc==SQLITE_DONE)return false;throw std::runtime_error(sqlite3_errmsg(db_));}
    Json first(const Json& params=Json::array()){
        bind(params);
        try{auto result=next()?row():Json();reset();return result;}
        catch(...){reset();throw;}
    }
    void exec(const Json& params=Json::array()){bind(params);while(next()){};}
    Json row() const {Json row=Json::object();for(int i=0;i<sqlite3_column_count(stmt_);++i){const auto* name=sqlite3_column_name(stmt_,i);
        switch(sqlite3_column_type(stmt_,i)){
          case SQLITE_NULL:row[name]=nullptr;break;
          case SQLITE_INTEGER:row[name]=sqlite3_column_int64(stmt_,i);break;
          case SQLITE_FLOAT:row[name]=sqlite3_column_double(stmt_,i);break;
          default:row[name]=std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt_,i)),sqlite3_column_bytes(stmt_,i));
        }}return row;
    }
};
}
