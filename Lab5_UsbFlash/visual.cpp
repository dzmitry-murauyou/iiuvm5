// ===========================================================================
//  visual.cpp — вся визуальная часть: тема, шрифты, раскладка, отрисовка
//
//  Модуль ничего не знает об USB, SetupAPI и IOCTL: он получает готовую
//  структуру visual::Model и рисует по ней окно. Вся логика — в main.cpp.
//
//  Оформление и приёмы раскладки те же, что в ЛР №4: тёмная тема GDI+,
//  карточки со скруглением, «поток сверху вниз» внутри карточки с обрезкой
//  по её границам (SetClip), поэтому элементы не наезжают друг на друга
//  при любом размере окна и любом масштабе DPI.
// ===========================================================================

#include "visual.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace visual {

using namespace Gdiplus;

// ---------------------------------------------------------------------------
//  Тема (как в ЛР №4): цвета и «дизайнерские» размеры в DIP
// ---------------------------------------------------------------------------
namespace theme {

const Color Bg(255, 10, 14, 24);
const Color Card(215, 18, 26, 44);
const Color Txt(255, 235, 240, 248);
const Color Sub(255, 165, 178, 198);
const Color Dim(255, 120, 130, 150);
const Color Line(255, 70, 95, 135);
const Color Good(255, 46, 204, 113);
const Color Warn(255, 241, 196, 15);
const Color Bad(255, 235, 87, 87);
const Color Field(235, 12, 18, 30);
const Color Btn(255, 30, 42, 64);
const Color BtnHot(255, 42, 58, 86);
const Color Accent(255, 84, 160, 255);   // «USB»-акцент интерфейса

const int Pad = 22;         // поля окна
const int Gap = 12;         // между карточками
const int HeaderH = 92;     // шапка
const int LogH = 120;       // карточка журнала
const int ActionH = 148;    // карточка «Действия»
const int PanelW = 392;     // карточка «Устройство»
const int RowH = 26;        // строка «подпись — значение»
const int InnerPad = 16;    // поле внутри карточки
const REAL Radius = 6.0f;
const int ItemH = 58;       // строка списка накопителей

}  // namespace theme

// ---------------------------------------------------------------------------
//  DPI
// ---------------------------------------------------------------------------
namespace {
int g_dpi = 96;
}

void SetDpi(int dpi) { g_dpi = (dpi >= 96 && dpi <= 480) ? dpi : 96; }

int S(int dip) { return MulDiv(dip, g_dpi, 96); }

// ---------------------------------------------------------------------------
//  Помощники рисования
// ---------------------------------------------------------------------------
namespace {

int Px(REAL value) { return (int)std::lround(value); }

void FillRectG(Graphics& g, const RECT& rc, const Color& color) {
    SolidBrush brush(color);
    g.FillRectangle(&brush, (INT)rc.left, (INT)rc.top, (INT)(rc.right - rc.left),
                    (INT)(rc.bottom - rc.top));
}

void RoundedRect(Graphics& g, const RECT& rc, const Color& fill, const Color& border,
                 REAL radius) {
    GraphicsPath path;
    const REAL x = (REAL)rc.left, y = (REAL)rc.top;
    const REAL w = (REAL)(rc.right - rc.left), h = (REAL)(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) return;
    const REAL d = std::min(radius * 2, std::min(w, h));
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
    Pen pen(border, 1.0f);
    g.DrawPath(&pen, &path);
}

REAL RadiusPx() { return S(theme::Radius * 4) / 4.0f; }

void Card(Graphics& g, const RECT& rc) { RoundedRect(g, rc, theme::Card, theme::Line, RadiusPx()); }

// Текст всегда обрезается по своему прямоугольнику с многоточием —
// переноса строк нет, поэтому слова не налезают друг на друга.
void TextG(Graphics& g, const std::wstring& text, const RECT& rc, Font* font, const Color& color,
           StringAlignment align = StringAlignmentNear) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush brush(color);
    RectF layout((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left),
                 (REAL)(rc.bottom - rc.top));
    g.DrawString(text.c_str(), -1, font, layout, &fmt, &brush);
}

int TextWidth(Graphics& g, const std::wstring& text, Font* font) {
    if (text.empty()) return 0;
    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF bounds;
    g.MeasureString(text.c_str(), -1, font, RectF(0, 0, 4000.0f, 100.0f), &fmt, &bounds);
    return Px(bounds.Width);
}

