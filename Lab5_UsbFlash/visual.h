// ===========================================================================
//  visual.h — интерфейс между логикой (main.cpp) и визуалом (visual.cpp)
//
//  Здесь только объявления: структура Model (что логика сообщает визуалу
//  для отрисовки) и класс View (окно: контролы, раскладка, отрисовка).
//  Никакой работы с SetupAPI, IOCTL и файлами в визуальном модуле нет.
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
#include <utility>
#include <vector>

namespace visual {

// Состояние поиска — как его показывает индикатор в шапке.
enum ScanState { ScanIdle = 0, ScanBusy = 1, ScanFailed = 2 };

// Идентификаторы дочерних контролов (общие для визуала и обработчика команд).
enum : int {
    ID_REFRESH = 1001,   // «Обновить» (F5) — объявлен первым: акселераторы
    ID_EJECT,            // «Безопасное извлечение» (Ctrl+E)
    ID_REPORT,           // «Сохранить отчёт» (Ctrl+S)
    ID_LIST,             // список USB-накопителей
    ID_DIR,              // поле каталога отчётов
    ID_BROWSE,           // «Обзор…»
    ID_OPEN_DIR,         // «Открыть папку»
    ID_LOG,              // журнал событий
};

// Одна строка карточки «Устройство».
struct Field {
    std::wstring label;
    std::wstring value;
};

// Строка списка накопителей (то, что видно в карточке «USB-накопители»).
struct DeviceRow {
    std::wstring title;      // модель или «USB-накопитель»
    std::wstring subtitle;   // «VID_xxxx · PID_xxxx · 2 тома»
    std::wstring capacity;   // «28,9 ГБ»
    bool has_drive = false;  // есть ли буквы дисков
    bool system = false;     // системный/загрузочный — извлекать нельзя
};

// Всё, что визуалу нужно знать о состоянии приложения.
struct Model {
    int scan_state = ScanIdle;
    std::wstring status = L"Поиск устройств…";   // текст в индикаторе шапки
    std::wstring rights = L"Обычные права";      // права процесса
    bool elevated = false;

    int device_count = 0;
    bool has_device = false;   // выбран ли накопитель
    bool busy = false;         // идёт извлечение

    std::wstring detail_title;   // имя выбранного устройства
    std::wstring detail_sub;     // краткая характеристика
    std::vector<Field> fields;   // строки «подпись — значение»
    std::vector<DeviceRow> rows; // список накопителей
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
    void Layout();                              // пересчёт раскладки и позиций
    void Paint(HDC dc, const RECT& client, const Model& model);
    bool DrawItem(const DRAWITEMSTRUCT* item);  // owner-draw: кнопки, списки
    void SetEnabled(const Model& model);

    // Доступ к контролам для логики (список, выбор, каталог отчётов).
    HWND DeviceList() const { return h_list_; }
    HWND DirectoryEdit() const { return h_dir_; }

    void AddLogLine(const std::wstring& line, int level);

    // Всплывающие уведомления внутри окна (короткие, до трёх одновременно).
    void PushToast(const std::wstring& text, int level);
    bool ExpireToasts();   // true, пока есть что показывать

    HBRUSH FieldBrush() const { return field_brush_; }

    int MinWidth() const;
    int MinHeight() const;

private:
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);

    void CreateControls();
    void ApplyFontsToControls();

    void PaintBackground(Gdiplus::Graphics& g, int w, int h);
    void DrawHeader(Gdiplus::Graphics& g, const Model& model);

    // Метка с цветной точкой: «Накопителей: 2», «Администратор».
    void DrawPill(Gdiplus::Graphics& g, const RECT& rc, const std::wstring& text,
                  const Gdiplus::Color& dot);
    void DrawDriveIcon(Gdiplus::Graphics& g, int x, int y, const Gdiplus::Color& accent);

    void DrawList(Gdiplus::Graphics& g, const Model& model);
    void DrawPanel(Gdiplus::Graphics& g, const Model& model);
    void DrawActions(Gdiplus::Graphics& g);
    void DrawLogCard(Gdiplus::Graphics& g);
    void DrawToasts(Gdiplus::Graphics& g);

    void DrawButton(const DRAWITEMSTRUCT* item);
    void DrawDeviceItem(const DRAWITEMSTRUCT* item);
    const DeviceRow* RowForItem(const DRAWITEMSTRUCT* item) const;
    void DrawLogItem(const DRAWITEMSTRUCT* item);

    HWND parent_ = nullptr;
    HINSTANCE inst_ = nullptr;
    bool created_ = false;

    HWND h_list_ = nullptr, h_refresh_ = nullptr, h_eject_ = nullptr;
    HWND h_report_ = nullptr, h_browse_ = nullptr, h_open_ = nullptr;
    HWND h_dir_ = nullptr, h_log_ = nullptr;
    HBRUSH field_brush_ = nullptr;

    Gdiplus::Font* f_title_ = nullptr;
    Gdiplus::Font* f_section_ = nullptr;
    Gdiplus::Font* f_label_ = nullptr;
    Gdiplus::Font* f_value_ = nullptr;
    Gdiplus::Font* f_button_ = nullptr;
    Gdiplus::Font* f_log_ = nullptr;

    // Геометрия карточек и элементов (в физических пикселях).
    RECT header_rc_{}, list_rc_{}, panel_rc_{}, action_rc_{}, log_rc_{};
    RECT title_rc_{}, subtitle_rc_{}, state_pill_{}, rights_pill_{};
    RECT list_head_{}, list_count_{}, list_body_{};
    RECT panel_head_{}, detail_title_rc_{}, detail_sub_rc_{}, hint_rc_{};
    RECT action_head_{}, dir_label_{}, dir_edit_rc_{};
    RECT log_head_{};

    int panel_rows_ = 6;       // сколько строк «подпись — значение» поместилось
    int separator_y_ = 0;      // разделитель в карточке «Устройство»
    int fields_top_ = 0;       // верх первой строки свойств

    struct Toast {
        std::wstring text;
        int level = 0;
        ULONGLONG until = 0;
    };
    // Копия строк списка на момент последней отрисовки: owner-draw строка
    // не имеет доступа к Model, поэтому визуал запоминает её у себя.
    std::vector<DeviceRow> list_rows_;
    std::vector<Toast> toasts_;   // новые — в конце
};

}  // namespace visual
