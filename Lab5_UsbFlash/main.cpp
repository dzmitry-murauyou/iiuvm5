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
        disk.usb_class = L"определяется интерфейсом (массовое хранилище)";
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

        // Тома этого накопителя — по номеру физического диска.
        auto it = volumes.find(disk.device_number);
        if (it != volumes.end()) {
            disk.volumes = it->second;
            for (const VolumeInfo& volume : disk.volumes) {
                if (volume.system) disk.system = true;
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

// Попытка безопасного извлечения с полным разбором результата.
EjectResult EjectDisk(UsbDisk& disk) {
    EjectResult result;

    std::wstring target_id;
    const DEVINST node = EjectTargetNode(disk, target_id);
    if (node == 0) {
        result.cr = CR_NO_SUCH_DEVNODE;
        result.message = L"Устройство не найдено — возможно, оно уже отключено";
        result.advice = {L"Обновите список клавишей F5."};
        return result;
    }

    // Системный диск извлекать нельзя: предупреждаем до запроса.
    if (disk.system) {
        result.cr = CR_REMOVE_VETOED;
        result.veto = PNP_VetoNonDisableable;
        result.message = L"Отказ: на накопителе находится системный том";
        result.advice = {L"Windows не позволяет извлечь диск, с которого загружена.",
                         L"Это защита от потери данных — извлечение невозможно."};
        return result;
    }

    // Шаг 1: подготовка томов (сброс кэшей, размонтирование).
    std::vector<std::wstring> notes;
    PrepareVolumesForEject(disk, notes);

    // Шаг 2: сам запрос на извлечение.
    //   CONFIGRET CM_Request_Device_EjectW(DEVINST, PNP_VETO_TYPE*, PWSTR, ULONG, ULONG)
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    wchar_t veto_name[MAX_PATH] = {};
    const CONFIGRET cr = CM_Request_Device_EjectW(node, &veto, veto_name,
                                                  (ULONG)std::size(veto_name), 0);

    result.cr = cr;
    result.veto = veto;
    result.veto_name = Trim(veto_name);
    result.target_id = target_id;

    if (cr == CR_SUCCESS) {
        result.ok = true;
        result.message = L"Накопитель извлечён — можно отключать";
        result.advice = {L"Кэши сброшены, тома размонтированы, питание порта снято."};
        return result;
    }

    // Шаг 3: отказ. Разбираем причину: код CONFIGRET + Veto-тип.
    result.ok = false;
    result.message = Format(L"Извлечение отклонено: %s", VetoText(veto).c_str());
    result.advice = VetoAdvice(veto);
    result.advice.push_back(Format(L"Код: %s", ConfigRetText(cr).c_str()));
    if (!result.veto_name.empty()) {
        result.advice.push_back(Format(L"Держит накопитель: %s", result.veto_name.c_str()));
    }
    for (const std::wstring& note : notes) result.advice.push_back(note);

    // Если система не назвала причину — проверяем типичные вручную.
    if (veto == PNP_VetoTypeUnknown && notes.empty()) {
        result.advice.push_back(L"Проверьте: открытые файлы на накопителе, антивирус,");
        result.advice.push_back(L"индексирование, права администратора.");
    }
    return result;
}

// ===========================================================================
//  10. Главное окно: вся логика приложения
// ===========================================================================

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
                                CW_USEDEFAULT, S(1120), S(820), nullptr, nullptr, inst_, this);
        if (!hwnd_) return false;

        ShowWindow(hwnd_, show);
        UpdateWindow(hwnd_);
        return true;
    }

    int Run() {
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

        report_dir_ = DefaultReportDir();
        EnsureDir(report_dir_);
        SetWindowTextW(view_.DirectoryEdit(), report_dir_.c_str());

        // Права процесса: извлечение и часть IOCTL требуют администратора.
        elevated_ = IsElevated();

        // Горячие клавиши: F5 — обновить, Ctrl+E — извлечь, Ctrl+S — отчёт.
        ACCEL accel[] = {
            {FVIRTKEY, VK_F5, ID_REFRESH},
            {FVIRTKEY | FCONTROL, 'E', ID_EJECT},
            {FVIRTKEY | FCONTROL, 'S', ID_REPORT},
        };
        accel_ = CreateAcceleratorTableW(accel, (int)std::size(accel));

        // Уведомления о подключении/отключении томов.
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = GUID_DEVINTERFACE_VOLUME;
        notify_volume_ = RegisterDeviceNotificationW(hwnd_, &filter,
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
        if (!notify_volume_ && !notify_usb_) {
            AddLog(1, L"Не удалось подписаться на WM_DEVICECHANGE — список только по F5");
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

        ScanResult scan = ScanUsbDisks();
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

        AddLog(0, Format(L"Перечисление завершено: накопителей %d", (int)disks_.size()));
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
            model_.status = L"Накопители не найдены";
        } else {
            model_.scan_state = ScanIdle;
            model_.status = Format(L"Накопителей: %d", (int)disks_.size());
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
            row.title = disk.model.empty() ? L"USB-накопитель" : disk.model;
            row.subtitle = Format(L"VID %s · PID %s",
                                  disk.vid.empty() ? L"—" : disk.vid.c_str(),
                                  disk.pid.empty() ? L"—" : disk.pid.c_str());
            for (size_t v = 0; v < disk.volumes.size(); ++v) {
                row.subtitle += (v ? L", " : L" · ");
                row.subtitle += std::wstring(1, disk.volumes[v].letter) + L":";
            }
            row.capacity = HumanSize(disk.capacity);
            row.has_drive = !disk.volumes.empty();
            row.system = disk.system;
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
        model_.detail_title = disk.model.empty() ? L"USB-накопитель" : disk.model;
        model_.detail_sub = Format(L"%s · %s · %s",
                                   disk.manufacturer.empty() ? L"производитель неизвестен"
                                                             : disk.manufacturer.c_str(),
                                   HumanSize(disk.capacity).c_str(), disk.speed.c_str());

        const auto field = [this](const wchar_t* label, const std::wstring& value) {
            model_.fields.push_back({label, value.empty() ? L"—" : value});
        };

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
        field(L"Номер диска", Format(L"PhysicalDrive%lu", disk.device_number));
        field(L"Сектор", disk.sector_size ? Format(L"%d байт", disk.sector_size) : L"—");
        field(L"Состояние", disk.system ? L"содержит системный том" : L"готов к извлечению");

        view_.SetEnabled(model_);
    }

    // -----------------------------------------------------------------------
    //  Безопасное извлечение (по заданию — п.4 и п.5)
    // -----------------------------------------------------------------------
    void EjectSelected() {
        const int index = SelectedIndex();
        if (index < 0 || index >= (int)disks_.size()) {
            Notify(L"Сначала выберите накопитель в списке", 1);
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
        AddLog(0, Format(L"Запрос безопасного извлечения: %s", disk.model.c_str()));
        AddLog(0, L"  узел: " + disk.instance_id);

        EjectResult result = EjectDisk(disk);

        if (result.ok) {
            AddLog(3, result.message);
            for (const std::wstring& advice : result.advice) AddLog(0, L"  " + advice);
            Notify(result.message, 3);
            disk.present = false;
            // Устройство исчезло — обновляем список (обычно приходит и WM_DEVICECHANGE).
            busy_ = false;
            Refresh(true);
            return;
        }

        // Отказ: выводим причину и рекомендации — это требование задания.
        AddLog(2, result.message);
        AddLog(0, Format(L"  CM_Request_Device_Eject: %s", ConfigRetText(result.cr).c_str()));
        AddLog(0, Format(L"  Veto: %s", VetoText(result.veto).c_str()));
        for (const std::wstring& advice : result.advice) AddLog(0, L"  " + advice);
        Notify(result.message, 2);

        busy_ = false;
        model_.busy = false;
        UpdateModel();
        InvalidateRect(hwnd_, nullptr, FALSE);
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

        text += Format(L"Найдено USB-накопителей: %d\r\n\r\n", (int)disks_.size());
        for (size_t i = 0; i < disks_.size(); ++i) {
            const UsbDisk& disk = disks_[i];
            text += Format(L"--- Накопитель %d ---\r\n", (int)i + 1);
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
            text += L"Объём:             " + HumanSize(disk.capacity) + L" (" +
                    ExactBytes(disk.capacity) + L")\r\n";
            text += L"Сектор:            " +
                    (disk.sector_size ? Format(L"%d байт\r\n", disk.sector_size) : L"—\r\n");
            text += L"Съёмный:           " + std::wstring(disk.removable ? L"да" : L"нет") + L"\r\n";
            text += L"Защита от записи:  " +
                    std::wstring(disk.write_protected ? L"да" : L"нет") + L"\r\n";
            text += Format(L"PhysicalDrive:     %lu\r\n", disk.device_number);
            text += L"Instance ID:       " + disk.instance_id + L"\r\n";
            text += L"Device Path:       " + disk.disk_path + L"\r\n";

            if (disk.volumes.empty()) {
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

    void OpenDirectory() {
        ApplyDirectory();
        EnsureDir(report_dir_);
        ShellExecuteW(hwnd_, L"open", report_dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // -----------------------------------------------------------------------
    //  Горячее подключение: WM_DEVICECHANGE
    // -----------------------------------------------------------------------
    void OnDeviceChange(WPARAM event, LPARAM data) {
        switch (event) {
            case DBT_DEVICEARRIVAL:
            case DBT_DEVICEREMOVECOMPLETE: {
                auto* header = reinterpret_cast<DEV_BROADCAST_HDR*>(data);
                if (header && header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                    auto* iface =
                        reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(header);
                    const std::wstring name = Uppercase(iface->dbcc_name);
                    const bool is_volume = name.find(L"USBSTOR") != std::wstring::npos ||
                                           IsEqualGUID(iface->dbcc_classguid,
                                                       GUID_DEVINTERFACE_VOLUME);
                    if (!is_volume) return;
                }
                const bool arrival = event == DBT_DEVICEARRIVAL;
                // Небольшая задержка не нужна: перечисление само по себе
                // занимает миллисекунды и видит уже готовое устройство.
                AddLog(arrival ? 3 : 0,
                       arrival ? L"Подключено USB-устройство (WM_DEVICECHANGE: DBT_DEVICEARRIVAL)"
                               : L"Отключено USB-устройство (WM_DEVICECHANGE: "
                                 L"DBT_DEVICEREMOVECOMPLETE)");
                Refresh(true);
                Notify(arrival ? L"USB-накопитель подключён" : L"USB-накопитель отключён",
                       arrival ? 3 : 0);
                break;
            }
            case DBT_DEVNODES_CHANGED:
                // Узел изменился — перечитываем без уведомления, чтобы не спамить.
                // Вместе с DEVICEARRIVAL/REMOVECOMPLETE это даёт два события
                // подряд, поэтому обновляем не чаще одного раза в 400 мс.
                RequestAutoRefresh();
                break;
            default:
                break;
        }
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
    bool elevated_ = false;
    bool busy_ = false;
    bool scan_failed_ = false;
    ULONGLONG auto_refresh_at_ = 0;   // время, когда можно обновлять список

    HDEVNOTIFY notify_volume_ = nullptr;
    HDEVNOTIFY notify_usb_ = nullptr;
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
