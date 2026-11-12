// ===========================================================================
//  visual.cpp — вся визуальная часть: тема, шрифты, раскладка, отрисовка
//
//  Модуль ничего не знает о камере, файлах и журнале как о данных: он получает
//  готовую структуру visual::Model и рисует по ней окно. Вся логика — в main.cpp.
//
//  Раскладка построена «потоком сверху вниз» внутри каждой карточки и
//  обрезается по её границам (SetClip), поэтому элементы не могут наехать
//  друг на друга при любом размере окна и любом масштабе DPI.
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
//  Тема (как в ЛР №1): цвета и «дизайнерские» размеры в DIP
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
const Color Preview(255, 8, 10, 16);

const int Pad = 22;        // поля окна
const int Gap = 12;        // между карточками
const int HeaderH = 92;    // шапка
const int LogH = 110;      // карточка журнала
const int SaveH = 140;     // карточка «Сохранение» (каталог, снимок, запись)
const int PanelW = 352;    // карточка «Камера»
const int RowH = 22;       // строка «подпись — значение»
const int InnerPad = 16;   // поле внутри карточки
const REAL Radius = 6.0f;

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

void Card(Graphics& g, const RECT& rc) {
    RoundedRect(g, rc, theme::Card, theme::Line, S(theme::Radius * 4) / 4.0f);
}

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
    const int label_w = S(96);
    RECT l{card.left + p, y, std::min(card.left + p + label_w, card.right - p),
           y + S(theme::RowH)};
    RECT v{l.right, y, card.right - p, y + S(theme::RowH)};
    TextG(g, label, l, label_font, theme::Sub);
    TextG(g, value.empty() ? L"—" : value, v, value_font, theme::Txt);
}

Color StateColor(int state) {
    switch (state) {
        case CameraOn:      return theme::Good;
        case CameraOpening: return theme::Warn;
        case CameraFailed:  return theme::Bad;
        default:            return theme::Dim;
    }
}

Color LevelColor(int level) {
    switch (level) {
        case 2: return theme::Bad;      // ошибка
        case 1: return theme::Warn;     // предупреждение
        case 3: return theme::Good;     // фоновый режим
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
    h_camera_ = Make(L"COMBOBOX", L"",
                     CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL |
                         WS_TABSTOP,
                     ID_CAMERA);
    h_res_ = Make(L"COMBOBOX", L"",
                  CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
                  ID_RES);
    h_refresh_ = Make(L"BUTTON", L"Обновить", BS_OWNERDRAW | WS_TABSTOP, ID_REFRESH);
    h_connect_ = Make(L"BUTTON", L"Подключить", BS_OWNERDRAW | WS_TABSTOP, ID_CONNECT);
    h_disconnect_ = Make(L"BUTTON", L"Отключить", BS_OWNERDRAW | WS_TABSTOP, ID_DISCONNECT);
    h_snapshot_ = Make(L"BUTTON", L"Снимок", BS_OWNERDRAW | WS_TABSTOP, ID_SNAPSHOT);
    h_report_ = Make(L"BUTTON", L"Сохранить отчёт", BS_OWNERDRAW | WS_TABSTOP, ID_REPORT);
    h_record_start_ = Make(L"BUTTON", L"Начать запись", BS_OWNERDRAW | WS_TABSTOP, ID_RECORD_START);
    h_record_stop_ = Make(L"BUTTON", L"Остановить запись", BS_OWNERDRAW | WS_TABSTOP, ID_RECORD_STOP);
    h_browse_ = Make(L"BUTTON", L"Обзор…", BS_OWNERDRAW | WS_TABSTOP, ID_BROWSE);
    h_tray_ = Make(L"BUTTON", L"Свернуть в трей", BS_OWNERDRAW | WS_TABSTOP, ID_TRAY_BUTTON);

    h_dir_ = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_DIR);
    h_log_ = Make(L"LISTBOX", L"",
                  WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                      WS_TABSTOP,
                  ID_LOG);
    SendMessageW(h_log_, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

void View::ApplyFontsToControls() {
    if (!h_log_) return;
    for (HWND combo : {h_camera_, h_res_}) {
        SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, S(22));
        SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)0, S(22));
    }
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
    // заголовок + блок состояния + кнопка трея (292) + поля окна
    return S(theme::Pad * 2 + 200 + 20 + 250 + 20 + 292) + S(60);
}

