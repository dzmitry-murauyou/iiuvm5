// ===========================================================================
//  main.cpp — ЛР №5, вариант А1: «USB Flash (Mass Storage) на Windows»
//
//  Задание (см. методичку, вариант 1):
//    1. Перечислить USB-накопители через SetupAPI
//       (GUID_DEVINTERFACE_DISK, GUID_DEVINTERFACE_USB_DEVICE).
//    2. Для каждого вывести VID, PID, серийный номер, производителя,
//       модель и объём.
//    3. Определить буквы логических дисков, сопоставленные накопителю.
//    4. Реализовать программное безопасное извлечение через
//       CM_Request_Device_Eject.
//    5. Обработать и вывести причину отказа (veto) при невозможности
//       извлечения.
//
//  Дополнительно (как в ЛР №4, чтобы программа была «живой»):
//    • горячее подключение через WM_DEVICECHANGE (DBT_DEVICEARRIVAL /
//      DBT_DEVICEREMOVECOMPLETE + GUID_DEVINTERFACE_VOLUME);
//    • скорость соединения из IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX
//      (драйвер USB-хаба отдаёт скорость и дескриптор устройства);
//    • журнал событий в окне и в файле, отчёт со всеми сведениями.
//
//  Архитектура — как в ЛР №4: логика (этот файл) отделена от визуала
//  (visual.cpp) структурой visual::Model; визуал ничего не знает про
//  SetupAPI и IOCTL.
// ===========================================================================
#include "visual.h"

#include <cfgmgr32.h>
#include <commctrl.h>
#include <dbt.h>
#include <devguid.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "gdiplus.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
 name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
 processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// GUID_DEVINTERFACE_* определяются в этом модуле: без initguid.h в obj-файле
// остаются только внешние ссылки, и линковка падает на
// GUID_DEVINTERFACE_USB_DEVICE / GUID_DEVINTERFACE_USB_HUB.
#include <initguid.h>
#include <devpkey.h>    // после initguid.h: DEVPKEY_* получают определение в этом модуле
#include <usbiodef.h>
#include <usbioctl.h>   // после initguid.h: структуры портов USB-хаба

namespace {

using namespace visual;

// ===========================================================================
//  1. Мелкие утилиты: строки, форматирование, файлы
// ===========================================================================
std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[2048];
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

bool DirExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
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

std::wstring DefaultReportDir() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, path))) {
        return JoinPath(path, L"UsbLab5");
    }
    return L"reports";
}

std::wstring ReportPath(const std::wstring& dir) {
    const std::wstring base = JoinPath(dir, L"usb_report_" + FileStamp());
    std::wstring candidate = base + L".txt";
    for (int i = 1; GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        candidate = base + L"_" + std::to_wstring(i) + L".txt";
    }
    return candidate;
}

std::wstring Uppercase(std::wstring text) {
    if (!text.empty()) CharUpperBuffW(text.data(), (DWORD)text.size());
    return text;
}

std::wstring Trim(const std::wstring& text) {
    size_t begin = 0, end = text.size();
    const auto space = [](wchar_t ch) {
        return ch == L' ' || ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L'\0';
    };
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// Объём в «человеческом» виде: 28,9 ГБ (как на упаковке накопителя).
std::wstring HumanSize(unsigned long long bytes) {
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;
    const double tb = gb * 1024.0;
    if (bytes == 0) return L"—";
    if ((double)bytes >= tb) return Format(L"%.2f ТБ", (double)bytes / tb);
    if ((double)bytes >= gb) return Format(L"%.1f ГБ", (double)bytes / gb);
    if ((double)bytes >= mb) return Format(L"%.0f МБ", (double)bytes / mb);
    return Format(L"%llu КБ", bytes / 1024);
}

// Точные байты с разделением разрядов — нужны в отчёте и подсказке.
std::wstring ExactBytes(unsigned long long bytes) {
    std::wstring digits = std::to_wstring(bytes);
    std::wstring out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.insert(out.begin(), L' ');
        out.insert(out.begin(), *it);
        ++count;
    }
    return out + L" байт";
}

// ===========================================================================
//  2. Модель данных: накопитель, том, результат извлечения
// ===========================================================================

// Один логический том (раздел), принадлежащий накопителю.
struct VolumeInfo {
    wchar_t letter = 0;            // 'E'
    std::wstring label;            // метка тома
    std::wstring file_system;      // NTFS, FAT32, exFAT…
    unsigned long long total = 0;  // размер тома
    unsigned long long free = 0;   // свободно
    bool system = false;           // системный/загрузочный — извлекать нельзя
};

// Всё, что программа узнала о накопителе.
struct UsbDisk {
    std::wstring instance_id;    // USBSTOR\DISK&VEN_...\...
    std::wstring disk_path;      // \\?\usbstor#disk&ven_...#...
    std::wstring friendly_name;  // «USB DISK 3.0 USB Device»
    std::wstring device_desc;    // описание из INF
    std::wstring manufacturer;   // производитель (строковый дескриптор USB)
    std::wstring model;          // модель
    std::wstring serial;         // серийный номер
    std::wstring vid;            // «0781»
    std::wstring pid;            // «5567»
    std::wstring revision;       // версия прошивки
    std::wstring usb_version;    // «USB 3.0» / «USB 2.0»
    std::wstring speed;          // «Высокая (480 Мбит/с)»
    std::wstring bus;            // «USB»
    std::wstring usb_class;      // «Mass Storage (0x08)» — класс из дескриптора
    std::wstring usb_id_source;  // откуда взяты VID/PID (для отчёта)

    // Тип устройства: накопитель или телефон/плеер (MTP, PTP, ADB, модем).
    enum class Kind { MassStorage, Portable };
    Kind kind = Kind::MassStorage;
    std::wstring device_kind;        // «USB-накопитель» / «Телефон (MTP)»
    std::vector<std::wstring> interfaces;   // функции устройства (для телефонов)
    std::wstring usb_node_id;        // Instance ID USB-узла (USB\VID_...&MI_..)
    std::wstring eject_note;         // чем извлекали (для журнала и отчёта)
    DWORD device_number = 0;     // номер физического диска
    unsigned long long capacity = 0;
    int sector_size = 0;
    bool removable = false;
    bool write_protected = false;
    bool system = false;             // среди томов есть системный
    bool present = true;             // устройство ещё подключено
    std::vector<VolumeInfo> volumes;
};

struct ScanResult {
    std::vector<UsbDisk> disks;
    std::vector<std::wstring> warnings;   // проблемы, которые стоит показать
    bool fatal = false;                   // перечисление вообще не удалось
};

// Итог попытки извлечения.
struct EjectResult {
    bool ok = false;
    CONFIGRET cr = CR_SUCCESS;
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    std::wstring veto_name;      // какой процесс/устройство держит накопитель
    std::wstring target_id;      // узел, к которому применялся запрос
    std::wstring message;        // короткая строка для журнала и плашки
    std::vector<std::wstring> advice;   // что делать при отказе

    // Диагностика: чем именно закончились этапы.
    DWORD last_error = 0;              // GetLastError после последнего вызова
    bool came_back = false;            // устройство исчезло (проверено повторно)
    bool fallback_used = false;        // применено отключение узла (DICS_DISABLE)
};

// ===========================================================================
//  3. RAII-обёртки и вспомогательные обёртки SetupAPI/CfgMgr32
// ===========================================================================

// Набор устройств SetupAPI: гарантирует SetupDiDestroyDeviceInfoList.
class DeviceInfoSet {
public:
    explicit DeviceInfoSet(HDEVINFO set) : set_(set) {}
    ~DeviceInfoSet() {
        if (set_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set_);
    }
    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    HDEVINFO get() const { return set_; }
    explicit operator bool() const { return set_ != INVALID_HANDLE_VALUE; }

private:
    HDEVINFO set_ = INVALID_HANDLE_VALUE;
};

// Ровно один хэндл (файлы устройств, тома): закрывается при выходе из области.
class Handle {
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }

    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// Строковое свойство устройства из реестра (SPDRP_*).
std::wstring RegistryString(HDEVINFO set, SP_DEVINFO_DATA& data, DWORD property) {
    DWORD type = 0;
    DWORD needed = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &data, property, &type, nullptr, 0, &needed);
    if (needed == 0) return {};
    std::vector<BYTE> buffer(needed + sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(set, &data, property, &type, buffer.data(), needed,
                                           nullptr)) {
        return {};
    }
    if (type != REG_SZ && type != REG_MULTI_SZ) return {};
    return Trim(reinterpret_cast<const wchar_t*>(buffer.data()));
}

// Разбор интерфейса: символьный путь + SP_DEVINFO_DATA «в одном флаконе».
// Важно: SP_DEVINFO_DATA нужно получать именно отсюда, а не через
// SetupDiEnumDeviceInfo по тому же индексу — порядок интерфейсов и элементов
// набора устройств не обязан совпадать.
struct InterfaceDetail {
    std::wstring path;
    SP_DEVINFO_DATA data{};
    bool valid = false;
};

InterfaceDetail GetInterfaceDetail(HDEVINFO set, SP_DEVICE_INTERFACE_DATA& interface_data) {
    InterfaceDetail result;

    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &interface_data, nullptr, 0, &needed, nullptr);
    if (needed == 0) return result;

    std::vector<BYTE> buffer(needed);
    auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    detail->DevicePath[0] = L'\0';

    result.data.cbSize = sizeof(result.data);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &interface_data, detail, needed, nullptr,
                                          &result.data)) {
        return result;
    }
    result.path = detail->DevicePath;
    result.valid = !result.path.empty();
    return result;
}

// Открыть диск/том/хаб по символьному пути.
Handle OpenDevice(const std::wstring& path, DWORD access = 0) {
    return Handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr));
}

// Строковое свойство узла через CfgMgr32 (DEVPKEY_*), без SetupAPI.
std::wstring NodeString(DEVINST node, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    DWORD size = 0;
    CM_Get_DevNode_PropertyW(node, &key, &type, nullptr, &size, 0);
    if (size == 0) return {};
    std::vector<BYTE> buffer(size + sizeof(wchar_t));
    if (CM_Get_DevNode_PropertyW(node, &key, &type, buffer.data(), &size, 0) != CR_SUCCESS) {
        return {};
    }
    if (type != DEVPROP_TYPE_STRING) return {};
    return Trim(reinterpret_cast<const wchar_t*>(buffer.data()));
}

// Instance ID узла и его родителя — короткие обёртки над CfgMgr32.
std::wstring NodeId(DEVINST node) {
    wchar_t id[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};
    return id;
}

DEVINST NodeParent(DEVINST node) {
    DEVINST parent = 0;
    return CM_Get_Parent(&parent, node, 0) == CR_SUCCESS ? parent : 0;
}

// ===========================================================================
//  4. IOCTL: объём, номер диска, признаки, серийный номер
// ===========================================================================

// Ёмкость носителя: IOCTL_DISK_GET_LENGTH_INFO, запасной вариант —
// IOCTL_STORAGE_READ_CAPACITY (некоторые съёмные устройства не поддерживают
// первый запрос).
unsigned long long DiskCapacity(HANDLE device, int& sector_size) {
    sector_size = 0;

    GET_LENGTH_INFORMATION length{};
    DWORD returned = 0;
    DISK_GEOMETRY geometry{};
    if (DeviceIoControl(device, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &geometry,
                        sizeof(geometry), &returned, nullptr)) {
        sector_size = (int)geometry.BytesPerSector;
    }

    if (DeviceIoControl(device, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
                        &returned, nullptr)) {
        return (unsigned long long)length.Length.QuadPart;
    }

    STORAGE_READ_CAPACITY capacity{};
    capacity.Version = sizeof(STORAGE_READ_CAPACITY);
    capacity.Size = sizeof(STORAGE_READ_CAPACITY);
    if (DeviceIoControl(device, IOCTL_STORAGE_READ_CAPACITY, nullptr, 0, &capacity,
                        sizeof(capacity), &returned, nullptr)) {
        const unsigned long long blocks = (unsigned long long)capacity.NumberOfBlocks.QuadPart;
        if (sector_size == 0 && blocks != 0) {
            sector_size = (int)((unsigned long long)capacity.DiskLength.QuadPart / blocks);
        }
        return (unsigned long long)capacity.DiskLength.QuadPart;
    }
    return 0;
}

// Номер физического диска: связывает SetupAPI-путь диска с его томами.
bool DiskNumber(HANDLE device, DWORD& number) {
    STORAGE_DEVICE_NUMBER info{};
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &info, sizeof(info),
                         &returned, nullptr)) {
        return false;
    }
    number = info.DeviceNumber;
    return true;
}

