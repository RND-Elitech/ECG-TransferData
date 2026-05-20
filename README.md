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
- **Auto-Reconnect WiFi**: Secara otomatis menyambung kembali ke jaringan jika koneksi terputus di tengah jalan tanpa perlu restart.

### 2. Komponen Kunci
- **TinyUSB (MSC Storage)**: Mengelola deskriptor USB dan protokol Mass Storage.
- **Wi-Fi & Networking**: Menghubungkan ESP32 ke jaringan Wi-Fi rumah sakit/klinik.
- **FTP Client**: Mengirim file data EKG secara otomatis ke server FTP di background.
- **Hardware Interceptor**: Menggunakan Linker Wrap GCC (`--wrap=sdmmc_write_sectors`) untuk melacak aktivitas tulis (*write*) kartu memori secara langsung.
- **Console REPL**: Antarmuka CLI melalui UART untuk debugging dan kontrol manual.

## Diagram Alur (Idle Detection & Auto-Upload)

Berikut adalah visualisasi apa yang terjadi antara Perawat (User), Mesin EKG, dan Dongle:

```text
[Perawat]                 [Mesin EKG]                     [Dongle ESP32]                 [Server FTP]
    |                          |                                 |                             |
    |-- 1. Simpan Rekam EKG -->|                                 |                             |
    |                          |-- 2. Tulis Data (Write) ------->|                             |
    |                          |                                 | (Status: Intercept Aktif)   |
    |                          |                                 | (LED: Putih Solid)          |
    |                          |   (Penulisan byte terakhir)     |                             |
    |                          |-- 3. Mesin Berhenti Menulis --->|                             |
    |                          |                                 | (Safeguard 5 Detik mulai)   |
    |                          |                                 | (LED: Biru Kedip Lambat)    |
    |                          |                                 |                             |
    |                          |   (Jika hening 5 detik tuntas)  |                             |
    |                          |                                 |-- 4. Ambil alih akses USB   |
    |                          |                                 |-- 5. Upload via FTP (Port 21)-->
    |                          |                                 | (LED: Biru Kedip Cepat)     |
    |                          |                                 |<-- 6. Upload Sukses --------|
    |                          |                                 |-- 7. Hapus File Internal    |
    |                          |                                 |-- 8. Kembalikan akses USB   |
    |                          |                                 | (LED: Hijau 2 Detik)        |
    |                          |                                 | (Sistem Kembali Standby)    |
```

---

## Kontrol Fisik (Tombol)

Perangkat dilengkapi dengan dua fungsi kontrol melalui tombol fisik pada ESP32:

| Tombol | Pin (GPIO) | Durasi | Aksi |
| :--- | :--- | :--- | :--- |
| **BOOT** | GPIO 0 | 3 Detik | **Aktifkan Dashboard (APSTA)**: Menyalakan WiFi AP sementara (2 menit) untuk akses dashboard tanpa reboot. |
| **RESET**| GPIO 1 | 3 Detik | **Factory Reset**: Menghapus seluruh konfigurasi WiFi & FTP dari NVS dan merestart perangkat. |

---

## Indikator Smart RGB LED (GPIO 48)

Dongle menggunakan Smart RGB LED (WS2812/NeoPixel) sebagai indikator sistem terpusat. LED ini memberikan status komunikasi jaringan sekaligus proses transmisi file:

| Warna LED | Pola Kedip | Status Sistem |
| :--- | :--- | :--- |
| **Putih** | Berkedip Cepat | **Connecting**: Sedang berusaha terhubung ke jaringan WiFi rumah sakit. |
| **Putih** | Berkedip Lambat | **AP Mode**: Dongle masuk ke mode Konfigurasi (WiFi `ECG-Gateway-XXXX` memancar). Buka portal `192.168.4.1` dari HP Anda. |
| **Putih** | Menyala Solid | **Standby**: Perangkat sudah siap pakai dan terhubung ke jaringan. Menunggu mesin EKG mengirim data. |
| **Biru** | Berkedip Lambat | **Safeguard**: Data dari EKG terdeteksi! Menunggu jeda aman 5 detik. |
| **Biru** | Berkedip Cepat | **Uploading**: Mengunggah data EKG di latar belakang menuju Server FTP. |
| **Hijau** | Menyala Solid (2 Detik) | **Upload Sukses**: Pengiriman file berhasil, file sumber telah dihapus. Sistem akan kembali ke Standby. |
| **Merah** | 1x Kedip Berulang | **Error WiFi / Offline**: Gagal terkoneksi atau koneksi WiFi terputus. Alat akan terus mencoba reconnect di background. |
| **Merah** | 2x Kedip Berulang | **Error FTP Login**: Host FTP tidak terjangkau atau kredensial salah. File di storage akan dihapus agar bisa mencoba lagi. |
| **Merah** | 3x Kedip Berulang | **Error Transfer**: Gagal saat mengirim file (koneksi putus). File tetap dihapus dari storage untuk mencegah penumpukan. |

---

## Antarmuka Web (Web Portal)

Sistem menggunakan **Captive Portal** yang secara otomatis muncul saat pengguna terhubung ke WiFi perangkat. Terdapat dua jenis tampilan portal:

### 1. Setup Portal (`root.html`)
Muncul otomatis jika perangkat belum dikonfigurasi atau gagal terhubung ke WiFi.
*   **Fungsi**: Konfigurasi SSID WiFi, Password WiFi, dan detail akun FTP Server.
*   **Akses**: Otomatis atau via `http://192.168.4.1`.
*   **Indikator Sinyal**: Saat melakukan scan WiFi, kualitas sinyal ditampilkan menggunakan label teks dinamis berwarna (Excellent, Good, Fair, Poor) berdasarkan nilai RSSI, memberikan pengalaman pengguna yang lebih intuitif dibanding angka dBm mentah.

### 2. Dashboard Portal (`dashboard.html`)
Web Server **tidak berjalan secara otomatis** saat perangkat berhasil terhubung ke WiFi pada kondisi normal (untuk menghemat memori dan menghindari gangguan). Portal ini hanya diaktifkan secara manual.
*   **Trigger**: Dipicu **hanya** dengan menahan tombol **BOOT (3 detik)**. Mode AP akan menyala selama waktu yang ditentukan. Setelah waktu habis, LED indikator akan kembali ke mode Standby.
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
| `IDLE_SAFEGUARD_MS`   | `5000`  | Jeda aman sebelum upload otomatis (5 detik) |
| `LED_PIN`             | `48`    | GPIO pin indikator Smart RGB LED WS2812 |

---


## Format File yang Didukung untuk Upload

| Ekstensi | Keterangan |
| :--- | :--- |
| `.xml` / `.XML` | Data rekaman EKG (format utama) |
| `.jpg` / `.JPG` | Gambar JPEG (Snapshot) |
| `.bmp` / `.BMP` | Gambar Bitmap |

*(Catatan: Penentuan MIME type tidak lagi dibutuhkan karena upload kini menggunakan protokol FTP).*

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

## Sistem Penyimpanan (Storage) & Persyaratan Perangkat Keras

- ESP32-S2 atau ESP32-S3 (memiliki dukungan USB OTG).
- Slot SD Card eksternal.

### Auto-Fallback Storage (SD Card ➔ Internal SPI Flash)
Sistem memiliki mekanisme failover otomatis untuk memastikan perangkat tetap dapat digunakan meski tidak ada SD Card yang terpasang:
1. Saat dinyalakan, perangkat akan mencoba mendeteksi SD Card sebanyak **3 kali percobaan**.
2. Jika SD Card terdeteksi, perangkat akan beroperasi normal menggunakan kapasitas eksternal.
3. Jika gagal setelah 3 percobaan, perangkat secara otomatis **beralih menggunakan Internal SPI Flash** melalui partisi FAT `storage` berkapasitas ~1MB yang didukung dengan `wear_levelling`.
4. Fallback ini menjamin mesin EKG tidak akan mengalami *error write* meskipun pengguna lupa memasukkan memori fisik.

---

## Dokumentasi Developer: Logika Error Handling & Indikator LED

Sistem ini dirancang untuk beroperasi secara mandiri (*headless*), sehingga indikator LED dan penanganan error sangat krusial:

### 1. Mekanisme Pelacakan Status Upload
Proses upload berjalan di *background task* FreeRTOS yang terpisah dari *idle detector*. Untuk sinkronisasi status:
- Modul `uploader` menggunakan flag internal `g_is_uploading` dan `s_last_upload_status`.
- Fungsi API disediakan: `uploader_is_busy()` dan `uploader_get_last_status()`.
- `idle_detector_task` melakukan polling pada flag tersebut setelah memicu `uploader_trigger()`.

### 2. Penanganan Error & Pembersihan Otomatis
Sistem memiliki pola kedipan warna merah spesifik untuk memudahkan *troubleshooting*:
- **1x Kedip (WiFi Fail / Offline)**: Gagal terkoneksi atau WiFi terputus. Sistem melakukan **Auto-Reconnect** secara otomatis. Jika terjadi saat Standby, indikator ini akan muncul hingga WiFi kembali normal.
- **2x Kedip (FTP Connect/Login Fail)**: Masalah pada server FTP. Untuk mencegah file "nyangkut", sistem akan **menghapus folder ecg_archive** secara otomatis agar mesin EKG bisa mencoba mengirim ulang data segar.
- **3x Kedip (Transfer Fail)**: Koneksi putus saat transfer byte. File tetap dihapus demi kebersihan storage.

### 3. Deteksi File Kosong (Silent Fallback)
Jika sistem mendeteksi aktivitas tulis (Idle Trigger) namun ternyata tidak ada file EKG valid yang ditemukan (misal hanya penulisan metadata sistem), uploader akan berhenti secara diam-diam (*Silent*) dan kembali ke mode Standby tanpa memicu indikator error Merah, guna menghindari kebingungan pengguna (*False Alarm*).

### 3. Hardware Activity Intercept (Linker Wrap)
Untuk mendeteksi kapan mesin EKG selesai menulis data tanpa bergantung pada *cache* sistem operasi, sistem menggunakan teknik *linker wrapping*:
- Fungsi driver asli `sdmmc_write_sectors` dibungkus menjadi `__wrap_sdmmc_write_sectors`.
- Setiap instruksi tulis fisik akan memperbarui `g_last_sd_activity_ms`.
- Hal ini memastikan masa tenang 5 detik benar-benar dihitung dari byte terakhir yang ditulis ke disk fisik.
