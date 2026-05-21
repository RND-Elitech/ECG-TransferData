# Panduan Penyuntikan Serial Number (SN) Dongle

> **Untuk:** Tim Perakitan & Produksi  
> **Tingkat:** Non-Developer  
> **Dokumen ini menjelaskan** cara memberikan nomor seri unik (`DONGLE-001`, `DONGLE-002`, dst.) kepada setiap unit Dongle menggunakan aplikasi PC sederhana — **tanpa perlu memahami pemrograman apapun.**

---

## Apa itu Serial Number (SN)?

Setiap unit Dongle yang diproduksi harus memiliki identitas unik. Identitas ini digunakan untuk:
- Membedakan setiap dongle satu sama lain di jaringan WiFi rumah sakit.
- Melacak unit mana yang terhubung ke pasien mana di sistem manajemen.
- Menampilkan nama unik pada dashboard dan nama WiFi Dongle (contoh: `DONGLE-005`).

Proses memberikan identitas ini disebut **"Penyuntikan Serial Number"** dan cukup dilakukan **satu kali** saat perakitan. Nomor ini akan **tetap tersimpan permanen** di dalam chip Dongle meskipun Dongle di-update firmware-nya.

---

## Persyaratan

Sebelum memulai, pastikan hal-hal berikut sudah terpenuhi:

| Kebutuhan | Keterangan |
| :--- | :--- |
| **PC Windows** | Windows 10 atau 11 (64-bit) |
| **Python 3.8+** | Sudah terinstal di PC. Cek dengan mengetik `python --version` di Command Prompt. |
| **Kabel USB** | Kabel USB-C (untuk menghubungkan Dongle ke PC) |
| **Firmware Aktif** | Dongle sudah di-flash firmware Dongle dan dalam kondisi **menyala/berjalan normal** (LED aktif) |
| **Aplikasi SN Injector** | File `sn_injector.py` di folder `tools/sn_injector/` dalam folder proyek |

> ⚠️ **Penting:** Dongle harus dalam kondisi **MENYALA dan menjalankan firmware**, bukan dalam mode flash/bootloader (LED merah solid). Tanyakan ke tim developer jika tidak yakin.

---

## Instalasi Awal (Hanya Sekali)

Langkah ini hanya perlu dilakukan **satu kali** di PC yang akan digunakan untuk produksi.

### Langkah 1: Buka Command Prompt

Tekan tombol `Win + R` di keyboard, ketik `cmd`, lalu tekan `Enter`.

### Langkah 2: Instal Library yang Dibutuhkan

Salin dan tempel perintah berikut ke dalam Command Prompt, lalu tekan `Enter`:

```
pip install pyserial
```

Tunggu hingga proses selesai. Jika muncul tulisan `Successfully installed pyserial`, instalasi berhasil.

---

## Cara Menjalankan Aplikasi

### Langkah 1: Navigasi ke Folder Aplikasi

Di Command Prompt, ketik perintah berikut (sesuaikan dengan lokasi folder proyek Anda):

```
cd "D:\Dokumen Rivaldi\ECG1200G\VsCode\clone\ECG-TransferData\ECG-TransferData\tools\sn_injector"
```

### Langkah 2: Jalankan Aplikasi

```
python sn_injector.py
```

Sebuah jendela aplikasi dengan tampilan gelap akan muncul di layar Anda.

---

## Prosedur Penyuntikan SN (Per Unit Dongle)

Ikuti langkah-langkah ini secara berurutan untuk **setiap unit Dongle** yang akan diberi nomor seri.

---

### ① Colokkan Dongle ke PC

Sambungkan Dongle ke port USB PC menggunakan kabel USB-C. Tunggu beberapa detik hingga Windows mengenali perangkat (bunyi "ding" dari Windows).

---

### ② Pilih Port COM yang Benar

Di aplikasi, klik tombol **⟳ Refresh**. Daftar port COM yang tersedia akan muncul di kotak dropdown.

Pilih port yang sesuai dengan Dongle Anda (biasanya bernama `COM3`, `COM4`, `COM5`, dsb.).

> 💡 **Tips:** Jika ada banyak pilihan COM dan Anda tidak tahu mana yang benar, coba cabut Dongle, klik Refresh, perhatikan daftar yang ada. Lalu colok lagi Dongle, klik Refresh lagi. Port COM baru yang muncul adalah milik Dongle Anda.

---

### ③ Masukkan Nomor Seri

Di kolom **"Serial Number yang akan disuntikkan"**, hapus teks yang ada dan ketikkan nomor seri untuk unit ini.

**Format yang direkomendasikan:** `DONGLE-001`

Nomor seri akan otomatis diubah menjadi HURUF KAPITAL.

> ⚠️ **Aturan Nomor Seri:**
> - Maksimal **31 karakter**
> - Boleh menggunakan huruf, angka, dan tanda hubung (`-`)
> - **Tidak boleh ada spasi**
> - Contoh yang benar: `DONGLE-001`, `DGL-B0001`, `ECG-RS001`

