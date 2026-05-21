# MedLink Dongle — SN Injector

Aplikasi Python dengan UI sederhana untuk menyuntikkan Serial Number (SN) ke memori NVS ESP32 melalui koneksi USB Serial tanpa perlu menggunakan `idf.py` atau `parttool.py`.

## Persyaratan

- Python 3.8 atau lebih baru
- Library `pyserial`

## Instalasi

```bash
cd tools/sn_injector
pip install -r requirements.txt
```

## Cara Menjalankan

```bash
python sn_injector.py
```

## Cara Pakai

1. **Colokkan Dongle** ke PC via kabel USB (pastikan firmware MedLink sudah berjalan).
2. Klik **⟳ Refresh** untuk mendeteksi port COM otomatis.
3. **Pilih port COM** yang sesuai (biasanya `COM3`, `COM4`, dsb.).
4. Ketikkan **Serial Number** yang diinginkan (contoh: `DONGLE-001`).
5. Klik tombol **⚡ INJEKSI SERIAL NUMBER**.
6. Tunggu konfirmasi **"Berhasil"** di layar dan log terminal.
7. **Cabut dan pasang ulang** Dongle agar nama WiFi dan Flashdisk berubah sesuai SN baru.
8. Klik **▲ +1** untuk auto-increment ke SN berikutnya (`DONGLE-002`), lalu ulangi proses untuk Dongle berikutnya.

## Catatan Penting

- Aplikasi ini **tidak akan** mempengaruhi firmware ESP-IDF. Ia hanya berkomunikasi melalui console UART bawaan.
- SN tersimpan di partisi NVS dan **tidak akan terhapus saat OTA Update**.
- Jika Dongle tidak merespons, pastikan ESP-IDF REPL console aktif (LED Dongle menyala normal, bukan mode bootloader).
