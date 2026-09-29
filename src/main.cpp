#include <windows.h>
#include <dwmapi.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <sstream>
#include <iomanip>
#include <memory>
#include <iterator>

namespace {

constexpr wchar_t kWindowClassName[] = L"TaskbarIconOnly.MainWindow";
constexpr wchar_t kMutexName[] = L"Local\\TaskbarIconOnly.SingleInstance";
constexpr wchar_t kAppName[] = L"Taskbar Icon Only";
constexpr wchar_t kAppIdPrefix[] = L"TaskbarIconOnly.";
constexpr wchar_t kRegistryPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr wchar_t kStateDirectoryName[] = L"TaskbarIconOnly";
constexpr wchar_t kStateFileName[] = L"state.ini";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SHOW_MAIN = WM_APP + 2;
constexpr UINT WM_RESTORE_REQUEST = WM_APP + 3;
constexpr UINT TIMER_SCAN = 1;

constexpr int IDC_STATUS = 1001;
constexpr int IDC_DETAILS = 1002;
constexpr int IDC_APPLY = 1003;
constexpr int IDC_RESTORE = 1004;
constexpr int IDC_EXIT = 1005;
constexpr int IDC_OPEN = 1006;

constexpr UINT_PTR kAppUserModelIdPropertyId = 5;
const GUID kAppUserModelIdFormat = {
    0x9f4c2855,
    0x9f79,
    0x4b39,
    {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}};
const PROPERTYKEY kAppUserModelIdKey = {
    kAppUserModelIdFormat,
    kAppUserModelIdPropertyId};

// Private property used only to recover cleanly after a forced termination.
// It is never used as an Explorer setting and is kept on the window itself.
const GUID kOriginalIdFormat = {
    0x2e8c7b11,
    0xc9c1,
    0x4b5e,
    {0x9a, 0x68, 0x92, 0xa7, 0x41, 0x12, 0x08, 0x6b}};
const PROPERTYKEY kOriginalIdKey = {kOriginalIdFormat, 1};

struct RegistryValueState {
    const wchar_t* name;
    bool existed = false;
    DWORD value = 0;
};

struct WindowState {
    std::wstring originalAppUserModelId;
    bool hadOriginalAppUserModelId = false;
    DWORD processId = 0;
    std::wstring processName;
};

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix) {
    const size_t length = wcslen(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0;
}

std::wstring GetStateDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                           static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return L"";
    }

    std::wstring directory(buffer, length);
    directory += L"\\";
    directory += kStateDirectoryName;
    return directory;
}

std::wstring GetStateFilePath() {
    const std::wstring directory = GetStateDirectory();
    return directory.empty() ? L"" : directory + L"\\" + kStateFileName;
}

void EnsureStateDirectory() {
    const std::wstring directory = GetStateDirectory();
    if (!directory.empty()) {
        CreateDirectoryW(directory.c_str(), nullptr);
    }
}

bool ReadRegistryDword(const wchar_t* name, RegistryValueState* state) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    const LONG result = RegQueryValueExW(
        key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&state->value),
        &size);
    RegCloseKey(key);

    state->name = name;
    state->existed = result == ERROR_SUCCESS && type == REG_DWORD &&
                     size == sizeof(DWORD);
    if (!state->existed) {
        state->value = 0;
    }
    return result == ERROR_SUCCESS;
}

bool WriteRegistryDword(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }

    const LONG result = RegSetValueExW(
        key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool DeleteRegistryValue(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_SET_VALUE,
                      &key) != ERROR_SUCCESS) {
        return false;
    }

    const LONG result = RegDeleteValueW(key, name);
    RegCloseKey(key);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

void RestoreRegistryValue(const RegistryValueState& state) {
    if (state.existed) {
        WriteRegistryDword(state.name, state.value);
    } else {
        DeleteRegistryValue(state.name);
    }
}