int View::MinHeight() const {
    // Высота карточки «Камера»: до разделителя плюс две строки информации
    // (остальные при нехватке места убираются адаптивной раскладкой).
    const int panel = S(42 + 20 + 26 + 10 + 20 + 26 + 12 + 32 + 12 + 1 + 12 + 2 * theme::RowH);
    return S(theme::Pad * 2 + theme::HeaderH + theme::Gap + theme::SaveH + theme::Gap +
             theme::LogH) +
           panel + S(30);
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
    save_rc_ = {pad, log_rc_.top - gap - S(theme::SaveH), w - pad, log_rc_.top - gap};
    panel_rc_ = {w - pad - S(theme::PanelW), S(theme::HeaderH), w - pad, save_rc_.top - gap};
    preview_rc_ = {pad, S(theme::HeaderH), panel_rc_.left - gap, save_rc_.top - gap};

    // Кнопка трея — вдвое крупнее прежней (292 × 64 DIP).
    tray_rc_ = {w - pad - S(292), S(14), w - pad, S(14) + S(64)};
    SetWindowPos(h_tray_, nullptr, tray_rc_.left, tray_rc_.top, tray_rc_.right - tray_rc_.left,
                 tray_rc_.bottom - tray_rc_.top, SWP_NOZORDER);

    // --- шапка -------------------------------------------------------------
    title_rc_ = {pad, S(14), w - pad, S(50)};
    subtitle_rc_ = {pad, S(52), w - pad, S(74)};

    // --- карточка «Предпросмотр» ------------------------------------------
    preview_head_ = {preview_rc_.left + p, preview_rc_.top + S(10), preview_rc_.right - p,
                     preview_rc_.top + S(34)};

    // --- карточка «Камера» -------------------------------------------------
    {
        const int left = panel_rc_.left + p;
        const int right = panel_rc_.right - p;
        const int field_h = S(26);

        panel_head_ = {left, panel_rc_.top + S(10), right, panel_rc_.top + S(34)};

        int y = panel_rc_.top + S(42);
        camera_label_ = {left, y, right, y + S(20)};
        y += S(20);

        const int refresh_w = S(86);
        const int combo_w = std::max(S(120), (int)(right - left) - refresh_w - S(8));
        SetWindowPos(h_camera_, nullptr, left, y, combo_w, S(240), SWP_NOZORDER);
        SetWindowPos(h_refresh_, nullptr, right - refresh_w, y, refresh_w, field_h, SWP_NOZORDER);
        y += field_h + S(10);

        res_label_ = {left, y, right, y + S(20)};
        y += S(20);

        SetWindowPos(h_res_, nullptr, left, y, right - left, S(240), SWP_NOZORDER);
        y += field_h + S(12);

        const int btn_w = (right - left - S(10)) / 2;
        SetWindowPos(h_connect_, nullptr, left, y, btn_w, S(32), SWP_NOZORDER);
        SetWindowPos(h_disconnect_, nullptr, left + btn_w + S(10), y,
                     right - (left + btn_w + S(10)), S(32), SWP_NOZORDER);
        y += S(32) + S(12);

        separator_y_ = y;
        info_top_ = y + S(12);

        const int bottom_limit = panel_rc_.bottom - S(12);
        const int space = bottom_limit - info_top_;

        // Если места мало (маленькое окно или большой DPI), сначала убираем
        // подсказку, потом «последний снимок», потом лишние строки — так ничего
        // не наезжает друг на друга и не вылезает за карточку.
        const int row_h = S(theme::RowH);
        const int last_h = S(6 + 18 + 22);
        const int hint_h = S(4 + 20);

        rows_ = 3;
        show_last_ = false;
        show_hint_ = false;
        if (space >= rows_ * row_h + last_h + hint_h) {
            show_last_ = true;
            show_hint_ = true;
        } else if (space >= rows_ * row_h + last_h) {
            show_last_ = true;
        } else {
            while (rows_ > 0 && space < rows_ * row_h) --rows_;
        }

        const int rows_bottom = info_top_ + rows_ * row_h;

        if (show_last_) {
            last_title_rc_ = {left, rows_bottom + S(6), right, rows_bottom + S(6) + S(18)};
            last_value_rc_ = {left, rows_bottom + S(24), right, rows_bottom + S(46)};
        } else {
            last_title_rc_ = last_value_rc_ = RECT{0, 0, 0, 0};
        }

        hint_rc_ = show_hint_ ? RECT{left, bottom_limit - S(20), right, bottom_limit}
                              : RECT{0, 0, 0, 0};
    }

    // --- карточка «Сохранение» --------------------------------------------
    {
        const int left = save_rc_.left + p;
        const int right = save_rc_.right - p;

        save_head_ = {left, save_rc_.top + S(8), right, save_rc_.top + S(30)};

        const int y1 = save_rc_.top + S(34);
        const int label_w = S(66);
        const int browse_w = S(84);
        dir_label_ = {left, y1, left + label_w, y1 + S(24)};

        const int edit_x = dir_label_.right + S(8);
        int edit_w = right - browse_w - S(8) - edit_x;
        edit_w = std::max(S(140), edit_w);
        dir_edit_rc_ = {edit_x, y1, edit_x + edit_w, y1 + S(24)};
        SetWindowPos(h_dir_, nullptr, edit_x, y1, edit_w, S(24), SWP_NOZORDER);
        SetWindowPos(h_browse_, nullptr, edit_x + edit_w + S(8), y1,
                     std::max(S(60), (int)right - (edit_x + edit_w + S(8))), S(24), SWP_NOZORDER);

        const int gap2 = S(10);
        const int avail_w = (int)(right - left) - gap2;
        const int w1 = std::max(S(110), avail_w * 32 / 100);

        // ряд 1: снимок и отчёт
        const int y2 = save_rc_.top + S(64);
        SetWindowPos(h_snapshot_, nullptr, left, y2, w1, S(28), SWP_NOZORDER);
        SetWindowPos(h_report_, nullptr, left + w1 + gap2, y2,
                     std::max(S(120), (int)right - (left + w1 + gap2)), S(28), SWP_NOZORDER);

        // ряд 2: запись видео (функционал базового монитора)
        const int y3 = save_rc_.top + S(100);
        const int half_w = (avail_w) / 2;
        SetWindowPos(h_record_start_, nullptr, left, y3, half_w, S(28), SWP_NOZORDER);
        SetWindowPos(h_record_stop_, nullptr, left + half_w + gap2, y3,
                     (int)right - (left + half_w + gap2), S(28), SWP_NOZORDER);
    }

    // --- карточка «Журнал» -------------------------------------------------
    {
        log_head_ = {log_rc_.left + p, log_rc_.top + S(8), log_rc_.right - p,
                     log_rc_.top + S(30)};
        const int top = log_rc_.top + S(38);
        SetWindowPos(h_log_, nullptr, log_rc_.left + p, top, log_rc_.right - log_rc_.left - 2 * p,
                     std::max(S(40), (int)log_rc_.bottom - S(12) - top), SWP_NOZORDER);
    }

    InvalidateRect(parent_, nullptr, FALSE);
}

