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
  2. Upload firmware.bin ke Supabase Storage
  3. Insert tabel firmware_updates       ← Naikkan nomor versi

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
  6. ESP32 fetch update terbaru dari REST API Supabase lewat HTTPS
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
  "firmware_url": "https://<PROJECT>.supabase.co/storage/v1/object/public/MedlinkDongle/MedLinkDongle.bin",
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
  "firmware_url": "https://<PROJECT>.supabase.co/storage/v1/object/public/MedlinkDongle/MedLinkDongle.bin"
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

## Setup Supabase (Satu Kali)

Sebagai pengganti file statis di GitHub, sistem OTA menggunakan **Supabase** (PostgreSQL REST API + Storage) agar lebih aman dan terstruktur.

### 1. Buat Tabel `firmware_updates` di SQL Editor
```sql
CREATE TABLE firmware_updates (
  id            BIGSERIAL PRIMARY KEY,
  version       TEXT      NOT NULL,
  firmware_url  TEXT      NOT NULL,
  release_notes TEXT      DEFAULT '',
  is_active     BOOLEAN   NOT NULL DEFAULT true,
  created_at    TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX ON firmware_updates (is_active, created_at DESC);
```

### 2. Aktifkan Keamanan (Row Level Security)
```sql
ALTER TABLE firmware_updates ENABLE ROW LEVEL SECURITY;

-- Izinkan akses publik (anon) HANYA untuk membaca firmware yang aktif
CREATE POLICY "Anon can read active firmware"
  ON firmware_updates
  FOR SELECT
  TO anon
  USING (is_active = true);
```

### 3. Buat Public Storage Bucket
Di dashboard Supabase, buat bucket bernama `MedlinkDongle` dengan tipe **Public**.

**Struktur Bucket (Kerangka Direktori):**
```text
MedlinkDongle/ (Bucket Name)
│
├── MedLinkDongle.bin       ← File firmware terbaru (di-overwrite setiap rilis baru)
└── (Opsional: Anda bisa menyimpan history versi lama dengan nama berbeda jika diperlukan)
```

> **Tips:** Praktik termudah adalah selalu menamai file binary rilis dengan nama yang persis sama (misal: `MedLinkDongle.bin`), lalu menimpa (overwrite) file tersebut di bucket setiap rilis. Dengan cara ini, Anda bisa menggunakan URL `firmware_url` yang sama terus-menerus di database; Anda cukup memperbarui nomor `version`-nya saja.

---

## Panduan Rilis Update Baru (Langkah Developer)

### 1. Update Nomor Versi di Kode (Penting!)
> **⚠️ PENGINGAT DEVELOPER:**
> Proses OTA Update **TIDAK mengubah teks source code** di laptop Anda. Anda **wajib mengubah angka versi** secara manual setiap kali merilis pembaruan.

Edit file `components/ota_manager/ota_manager.h`:
```c
// Sebelum:
#define APP_VERSION "1.1.2"

// Sesudah (Naikkan versinya):
#define APP_VERSION "1.1.3"
```

### 2. Build Firmware Baru
Jalankan kompilasi untuk membungkus kode terbaru Anda menjadi binary:
```bash
cd ECG-TransferData
idf.py build
```
File output: `build/FlashDriveTes.bin` (Atau `MedLinkDongle.bin` tergantung konfigurasi project).

### 3. Upload ke Supabase Storage
1. Buka Supabase Dashboard → **Storage** → Bucket `MedlinkDongle`
2. Upload file `MedLinkDongle.bin` hasil build
3. Klik **Get URL** pada file tersebut untuk menyalin public URL-nya.
   (Contoh: `https://xxxxxx.supabase.co/storage/v1/object/public/MedlinkDongle/MedLinkDongle.bin`)

### 4. Update Database Supabase
Jalankan query ini di **SQL Editor** Supabase untuk memberitahu alat bahwa ada versi baru:

```sql
-- 1. Nonaktifkan semua versi lama terlebih dahulu
UPDATE firmware_updates SET is_active = false;

-- 2. Tambahkan versi terbaru
INSERT INTO firmware_updates (version, firmware_url, release_notes, is_active)
VALUES (
  '1.1.3',
  'https://YOUR_PROJECT_REF.supabase.co/storage/v1/object/public/MedlinkDongle/MedLinkDongle.bin',
  'Perbaikan jeda EKG menjadi 1 detik',
  true
);
```

### 5. Verifikasi
1. Buka Dashboard Dongle (tekan tombol BOOT 3 detik atau saat reboot)
2. Bagian "Pembaruan Firmware" akan otomatis mendeteksi versi baru
3. Klik **⬇ Install Update** untuk memulai

---

## Keamanan

| Aspek | Status | Keterangan |
| :--- | :--- | :--- |
| **HTTPS Transport** | ✅ Aktif | Semua transfer menggunakan TLS (Port 443) |
| **Cert Verification** | ✅ Aktif | Memverifikasi sertifikat SSL Supabase melalui Root CA internal bawaan ESP-IDF |
| **Rollback Protection** | ✅ Aktif | Jika firmware crash → rollback otomatis ke partisi versi sebelumnya |
| **Auth Cek Versi** | ✅ Aman | Menggunakan `anon key` dengan Row Level Security. Hacker tidak bisa merusak database. |

---

## Troubleshooting

| Gejala | Kemungkinan Penyebab | Solusi |
| :--- | :--- | :--- |
| Dashboard tampilkan "Server Tidak Terjangkau" | Dongle tidak punya internet | Pastikan router terkoneksi ke internet |
| Tampil "Firmware Sudah Terbaru" padahal ada update | Lupa mengubah `APP_VERSION` atau DB salah | Cek `APP_VERSION` di firmware vs versi di tabel Supabase |
| OTA gagal dengan "Out of buffer" | Header server terlalu besar | Firmware saat ini sudah di-fix buffer ke 4096, pastikan sudah memakai versi terbaru |
| Setelah update, dongle rollback ke versi lama | Bug di firmware baru yang menyebabkan crash | Firmware lama tetap aman. Periksa log lewat USB untuk debugging. |

---

## Catatan Penting

> **Selama Proses Download**: Dongle tidak dapat menerima data dari mesin EKG. Proses download biasanya berlangsung **1–3 menit** tergantung kecepatan internet. Firmware lama tetap berjalan sepenuhnya (tidak terganggu) karena file baru diunduh ke partisi kosong, bukan partisi aktif.
