#include "migratool/gateway.hpp"
#include "migratool/scale.hpp"
#include "migratool/ui.hpp"

#include <commctrl.h>
#include <objbase.h>
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace mt;

int checks = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error("GUI CHECK FAILED: " + message);
}

std::wstring window_text(HWND hwnd) {
    const LRESULT len = SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    const LRESULT copied = SendMessageW(hwnd, WM_GETTEXT, static_cast<WPARAM>(buf.size()), reinterpret_cast<LPARAM>(buf.data()));
    buf.resize(static_cast<size_t>(std::max<LRESULT>(0, copied)));
    return buf;
}

HWND find_top_window_by_class(DWORD pid, const wchar_t* cls_name) {
    struct Search { DWORD pid; const wchar_t* cls; HWND hwnd{}; } s{pid, cls_name, nullptr};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* p = reinterpret_cast<Search*>(lp);
        DWORD owner = 0;
        GetWindowThreadProcessId(w, &owner);
        if (owner != p->pid) return TRUE;
        wchar_t buf[64]{};
        GetClassNameW(w, buf, 64);
        if (wcscmp(buf, p->cls) == 0 && IsWindowVisible(w)) {
            p->hwnd = w;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.hwnd;
}

HWND child_by_id(HWND parent, int id) {
    struct Search { int id; HWND hwnd{}; } s{id, nullptr};
    EnumChildWindows(parent, [](HWND w, LPARAM lp) -> BOOL {
        auto* p = reinterpret_cast<Search*>(lp);
        if (GetDlgCtrlID(w) == p->id) {
            p->hwnd = w;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.hwnd;
}

HWND child_by_class(HWND parent, const wchar_t* cls_name) {
    struct Search { const wchar_t* cls; HWND hwnd{}; } s{cls_name, nullptr};
    EnumChildWindows(parent, [](HWND w, LPARAM lp) -> BOOL {
        auto* p = reinterpret_cast<Search*>(lp);
        wchar_t buf[64]{};
        GetClassNameW(w, buf, 64);
        if (wcscmp(buf, p->cls) == 0) {
            p->hwnd = w;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.hwnd;
}

template <class Pred>
void wait_until(Pred pred, const std::string& msg, DWORD timeout_ms = 20000) {
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < timeout_ms) {
        if (pred()) {
            check(true, msg);
            return;
        }
        Sleep(40);
    }
    check(false, msg + " (timeout)");
}

uint64_t send_responsive(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, uint64_t max_ms = 200) {
    const auto t0 = std::chrono::steady_clock::now();
    DWORD_PTR res = 0;
    const LRESULT ok = SendMessageTimeoutW(hwnd, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &res);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(ok != 0, "GUI respondió sin colgarse a mensaje " + std::to_string(msg));
    check(ms <= static_cast<long long>(max_ms),
          "Interacción GUI <= " + std::to_string(max_ms) + " ms (observado=" + std::to_string(ms) + " ms)");
    return static_cast<uint64_t>(ms);
}

void save_window_png(HWND hwnd, const fs::path& out_path) {
    fs::create_directories(out_path.parent_path());
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int width = std::max<int>(1, rc.right - rc.left);
    const int height = std::max<int>(1, rc.bottom - rc.top);

    HDC screen_dc = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    HBITMAP bmp = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem_dc, bmp);

    BOOL printed = PrintWindow(hwnd, mem_dc, PW_RENDERFULLCONTENT);
    if (!printed) printed = PrintWindow(hwnd, mem_dc, 0);
    check(printed != FALSE, "PrintWindow capturó " + path_text(out_path.filename()));

    IWICImagingFactory* factory{};
    check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))),
          "WIC factory inicializado");
    IWICBitmap* wic_bmp{};
    check(SUCCEEDED(factory->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &wic_bmp)),
          "WIC bitmap creado desde HBITMAP");
    IWICStream* stream{};
    check(SUCCEEDED(factory->CreateStream(&stream)), "WIC stream creado");
    check(SUCCEEDED(stream->InitializeFromFilename(out_path.wstring().c_str(), GENERIC_WRITE)),
          "WIC stream abierto en " + path_text(out_path));
    IWICBitmapEncoder* encoder{};
    check(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)), "Encoder PNG creado");
    check(SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)), "Encoder inicializado");
    IWICBitmapFrameEncode* frame{};
    check(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)), "Frame PNG creado");
    check(SUCCEEDED(frame->Initialize(nullptr)), "Frame PNG inicializado");
    check(SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))), "Dimensiones PNG");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    check(SUCCEEDED(frame->WriteSource(wic_bmp, nullptr)), "WriteSource PNG");
    check(SUCCEEDED(frame->Commit()), "Frame Commit PNG");
    check(SUCCEEDED(encoder->Commit()), "Encoder Commit PNG");

    frame->Release();
    encoder->Release();
    stream->Release();
    wic_bmp->Release();
    factory->Release();
    SelectObject(mem_dc, old);
    DeleteObject(bmp);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
    check(fs::exists(out_path) && fs::file_size(out_path) > 1024, "Captura PNG válida: " + path_text(out_path));
}

