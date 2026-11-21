#pragma once
#include <array>
#include <vector>

namespace mt {
struct NavigationGroup {const wchar_t* title;const wchar_t* description;std::vector<int> pages;};
inline const std::array<NavigationGroup,5>& navigation_groups(){
    static const std::array<NavigationGroup,5> groups={{{L"Inicio",L"El estado del proyecto y el siguiente paso.",{0,14}},
        {L"Migración",L"Compare el origen con el destino y registre lo que ya migró.",{1,8,17,7,3,23,22}},
        {L"Actividad",L"Siga los trabajos y los cambios de sus agentes.",{16,13,6}},
        {L"Revisión",L"Compruebe resultados, duplicados y evidencia de la migración.",{4,24,5,15,21,18,2,11,10,9}},
        {L"Proyecto",L"Configure las carpetas y consulte las herramientas avanzadas.",{20,12,19}}}};
    return groups;
}
inline int navigation_group(int page){for(size_t i=0;i<navigation_groups().size();++i)for(auto item:navigation_groups()[i].pages)if(item==page)return static_cast<int>(i);return 0;}
inline bool navigation_search(int page){return page!=0&&page!=16&&page!=19&&page!=20&&page!=12;}
}
