#include "visual.h"

#include <commctrl.h>
#include <dbt.h>
#include <dshow.h>
#include <shellapi.h>
#include <shlobj.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "gdiplus.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
 name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
 processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using cv::Mat;
using namespace visual;


std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::string ToAnsi(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring NowStamp(bool milliseconds) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return milliseconds ? Format(L"%02u.%02u.%04u %02u:%02u:%02u.%03u", st.wDay, st.wMonth,
                                 st.wYear, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds)
                        : Format(L"%02u.%02u.%04u %02u:%02u:%02u", st.wDay, st.wMonth, st.wYear,
                                 st.wHour, st.wMinute, st.wSecond);
}

std::wstring FileStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    const wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

std::wstring FileName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

unsigned long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    if (DirExists(path)) return true;
    std::wstring partial;
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool boundary = (path[i] == L'\\' || path[i] == L'/');
        if (!boundary && i + 1 != path.size()) continue;
        if (partial.size() <= 3 && partial[1] == L':') continue;   // "C:\"
        std::wstring probe = partial;
        while (!probe.empty() && (probe.back() == L'\\' || probe.back() == L'/')) probe.pop_back();
        if (!probe.empty() && !DirExists(probe) && !CreateDirectoryW(probe.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return DirExists(path);
}

std::wstring DefaultSaveDir() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, path))) {
        return JoinPath(path, L"CameraLab4");
    }
    return L"snapshots";
}


std::wstring SnapshotPath(const std::wstring& dir) {
    const std::wstring base = JoinPath(dir, L"snapshot_" + FileStamp());
    std::wstring candidate = base + L".jpg";
    for (int i = 1; FileExists(candidate); ++i) {
        candidate = base + L"_" + std::to_wstring(i) + L".jpg";
    }
    return candidate;
}

std::wstring VideoPath(const std::wstring& dir) {
    const std::wstring base = JoinPath(dir, L"video_" + FileStamp());
    std::wstring candidate = base + L".avi";
    for (int i = 1; FileExists(candidate); ++i) {
        candidate = base + L"_" + std::to_wstring(i) + L".avi";
    }
    return candidate;
}

bool ImwriteAny(const std::wstring& path, const Mat& image, int quality) {
    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 1, 100)};
    try {
        if (cv::imwrite(ToUtf8(path), image, params)) return true;
        return cv::imwrite(ToAnsi(path), image, params);
    } catch (const cv::Exception&) {
        return false;
    }
}


struct Frame {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgr;   // 24 бита, BGR, построчно
    bool ok() const { return width > 0 && height > 0 && !bgr.empty(); }
};

struct CameraDevice {
    std::wstring name;   // «WebCam»
    std::wstring path;   // символьный путь устройства
};

// Перечисление камер через DirectShow (ICreateDevEnum + IPropertyBag).
std::vector<CameraDevice> EnumerateCameras() {
    std::vector<CameraDevice> result;

    ICreateDevEnum* dev_enum = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dev_enum)))) {
        return result;
    }

    IEnumMoniker* enum_moniker = nullptr;
    if (dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0) == S_OK) {
        IMoniker* moniker = nullptr;
        while (enum_moniker->Next(1, &moniker, nullptr) == S_OK) {
            IPropertyBag* bag = nullptr;
            CameraDevice device;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
                VARIANT value;
                VariantInit(&value);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) && value.bstrVal) {
                    device.name = value.bstrVal;
                }
                VariantClear(&value);
                VariantInit(&value);
                if (SUCCEEDED(bag->Read(L"DevicePath", &value, nullptr)) && value.bstrVal) {
                    device.path = value.bstrVal;
                }
                VariantClear(&value);
                bag->Release();
            }
            if (device.name.empty()) device.name = L"Камера #" + std::to_wstring(result.size());
            result.push_back(std::move(device));
            moniker->Release();
        }
    }
    if (enum_moniker) enum_moniker->Release();
    dev_enum->Release();
    return result;
}

enum class CamState { Off, Opening, On, Failed };

// Поток захвата: открывает камеру, постоянно читает кадры, хранит последний.
class Camera {
public:
    Camera() = default;
    ~Camera() { Stop(); }

