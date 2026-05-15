# ECG-TransferData (ESP32 USB MSC & Data Upload)

Proyek ini mengimplementasikan perangkat **USB Mass Storage Class (MSC)** menggunakan ESP32 (S2/S3). Perangkat ini bertindak sebagai jembatan antara mesin EKG (sebagai USB Host) dan server penyimpanan data melalui Wi-Fi.

---

## Analisis `main/app_main.c`

File `main/app_main.c` adalah inti dari aplikasi ini.

### 1. Fungsi Utama (Core Purpose)
Program ini memungkinkan ESP32 untuk:
- Berperan sebagai Flash Drive USB (MSC) yang dapat dibaca/tulis oleh PC atau perangkat medis (EKG).
- Mengelola penyimpanan data pada **SD Card** atau **Internal SPI Flash**.
- Mengunggah file data EKG (format `.XML`, `.JPG`, `.BMP`) ke server FTP secara otomatis.
- Mendeteksi kondisi idle USB (mesin EKG selesai rekam) dan memicu upload otomatis.

### 2. Komponen Kunci
- **TinyUSB (MSC Storage)**: Mengelola deskriptor USB dan protokol Mass Storage.
- **Wi-Fi & Networking**: Menghubungkan ESP32 ke jaringan Wi-Fi.
- **HTTP Client**: Mengirim file ke API server dengan metode `POST` multipart/form-data.
- **Console REPL**: Antarmuka CLI melalui UART untuk debugging dan kontrol manual.

## Diagram Alur (Idle Detection & Auto-Upload)

Berikut adalah visualisasi apa yang terjadi antara Perawat (User), Mesin EKG, dan Dongle:

```text
[Perawat]                 [Mesin EKG]                     [Dongle ESP32]                 [Server FTP]
    |                          |                                 |                             |
    |-- 1. Simpan Rekam EKG -->|                                 |                             |
    |                          |-- 2. Tulis File ke USB -------->|                             |
    |                          |                                 | (Status: Active / LED OFF)  |
    |                          |                                 |                             |
    |                          |   (Mesin selesai menulis)       |                             |
    |                          |-- 3. USB Idle ----------------->|                             |
    |                          |                                 | (Safeguard 30 Detik mulai)  |
    |                          |                                 | (LED Kedip Lambat)          |
    |                          |                                 |                             |
    |                          |   (Jika idle 30 detik tuntas)   |                             |
    |                          |                                 |-- 4. Ambil alih akses USB   |
    |                          |                                 |-- 5. Upload File (LED Cepat)--> 
    |                          |                                 |                             |
    |                          |                                 |<-- 6. Upload Sukses --------|
    |                          |                                 |-- 7. Hapus File Internal    |
    |                          |                                 |-- 8. Kembalikan akses USB   |
    |                          |                                 | (LED Solid 2 Detik -> OFF)  |
    |                          |                                 |                             |
```

---

## Kontrol Fisik (Tombol)

Perangkat dilengkapi dengan dua fungsi kontrol melalui tombol fisik pada ESP32:

| Tombol | Pin (GPIO) | Durasi | Aksi |
| :--- | :--- | :--- | :--- |
| **BOOT** | GPIO 0 | 3 Detik | **Aktifkan Dashboard (APSTA)**: Menyalakan WiFi AP sementara (2 menit) untuk akses dashboard tanpa reboot. |
| **RESET**| GPIO 1 | 3 Detik | **Factory Reset**: Menghapus seluruh konfigurasi WiFi & FTP dari NVS dan merestart perangkat. |

---

## Indikator LED (GPIO 48)

LED terhubung ke **GPIO 48** sebagai indikator status operasional secara *real-time*:

| Pola LED | Kondisi |
| :--- | :--- |
| **Mati (OFF)** | Normal — Idle, menunggu aktivitas EKG |
| **Berkedip Lambat** (1 Hz) | Safeguard aktif — Menunggu 30 detik konfirmasi USB idle |
| **Berkedip Cepat** (5 Hz) | Sedang upload file ke server FTP |
| **Menyala Solid** (2 detik) | Upload selesai berhasil, kembali ke mode idle |

---

## Antarmuka Web (Web Portal)

Sistem menggunakan **Captive Portal** yang secara otomatis muncul saat pengguna terhubung ke WiFi perangkat. Terdapat dua jenis tampilan portal:

### 1. Setup Portal (`root.html`)
Muncul otomatis jika perangkat belum dikonfigurasi atau gagal terhubung ke WiFi.
*   **Fungsi**: Konfigurasi SSID WiFi, Password WiFi, dan detail akun FTP Server.
*   **Akses**: Otomatis atau via `http://192.168.4.1`.

### 2. Dashboard Portal (`dashboard.html`)
Muncul saat perangkat beroperasi normal (Mode AP+STA).
*   **Trigger**: Otomatis selama 2 menit setelah boot, atau dipicu via tombol BOOT (3 detik).
*   **Fitur**:
    *   **AP Timer**: Menampilkan sisa waktu WiFi AP akan aktif.
    *   **Extend Time**: Menambah durasi aktif WiFi AP (+1 menit).
    *   **Device Info**: Menampilkan IP Address dan SSID yang sedang terhubung.
    *   **Danger Zone**: Tombol Factory Reset cepat (memerlukan password admin).
*   **mDNS**: Dapat diakses via `http://medlink-dongle.local`.

---

## Konfigurasi Global (Global Variables)

Semua konfigurasi utama terpusat dalam variabel global di bagian atas file `tusb_msc_main.c`. Ubah di satu tempat, berlaku ke seluruh sistem.

| Variabel | Nilai Default | Keterangan |
| :--- | :--- | :--- |
| `g_ap_timeout_sec`    | `120`   | Durasi default WiFi AP aktif (detik) |
| `IDLE_SAFEGUARD_SEC`  | `30`    | Jeda aman sebelum upload otomatis (detik) |
| `LED_PIN`             | `48`    | GPIO pin indikator LED status upload |

---


## Format File yang Didukung untuk Upload

| Ekstensi | MIME Type | Keterangan |
| :--- | :--- | :--- |
| `.xml` / `.XML` | `application/xml` | Data rekaman EKG (format utama) |
| `.jpg` / `.JPG` | `image/jpeg` | Gambar JPEG |
| `.bmp` / `.BMP` | `image/bmp` | Gambar Bitmap |

---

## Perintah Console (CLI Commands)

| Perintah | Keterangan |
| :--- | :--- |
| `upload` | Memulai proses pencarian dan pengunggahan file secara manual |
| `check` | Menampilkan daftar folder `ecg_archive` dan semua file valid di storage |
| `size` | Menampilkan kapasitas penyimpanan yang tersedia |
| `status` | Mengecek status kepemilikan storage (USB Host vs Aplikasi) |
| `mount` | Mengambil alih storage untuk digunakan oleh aplikasi |
| `expose` | Memberikan akses storage kepada USB Host |

---

## Konfigurasi Teknis

| Parameter | Nilai |
| :--- | :--- |
| **Base Path** | `/data` |
| **Server Upload** | (Berdasarkan konfigurasi FTP) |
| **Vendor ID USB** | `0x303A` (Espressif) |
| **Product ID USB** | `0x4002` |

---

## Persyaratan Perangkat Keras
- ESP32-S2 atau ESP32-S3 (memiliki dukungan USB OTG).
- Slot SD Card atau Flash eksternal.
