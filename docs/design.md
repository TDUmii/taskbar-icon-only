# Thiết kế kỹ thuật

## Phạm vi thay đổi

1. `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\TaskbarGlomLevel`
2. `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\MMTaskbarGlomLevel`
3. `System.AppUserModel.ID` của các cửa sổ desktop top-level đủ điều kiện.

App không chạm vào Start Menu, pinned layout, Taskbar alignment, StuckRects3, system tray hay file trong thư mục Windows.

## Vì sao dùng AppUserModelID

Đây là cơ chế Windows dành cho việc điều khiển nhóm Taskbar theo từng cửa sổ. Khi mỗi cửa sổ có một ID khác nhau, Explorer xem chúng là các app button khác nhau. Chế độ `TaskbarGlomLevel=0` cung cấp phần `hide labels`, còn các ID riêng cung cấp phần `don't combine`.

## Khôi phục an toàn

- Trước khi áp dụng, app đọc hai giá trị Taskbar hiện tại.
- Bản sao trạng thái được ghi vào LocalAppData để khôi phục sau khi crash.
- Mỗi cửa sổ được lưu AppUserModelID gốc trong một property key riêng của app.
- Khi dừng, app trả AppUserModelID gốc và registry value gốc.
- Khi chạy lại sau một lần bị kill, app phục hồi trạng thái stale trước khi áp dụng lại.