void NotifyTaskbarSettingsChanged() {
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            wchar_t className[64] = {};
            GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
            if (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
                _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
                SendMessageTimeoutW(hwnd, WM_SETTINGCHANGE, 0,
                                    reinterpret_cast<LPARAM>(L"TaskbarGlomLevel"),
                                    SMTO_ABORTIFHUNG, 250, nullptr);
            }
            return TRUE;
        },
        0);
}

bool ReadWindowProcessPath(HWND hwnd, DWORD* processId, std::wstring* path) {
    *processId = 0;
    path->clear();
    GetWindowThreadProcessId(hwnd, processId);
    if (*processId == 0) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 *processId);
    if (!process) {
        return false;
    }

    wchar_t buffer[4096] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    const BOOL success = QueryFullProcessImageNameW(process, 0, buffer, &size);
    CloseHandle(process);
    if (!success) {
        return false;
    }

    path->assign(buffer, size);
    return true;
}

bool IsShellWindowClass(HWND hwnd) {
    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const wchar_t* shellClasses[] = {
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Progman",
        L"WorkerW",
        L"SHELLDLL_DefView",
    };
    for (const wchar_t* shellClass : shellClasses) {
        if (_wcsicmp(className, shellClass) == 0) {
            return true;
        }
    }
    return false;
}

bool IsEligibleWindow(HWND hwnd, DWORD currentProcessId, DWORD* processId,
                      std::wstring* processPath) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || hwnd == nullptr ||
        hwnd == GetShellWindow() || IsShellWindowClass(hwnd)) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) != 0 || (exStyle & WS_EX_TOOLWINDOW) != 0 ||
        GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked) {
        return false;
    }

    if (!ReadWindowProcessPath(hwnd, processId, processPath) ||
        *processId == currentProcessId) {
        return false;
    }

    const std::wstring processName = ToLower(BaseName(*processPath));
    const wchar_t* excludedProcesses[] = {
        L"sihost.exe",
        L"searchhost.exe",
        L"startmenuexperiencehost.exe",
        L"shellexperiencehost.exe",
        L"textinputhost.exe",
        L"lockapp.exe",
        L"securityhealthsystray.exe",
        L"applicationframehost.exe",
    };
    for (const wchar_t* excluded : excludedProcesses) {
        if (processName == excluded) {
            return false;
        }
    }

    return true;
}

class WindowPropertyStore {
public:
    explicit WindowPropertyStore(HWND hwnd) {
        const HRESULT result = SHGetPropertyStoreForWindow(
            hwnd, IID_PPV_ARGS(&store_));
        if (FAILED(result)) {
            store_ = nullptr;
        }
    }

    ~WindowPropertyStore() {
        if (store_) {
            store_->Release();
        }
    }

    WindowPropertyStore(const WindowPropertyStore&) = delete;
    WindowPropertyStore& operator=(const WindowPropertyStore&) = delete;

    bool IsValid() const { return store_ != nullptr; }

    bool GetString(const PROPERTYKEY& key, std::wstring* value) const {
        value->clear();
        if (!store_) {
            return false;
        }

        PROPVARIANT propvar;
        PropVariantInit(&propvar);
        const HRESULT result = store_->GetValue(key, &propvar);
        if (SUCCEEDED(result)) {
            PWSTR allocated = nullptr;
            if (SUCCEEDED(PropVariantToStringAlloc(propvar, &allocated)) &&
                allocated) {
                value->assign(allocated);
                CoTaskMemFree(allocated);
            }
        }
        PropVariantClear(&propvar);
        return SUCCEEDED(result);
    }

    bool SetString(const PROPERTYKEY& key, const std::wstring& value) const {
        if (!store_) {
            return false;
        }

        PROPVARIANT propvar;
        PropVariantInit(&propvar);
        HRESULT result = InitPropVariantFromString(value.c_str(), &propvar);
        if (SUCCEEDED(result)) {
            result = store_->SetValue(key, propvar);
        }
        if (SUCCEEDED(result)) {
            result = store_->Commit();
        }
        PropVariantClear(&propvar);
        return SUCCEEDED(result);
    }

