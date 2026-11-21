#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <limits>

namespace demo {
std::int64_t Total(std::int64_t cents,int rate) {
    if(cents<0||rate<0||rate>100)throw std::out_of_range("amount");
    if(rate && cents>(std::numeric_limits<std::int64_t>::max()-50)/rate)throw std::overflow_error("amount");
    auto tax=(cents*rate+50)/100;
    if(cents>std::numeric_limits<std::int64_t>::max()-tax)throw std::overflow_error("amount");
    return cents+tax;
}
}
int main() {
    try {
        std::string input{std::istreambuf_iterator<char>(std::cin),{}};auto j=nlohmann::json::parse(input);
        if(!j.at("cents").is_number_integer()||!j.at("rate").is_number_integer())throw std::invalid_argument("integer");
        auto cents=j.at("cents").get<std::int64_t>();auto raw_rate=j.at("rate").get<std::int64_t>();
        if(cents<0||raw_rate<0||raw_rate>100){std::cout<<"{\"error\":\"out_of_range\"}\n";return 2;}
        auto total=demo::Total(cents,static_cast<int>(raw_rate));
#ifdef DEMO_DIFFERENT
        ++total;
#endif
        std::ofstream("receipt.txt",std::ios::binary)<<"total="<<total;
        std::cout<<nlohmann::json{{"currency","CRC"},{"total_cents",total}}.dump()<<'\n';return 0;
    }catch(const std::exception&){std::cout<<"{\"error\":\"invalid_input\"}\n";return 2;}
}
