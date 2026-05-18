# Simulasi Server Python (Flask) untuk Penerimaan File

Dokumen ini berisi panduan dan kode sumber untuk membuat server simulasi di PC menggunakan Python dan Flask. Server ini berfungsi untuk menerima file yang diunggah oleh ESP32 sebagai pengganti server produksi selama masa pengujian.

## Kode Sumber (`server.py`)

Buat file baru bernama `server.py` dan masukkan kode berikut:

```python
from flask import Flask, request
import os

app = Flask(__name__)

# Folder tempat menyimpan file yang diterima
UPLOAD_FOLDER = './file_diterima'
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# Sesuai dengan URL di ESP32: /api/ecg-1200g/upload
@app.route('/api/ecg-1200g/upload', methods=['POST'])
def upload_file():
    print("\n--- Ada Koneksi Masuk dari ESP32 ---")
    
    # Cek apakah ada file dengan field name 'file'
    if 'file' not in request.files:
        print("Error: Tidak ada part file dalam request")
        return {"status": "failed", "message": "No file part"}, 400
        
    file = request.files['file']
    
    if file.filename == '':
        print("Error: Nama file kosong")
        return {"status": "failed", "message": "No selected file"}, 400
        
    if file:
        filepath = os.path.join(UPLOAD_FOLDER, file.filename)
        file.save(filepath)
        print(f"Sukses: File '{file.filename}' berhasil diterima dan disimpan!")
        print(f"Lokasi: {filepath}")
        
        # Kembalikan HTTP 201 (Created) agar ESP32 menghapus file lokalnya
        return {"status": "success", "message": "File uploaded successfully"}, 201

if __name__ == '__main__':
    print("Server simulasi berjalan di port 3000...")
    print("Menunggu file dari ESP32...")
    # host='0.0.0.0' agar bisa diakses oleh perangkat lain dalam satu jaringan Wi-Fi
    app.run(host='0.0.0.0', port=3000, debug=True)
```

## Cara Menjalankan

### 1. Instalasi Dependensi
Pastikan Anda sudah menginstal Flask. Jika belum, jalankan perintah ini di terminal/CMD:

```bash
pip install flask
```

### 2. Menjalankan Server
Jalankan skrip Python yang telah dibuat:

```bash
python server.py
```

## Hal yang Perlu Diperhatikan

1. **Jaringan:** Pastikan PC dan ESP32 terhubung ke Wi-Fi yang sama.
2. **IP Address:** Pastikan IP PC Anda sesuai dengan yang didefinisikan di `app_main.c` (saat ini `192.168.13.156`).
3. **Firewall:** Jika koneksi ditolak, coba matikan sementara Windows Firewall untuk jaringan lokal Anda.