// Съёмность и защита от записи (три независимых источника).
void DiskFlags(HANDLE device, bool& removable, bool& write_protected) {
    DWORD returned = 0;

    DISK_GEOMETRY_EX geometry{};
    if (DeviceIoControl(device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry,
                        sizeof(geometry), &returned, nullptr)) {
        removable = geometry.Geometry.MediaType == RemovableMedia;
    }

    STORAGE_HOTPLUG_INFO hotplug{};
    hotplug.Size = sizeof(hotplug);
    if (DeviceIoControl(device, IOCTL_STORAGE_GET_HOTPLUG_INFO, nullptr, 0, &hotplug,
                        sizeof(hotplug), &returned, nullptr)) {
        removable = removable || hotplug.MediaRemovable != FALSE;
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<BYTE> buffer(1024);
    if (DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
                        (DWORD)buffer.size(), &returned, nullptr)) {
        auto* descriptor = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
        removable = descriptor->RemovableMedia != FALSE;
    }

    // IOCTL_DISK_IS_WRITABLE: запрос успешен, если носитель доступен для записи.
    // Неудача с ERROR_WRITE_PROTECT означает аппаратную/программную защиту.
    if (!DeviceIoControl(device, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &returned,
                         nullptr)) {
        write_protected = GetLastError() == ERROR_WRITE_PROTECT;
    }
}

// Серийный номер из дескриптора устройства хранения (резервный источник).
std::wstring StorageSerial(HANDLE device) {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    std::vector<BYTE> buffer(2048);
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
                         (DWORD)buffer.size(), &returned, nullptr)) {
        return {};
    }
    auto* descriptor = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    if (descriptor->SerialNumberOffset == 0 ||
        descriptor->SerialNumberOffset >= (DWORD)buffer.size()) {
        return {};
    }
    const char* serial =
        reinterpret_cast<const char*>(buffer.data() + descriptor->SerialNumberOffset);
    std::wstring wide;
    while (*serial) wide += (wchar_t)(unsigned char)*serial++;
    return Trim(wide);
}

// ===========================================================================
//  5. Скорость соединения: IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX
//
//  Это ровно тот «низкоуровневый IOCTL», о котором говорит методичка:
//  открываем родительский хаб (GUID_DEVINTERFACE_USB_HUB), узнаём номер
//  порта, к которому подключён накопитель, и запрашиваем информацию о порте.
//  В ответе — дескриптор устройства (idVendor/idProduct/bcdUSB) и скорость.
// ===========================================================================

std::wstring SpeedText(unsigned char speed) {
    switch (speed) {
        case UsbLowSpeed:       return L"Низкая (1,5 Мбит/с)";
        case UsbFullSpeed:      return L"Полная (12 Мбит/с)";
        case UsbHighSpeed:      return L"Высокая (480 Мбит/с)";
        case UsbSuperSpeed:     return L"SuperSpeed (5 Гбит/с)";
        default:                return L"неизвестна";
    }
}

// Ищем символьный путь хаба, к которому подключён узел device_node.
std::wstring ParentHubPath(DEVINST device_node, ULONG& port_number) {
    port_number = 0;

    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, device_node, 0) != CR_SUCCESS) return {};

    wchar_t parent_id[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(parent, parent_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};
    if (Uppercase(parent_id).find(L"ROOT_HUB") == std::wstring::npos) {
        // Между диском и хабом может стоять ещё один узел: поднимаемся выше.
        DEVINST grand = 0;
        if (CM_Get_Parent(&grand, parent, 0) != CR_SUCCESS) return {};
        if (CM_Get_Device_IDW(grand, parent_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};
    }

    // Номер порта — это Address родительского узла (DEVPKEY_Device_Address).
    DEVPROPTYPE type = 0;
    ULONG address = 0;
    DWORD size = sizeof(address);
    if (CM_Get_DevNode_PropertyW(parent, &DEVPKEY_Device_Address, &type,
                                 reinterpret_cast<PBYTE>(&address), &size, 0) == CR_SUCCESS) {
        port_number = address;
    }

    // Символьные пути всех хабов; нужный выбираем по Instance ID.
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_HUB),
                                          parent_id, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
        CR_SUCCESS) {
        return {};
    }
    if (length < 2) return {};

    std::vector<wchar_t> list(length + 1);
    if (CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_HUB), parent_id,
                                      list.data(), length,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return {};
    }
    return std::wstring(list.data());
}

// Человекочитаемое имя класса интерфейса USB (по bInterfaceClass).
const wchar_t* InterfaceClassName(unsigned char cls) {
    switch (cls) {
        case 0x01: return L"Audio";
        case 0x02: return L"CDC / модем";
        case 0x03: return L"HID";
        case 0x06: return L"PTP (камера)";
        case 0x07: return L"Printer";
        case 0x08: return L"Mass Storage";
        case 0x09: return L"Hub";
        case 0x0A: return L"CDC-Data";
        case 0x0B: return L"Smart Card";
        case 0x0D: return L"Content Security";
        case 0x0E: return L"Video";
        case 0x0F: return L"Personal Healthcare";
        case 0x10: return L"Audio/Video";
        case 0xDC: return L"Diagnostic";
        case 0xE0: return L"Wireless (Bluetooth)";
        case 0xEF: return L"Misc (MTP/ADB)";
        case 0xFE: return L"Application Specific";
        case 0xFF: return L"Vendor Specific (ADB)";
        default:   return L"класс 0x??";
    }
}

// Типичные VID телефонов/плееров: если телефон подключён в режиме «только
// зарядка», ни одного интерфейса данных нет, и отличить его можно по VID.
bool IsPhoneVendor(const std::wstring& vid) {
    static const wchar_t* kVendors[] = {
        L"04E8",   // Samsung
        L"18D1",   // Google / Nexus / Pixel (в режиме ADB)
        L"2717",   // Xiaomi
        L"22B8",   // Motorola
        L"0FCE",   // Sony
        L"12D1",   // Huawei
        L"2A70",   // OnePlus
        L"0BB4",   // HTC
        L"1D45",   // Oppo / Realme
        L"2D95",   // Vivo
        L"1004",   // LG
        L"22D9",   // OPPO / OnePlus
        L"2916",   // Yandex Phone
        L"2E04",   // YotaPhone
        L"05AC",   // Apple (в Windows — только зарядка/фото)
        L"054C",   // Sony (плееры)
        L"0930",   // Toshiba
        L"1B8E",   // Nokia
        L"0421",   // Nokia
        L"0E8D",   // MediaTek (инженерный режим)
    };
    for (const wchar_t* vendor : kVendors) {
        if (_wcsicmp(vid.c_str(), vendor) == 0) return true;
    }
    return false;
}

// Запрос к драйверу хаба: скорость, дескриптор устройства, статус порта.
bool QueryPortInfo(const std::wstring& hub_path, ULONG port, UsbDisk& disk) {
    Handle hub = OpenDevice(hub_path);
    if (!hub) return false;

    // Буфер с запасом: ответ содержит дескриптор и список открытых pipe.
    std::vector<BYTE> buffer(sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + 32 * sizeof(USB_PIPE_INFO));
    auto* info = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(buffer.data());
    info->ConnectionIndex = port;

    DWORD returned = 0;
    if (!DeviceIoControl(hub.get(), IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, info,
                         (DWORD)buffer.size(), info, (DWORD)buffer.size(), &returned, nullptr)) {
        return false;
    }
    if (info->ConnectionStatus != DeviceConnected) return false;

    disk.speed = SpeedText(info->Speed);
    const unsigned short bcd_usb = info->DeviceDescriptor.bcdUSB;
    disk.usb_version = Format(L"USB %u.%u", (bcd_usb >> 8) & 0xFF, (bcd_usb >> 4) & 0xF);

    // Дескриптор устройства — независимый источник VID/PID (сверяем с реестром).
    disk.vid = Format(L"%04X", info->DeviceDescriptor.idVendor);
    disk.pid = Format(L"%04X", info->DeviceDescriptor.idProduct);
    disk.revision = Format(L"%02X.%02X", (unsigned)(info->DeviceDescriptor.bcdDevice >> 8),
                           (unsigned)(info->DeviceDescriptor.bcdDevice & 0xFF));

    // bDeviceClass == 0 означает «класс задаётся интерфейсами» (IAD/composite).
    const unsigned char device_class = info->DeviceDescriptor.bDeviceClass;
    if (device_class == 0) {
        disk.usb_class = L"определяется интерфейсами (composite)";
    } else {
        const wchar_t* name = L"класс 0x";
        switch (device_class) {
            case 0x01: name = L"Audio"; break;
            case 0x02: name = L"CDC"; break;
            case 0x03: name = L"HID"; break;
            case 0x07: name = L"Printer"; break;
            case 0x08: name = L"Mass Storage"; break;
            case 0x09: name = L"Hub"; break;
            case 0x0B: name = L"Smart Card"; break;
            case 0x0E: name = L"Video"; break;
            case 0xE0: name = L"Wireless"; break;
            default: break;
        }
        disk.usb_class = Format(L"%s (0x%02X)", name, device_class);
    }

    return true;
}

// Перечисление функций составного USB-устройства (для телефонов это MTP,
// PTP и ADB). Windows создаёт для каждой функции отдельный дочерний узел
// с Instance ID вида USB\VID_18D1&PID_4EE2&MI_00\..., поэтому достаточно
// пройти детей USB-узла и прочитать их описания и Compatible IDs.
void CollectUsbFunctions(DEVINST usb_node, UsbDisk& disk) {
    if (usb_node == 0) return;

    DEVINST child = 0;
    if (CM_Get_Child(&child, usb_node, 0) != CR_SUCCESS) return;

    for (DEVINST node = child; node != 0;) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
            const std::wstring desc = NodeString(node, DEVPKEY_Device_DeviceDesc);
            const std::wstring name = NodeString(node, DEVPKEY_Device_FriendlyName);
            const std::wstring compat = NodeString(node, DEVPKEY_Device_CompatibleIds);
            const std::wstring all = Uppercase(desc + L" " + name + L" " + compat + L" " + id);

            std::wstring label;
            if (all.find(L"MTP") != std::wstring::npos) {
                label = L"MTP (передача файлов)";
            } else if (all.find(L"ADB") != std::wstring::npos) {
                label = L"ADB (отладка по USB)";
            } else if (all.find(L"PTP") != std::wstring::npos ||
                       all.find(L"CLASS_06") != std::wstring::npos) {
                label = L"PTP (камера/фото)";
            } else if (all.find(L"CLASS_08") != std::wstring::npos ||
                       all.find(L"MASS STORAGE") != std::wstring::npos) {
                label = L"Mass Storage (флеш-накопитель)";
            } else if (all.find(L"CLASS_02") != std::wstring::npos ||
                       all.find(L"MODEM") != std::wstring::npos) {
                label = L"Модем (CDC)";
            } else if (all.find(L"CLASS_0E") != std::wstring::npos) {
                label = L"Video (камера)";
            } else if (all.find(L"CLASS_03") != std::wstring::npos) {
                label = L"HID (кнопки/датчики)";
            } else if (!desc.empty()) {
                label = desc;
            } else if (!name.empty()) {
                label = name;
            } else {
                // Нет ни описания, ни известного класса — берём байт класса
                // из Instance ID (USB\VID_...&MI_01&Class_FF&...).
                const size_t cls = all.find(L"CLASS_");
                if (cls != std::wstring::npos && cls + 8 <= all.size()) {
                    const std::wstring hex = all.substr(cls + 6, 2);
                    const unsigned value = (unsigned)wcstoul(hex.c_str(), nullptr, 16);
                    label = Format(L"%s (0x%02X)", InterfaceClassName((unsigned char)value), value);
                }
            }

            if (!label.empty()) disk.interfaces.push_back(label);
        }
        DEVINST next = 0;
        if (CM_Get_Sibling(&next, node, 0) != CR_SUCCESS) break;
        node = next;
    }

    // Составное устройство без MI_xx: функции = классы из дескриптора.
    if (disk.interfaces.empty() && !disk.usb_class.empty()) {
        disk.interfaces.push_back(disk.usb_class);
    }
}

// ===========================================================================
//  6. Тома: какие буквы принадлежат этому накопителю
//
//  «Диск» и «том» в Windows — разные объекты. Правильный способ связать
//  их — сравнить номер физического диска (IOCTL_STORAGE_GET_DEVICE_NUMBER):
//  он одинаков у диска и у каждого созданного на нём тома.
// ===========================================================================

