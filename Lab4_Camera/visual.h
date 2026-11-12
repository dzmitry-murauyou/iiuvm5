// ===========================================================================
//  visual.h — интерфейс между логикой (main.cpp) и визуалом (visual.cpp)
//
//  Здесь только объявления: структура Model (что логика сообщает визуалу
//  для отрисовки) и класс View (окно: контролы, раскладка, отрисовка).
//  Никакой логики работы с камерой и файлами в визуальном модуле нет.
// ===========================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <objidl.h>   // PROPID: нужен gdiplus.h до включения GDI+
#include <gdiplus.h>

#include <string>
#include <vector>

namespace visual {

// Состояние камеры — как его показывает индикатор в шапке.
enum CameraState { CameraOff = 0, CameraOpening = 1, CameraOn = 2, CameraFailed = 3 };

// Идентификаторы дочерних контролов (общие для визуала и обработчика команд).
enum : int {
    ID_CAMERA = 1001,
    ID_REFRESH,
    ID_RES,
    ID_CONNECT,
    ID_DISCONNECT,
    ID_SNAPSHOT,
    ID_DIR,
    ID_BROWSE,
    ID_REPORT,
    ID_RECORD_START,
    ID_RECORD_STOP,
    ID_TRAY_BUTTON,
    ID_LOG,
};

// Кадр для предпросмотра: логика отдаёт указатель на BGR-данные на время Paint().
struct FrameView {
    const unsigned char* bgr = nullptr;
    int width = 0;
    int height = 0;
};

// Всё, что визуалу нужно знать о состоянии приложения.
struct Model {
    int camera_state = CameraOff;
    std::wstring status = L"Камера не подключена";
    std::wstring mode;           // «1280 × 720, 30 к/с»
    std::wstring format;         // «MJPG»
    std::wstring last_snapshot;  // имя последнего файла
    int snapshot_count = 0;
    bool recording = false;      // идёт запись видео
    bool has_devices = false;
    FrameView frame;
};

// --- Масштабирование по DPI -------------------------------------------------
void SetDpi(int dpi);
int S(int dip);   // «дизайнерские» пиксели -> физические

// --- Окно ------------------------------------------------------------------
class View {
public:
    View() = default;
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    bool Create(HWND parent, HINSTANCE inst);
    void Destroy();

    void RecreateFonts();                       // после смены DPI
    void Layout();                              // пересчёт раскладки и позиций контролов
    void Paint(HDC dc, const RECT& client, const Model& model);
    bool DrawItem(const DRAWITEMSTRUCT* item);  // owner-draw: кнопки, комбо, журнал
    void SetEnabled(const Model& model);

    // Доступ к контролам для логики (чтение выбора, текста каталога и т.п.)
    HWND CameraCombo() const { return h_camera_; }
    HWND ResolutionCombo() const { return h_res_; }
    HWND DirectoryEdit() const { return h_dir_; }

    void AddLogLine(const std::wstring& line, int level);

    // Всплывающие уведомления внутри окна (короткие, до трёх одновременно).
    void PushToast(const std::wstring& text, int level);
    bool ExpireToasts();   // true, пока есть что показывать

    HBRUSH FieldBrush() const { return field_brush_; }
    RECT PreviewRect() const { return preview_rc_; }

    int MinWidth() const;
    int MinHeight() const;

private:
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);

    void CreateControls();
    void ApplyFontsToControls();

    void PaintBackground(Gdiplus::Graphics& g, int w, int h);
    void DrawHeader(Gdiplus::Graphics& g, const Model& model);
    void DrawPreview(Gdiplus::Graphics& g, const Model& model);
    void DrawPanel(Gdiplus::Graphics& g, const Model& model);
    void DrawSaveCard(Gdiplus::Graphics& g);
    void DrawLogCard(Gdiplus::Graphics& g);

    void DrawToasts(Gdiplus::Graphics& g);

    void DrawButton(const DRAWITEMSTRUCT* item);
    void DrawCombo(const DRAWITEMSTRUCT* item);
    void DrawLogItem(const DRAWITEMSTRUCT* item);

    HWND parent_ = nullptr;
    HINSTANCE inst_ = nullptr;
    bool created_ = false;

    HWND h_camera_ = nullptr, h_refresh_ = nullptr, h_res_ = nullptr;
    HWND h_connect_ = nullptr, h_disconnect_ = nullptr;
    HWND h_snapshot_ = nullptr, h_browse_ = nullptr, h_report_ = nullptr;
    HWND h_record_start_ = nullptr, h_record_stop_ = nullptr;
    HWND h_tray_ = nullptr, h_dir_ = nullptr, h_log_ = nullptr;
    HBRUSH field_brush_ = nullptr;

    Gdiplus::Font* f_title_ = nullptr;
    Gdiplus::Font* f_section_ = nullptr;
    Gdiplus::Font* f_label_ = nullptr;
    Gdiplus::Font* f_value_ = nullptr;
    Gdiplus::Font* f_button_ = nullptr;
    Gdiplus::Font* f_log_ = nullptr;

    // Геометрия карточек и элементов (в физических пикселях).
    RECT preview_rc_{}, panel_rc_{}, save_rc_{}, log_rc_{}, tray_rc_{};
    RECT title_rc_{}, subtitle_rc_{};
    RECT camera_label_{}, res_label_{};
    RECT dir_label_{}, dir_edit_rc_{};
    RECT last_title_rc_{}, last_value_rc_{}, hint_rc_{};

    struct Toast {
        std::wstring text;
        int level = 0;
        ULONGLONG until = 0;
    };
    std::vector<Toast> toasts_;   // новые — в конце
    RECT panel_head_{}, preview_head_{}, save_head_{}, log_head_{};
    int separator_y_ = 0;
    int info_top_ = 0;
    int rows_ = 3;            // сколько строк «подпись — значение» помещается
    bool show_last_ = true;   // хватает ли места на «последний снимок»
    bool show_hint_ = true;   // хватает ли места на подсказку по клавишам
};

}  // namespace visual