---

### ④ Klik Tombol Injeksi

Klik tombol hijau besar bertuliskan **⚡ INJEKSI SERIAL NUMBER**.

Sebuah kotak konfirmasi akan muncul menampilkan ringkasan:

```
Anda akan menyuntikkan:
  Serial Number : DONGLE-001
  Port          : COM4

Lanjutkan?
```

Periksa kembali, lalu klik **Ya**.

---

### ⑤ Tunggu Konfirmasi Berhasil

Proses berlangsung selama **2–5 detik**. Perhatikan panel **"Log Komunikasi Serial"** di bagian bawah aplikasi untuk melihat progres.

**Jika BERHASIL**, akan muncul jendela pemberitahuan:
```
✅ Injeksi Berhasil!
Serial Number berhasil disimpan ke NVS.

  SN : DONGLE-001

Cabut & pasang ulang Dongle agar perubahan diterapkan.
Lalu klik [▲ +1] untuk menyiapkan SN berikutnya.
```

---

### ⑥ Cabut dan Pasang Ulang Dongle

Cabut kabel USB dari Dongle, tunggu 2 detik, lalu colokkan kembali. Setelah menyala ulang, Dongle akan menggunakan Serial Number baru sebagai identitasnya.

**Verifikasi (Opsional):** Buka daftar jaringan WiFi di HP atau laptop. Pastikan muncul jaringan bernama `DONGLE-001` (atau SN yang baru saja Anda suntikkan).

---

### ⑦ Lanjut ke Unit Berikutnya

Aplikasi akan otomatis menaikkan angka SN ke **DONGLE-002**. Anda juga bisa mengklik tombol **▲ +1** secara manual untuk menyesuaikan.

Ulangi langkah **①** hingga **⑥** untuk unit Dongle berikutnya.

---

## Tabel Pencatatan Produksi

Gunakan tabel berikut untuk mencatat setiap unit yang telah disuntikkan SN-nya:

| No. | Serial Number | Tanggal Injeksi | Teknisi | Keterangan |
| :-: | :--- | :--- | :--- | :--- |
| 1 | DONGLE-001 | | | |
| 2 | DONGLE-002 | | | |
| 3 | DONGLE-003 | | | |
| 4 | DONGLE-004 | | | |
| 5 | DONGLE-005 | | | |

---

## Mengatasi Masalah Umum

### ❌ "Tidak ada port serial yang terdeteksi"

| Kemungkinan Penyebab | Solusi |
| :--- | :--- |
| Kabel USB rusak atau hanya kabel charger (tanpa data) | Ganti dengan kabel USB yang mendukung transfer data |
| Driver USB belum terinstal | Download dan instal driver CP210x atau CH340 dari internet |
| Dongle belum menyala | Pastikan firmware sudah di-flash dan LED Dongle aktif |

### ❌ "Tidak ada respons dari Dongle dalam batas waktu"

| Kemungkinan Penyebab | Solusi |
| :--- | :--- |
| Salah memilih port COM | Coba pilih port COM yang lain dari daftar dropdown |
| Dongle dalam mode bootloader | Cabut daya, tahan tombol BOOT, colok USB, lepas tombol BOOT setelah 2 detik. Atau tanyakan tim developer. |
| Firmware belum terinstal | Hubungi tim developer untuk melakukan flash firmware terlebih dahulu |

### ❌ "Error membuka NVS"

Hubungi tim developer. Ini menandakan firmware Dongle yang digunakan belum mendukung perintah `set_sn`.

---

## Pertanyaan Umum

**Q: Apakah SN akan hilang jika Dongle di-update firmware (OTA)?**
A: **Tidak.** Serial Number disimpan di partisi memori khusus (NVS) yang terpisah dari area firmware. Proses OTA Update hanya mengganti firmware, bukan NVS.

**Q: Bagaimana jika saya salah ketik SN dan sudah terlanjur diinjeksi?**
A: Colok kembali Dongle tersebut, masukkan SN yang benar di aplikasi, dan klik Injeksi lagi. SN lama akan **tertimpa** oleh SN baru.

**Q: Apakah bisa menyuntikkan SN yang sama ke dua Dongle?**
A: Secara teknis bisa, tetapi **jangan dilakukan**. Dua Dongle dengan SN yang sama akan menyebabkan konflik nama di jaringan WiFi dan sistem manajemen.

**Q: Apa yang terjadi jika Dongle tidak pernah disuntikkan SN?**
A: Dongle akan otomatis menggunakan nama berbasis MAC Address (contoh: `Dongle-A1B2C3`). Dongle tetap berfungsi normal, hanya namanya kurang ramah untuk tim IT rumah sakit.

---

> **Hubungi tim developer** jika menemukan masalah yang tidak tercantum di panduan ini.