VolumeInfo VolumeOfLetter(wchar_t letter) {
    VolumeInfo info;
    info.letter = letter;

    wchar_t root[8] = {letter, L':', L'\\', 0};
    wchar_t label[MAX_PATH] = {};
    wchar_t fs[MAX_PATH] = {};
    DWORD serial = 0, max_component = 0, flags = 0;
    if (GetVolumeInformationW(root, label, (DWORD)std::size(label), &serial, &max_component, &flags,
                              fs, (DWORD)std::size(fs))) {
        info.label = label;
        info.file_system = fs;
    }

    ULARGE_INTEGER free_bytes{}, total_bytes{}, total_free{};
    if (GetDiskFreeSpaceExW(root, &free_bytes, &total_bytes, &total_free)) {
        info.total = total_bytes.QuadPart;
        info.free = free_bytes.QuadPart;
    }

    // Системный том извлекать нельзя: Windows ответит veto.
    wchar_t windir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windir, (DWORD)std::size(windir)) && windir[0]) {
        if (Uppercase(std::wstring(1, windir[0])) == Uppercase(std::wstring(1, letter))) {
            info.system = true;
        }
    }
    wchar_t system_dir[MAX_PATH] = {};
    if (GetSystemDirectoryW(system_dir, (DWORD)std::size(system_dir)) && system_dir[0]) {
        if (Uppercase(std::wstring(1, system_dir[0])) == Uppercase(std::wstring(1, letter))) {
            info.system = true;
        }
    }
    return info;
}

std::map<DWORD, std::vector<VolumeInfo>> VolumesByDiskNumber() {
    std::map<DWORD, std::vector<VolumeInfo>> result;

    const DWORD mask = GetLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if ((mask & (1u << (letter - L'A'))) == 0) continue;

        wchar_t volume_path[] = {L'\\', L'\\', L'?', L'\\', letter, L':', 0};
        Handle volume(CreateFileW(volume_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr));
        if (!volume) continue;

        STORAGE_DEVICE_NUMBER number{};
        DWORD returned = 0;
        if (!DeviceIoControl(volume.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number,
                             sizeof(number), &returned, nullptr)) {
            continue;
        }
        if (number.DeviceType != FILE_DEVICE_DISK) continue;

        result[number.DeviceNumber].push_back(VolumeOfLetter(letter));
    }
    return result;
}

// ===========================================================================
//  7. Разбор Instance ID: USBSTOR\DISK&VEN_SANDISK&PROD_ULTRA&REV_1.00\4C530...
// ===========================================================================

std::wstring CleanToken(std::wstring token) {
    std::replace(token.begin(), token.end(), L'_', L' ');
    return Trim(token);
}

void ParseUsbStorId(const std::wstring& instance_id, UsbDisk& disk) {
    const std::wstring upper = Uppercase(instance_id);

    // В Instance ID поля разделены знаком «&», поэтому ищем их по префиксу.
    const auto token = [&](const wchar_t* prefix) -> std::wstring {
        const size_t pos = upper.find(prefix);
        if (pos == std::wstring::npos) return {};
        const size_t begin = pos + wcslen(prefix);
        size_t end = upper.find(L'&', begin);
        if (end == std::wstring::npos) end = upper.find(L'\\', begin);
        if (end == std::wstring::npos) end = upper.size();
        return Trim(instance_id.substr(begin, end - begin));
    };

    const std::wstring vendor = token(L"VEN_");
    const std::wstring product = token(L"PROD_");
    const std::wstring revision = token(L"REV_");

    // VEN_/PROD_ — резервный источник: реестр и строковые дескрипторы
    // достовернее, поэтому заполняем только пустые поля.
    if (!vendor.empty() && disk.manufacturer.empty()) disk.manufacturer = CleanToken(vendor);
    if (!product.empty()) disk.model = CleanToken(product);
    if (!revision.empty()) disk.revision = revision;

    // Серийный номер — часть после второго обратного слэша.
    const size_t first = instance_id.find(L'\\');
    if (first != std::wstring::npos) {
        const size_t second = instance_id.find(L'\\', first + 1);
        if (second != std::wstring::npos) {
            const std::wstring tail = Trim(instance_id.substr(second + 1));
            // У части устройств вместо серийника стоит «&0» — это не серийник.
            if (!tail.empty() && tail[0] != L'&') disk.serial = tail;
        }
    }
}

// ===========================================================================
//  8. Основное перечисление USB-накопителей через SetupAPI
// ===========================================================================
ScanResult ScanUsbDisks() {
    ScanResult result;
    const std::map<DWORD, std::vector<VolumeInfo>> volumes = VolumesByDiskNumber();

    // GUID_DEVINTERFACE_DISK + DIGCF_PRESENT — «перечисление через SetupAPI».
    DeviceInfoSet set(SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!set) {
        result.fatal = true;
        result.warnings.push_back(Format(L"SetupDiGetClassDevs: ошибка %lu", GetLastError()));
        return result;
    }

    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set.get(), nullptr, &GUID_DEVINTERFACE_DISK, index,
                                     &interface_data);
         ++index) {
        InterfaceDetail detail = GetInterfaceDetail(set.get(), interface_data);
        if (!detail.valid) continue;

        // Отбираем только USB-накопители: путь USBSTOR-диска содержит «usbstor».
        const std::wstring& path = detail.path;
        if (Uppercase(path).find(L"USBSTOR") == std::wstring::npos) continue;

        UsbDisk disk;
        disk.disk_path = path;

        Handle device = OpenDevice(path);
        if (!device) {
            result.warnings.push_back(Format(L"Не удалось открыть «%s» (ошибка %lu)",
                                             FileName(path).c_str(), GetLastError()));
            continue;
        }

        disk.device_number = (DWORD)-1;
        if (!DiskNumber(device.get(), disk.device_number)) {
            result.warnings.push_back(
                Format(L"IOCTL_STORAGE_GET_DEVICE_NUMBER: ошибка %lu", GetLastError()));
        }

        disk.capacity = DiskCapacity(device.get(), disk.sector_size);
        DiskFlags(device.get(), disk.removable, disk.write_protected);

        // Instance ID и свойства устройства — через SetupAPI.
        SP_DEVINFO_DATA data = detail.data;
        {
            wchar_t id[MAX_PATH] = {};
            if (SetupDiGetDeviceInstanceIdW(set.get(), &data, id, (DWORD)std::size(id), nullptr)) {
                disk.instance_id = id;
            }
            disk.friendly_name = RegistryString(set.get(), data, SPDRP_FRIENDLYNAME);
            disk.device_desc = RegistryString(set.get(), data, SPDRP_DEVICEDESC);
            disk.bus = RegistryString(set.get(), data, SPDRP_ENUMERATOR_NAME);
            const std::wstring registry_manufacturer = RegistryString(set.get(), data, SPDRP_MFG);
            const std::wstring registry_name = RegistryString(set.get(), data, SPDRP_DEVICEDESC);
            if (!registry_name.empty() && disk.model.empty()) disk.model = registry_name;
            if (disk.manufacturer.empty()) disk.manufacturer = registry_manufacturer;
        }
        if (disk.instance_id.empty()) disk.instance_id = path;

        ParseUsbStorId(disk.instance_id, disk);
        if (disk.model.empty()) {
            disk.model = disk.friendly_name.empty() ? disk.device_desc : disk.friendly_name;
        }
        // «USB DISK 3.0 USB Device» -> «USB DISK 3.0».
        {
            const std::wstring tail = L" USB DEVICE";
            const std::wstring upper = Uppercase(disk.model);
            const size_t pos = upper.rfind(tail);
            if (pos != std::wstring::npos && pos + tail.size() == upper.size()) {
                disk.model = Trim(disk.model.substr(0, pos));
            }
        }
        if (disk.serial.empty()) disk.serial = StorageSerial(device.get());
        if (disk.bus.empty()) disk.bus = L"USB";

        // Узел в дереве устройств нужен и для свойств, и для IOCTL хаба,
        // и позже для извлечения — ищем его один раз.
        DEVINST node = 0;
        const bool have_node =
            CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(disk.instance_id.c_str()),
                               CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;

        // Дескриптор устройства через драйвер родительского хаба: точные
        // VID/PID/bcdUSB/класс и скорость соединения.
        if (have_node) {
            ULONG port = 0;
            const std::wstring hub_path = ParentHubPath(node, port);
            if (!hub_path.empty() && port != 0 && QueryPortInfo(hub_path, port, disk)) {
                // VID/PID из дескриптора USB достовернее, чем из реестра.
                disk.usb_id_source = L"дескриптор USB (IOCTL хаба)";
            }
        }
        if (disk.usb_class.empty()) disk.usb_class = L"Mass Storage (0x08)";
        if (disk.usb_id_source.empty()) {
            disk.usb_id_source = L"Instance ID (реестр)";
        }

        // Производитель: строковый дескриптор USB (DEVPKEY_*) — самый точный
        // источник, если реестр пуст.
        if (have_node) {
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, node, 0) == CR_SUCCESS) {
                const std::wstring usb_vendor = NodeString(parent, DEVPKEY_Device_Manufacturer);
                const std::wstring usb_name = NodeString(parent, DEVPKEY_Device_FriendlyName);
                if (disk.manufacturer.empty() && !usb_vendor.empty()) {
                    disk.manufacturer = usb_vendor;
                }
                if (disk.friendly_name.empty() && !usb_name.empty()) {
                    disk.friendly_name = usb_name;
                }
            }
        }

        // Функции устройства (для накопителя это просто Mass Storage).
        CollectUsbFunctions(have_node ? NodeParent(node) : 0, disk);
        if (have_node) disk.usb_node_id = NodeId(node);

        disk.kind = UsbDisk::Kind::MassStorage;
        disk.device_kind = L"USB-накопитель";

        // Тома этого накопителя — по номеру физического диска. Если номер
        // прочитать не удалось (остался -1), тома не приписываем: иначе все
        // тома системы «прилипли» бы к этому устройству.
        if (disk.device_number != (DWORD)-1) {
            auto it = volumes.find(disk.device_number);
            if (it != volumes.end()) {
                disk.volumes = it->second;
                for (const VolumeInfo& volume : disk.volumes) {
                    if (volume.system) disk.system = true;
                }
            }
        }

        result.disks.push_back(std::move(disk));
    }

    // Стабильная сортировка: сначала накопители с томами, затем по модели.
    std::sort(result.disks.begin(), result.disks.end(), [](const UsbDisk& a, const UsbDisk& b) {
        if (a.volumes.empty() != b.volumes.empty()) return !b.volumes.empty();
        return Uppercase(a.model) < Uppercase(b.model);
    });
    return result;
}

// Полный список USB-устройств (для отчёта): показывает, что на шине есть
// не только накопители — хабы, мыши, камеры и т.д.
std::vector<std::wstring> EnumerateUsbDevicesReport() {
    std::vector<std::wstring> lines;

    DeviceInfoSet set(SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!set) return lines;

    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set.get(), nullptr, &GUID_DEVINTERFACE_USB_DEVICE, index,
                                     &interface_data);
         ++index) {
        InterfaceDetail detail = GetInterfaceDetail(set.get(), interface_data);
        if (!detail.valid) continue;

        wchar_t id[MAX_PATH] = {};
        if (!SetupDiGetDeviceInstanceIdW(set.get(), &detail.data, id, (DWORD)std::size(id),
                                         nullptr)) {
            continue;
        }

        // VID/PID лежат прямо в Instance ID: USB\VID_0781&PID_5567\...
        const std::wstring instance(id);
        const std::wstring upper = Uppercase(instance);
        const size_t vid = upper.find(L"VID_");
        const size_t pid = upper.find(L"PID_");
        std::wstring id_text = L"—";
        if (vid != std::wstring::npos && vid + 8 <= instance.size() &&
            pid != std::wstring::npos && pid + 8 <= instance.size()) {
            id_text = instance.substr(vid + 4, 4) + L":" + instance.substr(pid + 4, 4);
        }

        const std::wstring desc = RegistryString(set.get(), detail.data, SPDRP_DEVICEDESC);
        lines.push_back(Format(L"    %-12s %s", id_text.c_str(), desc.c_str()));
    }
    if (lines.empty()) lines.push_back(L"    (устройства не найдены)");
    return lines;
}

// ===========================================================================
//  8.1. Телефоны и плееры: MTP/PTP-устройства (у них нет буквы диска)
//
//  Телефон в режиме «Передача файлов» (MTP) — не диск: буквы диска у него
//  не будет никогда, поэтому GUID_DEVINTERFACE_DISK его не видит. Зато
//  Windows создаёт для него узел класса WPD (Windows Portable Devices).
//  Его и перечисляем через SetupAPI: GUID_DEVCLASS_WPD.
// ===========================================================================

