# Panduan Konfigurasi Transfer Data via FTP

Dokumen ini menjelaskan cara menyiapkan **FTP Server** di sisi server dan cara mengkonfigurasi **ESP32 Gateway** di sisi client untuk dapat mengirimkan file ECG secara otomatis.

---

## Daftar Isi

1. [Gambaran Umum Alur](#1-gambaran-umum-alur)
2. [Sisi Server — Setup FTP Server](#2-sisi-server--setup-ftp-server)
   - [Opsi A: Windows (FileZilla Server)](#opsi-a-windows--filezilla-server)
   - [Opsi B: Linux (vsftpd)](#opsi-b-linux--vsftpd)
   - [Opsi C: Python (FTP Server Sederhana untuk Testing)](#opsi-c-python--ftp-server-sederhana-untuk-testing)
3. [Sisi Client — Konfigurasi ESP32 Gateway](#3-sisi-client--konfigurasi-esp32-gateway)
   - [Konfigurasi Default (Kompilasi)](#konfigurasi-default-kompilasi)
   - [Konfigurasi Dinamis via Console (Tanpa Build Ulang)](#konfigurasi-dinamis-via-console-tanpa-build-ulang)
4. [Struktur File yang Dikirim](#4-struktur-file-yang-dikirim)
5. [Troubleshooting](#5-troubleshooting)

---

## 1. Gambaran Umum Alur

```
[ Perangkat ECG (USB) ]
         │  (tulis file ke SD Card)
         ▼
[ ESP32 Gateway ]
         │  (baca file dari /data/ecg_archive/...)
         │  (koneksi FTP via Wi-Fi)
         ▼
[ FTP Server ]
         │  (simpan file .xml, .jpg, .bmp)
         ▼
[ Penyimpanan / Pemrosesan Data ]
```

Saat perintah `upload` diterima (via MQTT atau Console), ESP32 akan:
1. Mengambil alih storage dari USB Host.
2. Membuka koneksi ke FTP Server.
3. Login menggunakan username & password.
4. Mengunggah semua file dari folder `ecg_archive` terbaru.
5. Menghapus file lokal yang berhasil dikirim.
6. Mengembalikan storage ke USB Host.

---

## 2. Sisi Server — Setup FTP Server

> [!IMPORTANT]
> Pastikan FTP Server dan ESP32 Gateway berada dalam **jaringan yang sama** (LAN/Wi-Fi) atau IP Server dapat dijangkau oleh ESP32.

### Opsi A: Windows — FileZilla Server

**Instalasi:**
1. Download FileZilla Server dari [https://filezilla-project.org/download.php?type=server](https://filezilla-project.org/download.php?type=server).
2. Jalankan installer dan ikuti langkah-langkahnya.
3. Buka **FileZilla Server Interface**.

**Membuat User FTP:**
1. Buka menu **Edit → Users**.
2. Klik **Add** → masukkan nama user, misal: `iotgateway`.
3. Centang **Password** → masukkan password, misal: `ftppass`.
4. Klik tab **Shared Folders** → klik **Add** → pilih folder tujuan penyimpanan file ECG.
5. Centang izin **Read**, **Write**, **Delete**, **Append** untuk folder tersebut.
6. Klik **OK**.

**Konfigurasi Passive Mode & Firewall (PENTING!):**

Kegagalan paling umum pada transfer FTP adalah masalah **Data Connection** yang terblokir karena port pasif tidak sinkron antara aplikasi FTP Server dan Firewall OS.

1. **Atur Port Pasif di FileZilla Server:**
   - Buka menu **Edit → Settings → Passive mode settings**.
   - Pilih **Use the following IP**: masukkan IP lokal server Anda.
   - Konfigurasi port range, contoh: `50000 - 51000`.
   - **PENTING:** Jangan biarkan kosong atau menggunakan port default yang terlalu lebar jika Anda menggunakan firewall ketat.

2. **Sinkronkan dengan Firewall Windows:**
   Pastikan port range yang Anda masukkan di FileZilla Server **SAMA PERSIS** dengan yang dibuka di Firewall. Jika FileZilla memilih port di luar range yang diizinkan firewall (misal port 51079 sedangkan firewall hanya buka sampai 51000), transfer akan gagal dengan error `connect: Software caused connection abort`.

   Jalankan perintah ini di Command Prompt (Run as Administrator) untuk membuka port (sesuaikan range-nya jika Anda mengubahnya di FileZilla):
   ```cmd
   netsh advfirewall firewall add rule name="FTP21" dir=in action=allow protocol=TCP localport=21
   netsh advfirewall firewall add rule name="FTPPassive" dir=in action=allow protocol=TCP localport=50000-51000
   ```

**Mengecek IP Server:**
Buka Command Prompt dan jalankan:
```
ipconfig
```
Catat **IPv4 Address** dari adapter yang terhubung ke jaringan yang sama dengan ESP32.

---

### Opsi B: Linux — vsftpd

**Instalasi (Ubuntu/Debian):**
```bash
sudo apt update
sudo apt install vsftpd -y
```

**Membuat User FTP Khusus:**
```bash
# Buat user baru (tanpa akses shell)
sudo adduser --shell /usr/sbin/nologin ftpuser

# Set password
sudo passwd ftpuser
```

**Konfigurasi vsftpd:**
```bash
sudo nano /etc/vsftpd.conf
```

Pastikan baris-baris berikut ada dan tidak dikomentari:
```ini
# Izinkan koneksi local
local_enable=YES

# Izinkan upload
write_enable=YES

# Batasi user ke direktori home masing-masing
chroot_local_user=YES
allow_writeable_chroot=YES

# Aktifkan Passive Mode
pasv_enable=YES
pasv_min_port=50000
pasv_max_port=51000
pasv_address=<IP_SERVER_ANDA>
```

**Membuat Folder Tujuan & Atur Izin:**
```bash
# Buat folder penyimpanan di home user
sudo mkdir -p /home/ftpuser/ecg_upload
sudo chown ftpuser:ftpuser /home/ftpuser/ecg_upload
```

**Restart vsftpd:**
```bash
sudo systemctl restart vsftpd
sudo systemctl enable vsftpd
```

**Cek Status:**
```bash
sudo systemctl status vsftpd
```

**Firewall (jika menggunakan UFW):**
```bash
sudo ufw allow 21/tcp
sudo ufw allow 50000:51000/tcp
sudo ufw reload
```

---

### Opsi C: Python — FTP Server Sederhana untuk Testing

Cara paling cepat untuk menguji koneksi FTP dari ESP32 tanpa instalasi software tambahan.

**Prasyarat:**
- Python 3.x terinstall di komputer/server.

**Instalasi library:**
```bash
pip install pyftpdlib
```

**Menjalankan FTP Server:**
```bash
python -m pyftpdlib -p 21 -u ftpuser -P ftppass -d ./ecg_files -w
```

Keterangan opsi:
| Opsi | Keterangan |
|------|-----------|
| `-p 21` | Menggunakan port 21 |
| `-u ftpuser` | Username |
| `-P ftppass` | Password |
| `-d ./ecg_files` | Folder tujuan penyimpanan file |
| `-w` | Aktifkan izin tulis (write) |

> [!NOTE]
> Server Python ini hanya untuk keperluan testing/development. Untuk produksi, gunakan Opsi A atau B.

---

## 3. Sisi Client — Konfigurasi ESP32 Gateway

### Konfigurasi Default (Kompilasi)

Jika Anda ingin mengubah nilai default yang digunakan saat NVS belum pernah dikonfigurasi, edit baris berikut di file `main/app_main.c` **sebelum melakukan Build & Flash**:

```c
/* ─── Konfigurasi Perangkat ─── */
#define GATEWAY_SN       "GWTEST"         // Serial Number Gateway
#define WIFI_SSID        "NamaWiFi"       // Nama jaringan Wi-Fi
#define WIFI_PASSWORD    "PasswordWiFi"   // Password Wi-Fi
#define MQTT_BROKER_URI  "mqtts://..."    // URI MQTT Broker
#define MQTT_BROKER_PORT 8888             // Port MQTT
#define MQTT_USERNAME    "..."            // Username MQTT
#define MQTT_PASSWORD    "..."            // Password MQTT

// Konfigurasi FTP Default
#define FTP_SERVER_HOST  "192.168.1.10"  // IP Address FTP Server
#define FTP_SERVER_PORT  21              // Port FTP (default 21)
#define FTP_SERVER_USER  "ftpuser"       // Username FTP
#define FTP_SERVER_PASS  "ftppass"       // Password FTP
```

> [!WARNING]
> Nilai `#define` di atas **hanya aktif saat NVS kosong** (pertama kali flash atau setelah erase flash). Jika NVS sudah berisi konfigurasi, perangkat akan menggunakan nilai dari NVS.

---

### Konfigurasi Dinamis via Console (Tanpa Build Ulang)

Setelah perangkat di-flash, Anda dapat mengubah konfigurasi FTP kapan saja melalui **terminal serial** (misal: menggunakan ESP-IDF Monitor atau Serial Monitor di VS Code) **tanpa perlu kompilasi ulang**.

**Cara Mengakses Console:**
1. Hubungkan ESP32 ke PC via USB.
2. Buka **Serial Monitor** atau jalankan `idf.py monitor`.
3. Tunggu hingga perangkat boot dan prompt `esp32s3>` muncul.

**Perintah yang Tersedia:**

| Perintah | Keterangan |
|----------|-----------|
| `help` | Menampilkan semua perintah yang tersedia |
| `set_wifi <ssid> <password>` | Mengubah konfigurasi Wi-Fi |
| `set_mqtt <uri> <port> <user> <pass>` | Mengubah konfigurasi MQTT |
| `set_ftp <host> <port> <user> <pass>` | Mengubah konfigurasi FTP |
| `set_sn <gateway_sn>` | Mengubah Serial Number Gateway |
| `upload` | Memicu upload file secara manual |
| `check` | Menampilkan daftar file yang siap dikirim |
| `status` | Menampilkan status kepemilikan storage |
| `size` | Menampilkan kapasitas SD Card |

**Contoh Penggunaan:**

```bash
# Ubah konfigurasi FTP ke server baru
set_ftp 192.168.0.50 21 admin password123

# Ubah konfigurasi Wi-Fi
set_wifi NamaWiFiBaru PasswordBaru

# Cek file yang tersedia
check

# Upload manual
upload
```

> [!IMPORTANT]
> Setelah menjalankan perintah `set_*`, ESP32 **harus di-restart** agar konfigurasi baru berlaku. Konfigurasi tersimpan permanen di NVS dan tidak akan hilang meski perangkat dimatikan.

---

## 4. Struktur File yang Dikirim

ESP32 akan mencari dan mengirim file dari folder dengan pola nama **`ecg_archive`** di dalam SD Card (`/data/`).

**Struktur SD Card:**
```
/data/
└── ecg_archive/          ← Folder yang dibuat oleh perangkat ECG
    ├── 001.xml           ← Data EKG (XML)
    ├── 001.jpg           ← Gambar laporan (JPG)
    └── 001.bmp           ← Gambar waveform (BMP)
```

**Tipe file yang dikirim:**
| Ekstensi | Tipe | Keterangan |
|----------|------|-----------|
| `.xml` / `.XML` | application/xml | Data rekaman EKG |
| `.jpg` / `.JPG` | image/jpeg | Gambar laporan |
| `.bmp` / `.BMP` | image/bmp | Gambar waveform |

**Perilaku setelah upload:**
- File yang **berhasil dikirim** akan **dihapus otomatis** dari SD Card.
- Jika **semua file** dalam satu folder berhasil dikirim, **folder tersebut** juga akan dihapus.
- Jika ada file yang **gagal**, file tersebut tetap ada dan akan dicoba lagi pada upload berikutnya.

---

## 5. Troubleshooting

### Koneksi FTP Gagal

| Gejala | Kemungkinan Penyebab | Solusi |
|--------|---------------------|--------|
| `Koneksi FTP gagal` | IP Server salah | Cek IP server dengan `ipconfig` (Windows) atau `ip addr` (Linux). Gunakan `set_ftp` untuk mengubah |
| `Koneksi FTP gagal` | Firewall memblokir | Pastikan port 21 dan range port pasif sudah dibuka di firewall server |
| `Login FTP gagal` | Username/Password salah | Periksa kembali kredensial dan gunakan `set_ftp` untuk memperbaikinya |
| File tidak terkirim | Mode pasif tidak aktif | Aktifkan Passive Mode di server FTP |

### File Tidak Ditemukan

| Gejala | Kemungkinan Penyebab | Solusi |
|--------|---------------------|--------|
| `Tidak ada folder ecg_archive` | Perangkat ECG belum menulis file | Pastikan perangkat ECG sudah menyimpan data ke SD Card |
| `Storage sedang dipakai USB Host` | Upload dipicu saat komputer mengakses SD Card | Cabut kabel USB ke komputer terlebih dahulu sebelum upload |

### Testing Koneksi FTP dari PC

Untuk memastikan server FTP berjalan dengan benar sebelum menguji dari ESP32:

**Windows (Command Prompt):**
```cmd
ftp 192.168.1.10
```

**Linux:**
```bash
ftp 192.168.1.10
# Masukkan username dan password saat diminta
put /path/ke/file/test.xml
```

Jika koneksi dari PC berhasil tetapi dari ESP32 gagal, periksa kembali konfigurasi jaringan ESP32 (SSID/Password Wi-Fi).
