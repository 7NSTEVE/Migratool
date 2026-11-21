#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <iterator>

int wmain(int argc,wchar_t** argv) {
    if(argc>1&&std::wstring(argv[1])==L"sleep"){Sleep(5000);return 0;}
    if(argc>1&&std::wstring(argv[1])==L"child") {
        std::wstring command=L"\""+std::wstring(argv[0])+L"\" sleep";STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
        if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return 5;
        std::cout<<pi.dwProcessId;CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 0;
    }
    if(argc>1&&std::wstring(argv[1])==L"analyzer") {
        std::cout<<R"json({"protocol":"migratool.analyzer/1","symbols":[{"name":"custom_operation","kind":"procedure","line":1,"signature":"()"}],"dependencies":[],"diagnostics":[],"errors":0})json";return 0;
    }
    if(argc>1&&std::wstring(argv[1])==L"args") {
        for(int i=2;i<argc;++i)std::wcout<<argv[i]<<L"\n";return 0;
    }
    if(argc>1&&std::wstring(argv[1])==L"json-left"){std::cout<<"{\"x\":1,\"nested\":{\"a\":2,\"b\":3}}";return 0;}
    if(argc>1&&std::wstring(argv[1])==L"json-right"){std::cout<<"{\"nested\":{\"b\":3,\"a\":2},\"x\":1}";return 0;}
    if(argc>1&&std::wstring(argv[1])==L"different"){std::cout<<"different";return 0;}
    std::string input{std::istreambuf_iterator<char>(std::cin),{}};std::cout<<input;
    std::ofstream("effect.txt",std::ios::binary)<<input;return 0;
}