// Разбор Instance ID USB-узла: «USB\VID_18D1&PID_4EE2&MI_00\...».
// Возвращает true, если идентификаторы найдены.
bool ParseUsbInstanceId(const std::wstring& id, std::wstring& vid, std::wstring& pid) {
    const std::wstring upper = Uppercase(id);
    const size_t v = upper.find(L"VID_");
    const size_t p = upper.find(L"PID_");
    if (v == std::wstring::npos || p == std::wstring::npos) return false;
    if (v + 8 > id.size() || p + 8 > id.size()) return false;
    vid = id.substr(v + 4, 4);
    pid = id.substr(p + 4, 4);
    return true;
}

// Корневой USB-узел устройства: поднимаемся по родителям, пока Instance ID
// не станет «USB\VID_...» без &MI_ (это и есть физическое устройство).
DEVINST RootUsbNode(DEVINST node, std::wstring& root_id) {
    DEVINST current = node;
    for (int depth = 0; current != 0 && depth < 8; ++depth) {
        const std::wstring id = NodeId(current);
        const std::wstring upper = Uppercase(id);
        if (upper.rfind(L"USB\\", 0) == 0 && upper.find(L"&MI_") == std::wstring::npos) {
            root_id = id;
            return current;
        }
        DEVINST parent = NodeParent(current);
        if (parent == 0) break;
        current = parent;
    }
    return 0;
}

// Перечисление MTP/PTP-устройств (телефоны, плееры, фотоаппараты).
std::vector<UsbDisk> ScanPortableDevices(ScanResult& result) {
    std::vector<UsbDisk> devices;

    DeviceInfoSet set(SetupDiGetClassDevsW(&GUID_DEVCLASS_WPD, nullptr, nullptr, DIGCF_PRESENT));
    if (!set) {
        result.warnings.push_back(
            Format(L"SetupDiGetClassDevs(WPD): ошибка %lu", GetLastError()));
        return devices;
    }

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set.get(), index, &data); ++index) {
        const std::wstring instance_id = [&] {
            wchar_t id[MAX_DEVICE_ID_LEN] = {};
            if (!SetupDiGetDeviceInstanceIdW(set.get(), &data, id, (DWORD)std::size(id), nullptr)) {
                return std::wstring();
            }
            return std::wstring(id);
        }();
        if (instance_id.empty()) continue;

        UsbDisk disk;
        disk.kind = UsbDisk::Kind::Portable;
        disk.instance_id = instance_id;
        disk.friendly_name = RegistryString(set.get(), data, SPDRP_FRIENDLYNAME);
        disk.device_desc = RegistryString(set.get(), data, SPDRP_DEVICEDESC);
        disk.manufacturer = RegistryString(set.get(), data, SPDRP_MFG);
        disk.bus = RegistryString(set.get(), data, SPDRP_ENUMERATOR_NAME);
        disk.model = disk.friendly_name.empty() ? disk.device_desc : disk.friendly_name;

        DEVINST node = 0;
        if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance_id.c_str()),
                               CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            continue;
        }

        std::wstring root_id;
        const DEVINST root = RootUsbNode(node, root_id);
        if (root == 0) continue;   // это не USB-устройство (например, веб-камера WPD)

        disk.usb_node_id = root_id;
        ParseUsbInstanceId(root_id, disk.vid, disk.pid);
        if (disk.usb_id_source.empty()) disk.usb_id_source = L"Instance ID USB-узла (WPD)";

        // Описание и функции: у телефона их несколько (MTP + ADB + зарядка).
        CollectUsbFunctions(root, disk);
        for (const std::wstring& function : disk.interfaces) {
            if (function.find(L"MTP") != std::wstring::npos) {
                disk.device_kind = L"Телефон / плеер (MTP)";
            } else if (function.find(L"PTP") != std::wstring::npos && disk.device_kind.empty()) {
                disk.device_kind = L"Фотоаппарат (PTP)";
            } else if (function.find(L"ADB") != std::wstring::npos && disk.device_kind.empty()) {
                disk.device_kind = L"Телефон (ADB, без MTP)";
            }
        }
        if (disk.device_kind.empty()) {
            disk.device_kind = IsPhoneVendor(disk.vid)
                                   ? L"Телефон (MTP/PTP, режим без диска)"
                                   : L"Портативное устройство (MTP/PTP)";
        }

        // Скорость и VID/PID из дескриптора USB через драйвер хаба.
        ULONG port = 0;
        const std::wstring hub_path = ParentHubPath(root, port);
        if (!hub_path.empty() && port != 0) {
            QueryPortInfo(hub_path, port, disk);
            disk.usb_id_source = L"дескриптор USB (IOCTL хаба)";
        }
        if (disk.manufacturer.empty()) {
            disk.manufacturer = NodeString(root, DEVPKEY_Device_Manufacturer);
        }

        // У MTP-устройства нет ни диска, ни тома: объём недоступен без WPD API.
        // Если у телефона нет ни MTP, ни PTP, ни ADB — он подключён «только
        // зарядка»: ни списка файлов, ни буквы диска в этом режиме не будет.
        if (disk.interfaces.empty() && IsPhoneVendor(disk.vid)) {
            disk.interfaces.push_back(L"режим «только зарядка» (данных нет)");
        }

        disk.capacity = 0;
        disk.removable = true;
        devices.push_back(std::move(disk));
    }

    // Одно физическое устройство может быть представлено несколькими узлами WPD
    // (например, «Телефон» и «Камера») — оставляем по одному на USB-узел.
    std::vector<UsbDisk> unique;
    for (UsbDisk& device : devices) {
        const bool duplicate = std::any_of(
            unique.begin(), unique.end(),
            [&](const UsbDisk& other) { return other.usb_node_id == device.usb_node_id; });
        if (!duplicate) unique.push_back(std::move(device));
    }
    return unique;
}

// Полное перечисление: USB-накопители + телефоны/плееры (MTP/PTP).
ScanResult ScanAllDevices() {
    ScanResult result = ScanUsbDisks();
    for (UsbDisk& device : ScanPortableDevices(result)) {
        result.disks.push_back(std::move(device));
    }
    std::sort(result.disks.begin(), result.disks.end(), [](const UsbDisk& a, const UsbDisk& b) {
        // Сначала накопители (у них есть буква диска), затем телефоны.
        if (a.kind != b.kind) return a.kind == UsbDisk::Kind::MassStorage;
        if (a.volumes.empty() != b.volumes.empty()) return !b.volumes.empty();
        return Uppercase(a.model) < Uppercase(b.model);
    });
    return result;
}

// ===========================================================================
//  9. Безопасное извлечение и разбор причины отказа
// ===========================================================================

std::wstring ConfigRetText(CONFIGRET cr) {
    switch (cr) {
        case CR_SUCCESS:              return L"CR_SUCCESS";
        case CR_REMOVE_VETOED:        return L"CR_REMOVE_VETOED (извлечение запрещено)";
        case CR_NO_SUCH_DEVNODE:      return L"CR_NO_SUCH_DEVNODE (устройство исчезло)";
        case CR_INVALID_DEVNODE:      return L"CR_INVALID_DEVNODE";
        case CR_ACCESS_DENIED:        return L"CR_ACCESS_DENIED (нужны права администратора)";
        case CR_NOT_DISABLEABLE:      return L"CR_NOT_DISABLEABLE (устройство не отключаемое)";
        case CR_FAILURE:              return L"CR_FAILURE";
        case CR_OUT_OF_MEMORY:        return L"CR_OUT_OF_MEMORY";
        case CR_INVALID_POINTER:      return L"CR_INVALID_POINTER";
        case CR_INVALID_FLAG:         return L"CR_INVALID_FLAG";
        case CR_CALL_NOT_IMPLEMENTED: return L"CR_CALL_NOT_IMPLEMENTED";
        case CR_NO_SUCH_VALUE:        return L"CR_NO_SUCH_VALUE";
        default:                      return Format(L"CONFIGRET 0x%lX", (unsigned long)cr);
    }
}

// Расшифровка PNP_VETO_TYPE и рекомендация — это и есть «п.5 задания».
std::wstring VetoText(PNP_VETO_TYPE veto) {
    switch (veto) {
        case PNP_VetoTypeUnknown:          return L"причина не определена";
        case PNP_VetoLegacyDevice:         return L"устройство без поддержки Plug and Play";
        case PNP_VetoPendingClose:         return L"не завершена операция закрытия файла";
        case PNP_VetoWindowsApp:           return L"устройство занято приложением";
        case PNP_VetoWindowsService:       return L"устройство занято службой Windows";
        case PNP_VetoOutstandingOpen:      return L"есть открытые файлы или каталоги";
        case PNP_VetoDevice:               return L"устройство отклонило запрос";
        case PNP_VetoDriver:               return L"драйвер отклонил запрос";
        case PNP_VetoIllegalDeviceRequest: return L"недопустимый запрос к устройству";
        case PNP_VetoInsufficientPower:    return L"недостаточное питание порта";
        case PNP_VetoNonDisableable:       return L"устройство нельзя отключить";
        case PNP_VetoLegacyDriver:         return L"драйвер не поддерживает извлечение";
        case PNP_VetoInsufficientRights:   return L"недостаточно прав (нужен администратор)";
        case PNP_VetoAlreadyRemoved:       return L"устройство уже извлечено";
        default:                           return L"неизвестный Veto-тип";
    }
}

std::vector<std::wstring> VetoAdvice(PNP_VETO_TYPE veto) {
    switch (veto) {
        case PNP_VetoOutstandingOpen:
            return {L"Закройте окна Проводника, программы и документы, открытые с этого",
                    L"накопителя, затем повторите извлечение."};
        case PNP_VetoWindowsApp:
            return {L"Какое-то приложение держит накопитель. Имя процесса указано ниже —",
                    L"закройте его (или дождитесь завершения копирования) и повторите."};
        case PNP_VetoWindowsService:
            return {L"Накопитель использует служба Windows (индексирование, антивирус,",
                    L"резервное копирование). Дождитесь завершения операции и повторите."};
        case PNP_VetoInsufficientRights:
            return {L"Запустите программу от имени администратора и повторите извлечение."};
        case PNP_VetoNonDisableable:
        case PNP_VetoLegacyDriver:
            return {L"Драйвер устройства не поддерживает безопасное извлечение.",
                    L"Завершите работу с накопителем и отключите его вручную после остановки."};
        case PNP_VetoInsufficientPower:
            return {L"Порту не хватает питания. Подключите накопитель к другому порту",
                    L"(желательно USB 3.x) или через хаб с внешним питанием."};
        case PNP_VetoAlreadyRemoved:
            return {L"Накопитель уже извлечён — обновите список клавишей F5."};
        case PNP_VetoPendingClose:
            return {L"Дождитесь завершения закрытия файлов (1–2 секунды) и повторите."};
        default:
            return {L"Закройте все программы, работающие с накопителем, и повторите.",
                    L"Если отказ остаётся — отключите накопитель в Диспетчере устройств."};
    }
}

// Сбрасываем кэши и размонтируем тома — то же, что делает Проводник перед
// извлечением. Без этого CM_Request_Device_Eject часто возвращает Veto.
void PrepareVolumesForEject(const UsbDisk& disk, std::vector<std::wstring>& notes) {
    for (const VolumeInfo& volume : disk.volumes) {
        wchar_t volume_path[] = {L'\\', L'\\', L'?', L'\\', volume.letter, L':', 0};

        // 1. Сброс кэша файловой системы: гарантирует, что данные на носителе.
        Handle volume_handle(CreateFileW(volume_path, GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                         0, nullptr));
        if (!volume_handle) {
            notes.push_back(Format(L"Том %c: не открылся для сброса кэша (ошибка %lu)",
                                   volume.letter, GetLastError()));
            continue;
        }

        DWORD returned = 0;
        FlushFileBuffers(volume_handle.get());

        // 2. Блокируем и размонтируем том, чтобы никто не писал в процессе.
        if (DeviceIoControl(volume_handle.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                            &returned, nullptr)) {
            DeviceIoControl(volume_handle.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0,
                            &returned, nullptr);
        } else {
            notes.push_back(Format(L"Том %c: занят, блокировка не удалась (ошибка %lu)",
                                   volume.letter, GetLastError()));
        }

        // 3. Разрешаем извлечение носителя драйверу (снимаем «prevent»).
        PREVENT_MEDIA_REMOVAL prevent{};
        prevent.PreventMediaRemoval = FALSE;
        DeviceIoControl(volume_handle.get(), IOCTL_STORAGE_MEDIA_REMOVAL, &prevent, sizeof(prevent),
                        nullptr, 0, &returned, nullptr);
    }
}