void Dot(Graphics& g, int cx, int cy, int r, const Color& color) {
    SolidBrush brush(color);
    g.FillEllipse(&brush, cx - r, cy - r, r * 2, r * 2);
}

void Separator(Graphics& g, const RECT& card, int y) {
    Pen pen(theme::Line, 1.0f);
    g.DrawLine(&pen, (REAL)(card.left + S(theme::InnerPad)), (REAL)y,
               (REAL)(card.right - S(theme::InnerPad)), (REAL)y);
}

// Строка «подпись — значение»: подпись в фиксированной колонке, значение —
// в остатке ширины с многоточием.
void Row(Graphics& g, const RECT& card, int y, const std::wstring& label,
         const std::wstring& value, Font* label_font, Font* value_font) {
    const int p = S(theme::InnerPad);
    const int label_w = S(112);
    RECT l{card.left + p, y, std::min(card.left + p + label_w, card.right - p),
           y + S(theme::RowH)};
    RECT v{l.right, y, card.right - p, y + S(theme::RowH)};
    TextG(g, label, l, label_font, theme::Sub);
    TextG(g, value.empty() ? L"—" : value, v, value_font, theme::Txt);
}

Color StateColor(int state) {
    switch (state) {
        case ScanBusy:   return theme::Warn;
        case ScanFailed: return theme::Bad;
        default:         return theme::Good;
    }
}

Color LevelColor(int level) {
    switch (level) {
        case 2: return theme::Bad;      // ошибка
        case 1: return theme::Warn;     // предупреждение
        case 3: return theme::Good;     // успешное действие
        default: return theme::Txt;     // информация
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  Жизненный цикл
// ---------------------------------------------------------------------------
View::~View() { Destroy(); }

bool View::Create(HWND parent, HINSTANCE inst) {
    parent_ = parent;
    inst_ = inst;
    field_brush_ = CreateSolidBrush(RGB(12, 18, 30));
    RecreateFonts();
    CreateControls();
    ApplyFontsToControls();   // высоты пунктов списков под текущий DPI
    created_ = true;
    Layout();
    return true;
}

void View::Destroy() {
    for (Font** font : {&f_title_, &f_section_, &f_label_, &f_value_, &f_button_, &f_log_}) {
        delete *font;
        *font = nullptr;
    }
    if (field_brush_) {
        DeleteObject(field_brush_);
        field_brush_ = nullptr;
    }
    created_ = false;
}

// Шрифты в DIP: при смене DPI размеры пересчитываются, текст остаётся чётким.
void View::RecreateFonts() {
    for (Font** font : {&f_title_, &f_section_, &f_label_, &f_value_, &f_button_, &f_log_}) {
        delete *font;
        *font = nullptr;
    }

    FontFamily family(L"Segoe UI");
    // Одна гарнитура на всё приложение — строки и колонки выровнены.
    f_title_ = new Font(&family, (REAL)S(24), FontStyleBold, UnitPixel);
    f_section_ = new Font(&family, (REAL)S(16), FontStyleBold, UnitPixel);
    f_label_ = new Font(&family, (REAL)S(14), FontStyleRegular, UnitPixel);
    f_value_ = new Font(&family, (REAL)S(14), FontStyleBold, UnitPixel);
    f_button_ = new Font(&family, (REAL)S(14), FontStyleBold, UnitPixel);
    f_log_ = new Font(&family, (REAL)S(14), FontStyleRegular, UnitPixel);

    ApplyFontsToControls();
}

HWND View::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent_,
                           (HMENU)(INT_PTR)id, inst_, nullptr);
}

