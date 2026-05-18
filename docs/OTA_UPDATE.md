# Dokumentasi OTA (Over-The-Air) Update

## Ringkasan

Fitur OTA memungkinkan firmware ECG Dongle diperbarui **tanpa kabel USB** — cukup melalui jaringan WiFi yang sama dengan rumah sakit/klinik. Sistem ini menggunakan komponen native ESP-IDF (`esp_https_ota`) dengan mekanisme **dual-partition rollback** untuk keamanan maksimal.

---

## Alur Kerja Lengkap

### Diagram Alur

```
[Developer Rilis Versi Baru]
        │
        ▼
  1. idf.py build                        ← Kompilasi firmware baru
  2. Upload firmware.bin ke GitHub/Server
  3. Update version.json di server       ← Naikkan nomor versi

        │
        ▼
[Dongle ESP32 — Saat Dashboard Dibuka]

  ┌─────────────────────────────────────────────────────────────┐
  │ 4. Browser terhubung ke Dashboard (saat reboot / tombol BOOT)│
  └─────────────────────────────────────────────────────────────┘
        │
        ▼
  5. Dashboard otomatis memanggil GET /ota/check
        │
        ▼
  6. ESP32 fetch version.json dari server lewat HTTPS
        │
        ├─── Gagal (No Internet / Server Error)
        │         → Dashboard tampilkan "Server Tidak Terjangkau"
        │
        ▼
  7. ESP32 bandingkan versi server vs versi firmware saat ini
        │
        ├─── Sama / Server lebih lama
        │         → Dashboard tampilkan "✅ Firmware Sudah Terbaru (v1.0.0)"
        │
        └─── Server lebih baru
                  → Dashboard tampilkan "🔔 Update Tersedia: v1.1.0"
                  → Tampilkan tombol [⬇ Install Update]
                        │
                        ▼
              8. User klik tombol → POST /ota/start
                        │
                        ▼
              9. ESP32 download firmware.bin langsung ke partisi OTA kosong
                   (LED: Putih Solid — sistem tetap berjalan normal)
                        │
                        ▼
             10. Dashboard polling GET /ota/progress setiap 1.5 detik
                   → Progress bar bergerak 0–100%
                        │
                        ▼
             11a. Berhasil → ESP32 restart otomatis dalam 3 detik
                   → Firmware baru aktif dari partisi OTA
                   → Boot mark valid (tidak rollback)

             11b. Gagal → Dashboard tampilkan pesan error
                   → Firmware lama tetap berjalan (rollback otomatis)
```

---

## Arsitektur Dual-Partition OTA

### Skema Partisi (ESP32-S3 N16R8 — 16MB Flash)

```
┌──────────────────────────────────────────────────────────────────┐
│  FLASH 16MB (0x00000000 – 0x01000000)                            │
├────────────┬──────┬─────────────────────────────────────────────┤
│ nvs        │ 24KB │ Simpan konfigurasi WiFi, FTP, dll (NVS)      │
│ phy_init   │  4KB │ Kalibrasi WiFi hardware                       │
│ otadata    │  8KB │ Metadata OTA: menentukan partisi mana aktif   │
├────────────┼──────┼─────────────────────────────────────────────┤
│ ota_0      │ 7.5MB│ ← Slot firmware pertama                       │
│ ota_1      │ 7.5MB│ ← Slot firmware kedua (diisi saat update)     │
└────────────┴──────┴─────────────────────────────────────────────┘
```

### Mekanisme Pergantian Partisi

```
Kondisi Normal (firmware v1.0.0 aktif di ota_0):
  otadata → menunjuk ke ota_0

Saat Update ke v1.1.0:
  1. Download firmware baru → ditulis ke ota_1 (ota_0 tidak disentuh)
  2. Setelah selesai: otadata diperbarui → menunjuk ke ota_1
  3. Reboot → ESP32 booting dari ota_1 (firmware v1.1.0)
  4. app_main() memanggil esp_ota_mark_app_valid_cancel_rollback()
     → Firmware baru dianggap stabil, tidak akan rollback

Jika Firmware Baru Crash (sebelum mark valid):
  → Bootloader mendeteksi firmware tidak valid
  → otadata dikembalikan ke ota_0
  → Device booting dari v1.0.0 secara otomatis (Rollback)
```