// Узел, который нужно извлекать.
//
// Тонкость: для USB-накопителя корректно извлекать узел физического
// USB-устройства (USB\VID_xxxx&PID_xxxx\...), а не дочерний узел диска
// USBSTOR\DISK&VEN_...: именно он снимает питание с порта, как это делает
// Проводник. Если родитель не найден (например, диск подключён через
// сторонний драйвер) — извлекаем сам узел USBSTOR.
DEVINST EjectTargetNode(const UsbDisk& disk, std::wstring& target_id) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(disk.instance_id.c_str()),
                           CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return 0;
    }

    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, node, 0) == CR_SUCCESS) {
        wchar_t parent_id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(parent, parent_id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS &&
            Uppercase(parent_id).find(L"VID_") != std::wstring::npos) {
            target_id = parent_id;
            return parent;
        }
    }
    target_id = disk.instance_id;
    return node;
}

// --- 9.1. Проверка «устройство исчезло» -------------------------------------
// После успешного извлечения узел может ещё существовать несколько сотен
// миллисекунд, а может исчезнуть сразу. Проверяем именно Instance ID.
bool DeviceStillPresent(const std::wstring& instance_id) {
    DEVINST node = 0;
    return CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance_id.c_str()),
                              CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

// Ждём исчезновения узла: 6 попыток по 120 мс — обычно хватает 1–2.
bool WaitDeviceGone(const std::wstring& instance_id, int attempts = 6, DWORD delay_ms = 120) {
    for (int i = 0; i < attempts; ++i) {
        if (!DeviceStillPresent(instance_id)) return true;
        Sleep(delay_ms);
    }
    return !DeviceStillPresent(instance_id);
}

// --- 9.2. Отключение узла через SetupAPI (fallback) -------------------------
//
// CM_Request_Device_Eject может вернуть CR_REMOVE_VETOED или CR_FAILURE из-за
// фильтр-драйверов (антивирус, «умное» ПО накопителя). В этом случае остаётся
// штатный путь Диспетчера устройств: DIF_PROPERTYCHANGE + DICS_DISABLE —
// это тоже программное отключение устройства, только без снятия питания.
// Такой приём требует прав администратора, поэтому программа сначала
// проверяет, что процесс повышен.
bool DisableDeviceNode(const std::wstring& instance_id, DWORD& last_error) {
    DeviceInfoSet set(SetupDiCreateDeviceInfoList(nullptr, nullptr));
    if (!set) {
        last_error = GetLastError();
        return false;
    }

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    if (!SetupDiOpenDeviceInfoW(set.get(), instance_id.c_str(), nullptr, 0, &data)) {
        last_error = GetLastError();
        return false;
    }

    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = DICS_DISABLE;
    params.Scope = DICS_FLAG_GLOBAL;      // отключить для всей системы
    params.HwProfile = 0;

    if (!SetupDiSetClassInstallParamsW(set.get(), &data,
                                       &params.ClassInstallHeader, sizeof(params))) {
        last_error = GetLastError();
        return false;
    }
    if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set.get(), &data)) {
        last_error = GetLastError();
        return false;
    }
    last_error = ERROR_SUCCESS;
    return true;
}

// --- 9.3. Диагностика причины отказа ----------------------------------------
// Когда CM_Request_Device_Eject вернул CR_REMOVE_VETOED с «неизвестной»
// причиной, полезно проверить типичных «держателей»: тома на накопителе,
// открытые файлы, признак загрузочного диска. Это то, что просит п.5 задания.
void DiagnoseVolumes(const UsbDisk& disk, std::vector<std::wstring>& advice) {
    for (const VolumeInfo& volume : disk.volumes) {
        wchar_t root[8] = {volume.letter, L':', L'\\', 0};
        if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, nullptr, 0)) {
            continue;
        }

        // Пробуем открыть том «как для извлечения»: если не открывается —
        // его держит другой процесс, и это и есть причина veto.
        wchar_t path[] = {L'\\', L'\\', L'?', L'\\', volume.letter, L':', 0};
        Handle handle(CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                  nullptr));
        if (!handle) {
            advice.push_back(Format(L"Том %c: открыть для извлечения не удалось (ошибка %lu).",
                                    volume.letter, GetLastError()));
            continue;
        }

        DWORD returned = 0;
        if (!DeviceIoControl(handle.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &returned,
                             nullptr)) {
            advice.push_back(Format(L"Том %c: блокировка невозможна (ошибка %lu) —",
                                    volume.letter, GetLastError()));
            advice.push_back(L"  на нём есть открытые файлы или запущенные программы.");
        } else {
            // Блокировка сработала: отпускаем её, чтобы не мешать системе.
            DeviceIoControl(handle.get(), FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &returned,
                            nullptr);
        }
    }
}

// --- 9.4. Извлечение: полноценная процедура с диагностикой -------------------
EjectResult EjectDisk(UsbDisk& disk, bool elevated) {
    EjectResult result;

    std::wstring target_id;
    const DEVINST node = EjectTargetNode(disk, target_id);
    if (node == 0) {
        result.cr = CR_NO_SUCH_DEVNODE;
        result.last_error = GetLastError();
        result.message = L"Устройство не найдено — возможно, оно уже отключено";
        result.advice = {L"Обновите список клавишей F5."};
        return result;
    }
    result.target_id = target_id;

    // Системный диск извлекать нельзя: предупреждаем до запроса.
    if (disk.system) {
        result.cr = CR_REMOVE_VETOED;
        result.veto = PNP_VetoNonDisableable;
        result.message = L"Отказ: на накопителе находится системный том";
        result.advice = {L"Windows не позволяет извлечь диск, с которого загружена.",
                         L"Это защита от потери данных — извлечение невозможно."};
        return result;
    }

    // Шаг 1: подготовка томов. Для телефонов (MTP) томов нет — шаг пустой.
    std::vector<std::wstring> notes;
    if (!disk.volumes.empty()) PrepareVolumesForEject(disk, notes);

    // Шаг 2: сам запрос на извлечение (требование задания — CM_Request_Device_Eject).
    //   CONFIGRET CM_Request_Device_EjectW(DEVINST, PNP_VETO_TYPE*, PWSTR, ULONG, ULONG)
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    wchar_t veto_name[MAX_PATH] = {};
    const CONFIGRET cr = CM_Request_Device_EjectW(node, &veto, veto_name,
                                                  (ULONG)std::size(veto_name), 0);
    const DWORD api_error = GetLastError();

    result.cr = cr;
    result.veto = veto;
    result.veto_name = Trim(veto_name);
    result.last_error = api_error;

    if (cr == CR_SUCCESS) {
        // Важно: CR_SUCCESS ещё не значит, что устройство ушло. Проверяем.
        result.came_back = !WaitDeviceGone(disk.instance_id);
        result.ok = true;
        if (result.came_back) {
            result.message = L"Запрос принят, но устройство осталось подключённым";
            result.advice = {
                L"Windows сняла питание с порта, однако узел устройства не исчез.",
                L"Если накопитель всё ещё виден в Проводнике — извлеките его повторно",
                L"или отключите физически: данные уже сброшены на носитель."};
        } else {
            result.message = L"Накопитель извлечён — можно отключать";
            result.advice = {L"Кэши сброшены, тома размонтированы, питание порта снято."};
        }
        disk.eject_note = L"CM_Request_Device_Eject: CR_SUCCESS";
        return result;
    }

    // Шаг 3: отказ. Разбираем причину: код CONFIGRET + Veto-тип + GetLastError.
    result.ok = false;
    result.message = Format(L"Извлечение отклонено: %s", VetoText(veto).c_str());
    result.advice = VetoAdvice(veto);
    result.advice.push_back(Format(L"Код: %s, GetLastError = %lu",
                                   ConfigRetText(cr).c_str(), api_error));
    if (!result.veto_name.empty()) {
        result.advice.push_back(Format(L"Держит устройство: %s", result.veto_name.c_str()));
    }
    for (const std::wstring& note : notes) result.advice.push_back(note);

    // Причина «неизвестна» — проверяем тома самостоятельно.
    if (veto == PNP_VetoTypeUnknown && !disk.volumes.empty()) {
        DiagnoseVolumes(disk, result.advice);
    }

    // Шаг 4: fallback. Если драйвер не умеет извлекаться, но у нас есть права
    // администратора — штатно отключаем узел (как Диспетчер устройств).
    if (elevated) {
        DWORD disable_error = 0;
        if (DisableDeviceNode(disk.instance_id, disable_error) &&
            WaitDeviceGone(disk.instance_id)) {
            result.ok = true;
            result.fallback_used = true;
            result.message = L"Устройство отключено программно (DICS_DISABLE)";
            result.advice.insert(result.advice.begin(),
                                 {L"Драйвер отклонил извлечение, поэтому узел отключён "
                                  L"через SetupAPI — данные сброшены на носитель.",
                                  L"Отключить накопитель физически можно сразу."});
            disk.eject_note = L"CM_Request_Device_Eject отклонён, выполнен DICS_DISABLE";
        } else {
            result.advice.push_back(
                Format(L"Отключение узла тоже не удалось (ошибка %lu).", disable_error));
        }
    } else {
        result.advice.push_back(
            L"Запустите программу от имени администратора: тогда при отказе драйвера "
            L"узел будет отключён программно.");
    }

    if (!result.ok && result.veto == PNP_VetoTypeUnknown && notes.empty() &&
        disk.volumes.empty()) {
        result.advice.push_back(L"У MTP-устройства нет тома: закройте Проводник, "
                                L"«Фотографии» и программы синхронизации.");
    }
    if (disk.kind == UsbDisk::Kind::Portable && result.ok) {
        result.message = L"Телефон отключён от Windows — можно отсоединять";
    }
    return result;
}

// ===========================================================================
//  10. Главное окно: вся логика приложения
// ===========================================================================

// ===========================================================================
//  9.5. Диагностика для отчёта и кнопки «Диагностика»
//
//  Программа может «не видеть» телефон или отказывать в извлечении по
//  причинам, которые не видны из одного вызова API. Этот дамп снимает
//  состояние целиком: все узлы USB с VID/PID/классом/состоянием, все диски
//  (включая не-USB), все тома с номерами PhysicalDrive, все WPD-узлы.
//  Так сразу видно, например, что телефон есть в системе, но подключён
//  «только зарядка» (нет ни MTP, ни ADB), или что флешка видна, но тома у
//  неё нет.
// ===========================================================================

std::wstring NodeProblemText(ULONG problem) {
    switch (problem) {
        case 0:  return L"нет проблем";
        case 1:  return L"CM_PROB_NOT_CONFIGURED (10) — устройство не настроено";
        case 3:  return L"CM_PROB_OUT_OF_MEMORY";
        case 10: return L"CM_PROB_FAILED_START (10)";
        case 12: return L"CM_PROB_NORMAL_CONFLICT";
        case 14: return L"CM_PROB_NEED_RESTART (14) — требуется перезагрузка";
        case 18: return L"CM_PROB_REINSTALL (18) — переустановите драйвер";
        case 19: return L"CM_PROB_REGISTRY";
        case 21: return L"CM_PROB_WILL_BE_REMOVED (21)";
        case 22: return L"CM_PROB_DISABLED (22) — устройство отключено";
        case 24: return L"CM_PROB_DEVICE_NOT_THERE (24) — устройство отсутствует";
        case 28: return L"CM_PROB_FAILED_INSTALL (28) — драйвер не установлен";
        case 31: return L"CM_PROB_FAILED_ADD (31)";
        case 32: return L"CM_PROB_DISABLED_SERVICE";
        case 37: return L"CM_PROB_FAILED_DRIVER_ENTRY (37)";
        case 39: return L"CM_PROB_DRIVER_FAILED_LOAD (39)";
        case 43: return L"CM_PROB_FAILED_POST_START (43) — Windows остановила устройство";
        case 45: return L"CM_PROB_HALTED (45) — устройство не подключено";
        case 48: return L"CM_PROB_DISABLED_BY_POLICY (48)";
        case 52: return L"CM_PROB_UNSIGNED_DRIVER (52)";
        default: return Format(L"код проблемы %lu", problem);
    }
}