    bool Clear(const PROPERTYKEY& key) const {
        if (!store_) {
            return false;
        }

        PROPVARIANT propvar;
        PropVariantInit(&propvar);
        HRESULT result = store_->SetValue(key, propvar);
        if (SUCCEEDED(result)) {
            result = store_->Commit();
        }
        PropVariantClear(&propvar);
        return SUCCEEDED(result);
    }

private:
    IPropertyStore* store_ = nullptr;
};

bool GetWindowPropertyString(HWND hwnd, const PROPERTYKEY& key,
                             std::wstring* value) {
    WindowPropertyStore store(hwnd);
    return store.IsValid() && store.GetString(key, value);
}

bool SetWindowPropertyString(HWND hwnd, const PROPERTYKEY& key,
                             const std::wstring& value) {
    WindowPropertyStore store(hwnd);
    return store.IsValid() && store.SetString(key, value);
}

bool ClearWindowProperty(HWND hwnd, const PROPERTYKEY& key) {
    WindowPropertyStore store(hwnd);
    return store.IsValid() && store.Clear(key);
}

std::wstring MakeWindowAppUserModelId(const std::wstring& processPath,
                                      HWND hwnd) {
    std::wstringstream stream;
    stream << kAppIdPrefix << BaseName(processPath) << L"." << std::hex
           << std::uppercase << reinterpret_cast<ULONG_PTR>(hwnd);
    std::wstring value = stream.str();
    std::replace(value.begin(), value.end(), L' ', L'_');
    return value.substr(0, 127);
}

std::wstring EncodeOriginalId(const std::wstring& value,
                              bool hadOriginalId) {
    return hadOriginalId ? value : L"{empty}";
}

bool DecodeOriginalId(const std::wstring& value, std::wstring* decoded,
                      bool* hadOriginalId) {
    if (value == L"{empty}") {
        decoded->clear();
        *hadOriginalId = false;
        return true;
    }
    *decoded = value;
    *hadOriginalId = true;
    return true;
}

class TaskbarController {
public:
    explicit TaskbarController(HWND owner) : owner_(owner) {}

    bool Start() {
        if (active_) {
            ScanWindows();
            return true;
        }

        RecoverAfterUnexpectedExit();

        ReadRegistryDword(L"TaskbarGlomLevel", &taskbarGlomLevel_);
        ReadRegistryDword(L"MMTaskbarGlomLevel", &multiMonitorGlomLevel_);
        if (!WriteRegistryDword(L"TaskbarGlomLevel", 0) ||
            !WriteRegistryDword(L"MMTaskbarGlomLevel", 0)) {
            return false;
        }

        active_ = true;
        WriteStateFile(true);
        ScanWindows();
        NotifyTaskbarSettingsChanged();
        UpdateStatus();
        return true;
    }

    void Stop() {
        if (!active_) {
            RecoverAfterUnexpectedExit();
            UpdateStatus();
            return;
        }

        active_ = false;
        RestoreWindowProperties();
        RestoreRegistryValue(taskbarGlomLevel_);
        RestoreRegistryValue(multiMonitorGlomLevel_);
        DeleteStateFile();
        NotifyTaskbarSettingsChanged();
        UpdateStatus();
    }

    void Tick() {
        if (active_) {
            ScanWindows();
        }
    }

    bool IsActive() const { return active_; }
    size_t TrackedWindowCount() const { return states_.size(); }

private:
    void UpdateStatus() const;

