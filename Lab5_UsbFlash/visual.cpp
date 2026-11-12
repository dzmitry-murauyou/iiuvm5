// ===========================================================================
//  visual.cpp — вся визуальная часть: тема, шрифты, раскладка, отрисовка
//
//  Модуль ничего не знает об USB, SetupAPI и IOCTL: он получает готовую
//  структуру visual::Model и рисует по ней окно. Вся логика — в main.cpp.
// ===========================================================================

#include "visual.h"

#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace visual {

using namespace Gdiplus;

// ---------------------------------------------------------------------------
//  Тема: цвета и «дизайнерские» размеры в DIP
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
const int LogH = 262;       // журнал: заголовок, строка подсказки и девять записей
const int ActionH = 148;    // карточка «Действия»
const int PanelW = 556;     // карточка «Устройство» на всю высоту окна
const int RowH = 25;        // строка «подпись — значение»
const int LabelW = 168;     // колонка подписей в карточке «Устройство»
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
void Row(Graphics& g, const int label_left, const int value_left, const int right, int y,
         const std::wstring& label, const std::wstring& value, Font* label_font,
         Font* value_font) {
    RECT l{label_left, y, std::min(value_left, right), y + S(theme::RowH)};
    RECT v{value_left, y, right, y + S(theme::RowH)};
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
    // WS_EX_NOPARENTNOTIFY не нужен; для кнопок важен BS_OWNERDRAW + отсутствие
    // темы: у themed-кнопки Windows сама рисует светлую рамку и углы, которые
    // торчат из-под нашей скруглённой заливки.
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                parent_, (HMENU)(INT_PTR)id, inst_, nullptr);
    if (hwnd && _wcsicmp(cls, L"BUTTON") == 0) {
        // Отключаем визуальные стили для этого контрола: рисуем его сами.
        SetWindowTheme(hwnd, L"", L"");
        // Плоская рамка вместо объёмной.
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_CLIENTEDGE & ~WS_EX_STATICEDGE);
    }
    return hwnd;
}

void View::CreateControls() {
    // Список накопителей: owner-draw, чтобы каждая строка была «карточкой»
    // с моделью, VID/PID и объёмом.
    h_list_ = Make(L"LISTBOX", L"",
                   WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                       LBS_NOTIFY | WS_TABSTOP,
                   ID_LIST);
    h_refresh_ = Make(L"BUTTON", L"Обновить", BS_OWNERDRAW | WS_TABSTOP, ID_REFRESH);
    h_diag_ = Make(L"BUTTON", L"Диагностика", BS_OWNERDRAW | WS_TABSTOP, ID_DIAGNOSTICS);
    h_eject_ = Make(L"BUTTON", L"Безопасное извлечение", BS_OWNERDRAW | WS_TABSTOP, ID_EJECT);
    h_report_ = Make(L"BUTTON", L"Сохранить отчёт", BS_OWNERDRAW | WS_TABSTOP, ID_REPORT);
    h_browse_ = Make(L"BUTTON", L"Обзор…", BS_OWNERDRAW | WS_TABSTOP, ID_BROWSE);
    h_open_ = Make(L"BUTTON", L"Открыть папку", BS_OWNERDRAW | WS_TABSTOP, ID_OPEN_DIR);
    h_elevate_ = Make(L"BUTTON", L"Запустить от администратора", BS_OWNERDRAW | WS_TABSTOP,
                      ID_ELEVATE);

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
    // Список не растёт бесконечно: старые строки уходят, полный журнал — в файле.
    while (SendMessageW(h_log_, LB_GETCOUNT, 0, 0) > 2000) {
        SendMessageW(h_log_, LB_DELETESTRING, 0, 0);
    }
    ScrollLogToEnd();
}

void View::ScrollLogToEnd() {
    if (!h_log_) return;
    const int count = (int)SendMessageW(h_log_, LB_GETCOUNT, 0, 0);
    if (count > 0) SendMessageW(h_log_, LB_SETTOPINDEX, count - 1, 0);
}

int View::MinWidth() const {
    // карточка списка (кнопки «Обновить»/«Диагностика»/«Обзор…» + счётчик)
    // + карточка устройства (PanelW) + отступы и поля окна
    return S(theme::Pad * 2 + 470 + theme::Gap + theme::PanelW);
}