    void SetNotifyWindow(HWND hwnd) { hwnd_ = hwnd; }

    void Start(int index, int width, int height, double fps) {
        Stop();
        {
            // Кадр прошлой сессии не должен считаться «текущим».
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = Frame{};
        }
        running_.store(true);
        state_ = CamState::Opening;
        worker_ = std::thread(&Camera::Run, this, index, width, height, fps);
    }

    void Stop() {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
        if (writer_.isOpened()) writer_.release();
        recording_.store(false);
        state_ = CamState::Off;
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = Frame{};
    }

    bool Latest(Frame& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_.ok()) return false;
        out = latest_;
        return true;
    }


    bool StartRecording(const std::wstring& path, double fps) {
        if (state_.load() != CamState::On) return false;
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_path_ = path;
        record_fps_ = fps > 1.0 ? fps : 25.0;
        record_start_.store(true);
        record_stop_.store(false);
        return true;
    }

    void StopRecording() {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_stop_.store(true);
        recording_.store(false);   // интерфейс обновляется сразу, файл закроет поток
    }

    bool Recording() const { return recording_.load(); }

    // Ждёт кадр до timeout_ms. Нужно, чтобы снимок сразу после «Подключить»
    // не падал с ошибкой «нет видеосигнала», пока камера ещё разгоняется.
    bool WaitForFrame(Frame& out, int timeout_ms) const {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!frame_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this] { return latest_.ok(); })) {
            return false;
        }
        out = latest_;
        return true;
    }

    CamState State() const { return state_.load(); }
    double MeasuredFps() const { return measured_fps_.load(); }
    std::wstring FormatName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return format_;
    }

private:
    void Run(int index, int want_w, int want_h, double want_fps) {
        cv::VideoCapture capture;
        bool opened = false;
        try {
            opened = capture.open(index, cv::CAP_DSHOW);
            if (!opened) opened = capture.open(index);  
        } catch (const cv::Exception&) {
            opened = false;
        }
        if (!opened) {
            Notify(CamState::Failed, L"Не удалось открыть камеру (занята или недоступна)");
            running_.store(false);
            return;
        }

        // MJPG предпочтительнее: при том же USB-трафике доступно большее разрешение.
        capture.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        if (want_w > 0) capture.set(cv::CAP_PROP_FRAME_WIDTH, want_w);
        if (want_h > 0) capture.set(cv::CAP_PROP_FRAME_HEIGHT, want_h);
        if (want_fps > 0) capture.set(cv::CAP_PROP_FPS, want_fps);

        Mat frame;
        try {
            capture.read(frame);
        } catch (const cv::Exception&) {
            frame = Mat();
        }
        if (frame.empty()) {
            capture.release();
            Notify(CamState::Failed,
                   L"Камера не отдаёт кадры (проверьте доступ к камере в Windows)");
            running_.store(false);
            return;
        }

        {
            const int code = (int)capture.get(cv::CAP_PROP_FOURCC);
            std::string fourcc(4, '\0');
            for (int i = 0; i < 4; ++i) fourcc[i] = (char)((code >> (8 * i)) & 0xFF);
            std::lock_guard<std::mutex> lock(mutex_);
            format_ = std::wstring(fourcc.begin(), fourcc.end());
            if (format_.size() != 4) format_ = L"BGR24";
            for (wchar_t& ch : format_) {
                if (ch < 32) ch = L'?';
            }
        }
        Notify(CamState::On, Format(L"Камера подключена: %d × %d", frame.cols, frame.rows));

        auto fps_start = std::chrono::steady_clock::now();
        unsigned long long fps_frames = 0;
        int failures = 0;

        while (running_.load()) {
            try {
                capture.read(frame);
            } catch (const cv::Exception&) {
                frame = Mat();
            }

            if (frame.empty()) {
                if (++failures > 30) break;   // камеру выдернули
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            failures = 0;

            if (frame.channels() == 1) {
                cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
            } else if (frame.channels() == 4) {
                cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
            }

            Frame out;
            out.width = frame.cols;
            out.height = frame.rows;
            const size_t row = (size_t)frame.cols * 3;
            out.bgr.resize(row * (size_t)frame.rows);
            if (frame.step == row) {
                memcpy(out.bgr.data(), frame.data, row * (size_t)frame.rows);
            } else {
                for (int y = 0; y < frame.rows; ++y) {
                    memcpy(out.bgr.data() + row * (size_t)y, frame.ptr(y), row);
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_ = std::move(out);
            }
            frame_cv_.notify_all();

            // --- запись видео: файл открывается здесь, в потоке захвата ---
            {
                bool start = false, stop = false;
                std::wstring path;
                double fps = 25.0;
                {
                    std::lock_guard<std::mutex> lock(record_mutex_);
                    start = record_start_.exchange(false);
                    stop = record_stop_.exchange(false);
                    path = record_path_;
                    fps = record_fps_;
                }
                if (stop && writer_.isOpened()) {
                    writer_.release();
                    recording_.store(false);
                }
                if (start) {
                    if (writer_.isOpened()) writer_.release();
                    writer_.open(ToUtf8(path), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), fps,
                                 cv::Size(frame.cols, frame.rows));
                    recording_.store(writer_.isOpened());
                }
                if (writer_.isOpened()) writer_.write(frame);
            }

            ++fps_frames;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - fps_start);
            if (elapsed.count() >= 1000) {
                measured_fps_.store((double)fps_frames * 1000.0 / (double)elapsed.count());
                fps_frames = 0;
                fps_start = std::chrono::steady_clock::now();
            }

            if (hwnd_) PostMessageW(hwnd_, WM_APP + 2, 0, 0);   // «новый кадр»
        }

        capture.release();
        measured_fps_.store(0.0);
        if (running_.load()) {
            Notify(CamState::Failed, L"Камера перестала отдавать кадры — подключите её заново");
        }
        running_.store(false);
    }

    void Notify(CamState state, const std::wstring& text) {
        state_ = state;
        auto* payload = new std::wstring(text);
        if (!PostMessageW(hwnd_, WM_APP + 1, (WPARAM)state, (LPARAM)payload)) delete payload;
    }

    HWND hwnd_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<CamState> state_{CamState::Off};
    mutable std::condition_variable frame_cv_;

    // запись видео
    std::mutex record_mutex_;
    std::wstring record_path_;
    double record_fps_ = 25.0;
    std::atomic<bool> record_start_{false};
    std::atomic<bool> record_stop_{false};
    std::atomic<bool> recording_{false};
    cv::VideoWriter writer_;   // трогает только поток захвата

    mutable std::mutex mutex_;
    Frame latest_;
    std::wstring format_;

    std::atomic<double> measured_fps_{0.0};
};