    void ScanWindows() {
        scanChanged_ = false;

        EnumWindows(
            [](HWND hwnd, LPARAM parameter) -> BOOL {
                auto* context = reinterpret_cast<TaskbarController*>(parameter);
                DWORD processId = 0;
                std::wstring processPath;
                if (IsEligibleWindow(hwnd, GetCurrentProcessId(), &processId,
                                     &processPath)) {
                    context->scanChanged_ =
                        context->EnsureWindowIdentity(hwnd, processId,
                                                      processPath) ||
                        context->scanChanged_;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(this));

        for (auto iterator = states_.begin(); iterator != states_.end();) {
            if (!IsWindow(iterator->first)) {
                iterator = states_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        if (scanChanged_) {
            NotifyTaskbarSettingsChanged();
        }
        UpdateStatus();
    }

    bool EnsureWindowIdentity(HWND hwnd, DWORD processId,
                              const std::wstring& processPath) {
        if (states_.find(hwnd) != states_.end()) {
            return false;
        }

        std::wstring currentId;
        GetWindowPropertyString(hwnd, kAppUserModelIdKey, &currentId);

        if (StartsWith(currentId, kAppIdPrefix)) {
            std::wstring recoveredOriginal;
            if (GetWindowPropertyString(hwnd, kOriginalIdKey,
                                        &recoveredOriginal)) {
                bool hadOriginalId = false;
                DecodeOriginalId(recoveredOriginal, &currentId, &hadOriginalId);
            } else {
                currentId.clear();
            }
            if (currentId.empty()) {
                ClearWindowProperty(hwnd, kAppUserModelIdKey);
            } else {
                SetWindowPropertyString(hwnd, kAppUserModelIdKey, currentId);
            }
            ClearWindowProperty(hwnd, kOriginalIdKey);
        }

        WindowState state;
        state.originalAppUserModelId = currentId;
        state.hadOriginalAppUserModelId = !currentId.empty();
        state.processId = processId;
        state.processName = BaseName(processPath);

        const std::wstring encodedOriginal = EncodeOriginalId(
            state.originalAppUserModelId, state.hadOriginalAppUserModelId);
        const std::wstring newId = MakeWindowAppUserModelId(processPath, hwnd);
        if (!SetWindowPropertyString(hwnd, kOriginalIdKey, encodedOriginal) ||
            !SetWindowPropertyString(hwnd, kAppUserModelIdKey, newId)) {
            ClearWindowProperty(hwnd, kOriginalIdKey);
            return false;
        }

        states_.emplace(hwnd, std::move(state));
        return true;
    }

    void RestoreWindowProperties() {
        for (const auto& [hwnd, state] : states_) {
            if (!IsWindow(hwnd)) {
                continue;
            }

            if (state.hadOriginalAppUserModelId) {
                SetWindowPropertyString(hwnd, kAppUserModelIdKey,
                                        state.originalAppUserModelId);
            } else {
                ClearWindowProperty(hwnd, kAppUserModelIdKey);
            }
            ClearWindowProperty(hwnd, kOriginalIdKey);
        }
        states_.clear();
    }

    void RecoverStaleWindowIdentities() {
        EnumWindows(
            [](HWND hwnd, LPARAM) -> BOOL {
                std::wstring currentId;
                if (!GetWindowPropertyString(hwnd, kAppUserModelIdKey,
                                             &currentId) ||
                    !StartsWith(currentId, kAppIdPrefix)) {
                    return TRUE;
                }

                std::wstring encodedOriginal;
                if (GetWindowPropertyString(hwnd, kOriginalIdKey,
                                            &encodedOriginal)) {
                    std::wstring original;
                    bool hadOriginal = false;
                    DecodeOriginalId(encodedOriginal, &original, &hadOriginal);
                    if (hadOriginal) {
                        SetWindowPropertyString(hwnd, kAppUserModelIdKey,
                                                original);
                    } else {
                        ClearWindowProperty(hwnd, kAppUserModelIdKey);
                    }
                } else {
                    ClearWindowProperty(hwnd, kAppUserModelIdKey);
                }
                ClearWindowProperty(hwnd, kOriginalIdKey);
                return TRUE;
            },
            0);
    }

    void RecoverAfterUnexpectedExit() {
        RecoverStaleWindowIdentities();

        const std::wstring stateFile = GetStateFilePath();
        if (stateFile.empty() || GetPrivateProfileIntW(L"TaskbarIconOnly",
                                                        L"active", 0,
                                                        stateFile.c_str()) != 1) {
            return;
        }

        RegistryValueState taskbar{L"TaskbarGlomLevel"};
        RegistryValueState multiMonitor{L"MMTaskbarGlomLevel"};
        taskbar.existed = GetPrivateProfileIntW(
                              L"TaskbarIconOnly", L"taskbarExisted", 0,
                              stateFile.c_str()) == 1;
        taskbar.value = GetPrivateProfileIntW(L"TaskbarIconOnly",
                                              L"taskbarValue", 0,
                                              stateFile.c_str());
        multiMonitor.existed = GetPrivateProfileIntW(
                                    L"TaskbarIconOnly", L"multiMonitorExisted",
                                    0, stateFile.c_str()) == 1;
        multiMonitor.value = GetPrivateProfileIntW(
            L"TaskbarIconOnly", L"multiMonitorValue", 0, stateFile.c_str());
        RestoreRegistryValue(taskbar);
        RestoreRegistryValue(multiMonitor);
        DeleteStateFile();
        NotifyTaskbarSettingsChanged();
    }

    void WriteStateFile(bool active) const {
        EnsureStateDirectory();
        const std::wstring file = GetStateFilePath();
        if (file.empty()) {
            return;
        }

        WritePrivateProfileStringW(L"TaskbarIconOnly", L"active",
                                   active ? L"1" : L"0", file.c_str());
        if (active) {
            WritePrivateProfileStringW(
                L"TaskbarIconOnly", L"taskbarExisted",
                taskbarGlomLevel_.existed ? L"1" : L"0", file.c_str());
            WritePrivateProfileStringW(
                L"TaskbarIconOnly", L"taskbarValue",
                std::to_wstring(taskbarGlomLevel_.value).c_str(), file.c_str());
            WritePrivateProfileStringW(
                L"TaskbarIconOnly", L"multiMonitorExisted",
                multiMonitorGlomLevel_.existed ? L"1" : L"0", file.c_str());
            WritePrivateProfileStringW(
                L"TaskbarIconOnly", L"multiMonitorValue",
                std::to_wstring(multiMonitorGlomLevel_.value).c_str(),
                file.c_str());
        }
    }

    void DeleteStateFile() const {
        const std::wstring file = GetStateFilePath();
        if (!file.empty()) {
            DeleteFileW(file.c_str());
        }
    }

    HWND owner_ = nullptr;
    bool active_ = false;
    RegistryValueState taskbarGlomLevel_{L"TaskbarGlomLevel"};
    RegistryValueState multiMonitorGlomLevel_{L"MMTaskbarGlomLevel"};
    std::unordered_map<HWND, WindowState> states_;
    bool scanChanged_ = false;
};

HFONT CreateUiFont(int height, int weight = FW_NORMAL) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
}

class Application {
public:
    Application() = default;

    bool Initialize(HINSTANCE instance, bool startHidden) {
        instance_ = instance;
        INITCOMMONCONTROLSEX controls{
            sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);

        WNDCLASSEXW windowClass{
            sizeof(WNDCLASSEXW),
            CS_HREDRAW | CS_VREDRAW,
            &Application::WindowProc,
            0,
            0,
            instance_,
            LoadIconW(nullptr, IDI_APPLICATION),
            LoadCursorW(nullptr, IDC_ARROW),
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1),
            nullptr,
            kWindowClassName,
            LoadIconW(nullptr, IDI_APPLICATION)};
        if (!RegisterClassExW(&windowClass)) {
            return false;
        }

        window_ = CreateWindowExW(
            WS_EX_APPWINDOW, kWindowClassName, kAppName,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 560, 330, nullptr, nullptr, instance_,
            this);
        if (!window_) {
            return false;
        }

        controller_ = std::make_unique<TaskbarController>(window_);
        CreateControls();
        AddTrayIcon();
        SetTimer(window_, TIMER_SCAN, 350, nullptr);
        controller_->Start();
        ShowWindow(window_, startHidden ? SW_HIDE : SW_SHOW);
        UpdateControls();
        return true;
    }

    int Run() {
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
        Application* application =
            reinterpret_cast<Application*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            application = reinterpret_cast<Application*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(application));
        }
        if (!application) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        return application->HandleMessage(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_COMMAND:
        switch (LOWORD(wParam)) {
                    case IDC_APPLY:
                        controller_->Start();
                        UpdateControls();
                        return 0;
                    case IDC_RESTORE:
                        controller_->Stop();
                        UpdateControls();
                        return 0;
                    case IDC_EXIT:
                        Quit();
                        return 0;
                    case IDC_OPEN:
                        ShowMainWindow();
                        return 0;
                    default:
                        break;
                }
                break;
            case WM_TIMER:
                if (wParam == TIMER_SCAN && controller_) {
                    controller_->Tick();
                    UpdateControls();
                }
                return 0;
            case WM_TRAYICON:
                if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                    ShowMainWindow();
                } else if (lParam == WM_RBUTTONUP) {
                    ShowTrayMenu();
                }
                return 0;
            case WM_SHOW_MAIN:
                ShowMainWindow();
                return 0;
            case WM_RESTORE_REQUEST:
                if (controller_) {
                    controller_->Stop();
                }
                UpdateControls();
                if (wParam != 0) {
                    DestroyWindow(hwnd);
                }
                return 0;
            case WM_QUERYENDSESSION:
                if (controller_) {
                    controller_->Stop();
                }
                return TRUE;
            case WM_ENDSESSION:
                if (wParam) {
                    RemoveTrayIcon();
                    PostQuitMessage(0);
                }
                return 0;
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_DESTROY:
                RemoveTrayIcon();
                if (controller_) {
                    controller_->Stop();
                }
                if (font_) {
                    DeleteObject(font_);
                }
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void CreateControls() {
        font_ = CreateUiFont(-16);
        titleFont_ = CreateUiFont(-24, FW_SEMIBOLD);

        title_ = CreateWindowExW(0, L"STATIC", kAppName,
                                 WS_CHILD | WS_VISIBLE, 24, 22, 500, 38,
                                 window_, nullptr, instance_, nullptr);
        status_ = CreateWindowExW(0, L"STATIC", L"Đang khởi động...",
                                  WS_CHILD | WS_VISIBLE, 24, 74, 500, 28,
                                  window_, reinterpret_cast<HMENU>(IDC_STATUS),
                                  instance_, nullptr);
        details_ = CreateWindowExW(
            0, L"STATIC",
            L"Mỗi cửa sổ có một AppUserModelID riêng. Taskbar vẫn là giao diện Windows 11.",
            WS_CHILD | WS_VISIBLE, 24, 112, 500, 52, window_,
            reinterpret_cast<HMENU>(IDC_DETAILS), instance_, nullptr);
        apply_ = CreateWindowExW(0, L"BUTTON", L"Áp dụng lại",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 24, 188,
                                 150, 34, window_,
                                 reinterpret_cast<HMENU>(IDC_APPLY), instance_,
                                 nullptr);
        restore_ = CreateWindowExW(0, L"BUTTON", L"Khôi phục",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 190,
                                    188, 150, 34, window_,
                                    reinterpret_cast<HMENU>(IDC_RESTORE),
                                    instance_, nullptr);
        exit_ = CreateWindowExW(0, L"BUTTON", L"Thoát hẳn",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 356, 188,
                                150, 34, window_,
                                reinterpret_cast<HMENU>(IDC_EXIT), instance_,
                                nullptr);
        note_ = CreateWindowExW(
            0, L"STATIC",
            L"Không đổi Start Menu, vị trí Taskbar hoặc icon hệ thống. Đóng cửa sổ sẽ ẩn app xuống khay hệ thống.",
            WS_CHILD | WS_VISIBLE, 24, 246, 500, 42, window_, nullptr,
            instance_, nullptr);

        HWND controls[] = {title_, status_, details_, apply_, restore_, exit_,
                           note_};
        for (HWND control : controls) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(control == title_ ? titleFont_
                                                                      : font_),
                         TRUE);
        }
    }

    void AddTrayIcon() {
        trayIcon_.cbSize = sizeof(trayIcon_);
        trayIcon_.hWnd = window_;
        trayIcon_.uID = 1;
        trayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        trayIcon_.uCallbackMessage = WM_TRAYICON;
        trayIcon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(trayIcon_.szTip, kAppName);
        Shell_NotifyIconW(NIM_ADD, &trayIcon_);
        trayIconAdded_ = true;
    }

    void RemoveTrayIcon() {
        if (trayIconAdded_) {
            Shell_NotifyIconW(NIM_DELETE, &trayIcon_);
            trayIconAdded_ = false;
        }
    }

    void ShowTrayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDC_APPLY, L"Áp dụng lại");
        AppendMenuW(menu, MF_STRING, IDC_RESTORE, L"Khôi phục");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDC_OPEN, L"Mở cửa sổ");
        AppendMenuW(menu, MF_STRING, IDC_EXIT, L"Thoát hẳn");

        POINT point;
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_,
                       nullptr);
        DestroyMenu(menu);
    }

    void ShowMainWindow() {
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        UpdateControls();
    }

    void UpdateControls() {
        if (!controller_ || !status_) {
            return;
        }

        if (controller_->IsActive()) {
            std::wstring text = L"Đang hoạt động - đã tách " +
                                std::to_wstring(controller_->TrackedWindowCount()) +
                                L" cửa sổ";
            SetWindowTextW(status_, text.c_str());
            EnableWindow(restore_, TRUE);
        } else {
            SetWindowTextW(status_, L"Đã khôi phục cài đặt Taskbar");
            EnableWindow(restore_, FALSE);
        }
    }

    void Quit() {
        if (controller_) {
            controller_->Stop();
        }
        DestroyWindow(window_);
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND title_ = nullptr;
    HWND status_ = nullptr;
    HWND details_ = nullptr;
    HWND apply_ = nullptr;
    HWND restore_ = nullptr;
    HWND exit_ = nullptr;
    HWND note_ = nullptr;
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    NOTIFYICONDATAW trayIcon_{};
    bool trayIconAdded_ = false;
    std::unique_ptr<TaskbarController> controller_;
};

void TaskbarController::UpdateStatus() const {
    // The UI polls the controller after each timer tick. This method is kept as
    // a hook for future status notifications without coupling the core logic to
    // the window controls.
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    const std::wstring arguments = commandLine ? commandLine : L"";
    const bool restoreOnly = arguments.find(L"--restore") != std::wstring::npos;
    const bool startHidden = arguments.find(L"--tray") != std::wstring::npos;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClassName, nullptr);
        if (existing) {
            PostMessageW(existing, restoreOnly ? WM_RESTORE_REQUEST
                                               : WM_SHOW_MAIN,
                         restoreOnly ? 1 : 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        CloseHandle(mutex);
        return 1;
    }

    if (restoreOnly) {
        TaskbarController controller(nullptr);
        controller.Stop();
        CoUninitialize();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 0;
    }

    Application application;
    const bool initialized = application.Initialize(instance, startHidden);
    const int result = initialized ? application.Run() : 1;

    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
}