void View::CreateControls() {
    // Список накопителей: owner-draw, чтобы каждая строка была «карточкой»
    // с моделью, VID/PID и объёмом.
    h_list_ = Make(L"LISTBOX", L"",
                   WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                       LBS_NOTIFY | WS_TABSTOP,
                   ID_LIST);
    h_refresh_ = Make(L"BUTTON", L"Обновить", BS_OWNERDRAW | WS_TABSTOP, ID_REFRESH);
    h_eject_ = Make(L"BUTTON", L"Безопасное извлечение", BS_OWNERDRAW | WS_TABSTOP, ID_EJECT);
    h_report_ = Make(L"BUTTON", L"Сохранить отчёт", BS_OWNERDRAW | WS_TABSTOP, ID_REPORT);
    h_browse_ = Make(L"BUTTON", L"Обзор…", BS_OWNERDRAW | WS_TABSTOP, ID_BROWSE);
    h_open_ = Make(L"BUTTON", L"Открыть папку", BS_OWNERDRAW | WS_TABSTOP, ID_OPEN_DIR);

    h_dir_ = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_DIR);
    h_log_ = Make(L"LISTBOX", L"",
                  WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                      WS_TABSTOP,
                  ID_LOG);
    SendMessageW(h_log_, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

void View::ApplyFontsToControls() {
    if (!h_log_) return;
    SendMessageW(h_list_, LB_SETITEMHEIGHT, 0, S(theme::ItemH));
    SendMessageW(h_log_, LB_SETITEMHEIGHT, 0, S(22));
}

void View::AddLogLine(const std::wstring& line, int level) {
    if (!h_log_) return;
    const int index = (int)SendMessageW(h_log_, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    if (index < 0) return;
    SendMessageW(h_log_, LB_SETITEMDATA, index, (LPARAM)level);
    SendMessageW(h_log_, LB_SETTOPINDEX, index, 0);
    // Список не растёт бесконечно: старые строки уходят, полный журнал — в файле.
    while (SendMessageW(h_log_, LB_GETCOUNT, 0, 0) > 500) {
        SendMessageW(h_log_, LB_DELETESTRING, 0, 0);
    }
}

int View::MinWidth() const {
    // карточка списка (380) + карточка устройства (PanelW) + отступы и поля
    return S(theme::Pad * 2 + 380 + theme::Gap + theme::PanelW + 40);
}

int View::MinHeight() const {
    // шапка + список (три строки) + действия + журнал + поля окна
    return S(theme::Pad * 2 + theme::HeaderH + theme::Gap +
             42 + 3 * theme::ItemH + theme::Gap + theme::ActionH + theme::Gap + theme::LogH);
}

// ---------------------------------------------------------------------------
//  Раскладка
// ---------------------------------------------------------------------------
void View::Layout() {
    if (!created_ || !h_log_) return;

    RECT rc;
    GetClientRect(parent_, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    const int pad = S(theme::Pad), gap = S(theme::Gap);
    const int p = S(theme::InnerPad);

    log_rc_ = {pad, h - pad - S(theme::LogH), w - pad, h - pad};
    action_rc_ = {pad, log_rc_.top - gap - S(theme::ActionH), w - pad, log_rc_.top - gap};
    header_rc_ = {0, 0, w, S(theme::HeaderH)};

    // Слева — список накопителей, справа — карточка свойств.
    panel_rc_ = {w - pad - S(theme::PanelW), S(theme::HeaderH), w - pad, action_rc_.top - gap};
    list_rc_ = {pad, S(theme::HeaderH), panel_rc_.left - gap, action_rc_.top - gap};

    // --- шапка -------------------------------------------------------------
    // Метки фиксированной ширины: справа «права процесса», левее — состояние
    // поиска. Заголовок получает остаток и обрезается многоточием, поэтому
    // наезжать на метки он не может ни при каком тексте.
    {
        const int cy = S(22), pill_h = S(30);
        const int rights_w = S(172);
        const int state_w = S(232);
        rights_pill_ = {w - pad - rights_w, cy, w - pad, cy + pill_h};
        state_pill_ = {rights_pill_.left - S(10) - state_w, cy, rights_pill_.left - S(10),
                       cy + pill_h};
    }
    title_rc_ = {pad, S(14), state_pill_.left - S(16), S(50)};
    subtitle_rc_ = {pad, S(52), state_pill_.left - S(16), S(74)};

    // --- карточка «USB-накопители» ----------------------------------------
    {
        list_head_ = {list_rc_.left + p, list_rc_.top + S(10), list_rc_.right - p,
                      list_rc_.top + S(34)};

        const int refresh_w = S(112);
        const int browse_w = S(104);
        const int button_y = list_rc_.top + S(42);
        const int browse_x = list_rc_.right - p - browse_w;
        const int count_right = browse_x - S(10) - refresh_w - S(10);
        list_count_ = {list_rc_.left + p, button_y, count_right, button_y + S(28)};

        SetWindowPos(h_refresh_, nullptr, count_right + S(10), button_y, refresh_w, S(28),
                     SWP_NOZORDER);
        SetWindowPos(h_browse_, nullptr, browse_x, button_y, browse_w, S(28), SWP_NOZORDER);

        list_body_ = {list_rc_.left + p, button_y + S(38), list_rc_.right - p,
                      list_rc_.bottom - S(12)};
        SetWindowPos(h_list_, nullptr, list_body_.left, list_body_.top,
                     list_body_.right - list_body_.left, list_body_.bottom - list_body_.top,
                     SWP_NOZORDER);
    }

    // --- карточка «Устройство» --------------------------------------------
    {
        const int left = panel_rc_.left + p;
        const int right = panel_rc_.right - p;

        panel_head_ = {left, panel_rc_.top + S(10), right, panel_rc_.top + S(34)};

        detail_title_rc_ = {left, panel_rc_.top + S(42), right, panel_rc_.top + S(68)};
        detail_sub_rc_ = {left, panel_rc_.top + S(68), right, panel_rc_.top + S(90)};
        separator_y_ = panel_rc_.top + S(100);
        fields_top_ = separator_y_ + S(10);

        // Адаптивная раскладка: показываем столько строк, сколько влезает,
        // остальные просто не рисуются (полная информация есть в отчёте).
        const int bottom_limit = panel_rc_.bottom - S(12);
        const int hint_h = S(26);
        const int space = bottom_limit - fields_top_ - hint_h;
        panel_rows_ = std::clamp(space / S(theme::RowH), 0, 12);

        hint_rc_ = {left, bottom_limit - S(22), right, bottom_limit};
    }

    // --- карточка «Действия» ----------------------------------------------
    {
        const int left = action_rc_.left + p;
        const int right = action_rc_.right - p;

        action_head_ = {left, action_rc_.top + S(8), right, action_rc_.top + S(30)};

        // Ряд 1: каталог отчётов (кнопка «Обзор…» живёт в карточке списка).
        const int y1 = action_rc_.top + S(38);
        const int label_w = S(122);
        dir_label_ = {left, y1, left + label_w, y1 + S(26)};

        const int edit_x = dir_label_.right + S(8);
        dir_edit_rc_ = {edit_x, y1, right, y1 + S(26)};
        SetWindowPos(h_dir_, nullptr, dir_edit_rc_.left, dir_edit_rc_.top,
                     dir_edit_rc_.right - dir_edit_rc_.left,
                     dir_edit_rc_.bottom - dir_edit_rc_.top, SWP_NOZORDER);

        // Ряд 2: извлечение и отчёт (главные операции).
        const int y2 = action_rc_.top + S(76);
        const int gap2 = S(10);
        const int half = ((int)right - left - gap2) / 2;
        SetWindowPos(h_eject_, nullptr, left, y2, half, S(32), SWP_NOZORDER);
        SetWindowPos(h_report_, nullptr, left + half + gap2, y2,
                     (int)right - (left + half + gap2), S(32), SWP_NOZORDER);

        // Ряд 3: открыть папку с отчётами.
        const int y3 = action_rc_.top + S(114);
        SetWindowPos(h_open_, nullptr, left, y3, std::min(S(200), (int)(right - left)), S(26),
                     SWP_NOZORDER);
    }

    // --- карточка «Журнал» -------------------------------------------------
    {
        log_head_ = {log_rc_.left + p, log_rc_.top + S(8), log_rc_.right - p,
                     log_rc_.top + S(30)};
        const int top = log_rc_.top + S(38);
        SetWindowPos(h_log_, nullptr, log_rc_.left + p, top,
                     log_rc_.right - log_rc_.left - 2 * p,
                     std::max(S(40), (int)log_rc_.bottom - S(12) - top), SWP_NOZORDER);
    }

    InvalidateRect(parent_, nullptr, FALSE);
}

void View::SetEnabled(const Model& model) {
    const bool busy = model.scan_state == ScanBusy;
    const bool can_eject = model.has_device && !model.busy && !busy;

    // Пустой список прячем: иначе пустой LISTBOX закрасит собой подсказку
    // «USB-накопитель не найден», которую рисует карточка.
    ShowWindow(h_list_, model.rows.empty() ? SW_HIDE : SW_SHOW);

    EnableWindow(h_refresh_, (!busy && !model.busy) ? TRUE : FALSE);
    EnableWindow(h_list_, (!busy && !model.busy) ? TRUE : FALSE);
    EnableWindow(h_eject_, can_eject ? TRUE : FALSE);
    EnableWindow(h_report_, (!busy && !model.busy) ? TRUE : FALSE);
    EnableWindow(h_dir_, TRUE);
    EnableWindow(h_browse_, TRUE);
    EnableWindow(h_open_, TRUE);

    // Owner-draw кнопки не перерисовываются сами при смене состояния —
    // иначе «Извлечь» остаётся нарисованной в старом виде.
    for (HWND button : {h_refresh_, h_eject_, h_report_, h_browse_, h_open_}) {
        InvalidateRect(button, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
//  Отрисовка окна
// ---------------------------------------------------------------------------
void View::PaintBackground(Graphics& g, int w, int h) {
    SolidBrush base(theme::Bg);
    g.FillRectangle(&base, 0, 0, w, h);

    LinearGradientBrush sheen(Point(0, 0), Point(0, h), Color(255, 26, 34, 54),
                              Color(255, 8, 11, 20));
    g.FillRectangle(&sheen, 0, 0, w, h);
}

void View::Paint(HDC dc, const RECT& client, const Model& model) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    list_rows_ = model.rows;   // строки нужны обработчику DRAWITEM
    PaintBackground(g, client.right, client.bottom);
    DrawHeader(g, model);
    DrawList(g, model);
    DrawPanel(g, model);
    DrawActions(g);
    DrawLogCard(g);
    DrawToasts(g);
}

// Метка-«пилюля»: цветная точка и текст. Ширина уже посчитана в Layout()
// по фактическому тексту, поэтому заголовок и метки не наезжают друг на друга.
void View::DrawPill(Graphics& g, const RECT& rc, const std::wstring& text, const Color& dot) {
    if (rc.right <= rc.left) return;
    const REAL radius = rc.bottom > rc.top ? (rc.bottom - rc.top) / 2.0f : RadiusPx();
    RoundedRect(g, rc, Color(255, 20, 28, 46), theme::Line, radius);
    const int cy = (rc.top + rc.bottom) / 2;
    Dot(g, rc.left + S(14), cy, S(5), dot);
    RECT text_rc{rc.left + S(26), rc.top, rc.right - S(10), rc.bottom};
    TextG(g, text, text_rc, f_value_, theme::Txt);
}

void View::DrawHeader(Graphics& g, const Model& model) {
    g.SetClip(RectF(0.0f, 0.0f, (REAL)std::max<LONG>(1, header_rc_.right), (REAL)S(theme::HeaderH)));

    TextG(g, L"USB-накопители", title_rc_, f_title_, theme::Txt);
    TextG(g, L"SetupAPI · IOCTL · безопасное извлечение (ЛР №5, вариант А1)", subtitle_rc_,
          f_label_, theme::Sub);

    // Слева направо: состояние поиска, затем права процесса.
    DrawPill(g, state_pill_, model.status.empty() ? L"Поиск устройств…" : model.status,
             StateColor(model.scan_state));

    const Color rights = model.elevated ? theme::Good : theme::Warn;
    DrawPill(g, rights_pill_, model.rights, rights);

    // Тонкая линия под шапкой отделяет её от карточек.
    Pen pen(Color(255, 34, 46, 70), 1.0f);
    g.DrawLine(&pen, (REAL)S(theme::Pad), (REAL)(S(theme::HeaderH) - 1),
               (REAL)(header_rc_.right - S(theme::Pad)), (REAL)(S(theme::HeaderH) - 1));

    g.ResetClip();
}

// Значок флешки: корпус, колпачок и контакт — рисуется вектором, поэтому
// не зависит от DPI и размера значка.
void View::DrawDriveIcon(Graphics& g, int x, int y, const Color& accent) {
    const int unit = S(10);
    RECT body{x, y, x + unit * 3, y + unit * 2};
    RoundedRect(g, body, Color(255, 28, 40, 60), accent, S(2));
    RECT cap{x + unit * 3, y + unit / 2, x + unit * 4, y + unit * 2 - unit / 2};
    RoundedRect(g, cap, accent, accent, S(1));
    RectF slot((REAL)(x + unit / 2), (REAL)(y + unit / 2), (REAL)S(6), (REAL)S(2));
    SolidBrush slot_brush(Color(255, 10, 16, 26));
    g.FillRectangle(&slot_brush, slot);
}

void View::DrawList(Graphics& g, const Model& model) {
    g.SetClip(RectF((REAL)list_rc_.left, (REAL)list_rc_.top,
                    (REAL)(list_rc_.right - list_rc_.left),
                    (REAL)(list_rc_.bottom - list_rc_.top)));

    Card(g, list_rc_);
    TextG(g, L"USB-накопители", list_head_, f_section_, theme::Txt);

    std::wstring count = model.device_count == 0
                             ? L"Устройства не найдены"
                             : L"Найдено: " + std::to_wstring(model.device_count);
    TextG(g, count, list_count_, f_label_, theme::Sub);

    if (model.rows.empty()) {
        // Пустое состояние: значок, заголовок и подсказка по центру списка.
        const int cx = (list_body_.left + list_body_.right) / 2;
        const int cy = (list_body_.top + list_body_.bottom) / 2;
        DrawDriveIcon(g, cx - S(20), cy - S(46), theme::Dim);

        RECT head{list_body_.left, cy - S(18), list_body_.right, cy + S(8)};
        TextG(g, model.scan_state == ScanBusy ? L"Опрос устройств…" : L"USB-накопитель не найден",
              head, f_section_, theme::Sub, StringAlignmentCenter);

        RECT hint{list_body_.left, cy + S(10), list_body_.right, cy + S(34)};
        TextG(g,
              model.scan_state == ScanBusy
                  ? L"Идёт перечисление через SetupAPI"
                  : L"Подключите флешку — список обновится автоматически",
              hint, f_label_, theme::Dim, StringAlignmentCenter);
    }

    g.ResetClip();
}

void View::DrawPanel(Graphics& g, const Model& model) {
    g.SetClip(RectF((REAL)panel_rc_.left, (REAL)panel_rc_.top,
                    (REAL)(panel_rc_.right - panel_rc_.left),
                    (REAL)(panel_rc_.bottom - panel_rc_.top)));

    Card(g, panel_rc_);
    TextG(g, L"Устройство", panel_head_, f_section_, theme::Txt);

    if (!model.has_device) {
        RECT rc{panel_rc_.left + S(theme::InnerPad), panel_rc_.top,
                panel_rc_.right - S(theme::InnerPad), panel_rc_.bottom};
        TextG(g, L"Выберите накопитель в списке слева", rc, f_label_, theme::Dim,
              StringAlignmentCenter);
        g.ResetClip();
        return;
    }

    // Заголовок выбранного устройства с акцентом и маркером наличия тома.
    Dot(g, detail_title_rc_.left + S(5), detail_title_rc_.top + S(13), S(5),
        model.fields.empty() ? theme::Dim : theme::Accent);
    RECT title = detail_title_rc_;
    title.left += S(18);
    TextG(g, model.detail_title, title, f_value_, theme::Txt);

    TextG(g, model.detail_sub, detail_sub_rc_, f_label_, theme::Sub);

    Separator(g, panel_rc_, separator_y_);

    // Строки идут сверху вниз; рисуются только те, что поместились.
    int y = fields_top_;
    for (int i = 0; i < panel_rows_ && i < (int)model.fields.size(); ++i) {
        Row(g, panel_rc_, y, model.fields[i].label, model.fields[i].value, f_label_, f_value_);
        y += S(theme::RowH);
    }

    TextG(g, L"Полные сведения — в отчёте (кнопка «Сохранить отчёт»)", hint_rc_, f_label_,
          theme::Dim);

    g.ResetClip();
}

void View::DrawActions(Graphics& g) {
    g.SetClip(RectF((REAL)action_rc_.left, (REAL)action_rc_.top,
                    (REAL)(action_rc_.right - action_rc_.left),
                    (REAL)(action_rc_.bottom - action_rc_.top)));

    Card(g, action_rc_);
    TextG(g, L"Действия", action_head_, f_section_, theme::Txt);
    TextG(g, L"Каталог отчётов", dir_label_, f_label_, theme::Sub);

    // Рамка вокруг поля ввода: у EDIT нет своего оформления.
    RECT field = dir_edit_rc_;
    field.left -= 1;
    field.top -= 1;
    field.right += 1;
    field.bottom += 1;
    RoundedRect(g, field, Color(255, 12, 18, 30), theme::Line, RadiusPx());

    g.ResetClip();
}

void View::DrawLogCard(Graphics& g) {
    g.SetClip(RectF((REAL)log_rc_.left, (REAL)log_rc_.top, (REAL)(log_rc_.right - log_rc_.left),
                    (REAL)(log_rc_.bottom - log_rc_.top)));

    Card(g, log_rc_);
    TextG(g, L"Журнал событий", log_head_, f_section_, theme::Txt);

    // Горячие клавиши — справа в заголовке карточки.
    RECT keys{log_head_.left, log_head_.top, log_head_.right, log_head_.bottom};
    TextG(g, L"F5 — обновить · Ctrl+E — извлечь · Ctrl+S — отчёт", keys, f_label_, theme::Dim,
          StringAlignmentFar);

    g.ResetClip();
}

// ---------------------------------------------------------------------------
//  Owner-draw: кнопки, строки списка, строки журнала
// ---------------------------------------------------------------------------
bool View::DrawItem(const DRAWITEMSTRUCT* item) {
    if (!item) return false;
    switch (item->CtlType) {
        case ODT_BUTTON:    DrawButton(item); return true;
        case ODT_LISTBOX:
            if (item->CtlID == ID_LIST) {
                DrawDeviceItem(item);
                return true;
            }
            DrawLogItem(item);
            return true;
        default:
            return false;
    }
}

void View::PushToast(const std::wstring& text, int level) {
    if (text.empty()) return;
    if (toasts_.size() >= 3) toasts_.erase(toasts_.begin());   // старые уходят
    toasts_.push_back({text, level, GetTickCount64() + 2600});
}

bool View::ExpireToasts() {
    const ULONGLONG now = GetTickCount64();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [now](const Toast& t) { return t.until <= now; }),
                  toasts_.end());
    return !toasts_.empty();
}

// Уведомления рисуются поверх карточки устройства, стопкой (до трёх сразу),
// и плавно гаснут в последние 0,4 с.
void View::DrawToasts(Graphics& g) {
    if (toasts_.empty()) return;

    const ULONGLONG now = GetTickCount64();
    g.SetClip(RectF((REAL)panel_rc_.left, (REAL)panel_rc_.top,
                    (REAL)(panel_rc_.right - panel_rc_.left),
                    (REAL)(panel_rc_.bottom - panel_rc_.top)));

    const int right = panel_rc_.right - S(12);
    int y = panel_rc_.top + S(12);

    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        const ULONGLONG left = it->until > now ? it->until - now : 0;
        const int alpha = left < 400 ? (int)(255 * left / 400) : 255;
        if (alpha <= 4) break;

        Color accent = theme::Line;
        if (it->level == 1) accent = theme::Warn;
        else if (it->level == 2) accent = theme::Bad;
        else if (it->level == 3) accent = theme::Good;

        const int w = std::min(S(430), TextWidth(g, it->text, f_value_) + S(40));
        const int h = S(34);
        RECT rc{right - w, y, right, y + h};

        RoundedRect(g, rc, Color((BYTE)(235 * alpha / 255), 18, 26, 44),
                    Color((BYTE)alpha, accent.GetR(), accent.GetG(), accent.GetB()), RadiusPx());

        RECT bar{rc.left + S(8), rc.top + S(8), rc.left + S(12), rc.bottom - S(8)};
        FillRectG(g, bar, Color((BYTE)alpha, accent.GetR(), accent.GetG(), accent.GetB()));

        RECT text_rc{rc.left + S(20), rc.top, rc.right - S(10), rc.bottom};
        TextG(g, it->text, text_rc, f_value_, Color((BYTE)alpha, 235, 240, 248));

        y += h + S(6);
        if (y > panel_rc_.bottom - h) break;   // больше не помещается
    }

    g.ResetClip();
}

void View::DrawButton(const DRAWITEMSTRUCT* item) {
    Graphics g(item->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;

    Color fill = theme::Btn, border = theme::Line, text = theme::Txt;
    if (disabled) {
        // Отключённая кнопка остаётся читаемой: фон чуть светлее карточки,
        // текст — светлый серый (контраст ≈ 7:1), а не почти невидимый Dim.
        fill = Color(255, 26, 33, 48);
        border = Color(255, 62, 76, 102);
        text = Color(255, 158, 170, 190);
    } else if (item->CtlID == ID_EJECT) {
        // Опасная операция — красноватый акцент, как «стоп» в ЛР №4.
        fill = pressed ? Color(255, 132, 44, 40) : Color(255, 106, 34, 32);
        border = Color(255, 214, 96, 88);
        text = Color(255, 255, 236, 234);
    } else if (item->CtlID == ID_REFRESH) {
        fill = pressed ? Color(255, 30, 130, 90) : Color(255, 24, 110, 74);
        border = Color(255, 60, 200, 140);
        text = Color(255, 235, 255, 245);
    } else if (pressed) {
        fill = theme::BtnHot;
    }

    RECT rc = item->rcItem;
    RoundedRect(g, rc, fill, border, RadiusPx());

    wchar_t buffer[128] = {};
    GetWindowTextW(item->hwndItem, buffer, 128);
    TextG(g, buffer, rc, f_button_, text, StringAlignmentCenter);
}

// Строка списка накопителей: значок, модель, VID/PID с томами и объём справа.
// Содержимое берётся из копии model.rows, которую сохраняет Paint().
const DeviceRow* View::RowForItem(const DRAWITEMSTRUCT* item) const {
    const LRESULT data = SendMessageW(item->hwndItem, LB_GETITEMDATA, item->itemID, 0);
    if (data == LB_ERR || data < 0) return nullptr;
    const size_t index = (size_t)data;
    if (index >= list_rows_.size()) return nullptr;
    return &list_rows_[index];
}

void View::DrawDeviceItem(const DRAWITEMSTRUCT* item) {
    Graphics g(item->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    RECT rc = item->rcItem;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;

    FillRectG(g, rc, Color(255, 14, 20, 34));

    RECT card = rc;
    card.left += S(4);
    card.right -= S(4);
    card.top += S(3);
    card.bottom -= S(3);
    RoundedRect(g, card, selected ? Color(255, 32, 52, 82) : Color(255, 20, 28, 44),
                selected ? theme::Accent : theme::Line, RadiusPx());

    const DeviceRow* row = RowForItem(item);

    DrawDriveIcon(g, card.left + S(14), card.top + S(15),
                  selected ? theme::Accent : theme::Sub);

    RECT title{card.left + S(60), card.top + S(8), card.right - S(96), card.top + S(32)};
    TextG(g, row && !row->title.empty() ? row->title : L"USB-накопитель", title, f_value_,
          theme::Txt);

    RECT sub{card.left + S(60), card.top + S(31), card.right - S(96), card.top + S(52)};
    TextG(g, row ? row->subtitle : L"", sub, f_log_, theme::Sub);

    // Справа: объём и метка наличия тома.
    RECT cap{card.right - S(92), card.top + S(8), card.right - S(14), card.top + S(32)};
    TextG(g, row ? row->capacity : L"", cap, f_value_, theme::Txt, StringAlignmentFar);

    const bool system = row && row->system;
    const bool has_drive = row && row->has_drive;
    std::wstring tag = system ? L"системный" : (has_drive ? L"том смонтирован" : L"без тома");
    const Color tag_color = system ? theme::Bad : (has_drive ? theme::Good : theme::Dim);
    RECT tag_rc{card.right - S(130), card.top + S(31), card.right - S(14), card.top + S(52)};
    const int tag_w = TextWidth(g, tag, f_log_);
    Dot(g, tag_rc.right - tag_w - S(8), (tag_rc.top + tag_rc.bottom) / 2, S(4), tag_color);
    TextG(g, tag, tag_rc, f_log_, tag_color, StringAlignmentFar);
}

void View::DrawLogItem(const DRAWITEMSTRUCT* item) {
    Graphics g(item->hDC);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    wchar_t buffer[1024] = {};
    SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, (LPARAM)buffer);
    const std::wstring line(buffer);

    FillRectG(g, item->rcItem, Color(255, 14, 20, 34));

    const size_t time_len = 23;   // «29.10.2026 22:15:03.120»
    const std::wstring stamp = line.substr(0, std::min(time_len, line.size()));

    RECT rc = item->rcItem;
    rc.left += S(8);
    RECT time_rc = rc;
    // Ширину метки времени считаем по реальному тексту, иначе сообщение
    // прилипает к дате (или наоборот, дата обрезается).
    time_rc.right = std::min(rc.right, rc.left + TextWidth(g, stamp + L"  ", f_log_));
    TextG(g, stamp, time_rc, f_log_, theme::Dim);

    RECT text_rc = rc;
    text_rc.left = time_rc.right;
    const int level = (int)SendMessageW(item->hwndItem, LB_GETITEMDATA, item->itemID, 0);
    TextG(g, line.size() > time_len ? line.substr(time_len) : L"", text_rc, f_log_,
          LevelColor(level));
}

}  // namespace visual