// Все узлы класса USB (включая хабы) — с состоянием и признаком «есть диск».
std::vector<std::wstring> DumpUsbTree() {
    std::vector<std::wstring> lines;

    DeviceInfoSet set(SetupDiGetClassDevsW(&GUID_DEVCLASS_USB, nullptr, nullptr, DIGCF_PRESENT));
    if (!set) {
        lines.push_back(Format(L"  SetupDiGetClassDevs(USB): ошибка %lu", GetLastError()));
        return lines;
    }

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    int index = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set.get(), i, &data); ++i, ++index) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (!SetupDiGetDeviceInstanceIdW(set.get(), &data, id, (DWORD)std::size(id), nullptr)) {
            continue;
        }
        const std::wstring desc = RegistryString(set.get(), data, SPDRP_DEVICEDESC);
        const std::wstring friendly = RegistryString(set.get(), data, SPDRP_FRIENDLYNAME);

        ULONG status = 0, problem = 0;
        std::wstring state = L"?";
        if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) == CR_SUCCESS) {
            state = NodeProblemText(problem);
            if (problem == 0) state = L"работает";
        }

        wchar_t vid_text[8] = {}, pid_text[8] = {};
        std::wstring vid, pid;
        if (ParseUsbInstanceId(id, vid, pid)) {
            wcscpy_s(vid_text, vid.c_str());
            wcscpy_s(pid_text, pid.c_str());
        }

        lines.push_back(Format(L"  [%2d] %04s:%04s %-8s %s%s", index, vid_text, pid_text,
                               state.c_str(), desc.c_str(),
                               friendly.empty() || friendly == desc
                                   ? L""
                                   : (L"  («" + friendly + L"»)").c_str()));
        lines.push_back(Format(L"       %s", id));
    }
    if (index == 0) lines.push_back(L"  (узлов класса USB нет)");
    return lines;
}

// Все диски системы (не только USB) и все тома с привязкой к физическим дискам.
std::vector<std::wstring> DumpDisksAndVolumes() {
    std::vector<std::wstring> lines;

    const std::map<DWORD, std::vector<VolumeInfo>> volumes = VolumesByDiskNumber();

    DeviceInfoSet set(SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!set) {
        lines.push_back(Format(L"  SetupDiGetClassDevs(DISK): ошибка %lu", GetLastError()));
        return lines;
    }

    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    int count = 0;
    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(set.get(), nullptr, &GUID_DEVINTERFACE_DISK, i, &interface_data);
         ++i) {
        InterfaceDetail detail = GetInterfaceDetail(set.get(), interface_data);
        if (!detail.valid) continue;

        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        SetupDiGetDeviceInstanceIdW(set.get(), &detail.data, id, (DWORD)std::size(id), nullptr);

        const bool usb = Uppercase(detail.path).find(L"USBSTOR") != std::wstring::npos;

        DWORD number = (DWORD)-1;
        std::wstring size_text = L"?";
        Handle device = OpenDevice(detail.path);
        if (device) {
            DiskNumber(device.get(), number);
            int sector = 0;
            size_text = HumanSize(DiskCapacity(device.get(), sector));
        }

        std::wstring letters;
        auto it = volumes.find(number);
        if (it != volumes.end()) {
            for (const VolumeInfo& volume : it->second) {
                letters += std::wstring(1, volume.letter) + L": ";
            }
        }

        lines.push_back(Format(L"  [%d] %-5s PhysicalDrive%lu  %-9s тома: %s", count,
                               usb ? L"USB" : L"проч.", number, size_text.c_str(),
                               letters.empty() ? L"нет" : letters.c_str()));
        lines.push_back(Format(L"       %s", id));
        ++count;
    }
    if (count == 0) lines.push_back(L"  (дисков нет)");

    return lines;
}

// Все WPD-узлы: телефоны, плееры, фотоаппараты (то, что не является диском).
std::vector<std::wstring> DumpPortableNodes() {
    std::vector<std::wstring> lines;

    DeviceInfoSet set(SetupDiGetClassDevsW(&GUID_DEVCLASS_WPD, nullptr, nullptr, DIGCF_PRESENT));
    if (!set) {
        lines.push_back(Format(L"  SetupDiGetClassDevs(WPD): ошибка %lu", GetLastError()));
        return lines;
    }

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    int count = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set.get(), i, &data); ++i) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (!SetupDiGetDeviceInstanceIdW(set.get(), &data, id, (DWORD)std::size(id), nullptr)) {
            continue;
        }
        const std::wstring desc = RegistryString(set.get(), data, SPDRP_DEVICEDESC);

        DEVINST node = 0;
        std::wstring root_id;
        std::wstring root_text = L"(нет USB-родителя)";
        if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
            if (RootUsbNode(node, root_id) != 0) root_text = root_id;
        }

        lines.push_back(Format(L"  [%d] %s", count, desc.empty() ? L"(без описания)" : desc.c_str()));
        lines.push_back(Format(L"       %s", id));
        lines.push_back(Format(L"       USB: %s", root_text.c_str()));
        ++count;
    }
    if (count == 0) lines.push_back(L"  (WPD-устройств нет)");
    return lines;
}

std::vector<std::wstring> DumpDiagnostics() {
    std::vector<std::wstring> lines;
    lines.push_back(L"[USB-узлы (GUID_DEVCLASS_USB)]");
    for (const std::wstring& line : DumpUsbTree()) lines.push_back(line);
    lines.push_back(L"");
    lines.push_back(L"[Диски и тома системы]");
    for (const std::wstring& line : DumpDisksAndVolumes()) lines.push_back(line);
    lines.push_back(L"");
    lines.push_back(L"[Портативные устройства (WPD / MTP)]");
    for (const std::wstring& line : DumpPortableNodes()) lines.push_back(line);
    return lines;
}

const wchar_t* kClassName = L"Lab5UsbFlashWindow";

class App {
public:
    enum : UINT {
        TIMER_TOAST = 1,          // гашение всплывающих уведомлений
        TIMER_AUTO_REFRESH = 2,   // отложенное обновление по WM_DEVICECHANGE
    };

    App() = default;
    ~App() { Cleanup(); }

    bool Init(HINSTANCE inst, int show) {
        inst_ = inst;
        // Аргументы: --elevated (запуск после повышения прав), --dir <каталог>.
        {
            int count = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
            if (argv) {
                for (int i = 0; i < count; ++i) {
                    const std::wstring arg = argv[i];
                    if (arg == L"--elevated") started_elevated_ = true;
                    if (arg == L"--dir" && i + 1 < count) predefined_dir_ = argv[i + 1];
                }
                LocalFree(argv);
            }
        }
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &App::WndProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
        wc.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
                                     LR_DEFAULTSIZE | LR_SHARED);
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;

        hwnd_ = CreateWindowExW(0, kClassName, L"USB-накопители — ЛР №5 (вариант А1)",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, S(1180), S(880), nullptr, nullptr, inst_, this);
        if (!hwnd_) return false;