void View::SetEnabled(const Model& model) {
    const bool on = model.camera_state == CameraOn;
    const bool opening = model.camera_state == CameraOpening;
    const bool failed = model.camera_state == CameraFailed;

    EnableWindow(h_connect_, (!on && !opening && model.has_devices) ? TRUE : FALSE);
    EnableWindow(h_disconnect_, (on || opening || failed) ? TRUE : FALSE);
    EnableWindow(h_camera_, (!on && !opening) ? TRUE : FALSE);
    EnableWindow(h_refresh_, (!on && !opening) ? TRUE : FALSE);
    EnableWindow(h_res_, (!on && !opening) ? TRUE : FALSE);
    EnableWindow(h_snapshot_, on ? TRUE : FALSE);
    EnableWindow(h_record_start_, (on && !model.recording) ? TRUE : FALSE);
    EnableWindow(h_record_stop_, model.recording ? TRUE : FALSE);
    EnableWindow(h_dir_, TRUE);
    EnableWindow(h_browse_, TRUE);

    // Owner-draw кнопки не перерисовываются сами при смене состояния —
    // иначе «Снимок» остаётся нарисованным в старом виде.
    for (HWND button : {h_connect_, h_disconnect_, h_refresh_, h_snapshot_, h_browse_, h_report_,
                        h_record_start_, h_record_stop_, h_tray_}) {
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

    PaintBackground(g, client.right, client.bottom);
    DrawHeader(g, model);
    DrawPreview(g, model);
    DrawPanel(g, model);
    DrawSaveCard(g);
    DrawLogCard(g);
    DrawToasts(g);
}

void View::DrawHeader(Graphics& g, const Model& model) {
    g.SetClip(RectF(0.0f, 0.0f, (REAL)std::max<LONG>(1, tray_rc_.right + S(200)), (REAL)S(theme::HeaderH)));

    // Ширина блока состояния — по фактической ширине текста, поэтому
    // заголовок никогда не наезжает на индикатор.
    const std::wstring status = model.status.empty() ? L"Камера не подключена" : model.status;
    const int measured = TextWidth(g, status, f_value_);
    const int max_w = S(330);
    const int status_w = std::clamp(measured + S(24), S(150), max_w);

    const int status_right = tray_rc_.left - S(20);
    RECT status_rc{status_right - status_w, S(16), status_right, S(48)};

    const int text_right = std::max(S(theme::Pad) + S(120), (int)status_rc.left - S(20));
    title_rc_ = {S(theme::Pad), S(11), text_right, S(47)};
    subtitle_rc_ = {S(theme::Pad), S(49), text_right, S(71)};

    TextG(g, L"Фотонаблюдение", title_rc_, f_title_, theme::Txt);
    TextG(g, L"Снимки с камеры · скрытый режим работы", subtitle_rc_, f_label_, theme::Sub);

    const Color state = StateColor(model.camera_state);
    const int cy = (status_rc.top + status_rc.bottom) / 2;
    Dot(g, status_rc.left + S(6), cy, S(5), state);

    RECT status_text{status_rc.left + S(20), status_rc.top, status_rc.right, status_rc.bottom};
    TextG(g, status, status_text, f_value_, state);

    g.ResetClip();
}

void View::DrawPreview(Graphics& g, const Model& model) {
    g.SetClip(RectF((REAL)preview_rc_.left, (REAL)preview_rc_.top,
                    (REAL)(preview_rc_.right - preview_rc_.left),
                    (REAL)(preview_rc_.bottom - preview_rc_.top)));

    Card(g, preview_rc_);
    TextG(g, L"Предпросмотр", preview_head_, f_section_, theme::Txt);

    const int p = S(theme::InnerPad);
    RECT video{preview_rc_.left + p, preview_rc_.top + S(40), preview_rc_.right - p,
               preview_rc_.bottom - S(12)};
    RoundedRect(g, video, theme::Preview, theme::Line, S(theme::Radius * 4) / 4.0f);

    const FrameView& frame = model.frame;
    if (frame.bgr && frame.width > 0 && frame.height > 0) {
        const int avail_w = video.right - video.left - 2;
        const int avail_h = video.bottom - video.top - 2;
        if (avail_w > 4 && avail_h > 4) {
            const double scale =
                std::min((double)avail_w / frame.width, (double)avail_h / frame.height);
            const int w = std::max(1, (int)(frame.width * scale));
            const int h = std::max(1, (int)(frame.height * scale));
            const int x = video.left + 1 + (avail_w - w) / 2;
            const int y = video.top + 1 + (avail_h - h) / 2;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = frame.width;
            bmi.bmiHeader.biHeight = -frame.height;   // строки сверху вниз
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;

            // Видео выводим обычным GDI: GDI+ не умеет BGR24 напрямую.
            HDC vdc = g.GetHDC();
            SetStretchBltMode(vdc, HALFTONE);
            SetBrushOrgEx(vdc, 0, 0, nullptr);
            StretchDIBits(vdc, x, y, w, h, 0, 0, frame.width, frame.height, frame.bgr, &bmi,
                          DIB_RGB_COLORS, SRCCOPY);
            g.ReleaseHDC(vdc);
        }
    } else {
        const wchar_t* text = L"Камера не подключена";
        if (model.camera_state == CameraOpening) {
            text = L"Подключение камеры…";
        } else if (model.camera_state == CameraFailed) {
            text = L"Камера недоступна";
        } else if (model.camera_state == CameraOn) {
            text = L"Ожидание кадра…";
        }
        TextG(g, text, video, f_section_, theme::Dim, StringAlignmentCenter);
    }

    g.ResetClip();
}

void View::DrawPanel(Graphics& g, const Model& model) {
    g.SetClip(RectF((REAL)panel_rc_.left, (REAL)panel_rc_.top,
                    (REAL)(panel_rc_.right - panel_rc_.left),
                    (REAL)(panel_rc_.bottom - panel_rc_.top)));

    Card(g, panel_rc_);
    TextG(g, L"Камера", panel_head_, f_section_, theme::Txt);

    TextG(g, L"Устройство", camera_label_, f_label_, theme::Sub);
    TextG(g, L"Разрешение", res_label_, f_label_, theme::Sub);

    Separator(g, panel_rc_, separator_y_);

    // Строки идут сверху вниз и рисуются только те, что поместились.
    int y = info_top_;
    const std::wstring values[] = {model.mode, model.format,
                                   std::to_wstring(model.snapshot_count)};
    const wchar_t* labels[] = {L"Режим:", L"Формат:", L"Снимков:"};
    for (int i = 0; i < rows_ && i < 3; ++i) {
        Row(g, panel_rc_, y, labels[i], values[i], f_label_, f_value_);
        y += S(theme::RowH);
    }

    if (show_last_) {
        TextG(g, L"Последний снимок:", last_title_rc_, f_label_, theme::Sub);
        TextG(g, model.last_snapshot.empty() ? L"—" : model.last_snapshot, last_value_rc_,
              f_log_, theme::Txt);
    }
    if (show_hint_) {
        TextG(g, L"Снимок — «-» и «=» вместе", hint_rc_, f_label_, theme::Dim);
    }

    g.ResetClip();
}

void View::DrawSaveCard(Graphics& g) {
    g.SetClip(RectF((REAL)save_rc_.left, (REAL)save_rc_.top,
                    (REAL)(save_rc_.right - save_rc_.left),
                    (REAL)(save_rc_.bottom - save_rc_.top)));

    Card(g, save_rc_);
    TextG(g, L"Сохранение", save_head_, f_section_, theme::Txt);
    TextG(g, L"Каталог", dir_label_, f_label_, theme::Sub);

    // Рамка вокруг поля ввода: у EDIT нет своего оформления.
    RECT field = dir_edit_rc_;
    field.left -= 1;
    field.top -= 1;
    field.right += 1;
    field.bottom += 1;
    RoundedRect(g, field, Color(255, 12, 18, 30), theme::Line, S(theme::Radius * 4) / 4.0f);

    g.ResetClip();
}

void View::DrawLogCard(Graphics& g) {
    g.SetClip(RectF((REAL)log_rc_.left, (REAL)log_rc_.top, (REAL)(log_rc_.right - log_rc_.left),
                    (REAL)(log_rc_.bottom - log_rc_.top)));

    Card(g, log_rc_);
    TextG(g, L"Журнал событий", log_head_, f_section_, theme::Txt);

    g.ResetClip();
}

// ---------------------------------------------------------------------------
//  Owner-draw: кнопки, выпадающие списки, строки журнала
// ---------------------------------------------------------------------------
bool View::DrawItem(const DRAWITEMSTRUCT* item) {
    if (!item) return false;
    switch (item->CtlType) {
        case ODT_BUTTON:   DrawButton(item); return true;
        case ODT_COMBOBOX: DrawCombo(item); return true;
        case ODT_LISTBOX:  DrawLogItem(item); return true;
        default:           return false;
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

// Уведомления рисуются поверх предпросмотра, стопкой (до трёх сразу),
// и плавно гаснут в последние 0,4 с — «коротко выскакивают», не мешая работе.
void View::DrawToasts(Graphics& g) {
    if (toasts_.empty()) return;

    const ULONGLONG now = GetTickCount64();
    g.SetClip(RectF((REAL)preview_rc_.left, (REAL)preview_rc_.top,
                    (REAL)(preview_rc_.right - preview_rc_.left),
                    (REAL)(preview_rc_.bottom - preview_rc_.top)));

    const int right = preview_rc_.right - S(12);
    int y = preview_rc_.top + S(12);

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
                    Color((BYTE)alpha, accent.GetR(), accent.GetG(), accent.GetB()),
                    S(theme::Radius * 4) / 4.0f);

        RECT bar{rc.left + S(8), rc.top + S(8), rc.left + S(12), rc.bottom - S(8)};
        FillRectG(g, bar, Color((BYTE)alpha, accent.GetR(), accent.GetG(), accent.GetB()));

        RECT text_rc{rc.left + S(20), rc.top, rc.right - S(10), rc.bottom};
        TextG(g, it->text, text_rc, f_value_, Color((BYTE)alpha, 235, 240, 248));

        y += h + S(6);
        if (y > preview_rc_.bottom - h) break;   // больше не помещается
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
    } else if (item->CtlID == ID_CONNECT || item->CtlID == ID_SNAPSHOT) {
        fill = pressed ? Color(255, 30, 130, 90) : Color(255, 24, 110, 74);
        border = Color(255, 60, 200, 140);
        text = Color(255, 235, 255, 245);
    } else if (pressed) {
        fill = theme::BtnHot;
    }

    RECT rc = item->rcItem;
    RoundedRect(g, rc, fill, border, S(theme::Radius * 4) / 4.0f);

    wchar_t buffer[128] = {};
    GetWindowTextW(item->hwndItem, buffer, 128);
    TextG(g, buffer, rc, f_button_, text, StringAlignmentCenter);
}

void View::DrawCombo(const DRAWITEMSTRUCT* item) {
    Graphics g(item->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    if (item->itemID == (UINT)-1) {
        RoundedRect(g, item->rcItem, theme::Field, theme::Line, S(theme::Radius * 4) / 4.0f);
        const int sel = (int)SendMessageW(item->hwndItem, CB_GETCURSEL, 0, 0);
        std::wstring text;
        if (sel >= 0) {
            wchar_t buffer[256] = {};
            if (SendMessageW(item->hwndItem, CB_GETLBTEXT, sel, (LPARAM)buffer) != CB_ERR) {
                text = buffer;
            }
        }
        RECT text_rc = item->rcItem;
        text_rc.left += S(8);
        text_rc.right -= S(22);
        TextG(g, text, text_rc, f_label_, text.empty() ? theme::Dim : theme::Txt);

        const int cx = item->rcItem.right - S(13);
        const int cy = (item->rcItem.top + item->rcItem.bottom) / 2;
        Pen pen(theme::Sub, 1.4f);
        g.DrawLine(&pen, cx - S(4), cy - S(2), cx, cy + S(2));
        g.DrawLine(&pen, cx, cy + S(2), cx + S(4), cy - S(2));
        return;
    }

    wchar_t buffer[256] = {};
    SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, (LPARAM)buffer);
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    FillRectG(g, item->rcItem, selected ? theme::BtnHot : theme::Field);
    RECT text_rc = item->rcItem;
    text_rc.left += S(8);
    TextG(g, buffer, text_rc, f_label_, selected ? theme::Txt : theme::Sub);
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