---

## Komponen yang Terlibat

| Komponen | File | Fungsi |
| :--- | :--- | :--- |
| **ota_manager** | `components/ota_manager/ota_manager.c` | Fetch version.json, download firmware, tracking progress |
| **web_server** | `components/web_server/web_server.c` | Endpoint `/ota/check`, `/ota/start`, `/ota/progress` |
| **Dashboard UI** | `components/web_server/dashboard.html` | Tampilan cek update, progress bar, tombol install |
| **app_main** | `main/app_main.c` | Memanggil rollback mark saat boot |

---

## API HTTP Endpoints

### `GET /ota/check`
Dipanggil otomatis oleh dashboard saat halaman dimuat.

**Response sukses (ada update):**
```json
{
  "current_version": "1.0.0",
  "latest_version": "1.1.0",
  "update_available": true,
  "firmware_url": "https://github.com/.../firmware_v1.1.0.bin",
  "release_notes": "Perbaikan Auto-Reconnect WiFi dan error FTP"
}
```

**Response sukses (sudah terbaru):**
```json
{
  "current_version": "1.0.0",
  "latest_version": "1.0.0",
  "update_available": false,
  "firmware_url": "",
  "release_notes": ""
}
```

**Response gagal (tidak ada internet):**
```json
{
  "current_version": "1.0.0",
  "latest_version": "",
  "update_available": false,
  "error": "Gagal menghubungi server pembaruan."
}
```

---

### `POST /ota/start`
Dipanggil saat user klik tombol "Install Update".

**Request body (JSON):**
```json
{
  "firmware_url": "https://github.com/.../firmware_v1.1.0.bin"
}
```

**Response:**
```json
{ "status": "started" }        // Berhasil memulai download
{ "status": "already_running" } // OTA sudah berjalan
{ "status": "error" }           // Gagal membuat task
```

---

### `GET /ota/progress`
Di-poll oleh dashboard setiap 1.5 detik selama proses download berlangsung.

**Response:**
```json
{
  "state": "downloading",  // idle | checking | downloading | success | failed
  "progress": 65,          // 0-100
  "error": ""              // Pesan error jika state == failed
}
```

---

## Format `version.json` (Server)

File ini ditempatkan di server/GitHub dan menjadi sumber kebenaran versi:

```json
{
  "version": "1.1.0",
  "firmware_url": "https://raw.githubusercontent.com/OWNER/REPO/main/firmware.bin",
  "release_notes": "Perbaikan koneksi FTP, tambah indikator LED offline, dan OTA Update."
}
```

> File template tersedia di: `releases/version.json`

---

## Panduan Rilis Update Baru (Langkah Developer)

### 1. Build Firmware Baru
```bash
cd ECG-TransferData
idf.py build
```
File output: `build/FlashDriveTes.bin`

### 2. Update Nomor Versi di Kode
Edit file `components/ota_manager/ota_manager.h`:
```c
// Sebelum:
#define APP_VERSION "1.0.0"

// Sesudah:
#define APP_VERSION "1.1.0"
```
Kemudian build ulang untuk mendapatkan binary dengan versi terbaru.

### 3. Upload ke GitHub (Cara Termudah)

**Opsi A — GitHub Releases (Rekomendasi Produksi):**
1. Buat tag baru di GitHub: `git tag v1.1.0 && git push --tags`
2. Di halaman GitHub > Releases > Draft New Release
3. Upload file `build/FlashDriveTes.bin`
4. Salin URL download langsung dari GitHub (format `https://github.com/OWNER/REPO/releases/download/v1.1.0/FlashDriveTes.bin`)