        ShowWindow(hwnd_, show);
        UpdateWindow(hwnd_);
        return true;
    }

    int Run() {
        // Если приложение запущено без прав администратора и пользователь
        // не отказался от UAC — предлагаем повысить права сразу, один раз.
        if (!elevated_ && !started_elevated_) {
            const int answer = MessageBoxW(
                hwnd_,
                L"Для программного извлечения устройства нужны права администратора.\n\n"
                L"Перезапустить программу с повышенными правами?\n"
                L"(Если выбрать «Нет», извлечение через CM_Request_Device_Eject всё равно\n"
                L"попробует выполниться, а причина отказа будет показана в журнале.)",
                L"ЛР №5 — права администратора", MB_YESNO | MB_ICONQUESTION);
            if (answer == IDYES && RelaunchElevated()) {
                DestroyWindow(hwnd_);
                return 0;   // новый экземпляр уже работает с правами
            }
            AddLog(0, L"Работа без прав администратора");
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // Акселераторы работают и когда фокус в списке или поле ввода.
            if (accel_ && TranslateAcceleratorW(hwnd_, accel_, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }

private:
    // Повышение прав «на месте»: ShellExecuteW с runas перезапускает этот же
    // exe с тем же каталогом отчётов, поэтому ничего не теряется.
    // Причина: CM_Request_Device_Eject и DICS_DISABLE требуют прав
    // администратора; без них Windows отдаёт CR_ACCESS_DENIED.
    bool RelaunchElevated() {
        wchar_t exe[MAX_PATH] = {};
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;

        std::wstring params = L"--elevated";
        if (!report_dir_.empty()) params = L"--dir \"" + report_dir_ + L"\" --elevated";

        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"runas";
        info.lpFile = exe;
        info.lpParameters = params.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            AddLog(1, Format(L"Повышение прав отменено или недоступно (ошибка %lu)",
                             GetLastError()));
            return false;
        }
        return true;
    }

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
                    // Двойная буферизация: без неё список и карточки мерцают.
                    HDC mem = CreateCompatibleDC(dc);
                    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                    HGDIOBJ old = SelectObject(mem, bmp);
                    view_.Paint(mem, rc, model_);
                    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteObject(bmp);
                    DeleteDC(mem);
                }
                EndPaint(hwnd_, &ps);
                return 0;
            }

            case WM_TIMER:
                if (wp == TIMER_TOAST) {
                    if (!view_.ExpireToasts()) KillTimer(hwnd_, TIMER_TOAST);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else if (wp == TIMER_AUTO_REFRESH) {
                    KillTimer(hwnd_, TIMER_AUTO_REFRESH);
                    auto_refresh_at_ = 0;
                    if (!busy_) Refresh(true);
                }
                return 0;

            case WM_COMMAND:
                OnCommand(LOWORD(wp), HIWORD(wp));
                return 0;

            // Горячее подключение: WM_DEVICECHANGE приходит и с параметром
            // DBT_DEVNODES_CHANGED, и с конкретным GUID интерфейса тома.
            case WM_DEVICECHANGE:
                OnDeviceChange(wp, lp);
                return TRUE;

            case WM_DPICHANGED: {
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

    // -----------------------------------------------------------------------
    //  Создание окна и первое перечисление
    // -----------------------------------------------------------------------
    void OnCreate() {
        view_.Create(hwnd_, inst_);

        report_dir_ = predefined_dir_.empty() ? DefaultReportDir() : predefined_dir_;
        EnsureDir(report_dir_);
        SetWindowTextW(view_.DirectoryEdit(), report_dir_.c_str());

        // Права процесса: извлечение и часть IOCTL требуют администратора.
        elevated_ = IsElevated();

        // Горячие клавиши: F5 — обновить, Ctrl+E — извлечь, Ctrl+S — отчёт.
        ACCEL accel[] = {
            {FVIRTKEY, VK_F5, ID_REFRESH},
            {FVIRTKEY | FCONTROL, 'E', ID_EJECT},
            {FVIRTKEY | FCONTROL, 'S', ID_REPORT},
            {FVIRTKEY | FCONTROL, 'D', ID_DIAGNOSTICS},
        };
        accel_ = CreateAcceleratorTableW(accel, (int)std::size(accel));

        // Уведомления о подключении/отключении томов.
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = GUID_DEVINTERFACE_VOLUME;
        notify_volume_ = RegisterDeviceNotificationW(hwnd_, &filter,
                                                     DEVICE_NOTIFY_WINDOW_HANDLE);

        // Телефоны (MTP) приходят как устройства класса WPD — подписываемся
        // и на них, иначе список обновится только по DBT_DEVNODES_CHANGED.
        DEV_BROADCAST_DEVICEINTERFACE_W wpd_filter{};
        wpd_filter.dbcc_size = sizeof(wpd_filter);
        wpd_filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        wpd_filter.dbcc_classguid = GUID_DEVCLASS_WPD;
        notify_wpd_ = RegisterDeviceNotificationW(hwnd_, &wpd_filter,
                                                  DEVICE_NOTIFY_WINDOW_HANDLE);

        DEV_BROADCAST_DEVICEINTERFACE_W usb_filter{};
        usb_filter.dbcc_size = sizeof(usb_filter);
        usb_filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        usb_filter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;
        notify_usb_ = RegisterDeviceNotificationW(hwnd_, &usb_filter,
                                                  DEVICE_NOTIFY_WINDOW_HANDLE);

        AddLog(0, L"Приложение запущено");
        AddLog(0, elevated_ ? L"Права: администратор — извлечение и IOCTL доступны"
                            : L"Права: обычные — для извлечения может понадобиться администратор");
        if (!notify_volume_ && !notify_usb_ && !notify_wpd_) {
            AddLog(1, L"Не удалось подписаться на WM_DEVICECHANGE — список только по F5");
        }
        if (elevated_) {
            AddLog(0, L"Режим администратора: доступен программный fallback извлечения");
        } else {
            AddLog(1, L"Обычные права. Если извлечение отклонено драйвером, включите");
            AddLog(1, L"  «Запуск от имени администратора» — программа отключит узел сама.");
        }

        UpdateModel();
        Refresh(true);
    }

    // Проверка членства в группе администраторов (без запроса UAC).
    static bool IsElevated() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        bool elevated = false;
        if (GetTokenInformation(token, TokenElevation, &elevation, size, &size)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
        return elevated;
    }

    // -----------------------------------------------------------------------
    //  Журнал: окно + файл (чтобы события не терялись при аварийном закрытии)
    // -----------------------------------------------------------------------
    void AddLog(int level, const std::wstring& text) {
        const std::wstring line = NowStamp(true) + L"  " + text;
        log_.push_back(line);
        if (log_.size() > 2000) log_.erase(log_.begin());

        view_.AddLogLine(line, level);

        if (!report_dir_.empty()) {
            HANDLE h = CreateFileW(JoinPath(report_dir_, L"usb_log.txt").c_str(), FILE_APPEND_DATA,
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

    void Notify(const std::wstring& text, int level) {
        view_.PushToast(text, level);
        SetTimer(hwnd_, TIMER_TOAST, 80, nullptr);
    }

    // -----------------------------------------------------------------------
    //  Перечисление: сама работа (вызывается из UI-потока, время небольшое)
    // -----------------------------------------------------------------------
    // quiet — не показывать уведомление и не писать в журнал «завершено»
    // (нужно при автоматическом обновлении по WM_DEVICECHANGE).
    void Refresh(bool quiet) {
        if (busy_) return;   // защита от вложенного вызова из WM_DEVICECHANGE
        busy_ = true;
        scan_failed_ = false;
        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);

        ScanResult scan = ScanAllDevices();
        disks_ = std::move(scan.disks);
        scan_failed_ = scan.fatal;
        for (const std::wstring& warning : scan.warnings) AddLog(1, warning);

        // Сохраняем выбор пользователя по Instance ID, если устройство осталось.
        const std::wstring previous = SelectedInstanceId();

        HWND list = view_.DeviceList();
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        int select = -1;
        for (int i = 0; i < (int)disks_.size(); ++i) {
            const UsbDisk& disk = disks_[i];
            // В список кладём только индекс: всю строку рисует визуал по
            // visual::Model::rows (как и в ЛР №4, где Model — единственный
            // канал «логика -> оформление»).
            const std::wstring line = std::to_wstring(i);
            const int index = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
            if (index >= 0) {
                SendMessageW(list, LB_SETITEMDATA, index, (LPARAM)i);
                if (!previous.empty() && disk.instance_id == previous) select = index;
            }
        }
        if (select < 0 && !disks_.empty()) select = 0;
        SendMessageW(list, LB_SETCURSEL, select, 0);

        busy_ = false;
        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);

        AddLog(0, Format(L"Перечисление завершено: устройств %d", (int)disks_.size()));
        if (!quiet) {
            if (disks_.empty()) {
                Notify(L"USB-накопители не найдены", 1);
            } else {
                Notify(Format(L"Найдено накопителей: %d", (int)disks_.size()), 0);
            }
        }
    }

    std::wstring SelectedInstanceId() const {
        const int index = SelectedIndex();
        if (index < 0 || index >= (int)disks_.size()) return {};
        return disks_[index].instance_id;
    }

    // Индекс выбранного накопителя: берём из LB_GETITEMDATA (устойчиво
    // к сортировке и удалению строк).
    int SelectedIndex() const {
        HWND list = view_.DeviceList();
        const int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
        if (sel < 0) return -1;
        const LRESULT data = SendMessageW(list, LB_GETITEMDATA, sel, 0);
        if (data == LB_ERR || data < 0) return -1;
        return (int)data;
    }

    // -----------------------------------------------------------------------
    //  Модель для визуала
    // -----------------------------------------------------------------------
    void UpdateModel() {
        // Индикатор в шапке описывает состояние поиска, а не конкретное устройство.
        if (busy_) {
            model_.scan_state = ScanBusy;
            model_.status = L"Опрос устройств…";
        } else if (scan_failed_) {
            model_.scan_state = ScanFailed;
            model_.status = L"Ошибка перечисления";
        } else if (disks_.empty()) {
            model_.scan_state = ScanIdle;
            model_.status = L"Устройства не найдены";
        } else {
            model_.scan_state = ScanIdle;
            model_.status = Format(L"Найдено устройств: %d", (int)disks_.size());
        }
        model_.elevated = elevated_;
        model_.rights = elevated_ ? L"Администратор" : L"Обычные права";
        model_.device_count = (int)disks_.size();
        model_.busy = busy_;

        const int index = SelectedIndex();
        model_.has_device = index >= 0 && index < (int)disks_.size();

        model_.rows.clear();
        for (const UsbDisk& disk : disks_) {
            DeviceRow row;
            row.title = disk.model.empty()
                            ? (disk.kind == UsbDisk::Kind::Portable ? L"Телефон (MTP)"
                                                                    : L"USB-накопитель")
                            : disk.model;
            row.subtitle = Format(L"VID %s · PID %s",
                                  disk.vid.empty() ? L"—" : disk.vid.c_str(),
                                  disk.pid.empty() ? L"—" : disk.pid.c_str());
            for (size_t v = 0; v < disk.volumes.size(); ++v) {
                row.subtitle += (v ? L", " : L" · ");
                row.subtitle += std::wstring(1, disk.volumes[v].letter) + L":";
            }
            row.capacity = disk.kind == UsbDisk::Kind::MassStorage ? HumanSize(disk.capacity)
                                                                  : L"";
            row.has_drive = !disk.volumes.empty();
            row.system = disk.system;
            row.kind = disk.kind == UsbDisk::Kind::Portable ? L"Телефон (MTP)" : L"Накопитель";
            row.portable = disk.kind == UsbDisk::Kind::Portable;
            model_.rows.push_back(row);
        }

        model_.fields.clear();
        model_.detail_title.clear();
        model_.detail_sub.clear();
        if (!model_.has_device) {
            view_.SetEnabled(model_);
            return;
        }

        const UsbDisk& disk = disks_[index];
        const bool portable = disk.kind == UsbDisk::Kind::Portable;
        model_.detail_title = disk.model.empty()
                                  ? (portable ? L"Телефон (MTP)" : L"USB-накопитель")
                                  : disk.model;
        model_.detail_sub =
            portable ? Format(L"%s · MTP · %s",
                              disk.manufacturer.empty() ? L"производитель неизвестен"
                                                        : disk.manufacturer.c_str(),
                              disk.speed.empty() ? L"USB" : disk.speed.c_str())
                     : Format(L"%s · %s · %s",
                              disk.manufacturer.empty() ? L"производитель неизвестен"
                                                        : disk.manufacturer.c_str(),
                              HumanSize(disk.capacity).c_str(), disk.speed.c_str());

        const auto field = [this](const wchar_t* label, const std::wstring& value) {
            model_.fields.push_back({label, value.empty() ? L"—" : value});
        };

        field(L"Тип устройства", disk.device_kind);
        field(L"VID", disk.vid);
        field(L"PID", disk.pid);
        field(L"Источник VID/PID", disk.usb_id_source);
        field(L"Серийный номер", disk.serial);
        field(L"Производитель", disk.manufacturer);
        field(L"Модель", disk.model);
        field(L"Версия", disk.revision);
        field(L"Класс USB", disk.usb_class);
        field(L"Шина", disk.bus);
        field(L"Интерфейс", disk.usb_version.empty() ? L"USB" : disk.usb_version);
        field(L"Скорость", disk.speed);

        if (!portable) {
            field(L"Объём", HumanSize(disk.capacity));

            // Буквы дисков и тома — по заданию п.3.
            std::wstring letters;
            for (size_t i = 0; i < disk.volumes.size(); ++i) {
                if (i) letters += L", ";
                letters += std::wstring(1, disk.volumes[i].letter) + L":";
            }
            field(L"Буквы дисков", letters.empty() ? L"нет тома" : letters);

            for (const VolumeInfo& volume : disk.volumes) {
                std::wstring text = volume.file_system;
                if (!volume.label.empty()) text = volume.label + L" · " + text;
                text += Format(L" · свободно %s из %s", HumanSize(volume.free).c_str(),
                               HumanSize(volume.total).c_str());
                field(Format(L"Том %c:", volume.letter).c_str(), text);
            }

            field(L"Съёмный", disk.removable ? L"да" : L"нет");
            field(L"Защита от записи", disk.write_protected ? L"да" : L"нет");
            field(L"Номер диска", disk.device_number == (DWORD)-1
                                      ? L"—"
                                      : Format(L"PhysicalDrive%lu", disk.device_number));
            field(L"Сектор", disk.sector_size ? Format(L"%d байт", disk.sector_size) : L"—");
            field(L"Состояние", disk.system ? L"содержит системный том" : L"готов к извлечению");
        } else {
            // Режим подключения телефона — главное, что нужно объяснить:
            // в MTP-режиме буквы диска не будет никогда.
            std::wstring functions;
            for (size_t i = 0; i < disk.interfaces.size(); ++i) {
                functions += (i ? L", " : L"") + disk.interfaces[i];
            }
            field(L"Функции USB", functions.empty() ? L"—" : functions);
            field(L"Буквы дисков", L"нет: MTP не является диском");
            field(L"Как получить диск",
                  L"на телефоне: «USB → Передача файлов (UMS)», если есть");
            field(L"Состояние", L"MTP-устройство: извлекается как устройство");
            field(L"Узел USB", disk.usb_node_id);
        }

        view_.SetEnabled(model_);
    }

    // -----------------------------------------------------------------------
    //  Безопасное извлечение (по заданию — п.4 и п.5)
    // -----------------------------------------------------------------------
    void EjectSelected() {
        const int index = SelectedIndex();
        if (index < 0 || index >= (int)disks_.size()) {
            Notify(L"Сначала выберите устройство в списке", 1);
            return;
        }
        if (busy_) return;   // идёт перечисление или другое извлечение
        UsbDisk& disk = disks_[index];

        if (disk.system) {
            AddLog(1, L"Извлечение запрещено: накопитель содержит системный том");
            Notify(L"Системный накопитель извлечь нельзя", 1);
            return;
        }

        busy_ = true;
        model_.busy = true;
        model_.status = L"Извлечение…";
        view_.SetEnabled(model_);
        InvalidateRect(hwnd_, nullptr, FALSE);

        AddLog(0, Format(L"Запрос безопасного извлечения: %s (%s)", disk.model.c_str(),
                         disk.device_kind.c_str()));
        AddLog(0, L"  узел: " + disk.instance_id);
        if (!disk.interfaces.empty()) {
            std::wstring list;
            for (size_t i = 0; i < disk.interfaces.size(); ++i) {
                list += (i ? L", " : L"") + disk.interfaces[i];
            }
            AddLog(0, L"  функции: " + list);
        }
        // Извлечение может занимать до секунды (ожидание исчезновения узла),
        // поэтому перерисовываем окно до начала работы и после неё.
        UpdateWindow(hwnd_);

        EjectResult result = EjectDisk(disk, elevated_);

        AddLog(result.ok ? 3 : 2, result.message);
        AddLog(0, Format(L"  CM_Request_Device_Eject: %s", ConfigRetText(result.cr).c_str()));
        AddLog(0, Format(L"  Veto: %s (GetLastError = %lu)", VetoText(result.veto).c_str(),
                         result.last_error));
        for (const std::wstring& advice : result.advice) AddLog(0, L"  " + advice);
        if (!disk.eject_note.empty()) AddLog(0, L"  способ: " + disk.eject_note);

        Notify(result.message, result.ok ? 3 : 2);

        if (result.ok) {
            disk.present = false;
            busy_ = false;
            Refresh(true);   // устройство исчезло — перечитываем список
            return;
        }

        busy_ = false;
        model_.busy = false;
        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);

        // Без прав администратора драйвер почти всегда отвечает отказом.
        // Предлагаем перезапустить программу с повышением прав — сразу,
        // пока пользователь видит причину отказа в журнале.
        if (!elevated_) {
            const std::wstring question =
                L"Извлечение отклонено.\n\n" + result.message +
                L"\n\nЗапустить программу от имени администратора и повторить?";
            if (MessageBoxW(hwnd_, question.c_str(), L"ЛР №5 — нужны права администратора",
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                if (RelaunchElevated()) DestroyWindow(hwnd_);
            }
        }
    }

    // -----------------------------------------------------------------------
    //  Отчёт: полный дамп сведений — удобно приложить к лабораторной
    // -----------------------------------------------------------------------
    void SaveReport() {
        ApplyDirectory();
        const std::wstring path = ReportPath(report_dir_);

        std::wstring text;
        text += L"ЛР №5. Программирование шины USB. Вариант А1 — USB Flash (Mass Storage)\r\n";
        text += L"Платформа: Windows, SetupAPI + CfgMgr32 + IOCTL\r\n";
        text += L"Отчёт сформирован: " + NowStamp(false) + L"\r\n";
        text += L"Права процесса: " + std::wstring(elevated_ ? L"администратор" : L"обычные") + L"\r\n";
        text += std::wstring(78, L'=') + L"\r\n\r\n";

        text += Format(L"Найдено устройств: %d\r\n\r\n", (int)disks_.size());
        for (size_t i = 0; i < disks_.size(); ++i) {
            const UsbDisk& disk = disks_[i];
            const bool portable = disk.kind == UsbDisk::Kind::Portable;
            text += Format(L"--- Устройство %d ---\r\n", (int)i + 1);
            text += L"Тип устройства:    " + disk.device_kind + L"\r\n";
            if (!disk.interfaces.empty()) {
                std::wstring functions;
                for (size_t f = 0; f < disk.interfaces.size(); ++f) {
                    functions += (f ? L", " : L"") + disk.interfaces[f];
                }
                text += L"Функции USB:       " + functions + L"\r\n";
            }
            if (portable) {
                text += L"Буквы дисков:      нет — MTP не является диском\r\n";
                text += L"Узел USB:          " + disk.usb_node_id + L"\r\n";
            }
            text += L"Модель:            " + disk.model + L"\r\n";
            text += L"Производитель:     " + disk.manufacturer + L"\r\n";
            text += L"VID / PID:         " + disk.vid + L" / " + disk.pid + L"\r\n";
            text += L"Источник VID/PID:  " + disk.usb_id_source + L"\r\n";
            text += L"Серийный номер:    " + disk.serial + L"\r\n";
            text += L"Версия прошивки:   " + disk.revision + L"\r\n";
            text += L"Версия USB:        " + disk.usb_version + L"\r\n";
            text += L"Класс USB:         " + disk.usb_class + L"\r\n";
            text += L"Шина:              " + disk.bus + L"\r\n";
            text += L"Скорость:          " + disk.speed + L"\r\n";
            if (!portable) {
                text += L"Объём:             " + HumanSize(disk.capacity) + L" (" +
                        ExactBytes(disk.capacity) + L")\r\n";
            }
            text += L"Сектор:            " +
                    (disk.sector_size ? Format(L"%d байт\r\n", disk.sector_size) : L"—\r\n");
            text += L"Съёмный:           " + std::wstring(disk.removable ? L"да" : L"нет") + L"\r\n";
            text += L"Защита от записи:  " +
                    std::wstring(disk.write_protected ? L"да" : L"нет") + L"\r\n";
            text += Format(L"PhysicalDrive:     %lu\r\n", disk.device_number);
            text += L"Instance ID:       " + disk.instance_id + L"\r\n";
            text += L"Device Path:       " + disk.disk_path + L"\r\n";
            if (!disk.eject_note.empty()) text += L"Извлечение:        " + disk.eject_note + L"\r\n";

            if (portable) {
                text += L"Тома:              недоступны: MTP-устройство не имеет тома\r\n";
            } else if (disk.volumes.empty()) {
                text += L"Тома:              нет (носитель без файловой системы)\r\n";
            }
            for (const VolumeInfo& volume : disk.volumes) {
                text += Format(L"Том %c:            %s, %s, %s из %s%s\r\n", volume.letter,
                               volume.label.empty() ? L"(без метки)" : volume.label.c_str(),
                               volume.file_system.empty() ? L"ФС неизвестна"
                                                          : volume.file_system.c_str(),
                               HumanSize(volume.free).c_str(), HumanSize(volume.total).c_str(),
                               volume.system ? L", СИСТЕМНЫЙ" : L"");
            }
            text += L"\r\n";
        }

        text += L"Все USB-устройства (GUID_DEVINTERFACE_USB_DEVICE):\r\n";
        for (const std::wstring& line : EnumerateUsbDevicesReport()) text += line + L"\r\n";
        text += L"\r\n";

        text += std::wstring(78, L'-') + L"\r\nДиагностика шины (если устройство не видно):\r\n";
        for (const std::wstring& line : DumpDiagnostics()) text += line + L"\r\n";
        text += L"\r\n";

        text += std::wstring(78, L'=') + L"\r\nЖурнал событий:\r\n";
        for (const std::wstring& line : log_) text += line + L"\r\n";
        if (log_.empty()) text += L"(журнал пуст)\r\n";

        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            AddLog(2, L"Не удалось сохранить отчёт: " + FileName(path));
            Notify(L"Не удалось сохранить отчёт", 2);
            return;
        }
        const std::string utf8 = ToUtf8(text);
        DWORD written = 0;
        static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        WriteFile(h, bom, 3, &written, nullptr);
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);

        AddLog(3, L"Отчёт сохранён: " + FileName(path));
        Notify(L"Отчёт сохранён: " + FileName(path), 3);
    }

    void ApplyDirectory() {
        wchar_t buffer[MAX_PATH * 2] = {};
        GetWindowTextW(view_.DirectoryEdit(), buffer, (int)std::size(buffer));
        std::wstring dir = Trim(buffer);
        if (dir.empty()) dir = DefaultReportDir();

        if (!EnsureDir(dir)) {
            dir = DefaultReportDir();
            EnsureDir(dir);
            SetWindowTextW(view_.DirectoryEdit(), dir.c_str());
            Notify(L"Не удалось создать каталог — выбран по умолчанию", 2);
        }
        report_dir_ = dir;
    }

    void BrowseDirectory() {
        BROWSEINFOW info{};
        info.hwndOwner = hwnd_;
        info.lpszTitle = L"Каталог для отчётов и журнала";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE idl = SHBrowseForFolderW(&info);
        if (!idl) return;
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(idl, path)) {
            SetWindowTextW(view_.DirectoryEdit(), path);
            ApplyDirectory();
            AddLog(0, std::wstring(L"Каталог отчётов: ") + path);
        }
        CoTaskMemFree(idl);
    }

    // Кнопка «Диагностика»: полный снимок состояния шины в журнал и в файл.
    void ShowDiagnostics() {
        AddLog(0, L"=== ДИАГНОСТИКА ===");
        for (const std::wstring& line : DumpDiagnostics()) AddLog(0, line);
        AddLog(0, L"=== КОНЕЦ ДИАГНОСТИКИ ===");
        Notify(L"Диагностика выведена в журнал и usb_log.txt", 0);

        // Отдельный файл удобнее приложить к отчёту по лабораторной.
        ApplyDirectory();
        const std::wstring path = JoinPath(report_dir_, L"usb_diagnostics_" + FileStamp() + L".txt");
        std::wstring text = L"ЛР №5 — диагностика шины USB\r\n" + NowStamp(false) + L"\r\n" +
                            std::wstring(78, L'=') + L"\r\n";
        for (const std::wstring& line : DumpDiagnostics()) text += line + L"\r\n";

        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            AddLog(2, L"Не удалось записать файл диагностики: " + FileName(path));
            return;
        }
        const std::string utf8 = ToUtf8(text);
        DWORD written = 0;
        static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        WriteFile(h, bom, 3, &written, nullptr);
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);
        AddLog(3, L"Файл диагностики: " + FileName(path));
    }

    // Перезапуск с правами администратора по команде из интерфейса.
    void RestartAsAdmin() {
        if (elevated_) {
            Notify(L"Программа уже запущена от имени администратора", 0);
            return;
        }
        if (!RelaunchElevated()) {
            Notify(L"Повышение прав отменено", 1);
            return;
        }
        DestroyWindow(hwnd_);
    }

    void OpenDirectory() {
        ApplyDirectory();
        EnsureDir(report_dir_);
        ShellExecuteW(hwnd_, L"open", report_dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // -----------------------------------------------------------------------
    //  Горячее подключение: WM_DEVICECHANGE
    // -----------------------------------------------------------------------
    void OnDeviceChange(WPARAM event, LPARAM data) {
        if (event != DBT_DEVICEARRIVAL && event != DBT_DEVICEREMOVECOMPLETE &&
            event != DBT_DEVNODES_CHANGED) {
            return;
        }

        const bool arrival = event == DBT_DEVICEARRIVAL;
        std::wstring what;

        auto* header = reinterpret_cast<DEV_BROADCAST_HDR*>(data);
        if (header && header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
            auto* iface = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(header);
            const std::wstring name = Uppercase(iface->dbcc_name);

            // Что именно подключилось: накопитель (USBSTOR/VOLUME) или
            // телефон/плеер (WPD). Для WPD разбираем VID/PID прямо из пути
            // устройства: \\?\usb#vid_18d1&pid_4ee2#...
            if (name.find(L"USBSTOR") != std::wstring::npos ||
                IsEqualGUID(iface->dbcc_classguid, GUID_DEVINTERFACE_VOLUME)) {
                what = L"USB-накопитель";
            } else if (IsEqualGUID(iface->dbcc_classguid, GUID_DEVCLASS_WPD) ||
                       name.find(L"WPD") != std::wstring::npos) {
                what = L"MTP-устройство (телефон/плеер)";
            } else if (IsEqualGUID(iface->dbcc_classguid, GUID_DEVINTERFACE_USB_DEVICE) ||
                       name.find(L"USB#VID_") != std::wstring::npos) {
                what = L"USB-устройство";
            } else {
                return;   // чужие интерфейсы игнорируем
            }
        }

        if (event != DBT_DEVNODES_CHANGED) {
            const std::wstring text = what.empty()
                                          ? (arrival ? L"Подключено устройство"
                                                     : L"Отключено устройство")
                                          : (arrival ? L"Подключено: " : L"Отключено: ") + what;
            AddLog(arrival ? 3 : 0, text + L" (WM_DEVICECHANGE)");
            Notify(text, arrival ? 3 : 0);
        }
        RequestAutoRefresh();
    }

    // Отложенное (склеенное) обновление списка по WM_DEVICECHANGE.
    void RequestAutoRefresh() {
        const ULONGLONG now = GetTickCount64();
        if (auto_refresh_at_ != 0 && now < auto_refresh_at_) return;   // уже запланировано
        auto_refresh_at_ = now + 400;
        SetTimer(hwnd_, TIMER_AUTO_REFRESH, 400, nullptr);
    }

    // -----------------------------------------------------------------------
    //  Команды и завершение
    // -----------------------------------------------------------------------
    void OnCommand(int id, int code) {
        if (id == ID_LIST && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
            UpdateModel();
            InvalidateRect(hwnd_, nullptr, FALSE);
            // Двойной щелчок по строке — сразу извлечь накопитель.
            if (code == LBN_DBLCLK) EjectSelected();
            return;
        }
        // BN_CLICKED — от кнопок, 1 — от акселератора (Ctrl+E / Ctrl+S / F5).
        if (code != BN_CLICKED && code != 1) return;

        switch (id) {
            case ID_REFRESH:  Refresh(false); break;
            case ID_EJECT:    EjectSelected(); break;
            case ID_REPORT:   SaveReport(); break;
            case ID_BROWSE:   BrowseDirectory(); break;
            case ID_OPEN_DIR: OpenDirectory(); break;
            case ID_ELEVATE:     RestartAsAdmin(); break;
            case ID_DIAGNOSTICS: ShowDiagnostics(); break;
            default: break;
        }
    }

    void Cleanup() {
        if (cleaned_) return;
        cleaned_ = true;

        KillTimer(hwnd_, TIMER_TOAST);
        KillTimer(hwnd_, TIMER_AUTO_REFRESH);
        if (accel_) {
            DestroyAcceleratorTable(accel_);
            accel_ = nullptr;
        }
        if (notify_volume_) {
            UnregisterDeviceNotification(notify_volume_);
            notify_volume_ = nullptr;
        }
        if (notify_usb_) {
            UnregisterDeviceNotification(notify_usb_);
            notify_usb_ = nullptr;
        }
        if (notify_wpd_) {
            UnregisterDeviceNotification(notify_wpd_);
            notify_wpd_ = nullptr;
        }
        AddLog(0, L"Приложение закрыто");
        view_.Destroy();
    }

    // --- поля --------------------------------------------------------------
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    bool cleaned_ = false;

    View view_;
    Model model_;

    std::vector<UsbDisk> disks_;
    std::vector<std::wstring> log_;

    std::wstring report_dir_;
    std::wstring predefined_dir_;
    bool elevated_ = false;
    bool started_elevated_ = false;   // пришли как «--elevated»
    bool busy_ = false;
    bool scan_failed_ = false;
    ULONGLONG auto_refresh_at_ = 0;   // время, когда можно обновлять список

    HDEVNOTIFY notify_volume_ = nullptr;
    HDEVNOTIFY notify_usb_ = nullptr;
    HDEVNOTIFY notify_wpd_ = nullptr;
    HACCEL accel_ = nullptr;
};

}  // namespace

// ===========================================================================
//  11. Точка входа
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

    // GDI+ — основа оформления (как в ЛР №4).
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) return 1;

    // COM нужен для диалога выбора каталога (Shell API).
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
