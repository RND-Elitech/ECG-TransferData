# ECG-TransferData (ESP32 USB MSC & Data Upload)

Proyek ini mengimplementasikan perangkat **USB Mass Storage Class (MSC)** menggunakan ESP32 (S2/S3). Perangkat ini bertindak sebagai jembatan antara mesin EKG (sebagai USB Host) dan server penyimpanan data melalui Wi-Fi.

## Analisis `main/tusb_msc_main.c`

File `main/tusb_msc_main.c` adalah inti dari aplikasi ini. Berikut adalah penjelasan mendalam mengenai komponen dan alurnya:

### 1. Fungsi Utama (Core Purpose)
Program ini memungkinkan ESP32 untuk:
*   Berperan sebagai Flash Drive USB (MSC) yang dapat dibaca/tulis oleh PC atau perangkat medis (EKG).
*   Mengelola penyimpanan data pada **SD Card** atau **Internal SPI Flash**.
*   Mengunggah file data EKG (format `.XML`) ke server melalui protokol HTTP secara otomatis atau melalui perintah.

### 2. Komponen Kunci
*   **TinyUSB (MSC Storage)**: Mengelola deskriptor USB dan protokol Mass Storage agar penyimpanan ESP32 terbaca sebagai drive USB.
*   **Wi-Fi & Networking**: Menghubungkan ESP32 ke jaringan Wi-Fi (SSID: `Elitech`) dengan dukungan IP Statis atau DHCP.
*   **MQTT Client**: Berfungsi sebagai jalur kontrol jarak jauh. Mendengarkan topik `ecg1200G/upload` untuk memicu proses transfer data.
*   **HTTP Client**: Menggunakan metode `POST` dengan format `multipart/form-data` untuk mengirim file ke API server.
*   **Console REPL**: Antarmuka baris perintah (CLI) melalui UART untuk debugging dan kontrol manual.

### 3. Alur Kerja Perangkat (Workflow)
1.  **Inisialisasi**: Menyiapkan NVS, Wi-Fi, dan MQTT.
2.  **Storage Setup**: Menginisialisasi SD Card/SPI Flash dan menyiapkan sistem file FATFS.
3.  **USB Setup**: Memasang driver TinyUSB. Secara default, penyimpanan diatur dalam mode **Expose** (dapat diakses oleh USB Host/PC).
4.  **Akses Eksklusif**: Kode ini menangani pembatasan akses. Jika USB Host sedang menggunakan storage, aplikasi ESP32 tidak bisa mengakses file, dan sebaliknya.
5.  **Proses Upload (Otomatis via MQTT)**:
    *   Menerima perintah `upload` dari MQTT.
    *   Melepas akses USB Host (Unmount dari USB).
    *   Mencari folder `ecg_archive` terbaru dan mencari file `.XML` di dalamnya.
    *   Mengunggah file tersebut ke `http://192.168.13.156:3000/api/ecg-1200g/upload`.
    *   Setelah berhasil, menghapus folder archive yang sudah diunggah.
    *   Mengembalikan akses storage ke USB Host (Expose kembali).

### 4. Perintah Console (CLI Commands)
Anda dapat berinteraksi dengan perangkat melalui terminal serial menggunakan perintah berikut:
*   `upload`: Memulai proses pencarian dan pengunggahan file XML secara manual.
*   `check`: Menampilkan daftar folder `ecg_archive` dan file XML yang tersedia di storage.
*   `size`: Menampilkan kapasitas penyimpanan yang tersedia.
*   `status`: Mengecek apakah storage saat ini sedang dikuasai oleh USB Host atau aplikasi.
*   `mount`: Mengambil alih storage untuk digunakan oleh aplikasi ESP32.
*   `expose`: Memberikan akses storage kepada USB Host (PC/EKG).

---

## Panduan Penggunaan

### 1. Koneksi Hardware
*   Hubungkan ESP32 ke komputer atau mesin EKG melalui port USB (OTG).
*   ESP32 akan terdeteksi sebagai **USB Mass Storage** (seperti Flash Drive) dengan nama "Example MSC".

### 2. Pengiriman Data (Upload)
Ada dua cara untuk memicu pengiriman data dari perangkat ke server:

#### A. Melalui MQTT (Otomatis/Jarak Jauh)
Kirim pesan teks (payload) ke topik berikut menggunakan aplikasi MQTT client (seperti MQTT Explorer):
*   **Topic**: `ecg1200G/upload`
*   **Payload**: `upload`

**Efek**: ESP32 akan otomatis memutus koneksi USB ke PC/EKG (unmount), mencari folder `ecg_archive` terbaru, mencari file `.XML` di dalamnya, mengunggahnya ke server, menghapus folder tersebut setelah berhasil, dan menyambungkan kembali koneksi USB.

#### B. Melalui Konsol Serial (Manual)
1.  Buka terminal serial (misal: Monitor di VS Code atau Putty) dengan baudrate **115200**.
2.  Ketik perintah:
    ```bash
    upload
    ```
3.  Gunakan perintah `check` untuk melihat daftar folder dan file yang ada di penyimpanan.

### 3. Struktur Folder di Drive USB
Agar sistem otomatis mengenali file, pastikan struktur folder di dalam drive USB adalah sebagai berikut:
`D: (Drive ESP32) / ecg_archive / [file_data].XML`

---

## Konfigurasi Teknis
*   **Base Path**: `/data`
*   **Server Target**: `192.168.13.156` (Port 3000)
*   **MQTT Broker**: `dev.samelement.com` (Port 8883, MQTTS)
*   **MQTT User**: `iotgateway`
*   **Vendor ID USB**: `0x303A` (Espressif)
*   **Product ID USB**: `0x4002`

## Persyaratan Perangkat Keras
*   ESP32-S2 atau ESP32-S3 (memiliki dukungan USB OTG).
*   Slot SD Card atau Flash eksternal.