**Opsi B — GitHub Raw File di folder releases/ (Untuk Testing Cepat):**
1. Salin `build/FlashDriveTes.bin` ke folder lokal `releases/` dan ubah namanya menjadi `firmware.bin`.
2. Commit dan push: `git add releases/firmware.bin && git commit -m "Update firmware v1.1.0" && git push`
3. URL: `https://raw.githubusercontent.com/OWNER/REPO/BRANCH/releases/firmware.bin`

### 4. Update `version.json` di Server
Edit file `releases/version.json` yang sudah diupload sebelumnya:
```json
{
  "version": "1.1.0",
  "firmware_url": "https://github.com/rivaldisinkoprima/ECG-TransferData/releases/download/v1.1.0/FlashDriveTes.bin",
  "release_notes": "Deskripsi singkat perubahan versi ini."
}
```
Commit dan push perubahan ini.

### 5. Verifikasi
1. Buka Dashboard Dongle (tekan tombol BOOT 3 detik atau saat reboot)
2. Bagian "Pembaruan Firmware" akan otomatis mendeteksi versi baru
3. Klik **⬇ Install Update** untuk memulai

---

## Konfigurasi URL Server (Sesuaikan di Kode)

Edit file `components/ota_manager/ota_manager.h`:
```c
// Ganti dengan URL repositori GitHub Anda:
#define OTA_VERSION_URL \
    "https://raw.githubusercontent.com/OWNER/REPO/BRANCH/releases/version.json"
```

---

## Keamanan

| Aspek | Status | Keterangan |
| :--- | :--- | :--- |
| **HTTPS Transport** | ✅ Aktif | Semua transfer menggunakan TLS |
| **Cert Verification** | ✅ Aktif | Menggunakan `esp_crt_bundle_attach` untuk memverifikasi sertifikat SSL GitHub melalui Root CA internal bawaan ESP-IDF |
| **Rollback Protection** | ✅ Aktif | Firmware crash → rollback otomatis ke versi sebelumnya |
| **Auth Token** | ❌ Belum ada | URL publik GitHub tidak memerlukan auth, private repo perlu header token |

> **Untuk lingkungan produksi klinis yang lebih ketat**, pertimbangkan menggunakan server internal rumah sakit (tidak perlu akses internet publik) dengan sertifikat TLS yang valid.

---

## Troubleshooting

| Gejala | Kemungkinan Penyebab | Solusi |
| :--- | :--- | :--- |
| Dashboard tampilkan "Server Tidak Terjangkau" | Dongle tidak punya akses internet, hanya LAN lokal | Pastikan router memiliki koneksi internet aktif |
| Progress bar mandek di 0% | URL firmware salah atau server lambat | Cek URL di `version.json`, coba akses dari browser PC |
| OTA gagal dengan "OTA begin gagal" | Partisi belum di-flash dengan skema dual-OTA | Lakukan full flash ulang via USB: `idf.py flash` |
| Setelah update, dongle rollback ke versi lama | `esp_ota_mark_app_valid_cancel_rollback()` tidak dipanggil | Pastikan fungsi tersebut ada di baris pertama `app_main()` |
| Dongle tidak bisa download, timeout | Ukuran firmware >7.5MB atau koneksi lemah | Cek ukuran binary, perpanjang `timeout_ms` di `ota_manager.c` |

---

## Catatan Penting

> **Flash Ulang Pertama Kali Wajib**: Karena partisi tabel diubah dari `factory` (satu slot) menjadi `ota_0`/`ota_1` (dual slot), Anda **harus melakukan flash ulang via kabel USB satu kali** sebelum fitur OTA bisa digunakan. Perintah: `idf.py flash`

> **Selama Proses Download**: Dongle tidak dapat menerima data dari mesin EKG. Proses download biasanya berlangsung **1–3 menit** tergantung kecepatan internet. Firmware lama tetap berjalan sepenuhnya selama download (download ke partisi kosong, bukan partisi aktif).