// ===========================================================================
//  4. Главное окно: логика приложения
// ===========================================================================

const wchar_t* kClassName = L"Lab4CameraWindow";

const wchar_t* kResTitles[] = {L"1920 × 1080", L"1280 × 720", L"1024 × 768",
                               L"960 × 540",   L"800 × 600",  L"640 × 480"};
const int kResW[] = {1920, 1280, 1024, 960, 800, 640};
const int kResH[] = {1080, 720, 768, 540, 600, 480};

// Частота кадров задаётся фиксированно: в интерфейсе лишняя настройка не нужна.
const double kCaptureFps = 30.0;
const double kRecordFps = 25.0;    // частота записи видео

// Снимок — одновременное нажатие «-» и «=». RegisterHotKey такую пару
// (две обычные клавиши) зарегистрировать не может, поэтому состояние клавиш
// опрашивается таймером: 25 мс достаточно, чтобы реакция была мгновенной,
// и при этом приложение не тратит заметного времени.
const UINT kChordPollMs = 25;

class App {
public:
    enum : UINT {
        WM_STATE = WM_APP + 1,   // состояние камеры из потока захвата
        WM_FRAME = WM_APP + 2,   // готов новый кадр
        WM_TRAY = WM_APP + 3,    // события значка в трее
    };

    enum : UINT {
        TIMER_INFO = 1,    // обновление счётчиков
        TIMER_CHORD = 2,   // опрос пары клавиш «-» + «=»
        TIMER_TOAST = 3,   // гашение всплывающих уведомлений
    };

    enum : int {
        HOTKEY_SNAPSHOT_ALT = 1,   // резервная комбинация Ctrl+Shift+F12
    };