int listview_count(HWND lv) {
    return static_cast<int>(SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0));
}

void listview_select(HWND lv, int index) {
    HWND hdr = ListView_GetHeader(lv);
    RECT hrc{};
    if (hdr) GetWindowRect(hdr, &hrc);
    const int header_h = std::max<int>(20, static_cast<int>(hrc.bottom - hrc.top));
    const int y = header_h + 10 + index * 18;
    PostMessageW(lv, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, y));
    PostMessageW(lv, WM_LBUTTONUP, 0, MAKELPARAM(24, y));
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    fs::path evidence_dir;
    try {
        if (argc < 2) return 2;
        const auto repo_root = fs::path(argv[1]);
        const auto qa = repo_root / L"build/qa-scale/tests-tmp" / wide("gui_" + unique_id());
        evidence_dir = repo_root / L"build/qa-scale/evidence/gui-acceptance";
        fs::create_directories(qa / L"source" / L"empty_dir");
        fs::create_directories(qa / L"source" / L"excluded_dir");
        fs::create_directories(qa / L"target");
        fs::create_directories(evidence_dir);

        // 1. Seed 10,500 files + folder coverage causes
        constexpr int BUCKETS = 105;
        for (int b = 0; b < BUCKETS; ++b) {
            const auto bucket = qa / L"source" / wide("mod_" + std::to_string(b));
            fs::create_directories(bucket);
            for (int k = 0; k < 100; ++k) {
                const int idx = b * 100 + k;
                write_text(bucket / wide("f_" + std::to_string(idx) + ".py"), "def fn_" + std::to_string(idx) + "():\n    return " + std::to_string(idx) + "\n");
            }
        }
        write_text(qa / L"source/excluded_dir/skip.py", "x = 1\n");
        write_text(qa / L"source/no_ext_bin", "RAW_BINARY_BLOB");
        const auto hidden_path = qa / L"source/hidden_note.txt";
        write_text(hidden_path, "hidden text\n");
        SetFileAttributesW(hidden_path.c_str(), FILE_ATTRIBUTE_HIDDEN);
        write_text(qa / L"target/t_0.py", "def tf():\n    return 0\n");

        auto proj = Project::create(qa / L"migration.json", "gui-acceptance-10k",
            Json::array({{{"id", "source"}, {"role", "source"}, {"path", "source"}},
                         {{"id", "target"}, {"role", "target"}, {"path", "target"}}}));
        proj.config["inventory_engine"] = "durable";
        proj.config["exclude_dirs"] = {"excluded_dir"};
        proj.save();
        proj = Project::load(proj.file);

        const auto seeded = scale_run(proj, {{"actor", "gui-seed"}, {"defer_analysis", true}});
        check(seeded.at("state") == "completed", "Censo inicial de 10,500+ archivos completado");

        // Add claim and mapping so filters have matching rows
        const auto claimed = agent_request(proj, "acquire_task", {{"actor", "agent-gui-claim"}, {"lease_seconds", 600}});
        const std::string claimed_id = claimed.at("task_id").get<std::string>();
        {
            Database db(proj);
            const auto second_row = db.query("SELECT id FROM files WHERE root='source' AND relevant=1 AND ('F'||id)<>? AND deleted=0 ORDER BY id LIMIT 1", {claimed_id});
            check(!second_row.empty(), "Archivo source secundario no reclamado encontrado");
            const auto second_src = "F" + std::to_string(second_row[0].at("id").get<int64_t>());
            const auto tgt = "F" + std::to_string(db.integer("SELECT id FROM files WHERE root='target' AND deleted=0 ORDER BY id LIMIT 1"));
            agent_batch(proj, {{"actor", "seed-map"}, {"expected_revision", db.revision()},
                {"operations", {{{"op", "map"}, {"source", second_src}, {"targets", {tgt}}, {"complete", false}, {"note", "seed map"}},
                                {{"op", "upsert_phase"}, {"id", "P-GUI-1"}, {"title", "Fase inicial GUI"}, {"ordinal", 1}, {"state", "IN_PROGRESS"}},
                                {{"op", "upsert_task"}, {"id", "T-GUI-1"}, {"phase_id", "P-GUI-1"}, {"title", "Tarea inicial GUI"}, {"state", "HALF_DONE"}}}}});
        }
        proj.config["cases"] = Json::array({
            {{"id", "tt_preserve"}, {"kind", "target_test"}, {"root", "target"},
             {"argv", {"cmd.exe", "/c", "echo ok"}}, {"expected_exit", 0}, {"cwd", "target"}, {"timeout_ms", 12000}}
        });
        proj.save();
        proj = Project::load(proj.file);

        // Verify HIG contrast metrics directly from ui::system_palette()
        const auto pal = ui::system_palette();
        check(ui::contrast(pal.text, pal.surface) >= 4.5, "Contraste texto/superficie >= 4.5:1");
        check(ui::contrast(pal.selection_text, pal.selection) >= 4.5, "Contraste selección >= 4.5:1");
        check(ui::contrast(pal.accent, pal.surface) >= 4.5, "Contraste acento/superficie >= 4.5:1");

        // Launch real Migratool-0.3.exe with the project file
        const auto gui_exe = executable_dir() / L"Migratool-0.3.exe";
        std::wstring cmd = quote_arg(gui_exe.wstring()) + L" " + quote_arg(proj.file.wstring());
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi{};
        check(CreateProcessW(gui_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, qa.c_str(), &si, &pi) != FALSE,
              "CreateProcessW lanzó Migratool-0.3.exe");
        struct ProcClose { HANDLE p; ~ProcClose() { CloseHandle(p); } } proc_guard{pi.hProcess};
        CloseHandle(pi.hThread);

        HWND main_hwnd = nullptr;
        wait_until([&] {
            main_hwnd = find_top_window_by_class(pi.dwProcessId, L"MigratoolWindow");
            return main_hwnd != nullptr;
        }, "Ventana principal MigratoolWindow visible");

        HWND scale_hwnd = nullptr;
        HWND lv = nullptr;
        wait_until([&] {
            scale_hwnd = child_by_class(main_hwnd, L"MigratoolScaleView");
            lv = scale_hwnd ? child_by_id(scale_hwnd, 9) : nullptr;
            return lv != nullptr && listview_count(lv) > 0;
        }, "Vista escalable MigratoolScaleView cargada (página inicial)");

        // Switch to Origen <-> destino (page 1 -> command 1001) & measure post-load interactive responsiveness <= 200 ms
        send_responsive(main_hwnd, WM_COMMAND, 1002, 0, 200);
        const auto t_nav1 = send_responsive(main_hwnd, WM_COMMAND, 1001, 0, 200);
        wait_until([&] { return listview_count(lv) == 200; }, "Origen y destino muestra página de 200 filas");
        const auto lv_style = GetWindowLongPtrW(lv, GWL_STYLE);
        check((lv_style & LVS_OWNERDATA) != 0, "Lista de Origen y destino usa LVS_OWNERDATA virtual");

        // Verify >10,000 rows reachable smoothly via --preview-inventory on the live window OR clicking page Next
        const auto preview_json = qa / L"preview-inventory.json";
        const auto preview_proc = run_process(
            {path_text(executable_dir() / L"gui_lifecycle.exe"), "--preview-inventory",
             std::to_string(pi.dwProcessId), path_text(proj.file), path_text(preview_json)},
            qa, "", qa / L"preview_proc", 30000);
        check(preview_proc.at("exit_code") == 0, "Navegación de >10,000 filas (51 páginas) verificada en vivo");
        const auto pinv = read_json(preview_json);
        check(pinv.at("pages").size() >= 51 &&
              pinv.at("first_index_after_10000").get<int>() == 10001 &&
              pinv.at("exact_final_path_checked") == true,
              "Página 51 y fila 10,001 presentes en catálogo virtual: " + pinv.dump());
        // Switch back to Origen <-> destino (page 1 -> command 1001) before screenshot & filter check
        send_responsive(main_hwnd, WM_COMMAND, 1001, 0, 200);
        wait_until([&] { return listview_count(lv) == 200; }, "Regresa a Origen <-> destino con página de 200 filas");
        save_window_png(main_hwnd, evidence_dir / L"01-origen-destino-10k.png");

        // Test pending filter combo (id 21 -> index 6 = claimed)
        HWND combo = child_by_id(scale_hwnd, 21);
        check(combo != nullptr, "Combo de filtro de pendientes presente");
        if (!IsWindowVisible(combo)) send_responsive(scale_hwnd, WM_COMMAND, 18, 0, 200);
        send_responsive(combo, CB_SETCURSEL, 6, 0, 200);
        send_responsive(scale_hwnd, WM_COMMAND, MAKEWPARAM(21, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo), 200);
        wait_until([&] { return listview_count(lv) == 1; }, "Filtro 'Reclamados por un agente' muestra 1 fila reclamada (" + claimed_id + ")");
        save_window_png(main_hwnd, evidence_dir / L"02-filter-claimed.png");

        // Reset filter to All (0) and perform concurrent CLI write
        send_responsive(combo, CB_SETCURSEL, 0, 0, 200);
        send_responsive(scale_hwnd, WM_COMMAND, MAKEWPARAM(21, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo), 200);
        wait_until([&] { return listview_count(lv) == 200; }, "Filtro restablecido a todos los archivos");

        {
            Database db(proj);
            const Json batch_req = {{"operations", Json::array({{{"op", "note"}, {"entity", claimed_id}, {"text", "nota concurrente CLI"}}})}};
            const auto cli_batch = run_process(
                {path_text(executable_dir() / L"migratool-cli-0.3.exe"), "agent", "batch",
                 "--project", path_text(proj.file), "--actor", "concurrent-cli",
                 "--expected-revision", std::to_string(db.revision()),
                 "--json", batch_req.dump()},
                qa, "", qa / L"cli_note", 15000);
            check(cli_batch.at("exit_code") == 0, "Escritura CLI concurrente con GUI abierta terminó con código 0");
        }
        send_responsive(scale_hwnd, WM_COMMAND, 4, 0, 200); // Actualizar
        wait_until([&] { return listview_count(lv) == 200; }, "Actualización tras escritura CLI concurrente");

        // Export inventory & verify Trabajos del proyecto (1016): Etapa width + Export detail top lines
        const auto gui_export_out = qa / L"exported.jsonl";
        const auto export_queued = scale_export(proj, {{"out", path_text(gui_export_out)}, {"format", "jsonl"}, {"actor", "gui-accept"}, {"wait", false}});
        const std::string export_jid = export_queued.at("job_id").get<std::string>();
        wait_until([&] {
            const auto j = scale_jobs(proj, {{"id", export_jid}}).at("items").at(0);
            return j.at("state") == "completed";
        }, "Trabajo de exportación completado", 30000);

        send_responsive(main_hwnd, WM_COMMAND, 1016, 0, 200);
        wait_until([&] {
            return listview_count(lv) >= 2 && Header_GetItemCount(ListView_GetHeader(lv)) >= 3;
        }, "Centro de trabajos muestra censo y exportación");
        Sleep(100);

        // Select top row (most recent job = export) reliably even if background poll refreshes Trabajos once
        HWND detail_edit = child_by_id(scale_hwnd, 11);
        ULONGLONG last_click_tick = 0;
        wait_until([&] {
            const auto sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
            const auto tick = GetTickCount64();
            if (sel != 0 && tick - last_click_tick >= 200) {
                listview_select(lv, 0);
                last_click_tick = tick;
            }
            const auto txt = utf8(window_text(detail_edit));
            return txt.find("Destino:") != std::string::npos && txt.find("SHA-256:") != std::string::npos;
        }, "Detalle de trabajo de exportación muestra Destino y SHA-256 al inicio");
        const auto detail_txt = utf8(window_text(detail_edit));
        const auto pos_dest = detail_txt.find("Destino:");
        check(pos_dest != std::string::npos && pos_dest < 180,
              "Ruta de destino aparece al principio del detalle (offset=" + std::to_string(pos_dest) + ")");
        const int col_count = Header_GetItemCount(ListView_GetHeader(lv));
        check(col_count >= 3, "Centro de trabajos tiene columnas de trabajo");
        const int etapa_width = ListView_GetColumnWidth(lv, 2); // kind(0), state(1), phase/Etapa(2)
        check(etapa_width >= 200, "Columna Etapa tiene ancho suficiente (>=200 px, actual=" + std::to_string(etapa_width) + ")");
        save_window_png(main_hwnd, evidence_dir / L"03-centro-trabajos-export.png");

        // Folder Coverage (1017): lazy TreeView expansion + coverage causes + Recorrer aquí
        send_responsive(main_hwnd, WM_COMMAND, 1017, 0, 200);
        HWND tree = child_by_id(scale_hwnd, 12);
        wait_until([&] {
            return TreeView_GetCount(tree) >= 2 && listview_count(lv) >= 1;
        }, "Cobertura por carpeta muestra árbol de raíces y causas");
        HTREEITEM first_root = TreeView_GetRoot(tree);
        check(first_root != nullptr, "TreeView tiene nodo raíz source");
        TreeView_SelectItem(tree, first_root);
        TreeView_Expand(tree, first_root, TVE_EXPAND);
        wait_until([&] { return TreeView_GetChild(tree, first_root) != nullptr; },
                   "TreeView carga subcarpetas de forma diferida al expandir");
        save_window_png(main_hwnd, evidence_dir / L"04-cobertura-carpeta.png");

        // Trigger Recorrer aquí on Coverage view BEFORE switching away while page == 17 & root selected
        const int before_sync_jobs = scale_jobs(proj, {{"limit", 50}}).at("total").get<int>();
        send_responsive(scale_hwnd, WM_COMMAND, 14, 0, 200);
        wait_until([&] {
            return scale_jobs(proj, {{"limit", 50}}).at("total").get<int>() > before_sync_jobs;
        }, "Recorrer aquí encola trabajo acotado desde GUI", 15000);

        // Verify Guided Configuration Form (ID_CONFIG = 12) opens without raw JSON editor
        PostMessageW(main_hwnd, WM_COMMAND, 12, 0);
        HWND form_hwnd = nullptr;
        wait_until([&] {
            form_hwnd = find_top_window_by_class(pi.dwProcessId, L"MigratoolForm");
            return form_hwnd != nullptr && child_by_id(form_hwnd, 90) != nullptr && child_by_id(form_hwnd, 91) != nullptr;
        }, "Formulario guiado de configuración abierto con selectores de modo y duplicados");
        save_window_png(form_hwnd, evidence_dir / L"05-configuracion-guiada.png");
        PostMessageW(form_hwnd, WM_COMMAND, IDCANCEL, 0);
        wait_until([&] { return find_top_window_by_class(pi.dwProcessId, L"MigratoolForm") == nullptr; },
                   "Formulario guiado cerrado limpiamente");

        // Return Section 1 (Migración) to Origen <-> destino (page 1) before 200% zoom layout validation
        send_responsive(main_hwnd, WM_COMMAND, 1001, 0, 200);
        wait_until([&] { return listview_count(lv) == 200; }, "Regresa a Origen <-> destino antes de layout 200%");

        // Zoom 200% layout + screenshot + WM_DPICHANGED + WM_THEMECHANGED
        const auto layout_proc = run_process(
            {path_text(executable_dir() / L"gui_lifecycle.exe"), "--preview-layout",
             std::to_string(pi.dwProcessId), "200", "1440", "900"},
            qa, "", qa / L"layout_200", 15000);
        check(layout_proc.at("exit_code") == 0,
              "Preview layout 200% en GUI terminó con éxito: " + layout_proc.value("stdout", ""));
        // Apply 200% zoom directly in GUI via ID_ZOOM_IN (52) x4
        for (int z = 0; z < 4; ++z) send_responsive(main_hwnd, WM_COMMAND, 52, 0, 200);
        Sleep(150);
        save_window_png(main_hwnd, evidence_dir / L"06-zoom-200.png");
        send_responsive(main_hwnd, WM_COMMAND, 54, 0, 200); // reset 100%

        // DPI change & theme change responsiveness checks
        RECT dpi_rc{0, 0, 1600, 1000};
        send_responsive(main_hwnd, WM_DPICHANGED, MAKEWPARAM(192, 192), reinterpret_cast<LPARAM>(&dpi_rc), 250);
        RECT back_rc{0, 0, 1500, 960};
        send_responsive(main_hwnd, WM_DPICHANGED, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&back_rc), 250);
        send_responsive(main_hwnd, WM_THEMECHANGED, 0, 0, 200);
        send_responsive(main_hwnd, WM_SYSCOLORCHANGE, 0, 0, 200);

        // Page 23 (1023): Matriz de módulos responsiveness <= 200ms + columns/rows + screenshot
        const auto t_p23 = send_responsive(main_hwnd, WM_COMMAND, 1023, 0, 200);
        wait_until([&] { return listview_count(lv) > 0 && Header_GetItemCount(ListView_GetHeader(lv)) >= 8; },
                   "Matriz de módulos (página 23) cargó columnas y filas");
        check(t_p23 <= 200.0, "Cambio a Matriz de módulos <= 200ms");
        save_window_png(main_hwnd, evidence_dir / L"07-matriz-modulos.png");

        // Page 24 (1024): Fases, tareas y documentos + screenshot
        const auto t_p24 = send_responsive(main_hwnd, WM_COMMAND, 1024, 0, 200);
        wait_until([&] { return listview_count(lv) >= 2; },
                   "Control integral (página 24) muestra fase y tarea sembradas");
        check(t_p24 <= 200.0, "Cambio a Control integral <= 200ms");
        save_window_png(main_hwnd, evidence_dir / L"08-control-integral.png");

        // Verify ID_PHASE_UPSERT (65) opens form with state combo (id 100)
        PostMessageW(main_hwnd, WM_COMMAND, 65, 0);
        wait_until([&] {
            form_hwnd = find_top_window_by_class(pi.dwProcessId, L"MigratoolForm");
            return form_hwnd != nullptr && child_by_id(form_hwnd, 100) != nullptr;
        }, "Formulario de fase abierto con selector de estado");
        PostMessageW(form_hwnd, WM_COMMAND, IDCANCEL, 0);
        wait_until([&] { return find_top_window_by_class(pi.dwProcessId, L"MigratoolForm") == nullptr; },
                   "Formulario de fase cerrado");

        // Verify ID_VERIFY_CONFIG (64) preserves expected_exit, cwd, timeout_ms via JSON dump
        PostMessageW(main_hwnd, WM_COMMAND, 64, 0);
        wait_until([&] {
            form_hwnd = find_top_window_by_class(pi.dwProcessId, L"MigratoolForm");
            return form_hwnd != nullptr && child_by_id(form_hwnd, 101) != nullptr;
        }, "Formulario de compilación y pruebas abierto");
        {
            HWND cases_edit = child_by_id(form_hwnd, 101);
            std::wstring txt(GetWindowTextLengthW(cases_edit) + 1, L'\0');
            GetWindowTextW(cases_edit, txt.data(), static_cast<int>(txt.size()));
            check(txt.find(L"expected_exit") != std::wstring::npos &&
                  txt.find(L"timeout_ms") != std::wstring::npos &&
                  txt.find(L"cwd") != std::wstring::npos,
                  "ID_VERIFY_CONFIG conserva expected_exit, timeout_ms y cwd");
        }
        PostMessageW(form_hwnd, WM_COMMAND, IDCANCEL, 0);
        wait_until([&] { return find_top_window_by_class(pi.dwProcessId, L"MigratoolForm") == nullptr; },
                   "Formulario de compilación cerrado");

        // Verify ID_MARK (25) opens with stage (92) and disposition (98) combos
        send_responsive(main_hwnd, WM_COMMAND, 1001, 0, 200);
        wait_until([&] { return listview_count(lv) > 0; }, "Regresa a página 1 para probar ID_MARK");
        PostMessageW(main_hwnd, WM_COMMAND, 25, 0);
        wait_until([&] {
            form_hwnd = find_top_window_by_class(pi.dwProcessId, L"MigratoolForm");
            return form_hwnd != nullptr && child_by_id(form_hwnd, 92) != nullptr && child_by_id(form_hwnd, 98) != nullptr;
        }, "Formulario ID_MARK abierto con combos stage (92) y disposition (98)");
        check(SendMessageW(child_by_id(form_hwnd, 92), CB_GETCOUNT, 0, 0) == 5, "Combo stage tiene 5 opciones");
        check(SendMessageW(child_by_id(form_hwnd, 98), CB_GETCOUNT, 0, 0) == 4, "Combo disposition tiene 4 opciones");
        PostMessageW(form_hwnd, WM_COMMAND, IDCANCEL, 0);
        wait_until([&] { return find_top_window_by_class(pi.dwProcessId, L"MigratoolForm") == nullptr; },
                   "Formulario ID_MARK cerrado");

        // Page 0 (1000): Estado del proyecto includes development_control rows
        send_responsive(main_hwnd, WM_COMMAND, 1000, 0, 200);
        wait_until([&] { return listview_count(lv) >= 10; }, "Página 0 muestra filas incluyendo development_control");

        // Close GUI cleanly
        PostMessageW(main_hwnd, WM_CLOSE, 0, 0);
        const DWORD wait_exit = WaitForSingleObject(pi.hProcess, 10000);
        check(wait_exit == WAIT_OBJECT_0, "Migratool-0.3.exe cierra limpiamente tras pruebas GUI");
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        check(exit_code == 0, "Migratool-0.3.exe retorna exit_code=0");

        const Json receipt = {
            {"passed", true},
            {"checks", checks},
            {"nav1_ms", t_nav1},
            {"etapa_width_px", etapa_width},
            {"claimed_entity", claimed_id},
            {"export_job_id", export_jid},
            {"screenshots", {
                path_text(evidence_dir / L"01-origen-destino-10k.png"),
                path_text(evidence_dir / L"02-filter-claimed.png"),
                path_text(evidence_dir / L"03-centro-trabajos-export.png"),
                path_text(evidence_dir / L"04-cobertura-carpeta.png"),
                path_text(evidence_dir / L"05-configuracion-guiada.png"),
                path_text(evidence_dir / L"06-zoom-200.png")
            }},
            {"build", build_identity()},
            {"at", now()}
        };
        write_json(evidence_dir / L"receipt.json", receipt);
        std::cout << "PASS " << checks << " GUI acceptance checks. Screenshots in " << path_text(evidence_dir) << '\n';
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