int View::MinHeight() const {
    // шапка + список (две строки) + действия + журнал + поля окна
    return S(theme::HeaderH + theme::Gap + 80 + 2 * theme::ItemH + theme::Gap + theme::ActionH +
             theme::Gap + theme::LogH + theme::Pad);
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

    // Слева, сверху вниз: список накопителей, «Действия», журнал.
    // Эти три карточки занимают всю высоту окна и задают нижнюю границу.
    log_rc_ = {pad, h - pad - S(theme::LogH), w - pad - S(theme::PanelW) - gap, h - pad};
    action_rc_ = {pad, log_rc_.top - gap - S(theme::ActionH), log_rc_.right,
                  log_rc_.top - gap};
    list_rc_ = {pad, S(theme::HeaderH), log_rc_.right, action_rc_.top - gap};

    // Справа — карточка «Устройство» во всю высоту окна: характеристик
    // много, и все они должны помещаться без прокрутки.
    panel_rc_ = {w - pad - S(theme::PanelW), S(theme::HeaderH), w - pad, h - pad};
    header_rc_ = {0, 0, w, S(theme::HeaderH)};

    // --- шапка -------------------------------------------------------------
    // Метки фиксированной ширины: справа «права процесса», левее — состояние
    // поиска. Заголовок получает остаток и обрезается многоточием, поэтому
    // наезжать на метки он не может ни при каком тексте.
    {
        const int cy = S(22), pill_h = S(30);   // шапка 92 DIP: полоса 22…52
        const int rights_w = S(172);
        const int state_w = S(232);
        rights_pill_ = {w - pad - rights_w, cy, w - pad, cy + pill_h};
        state_pill_ = {rights_pill_.left - S(10) - state_w, cy, rights_pill_.left - S(10),
                       cy + pill_h};
    }
    title_rc_ = {pad, S(22), state_pill_.left - S(16), S(52)};

    // --- карточка «USB-накопители» ----------------------------------------
    {
        list_head_ = {list_rc_.left + p, list_rc_.top + S(10), list_rc_.right - p,
                      list_rc_.top + S(34)};

        // Кнопки в один ряд: [«Обновить»] [«Диагностика»] … [«Обзор…»]
        const int refresh_w = S(104);
        const int diag_w = S(116);
        const int browse_w = S(96);
        const int gap_btn = S(8);
        const int button_y = list_rc_.top + S(42);
        const int browse_x = list_rc_.right - p - browse_w;
        const int diag_x = browse_x - gap_btn - diag_w;
        const int refresh_x = diag_x - gap_btn - refresh_w;
        const int count_right = refresh_x - S(10);
        list_count_ = {list_rc_.left + p, button_y, count_right, button_y + S(28)};

        // При узком окне кнопки сдвигаются влево, но не наезжают на счётчик.
        SetWindowPos(h_refresh_, nullptr, std::max<int>(list_rc_.left + p, refresh_x), button_y,
                     refresh_w, S(28), SWP_NOZORDER);
        SetWindowPos(h_diag_, nullptr,
                     std::max<int>(list_rc_.left + p + refresh_w + gap_btn, diag_x), button_y,
                     diag_w, S(28), SWP_NOZORDER);
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

        // Сколько строк помещается по высоте карточки — столько и рисуем.
        const int bottom_limit = panel_rc_.bottom - S(12);
        const int space = bottom_limit - fields_top_;
        panel_rows_ = std::max(1, (int)(space / S(theme::RowH)));
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

        // Ряд 3: открыть папку с отчётами и повышение прав.
        const int y3 = action_rc_.top + S(114);
        const int open_w = std::min(S(180), ((int)right - left - gap2) / 2);
        SetWindowPos(h_open_, nullptr, left, y3, open_w, S(26), SWP_NOZORDER);
        const int elev_x = left + open_w + gap2;
        SetWindowPos(h_elevate_, nullptr, elev_x, y3, std::max(S(150), (int)right - elev_x), S(26),
                     SWP_NOZORDER);
    }

    // --- карточка «Журнал» -------------------------------------------------
    {
        log_head_ = {log_rc_.left + p, log_rc_.top + S(8), log_rc_.right - p,
                     log_rc_.top + S(30)};
        // Вторая строка карточки — горячие клавиши: на узком окне они
        // не помещаются рядом с заголовком, поэтому идут отдельной строкой.
        log_keys_ = {log_rc_.left + p, log_rc_.top + S(30), log_rc_.right - p,
                     log_rc_.top + S(48)};
        const int top = log_rc_.top + S(52);
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
    EnableWindow(h_diag_, (!busy) ? TRUE : FALSE);
    EnableWindow(h_list_, (!busy && !model.busy) ? TRUE : FALSE);
    EnableWindow(h_eject_, can_eject ? TRUE : FALSE);
    EnableWindow(h_report_, (!busy && !model.busy) ? TRUE : FALSE);
    EnableWindow(h_dir_, TRUE);
    EnableWindow(h_browse_, TRUE);
    EnableWindow(h_open_, TRUE);

    // Кнопка повышения прав нужна только когда прав нет.
    ShowWindow(h_elevate_, model.elevated ? SW_HIDE : SW_SHOW);
    EnableWindow(h_elevate_, model.elevated ? FALSE : TRUE);

    // Owner-draw кнопки не перерисовываются сами при смене состояния —
    // иначе «Извлечь» остаётся нарисованной в старом виде.
    for (HWND button : {h_refresh_, h_diag_, h_eject_, h_report_, h_browse_, h_open_, h_elevate_}) {
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

// Значок телефона: скруглённый корпус и экран. Нужен, чтобы MTP-устройство
// визуально отличалось от флешки — у него нет и не будет буквы диска.
void View::DrawPhoneIcon(Graphics& g, int x, int y, const Color& accent) {
    const int unit = S(10);
    RECT body{x + unit / 2, y, x + unit * 3, y + unit * 2};
    RoundedRect(g, body, Color(255, 28, 40, 60), accent, S(2));
    RECT screen{x + unit, y + S(4), x + unit * 3 - S(4), y + unit * 2 - S(6)};
    SolidBrush screen_brush(Color(255, 10, 16, 26));
    g.FillRectangle(&screen_brush, (INT)screen.left, (INT)screen.top,
                    (INT)(screen.right - screen.left), (INT)(screen.bottom - screen.top));
    Dot(g, x + unit * 3 - S(6), y + unit * 2 - S(3), S(1), accent);
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
        DrawDriveIcon(g, cx - S(20), cy - S(48), theme::Dim);

        RECT head{list_body_.left, cy - S(20), list_body_.right, cy + S(6)};
        TextG(g, model.scan_state == ScanBusy ? L"Опрос устройств…" : L"USB-устройства не найдены",
              head, f_section_, theme::Sub, StringAlignmentCenter);

        RECT hint{list_body_.left, cy + S(8), list_body_.right, cy + S(30)};
        TextG(g,
              model.scan_state == ScanBusy
                  ? L"Идёт перечисление через SetupAPI"
                  : L"Флешка подключается сразу, телефон — после выбора «Передача файлов»",
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
        TextG(g, L"Выберите устройство в списке слева", rc, f_label_, theme::Dim,
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

    // Одна широкая колонка: подпись фиксированной ширины, значение — остаток.
    const int p = S(theme::InnerPad);
    const int label_left = panel_rc_.left + p;
    const int value_left = label_left + S(theme::LabelW);

    for (int i = 0; i < panel_rows_ && i < (int)model.fields.size(); ++i) {
        Row(g, label_left, value_left, panel_rc_.right - p, fields_top_ + i * S(theme::RowH),
            model.fields[i].label, model.fields[i].value, f_label_, f_value_);
    }

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

    // Горячие клавиши — отдельной строкой под заголовком карточки.
    TextG(g, L"F5 обновить · Ctrl+E извлечь · Ctrl+L журнал · Ctrl+S отчёт · "
             L"Ctrl+D диагностика",
          log_keys_, f_label_, theme::Dim);

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
        // Опасная операция — красноватый акцент.
        fill = pressed ? Color(255, 132, 44, 40) : Color(255, 106, 34, 32);
        border = Color(255, 214, 96, 88);
        text = Color(255, 255, 236, 234);
    } else if (item->CtlID == ID_DIAGNOSTICS) {
        fill = pressed ? Color(255, 48, 62, 90) : Color(255, 34, 46, 70);
        border = Color(255, 108, 132, 172);
        text = Color(255, 226, 236, 250);
    } else if (item->CtlID == ID_ELEVATE) {
        // Не опасная, но важная операция — синий акцент.
        fill = pressed ? Color(255, 40, 74, 120) : Color(255, 30, 56, 92);
        border = theme::Accent;
        text = Color(255, 226, 238, 255);
    } else if (item->CtlID == ID_REFRESH) {
        fill = pressed ? Color(255, 30, 130, 90) : Color(255, 24, 110, 74);
        border = Color(255, 60, 200, 140);
        text = Color(255, 235, 255, 245);
    } else if (pressed) {
        fill = theme::BtnHot;
    }

    RECT rc = item->rcItem;

    // Сначала закрашиваем весь прямоугольник под кнопкой: иначе в углах
    // скругления остаётся системный светлый фон — они выглядят белыми
    // «уголками». Цвет берём как у карточки: кнопки всегда лежат на ней.
    FillRectG(g, rc, theme::Card);
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
    const bool portable = row && row->portable;

    if (portable) {
        DrawPhoneIcon(g, card.left + S(12), card.top + S(15),
                      selected ? theme::Accent : theme::Sub);
    } else {
        DrawDriveIcon(g, card.left + S(14), card.top + S(15),
                      selected ? theme::Accent : theme::Sub);
    }

    RECT title{card.left + S(60), card.top + S(8), card.right - S(112), card.top + S(32)};
    TextG(g, row && !row->title.empty() ? row->title : L"USB-накопитель", title, f_value_,
          theme::Txt);

    RECT sub{card.left + S(60), card.top + S(31), card.right - S(112), card.top + S(52)};
    TextG(g, row ? row->subtitle : L"", sub, f_log_, theme::Sub);

    // Справа: объём и метка наличия тома.
    RECT cap{card.right - S(110), card.top + S(8), card.right - S(14), card.top + S(32)};
    TextG(g, row ? row->capacity : L"", cap, f_value_, theme::Txt, StringAlignmentFar);

    const bool system = row && row->system;
    const bool has_drive = row && row->has_drive;
    std::wstring tag;
    Color tag_color = theme::Dim;
    if (portable) {
        tag = L"MTP";
        tag_color = theme::Accent;
    } else if (system) {
        tag = L"системный";
        tag_color = theme::Bad;
    } else if (has_drive) {
        tag = L"Том смонтирован";
        tag_color = theme::Good;
    } else {
        tag = L"без тома";
        tag_color = theme::Dim;
    }
    RECT tag_rc{card.right - S(180), card.top + S(31), card.right - S(14), card.top + S(52)};
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