    enum : int {
        ID_MENU_SHOW = 2001,
        ID_MENU_SNAPSHOT,
        ID_MENU_EXIT,
    };

    App() = default;
    ~App() { Cleanup(); }

    bool Init(HINSTANCE inst, int show) {
        inst_ = inst;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &App::WndProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
                                     LR_DEFAULTSIZE | LR_SHARED);
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;

        hwnd_ = CreateWindowExW(0, kClassName, L"Фотонаблюдение",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, S(1160), S(800), nullptr, nullptr, inst_, this);
        if (!hwnd_) return false;

        ShowWindow(hwnd_, show);
        UpdateWindow(hwnd_);
        return true;
    }

    int Run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }

private:
    // -----------------------------------------------------------------------
    //  Окно и сообщения
    // -----------------------------------------------------------------------
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<App*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE:
                OnCreate();
                return 0;

            case WM_SIZE:
                view_.Layout();
                return 0;

            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lp);
                info->ptMinTrackSize.x = view_.MinWidth();
                info->ptMinTrackSize.y = view_.MinHeight();
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;   // фон рисуем сами

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                RECT rc;
                GetClientRect(hwnd_, &rc);
                if (rc.right > 0 && rc.bottom > 0) {
                    // Двойная буферизация: видео не мерцает на 30 кадрах в секунду.
                    HDC mem = CreateCompatibleDC(dc);
                    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                    HGDIOBJ old = SelectObject(mem, bmp);
                    Paint(mem, rc);
                    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteObject(bmp);
                    DeleteDC(mem);
                }
                EndPaint(hwnd_, &ps);
                return 0;
            }

            case WM_TIMER:
                OnTimer((UINT)wp);
                return 0;

            case WM_COMMAND:
                OnCommand(LOWORD(wp), HIWORD(wp));
                return 0;

            case WM_HOTKEY:
                OnHotkey();
                return 0;

            case WM_DEVICECHANGE:
                if (wp == DBT_DEVNODES_CHANGED || wp == DBT_DEVICEARRIVAL ||
                    wp == DBT_DEVICEREMOVECOMPLETE) {
                    RefreshDevices(false);
                }
                return TRUE;

            case WM_STATE: {
                auto* text = reinterpret_cast<std::wstring*>(lp);
                OnCameraState((CamState)wp, text ? *text : L"");
                delete text;
                return 0;
            }

            case WM_FRAME: {
                // Перерисовываем только область предпросмотра — остальное статично.
                RECT rc = view_.PreviewRect();
                InvalidateRect(hwnd_, &rc, FALSE);
                return 0;
            }

            case WM_TRAY:
                OnTray(lp);
                return 0;

            case WM_DPICHANGED: {
                // Сменился монитор или масштаб — пересчитываем шрифты и раскладку.
                SetDpi((int)HIWORD(wp));
                view_.RecreateFonts();
                RECT* rc = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left,
                             rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
                view_.Layout();
                InvalidateRect(hwnd_, nullptr, TRUE);
                return 0;
            }

            case WM_DRAWITEM: {
                auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
                if (view_.DrawItem(item)) return TRUE;
                break;
            }

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                HDC dc = (HDC)wp;
                SetTextColor(dc, RGB(235, 240, 248));
                SetBkColor(dc, RGB(12, 18, 30));
                return (LRESULT)view_.FieldBrush();
            }

            case WM_CLOSE:
                DestroyWindow(hwnd_);
                return 0;

            case WM_DESTROY:
                Cleanup();
                PostQuitMessage(0);
                return 0;

            default:
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void Paint(HDC dc, const RECT& client) {
        // Кадр берём один раз за отрисовку: указатель живёт до конца Paint().
        if (camera_.Latest(paint_frame_)) {
            has_frame_ = true;
        } else if (camera_.State() != CamState::On) {
            has_frame_ = false;   // камера остановлена — показываем заглушку
        }
        // Разовый пропуск кадра при работающей камере не должен превращаться
        // в надпись «нет видеосигнала» — остаётся последний показанный кадр.
        if (has_frame_) {
            model_.frame.bgr = paint_frame_.bgr.data();
            model_.frame.width = paint_frame_.width;
            model_.frame.height = paint_frame_.height;
        } else {
            model_.frame = FrameView{};
        }
        view_.Paint(dc, client, model_);
    }

    void OnCreate() {
        view_.Create(hwnd_, inst_);

        for (const wchar_t* title : kResTitles) {
            SendMessageW(view_.ResolutionCombo(), CB_ADDSTRING, 0, (LPARAM)title);
        }
        SendMessageW(view_.ResolutionCombo(), CB_SETCURSEL, 1, 0);

        save_dir_ = DefaultSaveDir();
        EnsureDir(save_dir_);
        SetWindowTextW(view_.DirectoryEdit(), save_dir_.c_str());

        camera_.SetNotifyWindow(hwnd_);
        AddTrayIcon();

        // Снимок: «-» и «=» одновременно. Резерв — Ctrl+Shift+F12.
        alt_hotkey_ok_ = RegisterHotKey(hwnd_, HOTKEY_SNAPSHOT_ALT, MOD_CONTROL | MOD_SHIFT,
                                        VK_F12) != FALSE;
        SetTimer(hwnd_, TIMER_INFO, 500, nullptr);
        SetTimer(hwnd_, TIMER_CHORD, kChordPollMs, nullptr);

        AddLog(0, L"Приложение запущено");

        RefreshDevices(true);
        view_.Layout();
        UpdateModel();
    }

    // -----------------------------------------------------------------------
    //  Журнал
    // -----------------------------------------------------------------------
    void AddLog(int level, const std::wstring& text) {
        const std::wstring line = NowStamp(true) + L"  " + text;
        log_.push_back(line);
        if (log_.size() > 2000) log_.erase(log_.begin());

        view_.AddLogLine(line, level);

        // Дублируем в файл: журнал сохраняется даже при аварийном закрытии.
        if (!save_dir_.empty()) {
            HANDLE h = CreateFileW(JoinPath(save_dir_, L"camera_log.txt").c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                const std::string utf8 = ToUtf8(line + L"\r\n");
                DWORD written = 0;
                LARGE_INTEGER size{};
                if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
                    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
                    WriteFile(h, bom, 3, &written, nullptr);
                }
                WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
                CloseHandle(h);
            }
        }
    }

    // -----------------------------------------------------------------------
    //  Камера
    // -----------------------------------------------------------------------
    void RefreshDevices(bool initial) {
        devices_ = EnumerateCameras();
        HWND combo = view_.CameraCombo();
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (const auto& device : devices_) {
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)device.name.c_str());
        }

        if (!devices_.empty()) {
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
            if (!initial) {
                Notify(Format(L"Найдено камер: %d", (int)devices_.size()), 0);
            }
        } else {
            SendMessageW(combo, CB_SETCURSEL, (WPARAM)-1, 0);
            Notify(L"Камеры не найдены — проверьте подключение и доступ в Windows", 1);
        }

        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Connect() {
        if (camera_.State() == CamState::On || camera_.State() == CamState::Opening) return;
        if (devices_.empty()) return;

        int index = (int)SendMessageW(view_.CameraCombo(), CB_GETCURSEL, 0, 0);
        index = std::clamp(index, 0, (int)devices_.size() - 1);
        int res = (int)SendMessageW(view_.ResolutionCombo(), CB_GETCURSEL, 0, 0);
        res = std::clamp(res, 0, (int)std::size(kResTitles) - 1);
        status_text_ = L"Подключение камеры…";
        AddLog(0, Format(L"Подключение «%s» (%s, %.0f к/с)", devices_[index].name.c_str(),
                         kResTitles[res], kCaptureFps));
        camera_.Start(index, kResW[res], kResH[res], kCaptureFps);
        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Disconnect() {
        camera_.Stop();
        status_text_ = L"Камера не подключена";
        AddLog(0, L"Камера отключена");
        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnCameraState(CamState state, const std::wstring& text) {
        status_text_ = text;
        switch (state) {
            case CamState::On:      AddLog(0, text); break;
            case CamState::Failed:  AddLog(2, text); break;
            case CamState::Opening: AddLog(0, text); break;
            case CamState::Off:     break;   // штатное отключение уже записано выше
        }
        if (state == CamState::Failed) Notify(text, 2);
        UpdateModel();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // -----------------------------------------------------------------------
    //  Снимок и отчёт
    // -----------------------------------------------------------------------
    void ApplyDirectory() {
        wchar_t buffer[MAX_PATH * 2] = {};
        GetWindowTextW(view_.DirectoryEdit(), buffer, (int)std::size(buffer));
        std::wstring dir = buffer;
        while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'\r')) dir.pop_back();
        if (dir.empty()) dir = DefaultSaveDir();

        if (!EnsureDir(dir)) {
            dir = DefaultSaveDir();
            EnsureDir(dir);
            SetWindowTextW(view_.DirectoryEdit(), dir.c_str());
            Notify(L"Не удалось создать каталог — выбран каталог по умолчанию", 2);
        }
        save_dir_ = dir;
    }

    void TakeSnapshot(bool background) {
        ApplyDirectory();

        Frame frame;
        bool have = camera_.Latest(frame);
        if (!have && (camera_.State() == CamState::On || camera_.State() == CamState::Opening)) {
            // Даём камере шанс выдать первый кадр (сразу после «Подключить»).
            have = camera_.WaitForFrame(frame, 1500);
        }
        if (!have) {
            std::wstring reason = L"нет видеосигнала";
            switch (camera_.State()) {
                case CamState::Off:     reason = L"камера не подключена"; break;
                case CamState::Opening: reason = L"камера не успела выдать кадр"; break;
                case CamState::Failed:  reason = L"камера недоступна"; break;
                default: break;
            }
            AddLog(1, L"Снимок не создан: " + reason);
            Notify(L"Снимок не создан: " + reason, 1);
            return;
        }

        const std::wstring path = SnapshotPath(save_dir_);
        Mat mat(frame.height, frame.width, CV_8UC3, frame.bgr.data());
        if (!ImwriteAny(path, mat, 92)) {
            AddLog(2, L"Не удалось сохранить снимок: " + FileName(path));
            Notify(L"Не удалось сохранить снимок: " + FileName(path), 2);
            return;
        }

        ++snapshot_count_;
        last_snapshot_ = FileName(path);
        AddLog(background ? 3 : 0,
               Format(L"Снимок сохранён: %s (%d × %d, %.1f КБ)", FileName(path).c_str(),
                      frame.width, frame.height, (double)FileSize(path) / 1024.0));
        Notify(std::wstring(background ? L"Снимок (скрытый режим): " : L"Снимок сохранён: ") +
                   FileName(path),
               3);
        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SaveReport() {
        ApplyDirectory();
        const std::wstring path = JoinPath(save_dir_, L"report_" + FileStamp() + L".txt");

        std::wstring text;
        text += L"ЛР №4. Фотонаблюдение с веб-камеры (скрытый режим)\r\n";
        text += L"Отчёт сформирован: " + NowStamp(false) + L"\r\n";
        text += std::wstring(78, L'=') + L"\r\n\r\n";
        for (const auto& line : log_) text += line + L"\r\n";
        if (log_.empty()) text += L"(журнал пуст)\r\n";

        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            AddLog(2, L"Не удалось сохранить отчёт: " + FileName(path));
            return;
        }
        const std::string utf8 = ToUtf8(text);
        DWORD written = 0;
        static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        WriteFile(h, bom, 3, &written, nullptr);
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);

        Notify(L"Отчёт сохранён: " + FileName(path), 0);
    }

    // -----------------------------------------------------------------------
    //  Запись видео (функционал базового монитора)
    // -----------------------------------------------------------------------
    void StartRecording() {
        if (camera_.State() != CamState::On) {
            Notify(L"Запись не начата: камера не подключена", 1);
            return;
        }
        ApplyDirectory();
        const std::wstring path = VideoPath(save_dir_);
        if (!camera_.StartRecording(path, kRecordFps)) {
            Notify(L"Не удалось начать запись", 2);
            return;
        }
        last_video_ = FileName(path);
        model_.recording = true;
        view_.SetEnabled(model_);
        Notify(L"Запись начата: " + last_video_, 3);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void StopRecording() {
        camera_.StopRecording();
        model_.recording = false;
        view_.SetEnabled(model_);
        Notify(last_video_.empty() ? L"Запись остановлена"
                                   : L"Запись остановлена: " + last_video_,
               0);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // -----------------------------------------------------------------------
    //  Скрытый режим: трей и горячая клавиша
    // -----------------------------------------------------------------------
    void AddTrayIcon() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = 1;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        data.uCallbackMessage = WM_TRAY;
        data.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
                                       LR_DEFAULTSIZE | LR_SHARED);
        if (!data.hIcon) data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(data.szTip, L"Фотонаблюдение (снимок: «-» и «=»)");
        tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    }

    void HideToTray() {
        // Без открытой камеры фоновый снимок невозможен — подключаем автоматически.
        if (camera_.State() != CamState::On && !devices_.empty()) {
            Connect();   // без камеры фоновый снимок невозможен
        }
        ShowWindow(hwnd_, SW_HIDE);
        hidden_ = true;
        Balloon(L"Фотонаблюдение включено",
                L"Окно скрыто. «-» и «=» одновременно — снимок без открытия окна.\n"
                L"Двойной щелчок по значку вернёт окно.",
                NIIF_INFO);
    }

    void Restore() {
        ShowWindow(hwnd_, SW_SHOW);
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        hidden_ = false;
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // Снимок по резервной комбинации Ctrl+Shift+F12.
    void OnHotkey() {
        TakeSnapshot(true);
    }

    // Опрос пары клавиш «-» + «=»: снимок по одновременному нажатию.
    void PollChord() {
        const bool minus = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;
        const bool plus = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;
        const bool both = minus && plus;

        if (both && !chord_down_) {
            chord_down_ = true;
            TakeSnapshot(true);
        } else if (!both) {
            chord_down_ = false;
        }
    }

    void OnTray(LPARAM event) {
        switch (LOWORD(event)) {
            case WM_LBUTTONDBLCLK:
                Restore();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU: {
                POINT pt;
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_MENU_SHOW, L"Показать окно");
                AppendMenuW(menu, MF_STRING, ID_MENU_SNAPSHOT, L"Сделать снимок");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Выход");
                SetForegroundWindow(hwnd_);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_,
                               nullptr);
                DestroyMenu(menu);
                PostMessageW(hwnd_, WM_NULL, 0, 0);
                break;
            }
            default:
                break;
        }
    }

    // Короткое уведомление: в окне — всплывающая плашка (до трёх сразу),
    // в скрытом режиме — balloon tip у значка в трее.
    void Notify(const std::wstring& text, int level) {
        if (hidden_) {
            Balloon(L"Фотонаблюдение", text,
                    level == 2 ? NIIF_ERROR : (level == 1 ? NIIF_WARNING : NIIF_INFO));
            return;
        }
        view_.PushToast(text, level);
        SetTimer(hwnd_, TIMER_TOAST, 80, nullptr);
    }

    void Balloon(const std::wstring& title, const std::wstring& text, DWORD flags) {
        if (!tray_added_) return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = 1;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = flags;
        wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    // -----------------------------------------------------------------------
    //  Команды, таймер, завершение
    // -----------------------------------------------------------------------
    void OnCommand(int id, int code) {
        switch (id) {
            case ID_CONNECT:
                if (code == BN_CLICKED) Connect();
                break;
            case ID_DISCONNECT:
                if (code == BN_CLICKED) Disconnect();
                break;
            case ID_REFRESH:
                if (code == BN_CLICKED) RefreshDevices(false);
                break;
            case ID_SNAPSHOT:
                if (code == BN_CLICKED) TakeSnapshot(false);
                break;
            case ID_BROWSE:
                if (code == BN_CLICKED) BrowseDirectory();
                break;
            case ID_REPORT:
                if (code == BN_CLICKED) SaveReport();
                break;
            case ID_RECORD_START:
                if (code == BN_CLICKED) StartRecording();
                break;
            case ID_RECORD_STOP:
                if (code == BN_CLICKED) StopRecording();
                break;
            case ID_TRAY_BUTTON:
                if (code == BN_CLICKED) HideToTray();
                break;
            case ID_MENU_SHOW:
                Restore();
                break;
            case ID_MENU_SNAPSHOT:
                TakeSnapshot(true);
                break;
            case ID_MENU_EXIT:
                DestroyWindow(hwnd_);
                break;
            default:
                break;
        }
    }

    void OnTimer(UINT id) {
        if (id == TIMER_CHORD) {
            PollChord();
            return;
        }
        if (id == TIMER_TOAST) {
            // Пока есть плашки — перерисовываем; когда все погасли, таймер не нужен.
            if (!view_.ExpireToasts()) KillTimer(hwnd_, TIMER_TOAST);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (id != TIMER_INFO) return;

        Frame frame;
        if (camera_.Latest(frame)) {
            const double fps = camera_.MeasuredFps();
            model_.mode = Format(L"%d × %d", frame.width, frame.height);
            if (fps > 0.5) model_.mode += Format(L", %.0f к/с", fps);
            model_.format = camera_.FormatName();
        } else {
            model_.mode.clear();
            model_.format.clear();
        }

        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Логика сообщает визуалу, что показывать, и включает/выключает контролы.
    void UpdateModel() {
        model_.camera_state = (int)camera_.State();
        model_.has_devices = !devices_.empty();
        model_.status = status_text_;
        model_.snapshot_count = snapshot_count_;
        model_.last_snapshot = last_snapshot_;
        model_.recording = camera_.Recording();
        view_.SetEnabled(model_);
    }

    void BrowseDirectory() {
        BROWSEINFOW info{};
        info.hwndOwner = hwnd_;
        info.lpszTitle = L"Каталог для сохранения снимков и журнала";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE idl = SHBrowseForFolderW(&info);
        if (!idl) return;
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(idl, path)) {
            SetWindowTextW(view_.DirectoryEdit(), path);
            ApplyDirectory();
            AddLog(0, std::wstring(L"Каталог сохранения: ") + path);
        }
        CoTaskMemFree(idl);
    }

    void Cleanup() {
        if (cleaned_) return;
        cleaned_ = true;

        KillTimer(hwnd_, TIMER_INFO);
        KillTimer(hwnd_, TIMER_CHORD);
        KillTimer(hwnd_, TIMER_TOAST);
        AddLog(0, L"Приложение закрыто");   // последняя запись журнала
        if (alt_hotkey_ok_) {
            UnregisterHotKey(hwnd_, HOTKEY_SNAPSHOT_ALT);
            alt_hotkey_ok_ = false;
        }
        camera_.Stop();

        if (tray_added_) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = hwnd_;
            data.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &data);
            tray_added_ = false;
        }

        view_.Destroy();
    }

    // --- поля --------------------------------------------------------------
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    bool cleaned_ = false;

    View view_;
    Model model_;
    Frame paint_frame_;

    Camera camera_;
    std::vector<CameraDevice> devices_;
    std::vector<std::wstring> log_;   // строки журнала для отчёта

    std::wstring save_dir_;
    std::wstring status_text_ = L"Камера не подключена";
    std::wstring last_snapshot_;
    std::wstring last_video_;
    int snapshot_count_ = 0;

    bool tray_added_ = false;
    bool alt_hotkey_ok_ = false;
    bool chord_down_ = false;
    bool has_frame_ = false;   // последний кадр можно показывать
    bool hidden_ = false;
};

}  // namespace

// ===========================================================================
//  5. Точка входа
// ===========================================================================
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    // DPI: без этого шрифты выглядят мелкими и размытыми на масштабе 125–150 %.
    if (HMODULE user32 = GetModuleHandleW(L"user32")) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto set_ctx = reinterpret_cast<SetCtxFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
    }
    {
        HDC screen = GetDC(nullptr);
        int dpi = 96;
        if (screen) {
            dpi = GetDeviceCaps(screen, LOGPIXELSY);
            ReleaseDC(nullptr, screen);
        }
        SetDpi(dpi);
    }

    // GDI+ — основа оформления (как в ЛР №1).
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) return 1;

    // COM нужен для DirectShow (перечисление камер) и диалога выбора каталога.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    int result = 0;
    {
        App app;
        if (!app.Init(inst, show)) {
            MessageBoxW(nullptr, L"Не удалось создать окно приложения.", L"Ошибка",
                        MB_OK | MB_ICONERROR);
            result = 1;
        } else {
            result = app.Run();
        }
    }

    if (SUCCEEDED(com)) CoUninitialize();
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return result;
}
