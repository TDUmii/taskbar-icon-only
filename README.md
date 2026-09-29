# Taskbar Icon Only

Tiện ích Windows 11 nhỏ gọn để giữ từng cửa sổ là một icon Taskbar riêng, nhưng ẩn title/label. App dùng chính Taskbar Windows 11, không thay thế Taskbar và không cài Windhawk.

## Kết quả mong muốn

- Hai cửa sổ Chrome xuất hiện thành hai icon Chrome riêng.
- Hai cửa sổ TeamViewer xuất hiện thành hai icon TeamViewer riêng.
- Không hiện tên trang web hoặc title cửa sổ trên Taskbar.
- Không đổi Start Menu, vị trí Taskbar hoặc icon hệ thống.

## Cách hoạt động

Windows nhóm các cửa sổ theo `System.AppUserModel.ID`. App đặt một ID riêng cho từng cửa sổ top-level đủ điều kiện thông qua `SHGetPropertyStoreForWindow`, sau đó đặt chế độ Taskbar sang `Always, hide labels` bằng hai giá trị HKCU `TaskbarGlomLevel` và `MMTaskbarGlomLevel`.

Khi khởi động, app quét một lần các cửa sổ đang mở. Sau đó app dùng `SetWinEventHook` của Windows để nhận sự kiện cửa sổ mới được tạo, được hiển thị hoặc bị đóng. App không chạy timer quét lặp, vì vậy khi không có thay đổi cửa sổ thì gần như không sử dụng CPU.

Tài liệu Windows liên quan:

- [Application User Model IDs](https://learn.microsoft.com/en-us/windows/win32/shell/appids)
- [SHGetPropertyStoreForWindow](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-shgetpropertystoreforwindow)
- [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook)
- [Windows 11 Taskbar combine buttons setting](https://learn.microsoft.com/en-us/windows/apps/develop/settings/settings-windows-11#personalization---taskbar---combine-buttons)

Đây là thay đổi trong phạm vi tài khoản người dùng hiện tại. App không cần quyền Administrator, không inject DLL vào Explorer và không sửa file hệ thống. Khi bấm `Khôi phục` hoặc `Thoát hẳn`, AppUserModelID của từng cửa sổ và hai giá trị Taskbar được trả về trạng thái trước đó.

Nếu app bị kết thúc cưỡng bức, trạng thái khôi phục được lưu trong `%LOCALAPPDATA%\TaskbarIconOnly\state.ini` và AppUserModelID gốc được lưu tạm trên chính cửa sổ để lần chạy sau tự dọn dẹp.

## Build trên Windows

Yêu cầu:

- Windows 11 x64
- CMake 3.20 trở lên
- MinGW-w64 hoặc Clang có bộ Windows SDK tương thích

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build --config Release
```

File build nằm ở `build/TaskbarIconOnly.exe`. Bản Release link tĩnh runtime C++ và chỉ dùng DLL hệ thống Windows, vì vậy không cần cài thêm runtime hoặc chép DLL cạnh app. Thư mục build được ignore và không đưa lên Git.

Để khôi phục trạng thái sau khi app bị đóng cưỡng bức, có thể chạy:

```powershell
TaskbarIconOnly.exe --restore
```

Có thể khởi động thẳng xuống khay hệ thống:

```powershell
TaskbarIconOnly.exe --tray
```

## Ghi chú tương thích

App chỉ tác động tới cửa sổ desktop top-level có thể hiện thành nút trên Taskbar. Các cửa sổ hệ thống như Start, Search, System tray, Shell tray và một số host nội bộ của Windows được bỏ qua. Một số ứng dụng chạy trong sandbox hoặc cửa sổ được bảo vệ có thể không cho phép ghi property; app bỏ qua cửa sổ đó thay vì can thiệp sâu hơn.

Taskbar của Windows vẫn có thể hiển thị overflow khi có quá nhiều cửa sổ. App không thay đổi vị trí Taskbar, kích thước icon hoặc các biểu tượng hệ thống.

## License

MIT. Xem [LICENSE](LICENSE).
