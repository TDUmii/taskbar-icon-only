#include <windows.h>
#include <dwmapi.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <iostream>
#include <string>

namespace {

const GUID kAppUserModelIdFormat = {
    0x9f4c2855,
    0x9f79,
    0x4b39,
    {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}};
const PROPERTYKEY kAppUserModelIdKey = {kAppUserModelIdFormat, 5};
const GUID kOriginalIdFormat = {
    0x2e8c7b11,
    0xc9c1,
    0x4b5e,
    {0x9a, 0x68, 0x92, 0xa7, 0x41, 0x12, 0x08, 0x6b}};
const PROPERTYKEY kOriginalIdKey = {kOriginalIdFormat, 1};

std::wstring GetString(HWND hwnd, const PROPERTYKEY& key) {
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) ||
        !store) {
        return L"<property-store-error>";
    }

    PROPVARIANT propvar;
    PropVariantInit(&propvar);
    std::wstring result = L"<none>";
    if (SUCCEEDED(store->GetValue(key, &propvar))) {
        PWSTR text = nullptr;
        if (SUCCEEDED(PropVariantToStringAlloc(propvar, &text)) && text) {
            result = text;
            CoTaskMemFree(text);
        }
    }
    PropVariantClear(&propvar);
    store->Release();
    return result;
}
std::wstring ProcessPath(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 processId);
    if (!process) {
        return L"<access-denied>";
    }
    wchar_t buffer[4096] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    const BOOL success = QueryFullProcessImageNameW(process, 0, buffer, &size);
    CloseHandle(process);
    return success ? std::wstring(buffer, size) : L"<unknown>";
}

BOOL CALLBACK Enumerate(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr ||
        (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0 ||
        (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked) {
        return TRUE;
    }

    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0 ||
        _wcsicmp(className, L"Progman") == 0 ||
        _wcsicmp(className, L"WorkerW") == 0) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));

    std::wcout << L"HWND=0x" << std::hex << reinterpret_cast<ULONG_PTR>(hwnd)
               << std::dec << L" PID=" << processId << L" title=\"" << title
               << L"\" path=\"" << ProcessPath(processId) << L"\"\n"
               << L"  AppUserModelID=" << GetString(hwnd, kAppUserModelIdKey)
               << L"\n  OriginalID=" << GetString(hwnd, kOriginalIdKey) << L"\n";
    return TRUE;
}

}  // namespace

int wmain() {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(result)) {
        return 1;
    }
    EnumWindows(Enumerate, 0);
    CoUninitialize();
    return 0;
}
