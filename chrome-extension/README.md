# Virtual Camera Controller (Chrome Extension)

Controls the local virtual camera (Python + v4l2loopback) from Google Meet / Zoom web / Teams.

## Install (developer mode)
1. Buka Chrome → `chrome://extensions`
2. Aktifkan "Developer mode" (kanan atas).
3. Klik "Load unpacked" → pilih folder `chrome-extension`.
4. Pastikan skrip Python jalan: `./run.sh` (HTTP API di http://127.0.0.1:8765).

## Pakai
- Meet/Zoom/Teams mendukung shortcut keyboard: `1`–`4` untuk mode dan `b` untuk Capture BG. Tidak ada panel floating yang menutup UI.
- Popup toolbar extension (jika diklik) berisi mode, Capture BG, Reset BG, dan threshold slider.
- Di Meet/Zoom/Teams: Settings → Camera → pilih "VirtualCam".

## Catatan
- Extension ini hanya mengontrol API lokal; kamera virtual tetap harus jalan (`./run.sh`).
- Browser mungkin blok request ke localhost dari halaman HTTPS → jika tombol tidak bekerja, gunakan popup extension (lebih reliable) atau install allow-insecure-localhost. Untuk Meet (HTTPS) → localhost, Chrome biasanya mengizinkan karena host_permissions sudah dideklarasikan.
## Keyboard shortcuts in Meet/Zoom/Teams
When focus is on the call page (and not inside a text box):

- `1` → Normal
- `2` → Remove
- `3` → Virtual background
- `4` → Blur

These keys are ignored while typing in chat or another editable field.
- `b` → Capture background
