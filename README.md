# ECG-TransferData (ESP32 USB MSC & Data Upload)

Proyek ini mengimplementasikan perangkat **USB Mass Storage Class (MSC)** menggunakan ESP32 (S2/S3). Perangkat ini bertindak sebagai jembatan antara mesin EKG (sebagai USB Host) dan server penyimpanan data melalui Wi-Fi.

---

## Analisis `main/tusb_msc_main.c`

File `main/tusb_msc_main.c` adalah inti dari aplikasi ini.

### 1. Fungsi Utama (Core Purpose)
Program ini memungkinkan ESP32 untuk:
- Berperan sebagai Flash Drive USB (MSC) yang dapat dibaca/tulis oleh PC atau perangkat medis (EKG).
- Mengelola penyimpanan data pada **SD Card** atau **Internal SPI Flash**.
- Mengunggah file data EKG (format `.XML`, `.JPG`, `.BMP`) ke server melalui HTTP secara otomatis.
- Berkomunikasi secara dua arah dengan backend melalui protokol **MQTT over TLS (MQTTS)**.

### 2. Komponen Kunci
- **TinyUSB (MSC Storage)**: Mengelola deskriptor USB dan protokol Mass Storage.
- **Wi-Fi & Networking**: Menghubungkan ESP32 ke jaringan Wi-Fi.
- **MQTT Client**: Jalur kontrol jarak jauh untuk menerima perintah dan mempublikasikan status.
- **HTTP Client**: Mengirim file ke API server dengan metode `POST` multipart/form-data.
- **Console REPL**: Antarmuka CLI melalui UART untuk debugging dan kontrol manual.

### 3. Alur Kerja Perangkat (Workflow)
1. **Inisialisasi**: Menyiapkan NVS, Wi-Fi, dan MQTT.
2. **USB Setup**: Storage di-expose ke USB Host (mode Expose) secara default.
3. **MQTT Connect**: Begitu terhubung, ESP32 langsung mempublikasikan beberapa pesan (lihat bagian MQTT).
5. **Proses Upload via MQTT**:
   - Menerima perintah JSON dari topik `iotgateway/{gateway_sn}/upload`.
   - Mengambil alih storage dari USB Host (otomatis).
   - Mencari folder `ecg_archive` terbaru, lalu mengunggah **semua** file valid (`.xml`, `.jpg`, `.bmp`) di dalamnya secara berurutan.
   - Setelah berhasil, menghapus file yang telah terunggah. Folder dihapus jika sudah kosong.
   - Mengembalikan storage ke USB Host.

---

## Konfigurasi Global (Global Variables)

Semua konfigurasi utama terpusat dalam variabel global di bagian atas file `tusb_msc_main.c`. Ubah di satu tempat, berlaku ke seluruh sistem.

| Variabel | Nilai Default | Keterangan |
| :--- | :--- | :--- |
| `GATEWAY_SN` | `"B0001"` | Serial Number / ID unik perangkat |
| `MQTT_BROKER_URI` | `"mqtts://dev.samelement.com"` | URI broker MQTT |
| `MQTT_BROKER_PORT` | `8888` | Port broker MQTT |
| `MQTT_USERNAME` | `"iotgateway"` | Username autentikasi MQTT |
| `MQTT_PASSWORD` | `"iotgateway10nice"` | Password autentikasi MQTT |

---

## Topik MQTT

Semua topik menggunakan `gateway_sn` secara dinamis sehingga mendukung banyak perangkat sekaligus.

### A. Publish (ESP32 → Broker)

| Topik | Trigger | Retain | Keterangan |
| :--- | :--- | :--- | :--- |
| `iotgateway/{gateway_sn}/dongle/status/online` | Saat konek & saat putus (LWT) | Ya | Mengirim status online/offline |
| `iotgateway/{gateway_sn}/dongle/info` | Saat konek | Ya | Mengirim info firmware & hardware |
| `iotgateway/{gateway_sn}/dongle/function` | Saat konek | Ya | Mengirim IP dan fungsi perangkat |
| `iotgateway/{gateway_sn}/dongle/ip` | Saat menerima perintah `get` | Tidak | Membalas permintaan IP address |
| `iotgateway/{gateway_sn}/dongle/upload/status` | Saat selesai upload | Tidak | Mengirim hasil sukses/gagal upload |
| `iotgateway/{gateway_sn}/dongle/format/status` | Saat selesai format | Tidak | Mengirim hasil sukses/gagal format |

#### Payload: `dongle/status/online` (Online)
```json
{
  "gateway_sn": "B0001",
  "data": {
    "online": true
  }
}
```

#### Payload: `dongle/status/online` (Offline — LWT otomatis dari broker jika perangkat mati mendadak)
```json
{
  "gateway_sn": "B0001",
  "data": {
    "online": false
  }
}
```

#### Payload: `dongle/info`
```json
{
  "gateway_sn": "B0001",
  "data": {
    "model": "EcgDongle-01",
    "firmware_version": "1.1.1",
    "hardware_revision": "R1.1",
    "build_date": "2026-05-08"
  }
}
```

#### Payload: `dongle/function`
```json
{
  "gateway_sn": "B0001",
  "data": {
    "ip": "192.168.x.x",
    "device_function": "ecg1200g"
  }
}
```

#### Payload: `dongle/ip` (Respons cek IP)
```json
{
  "gateway_sn": "B0001",
  "data": {
    "ip": "192.168.x.x"
  }
}
```

#### Payload: `dongle/upload/status` (Selesai Upload)
```json
{
  "gateway_sn": "B0001",
  "data": {
    "status": "completed"
  }
}
```
> **Catatan**: Jika gagal, status bernilai `"failed"`. Informasi jumlah file yang sukses/gagal dapat dilihat melalui log terminal.

#### Payload: `dongle/format/status` (Selesai Format)
```json
{
  "gateway_sn": "B0001",
  "data": {
    "status": "completed"
  }
}
```
> **Catatan**: Jika gagal, status bernilai `"failed"`.

---

### B. Subscribe (Broker → ESP32)

| Topik | Payload | Aksi |
| :--- | :--- | :--- |
| `iotgateway/{gateway_sn}/dongle/upload` | JSON dengan `command: "upload"` | Memicu proses upload file ke server |
| `iotgateway/{gateway_sn}/dongle/format` | JSON dengan `command: "format"` | Memicu proses format storage |
| `iotgateway/{gateway_sn}/dongle/ip/get` | `get` | Memicu ESP32 membalas dengan IP address-nya |

#### Payload: Perintah Upload
```json
{
  "gateway_sn": "B0001",
  "data": {
    "command": "upload"
  }
}
```

> **Catatan**: ESP32 memvalidasi bahwa `gateway_sn` di dalam JSON cocok dengan miliknya dan nilai `command` adalah `"upload"` sebelum menjalankan proses.

#### Payload: Perintah Format
```json
{
  "gateway_sn": "B0001",
  "data": {
    "command": "format"
  }
}
```

> **Catatan**: ESP32 juga memvalidasi bahwa `gateway_sn` cocok dan nilai `command` adalah `"format"` sebelum memformat storage.

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
| **Server Upload** | `http://192.168.13.156:3000/api/ecg-1200g/upload` |
| **MQTT Broker** | `mqtts://dev.samelement.com:8888` |
| **Vendor ID USB** | `0x303A` (Espressif) |
| **Product ID USB** | `0x4002` |

---

## Persyaratan Perangkat Keras
- ESP32-S2 atau ESP32-S3 (memiliki dukungan USB OTG).
- Slot SD Card atau Flash eksternal.
