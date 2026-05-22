"""
MedLink Dongle — Serial Number Injector
========================================
Aplikasi desktop sederhana untuk menyuntikkan Serial Number (SN)
ke dalam memori NVS ESP32 melalui koneksi USB Serial.

Cara pakai:
1. Colokkan Dongle ke PC via USB (mode Serial/JTAG)
2. Pilih port COM yang sesuai
3. Masukkan nomor seri yang diinginkan (misal: DONGLE-001)
4. Klik "Injeksi SN" dan tunggu konfirmasi
"""

import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext
import serial
import serial.tools.list_ports
import threading
import time


# ─────────────────────────────────────────────────────────
# Konstanta
# ─────────────────────────────────────────────────────────
BAUD_RATE    = 115200
CMD_TIMEOUT  = 5       # detik tunggu respons dari Dongle
SUCCESS_KEYWORD = "SUKSES"
PROMPT_BYTES    = b">"   # Prompt dari esp_console REPL

APP_TITLE   = "MedLink Dongle — SN Injector"
COLOR_BG    = "#1a1a2e"
COLOR_CARD  = "#16213e"
COLOR_ACCENT= "#0f3460"
COLOR_GREEN = "#00d4aa"
COLOR_RED   = "#e94560"
COLOR_TEXT  = "#e0e0e0"
COLOR_MUTED = "#8888aa"
COLOR_BORDER= "#2a2a4e"


# ─────────────────────────────────────────────────────────
# Logika serial (berjalan di thread terpisah)
# ─────────────────────────────────────────────────────────
def inject_sn(port: str, sn: str, on_log, on_done):
    """
    Mengirim perintah 'set_sn <SN>' ke ESP32 via UART.
    on_log(str): callback untuk menampilkan log ke UI
    on_done(bool, str): callback saat selesai (sukses/gagal, pesan)
    """
    try:
        on_log(f"Membuka port {port} ({BAUD_RATE} baud)...")
        with serial.Serial(port, BAUD_RATE, timeout=1) as ser:
            time.sleep(0.5)  # Beri waktu ESP32 mengenali koneksi

            # Kosongkan buffer masuk
            ser.reset_input_buffer()

            # Kirim perintah set_sn
            cmd = f"set_sn {sn}\n"
            on_log(f"Mengirim perintah: {cmd.strip()}")
            ser.write(cmd.encode("utf-8"))

            # Tunggu dan kumpulkan respons selama CMD_TIMEOUT detik
            deadline = time.time() + CMD_TIMEOUT
            response_lines = []
            while time.time() < deadline:
                if ser.in_waiting:
                    line = ser.readline().decode("utf-8", errors="replace").strip()
                    if line:
                        on_log(f"  << {line}")
                        response_lines.append(line)
                        if SUCCESS_KEYWORD in line:
                            on_done(True, f"Serial Number '{sn}' berhasil disimpan!")
                            return
                        if "Gagal" in line or "Error" in line:
                            on_done(False, f"Dongle melaporkan error: {line}")
                            return

        # Timeout
        on_done(False, "Tidak ada respons dari Dongle dalam batas waktu.\n"
                        "Pastikan ESP-IDF console aktif (firmware sudah di-flash & berjalan).")

    except serial.SerialException as e:
        on_done(False, f"Gagal membuka port serial:\n{e}")
    except Exception as e:
        on_done(False, f"Error tidak terduga:\n{e}")


# ─────────────────────────────────────────────────────────
# Kelas UI Utama
# ─────────────────────────────────────────────────────────
class SNInjectorApp(tk.Tk):

    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.resizable(False, False)
        self.configure(bg=COLOR_BG)

        # Pusatkan window
        self.update_idletasks()
        w, h = 560, 620
        x = (self.winfo_screenwidth() - w) // 2
        y = (self.winfo_screenheight() - h) // 2
        self.geometry(f"{w}x{h}+{x}+{y}")

        self._build_ui()
        self._refresh_ports()

    # ── Bangun layout UI ──────────────────────────────────
    def _build_ui(self):
        # === Header ===
        header = tk.Frame(self, bg=COLOR_ACCENT, pady=16)
        header.pack(fill="x")

        tk.Label(header, text="🔧  MedLink Dongle", font=("Segoe UI", 18, "bold"),
                 fg=COLOR_GREEN, bg=COLOR_ACCENT).pack()
        tk.Label(header, text="Serial Number Injector", font=("Segoe UI", 10),
                 fg=COLOR_MUTED, bg=COLOR_ACCENT).pack()

        # === Kartu Konfigurasi ===
        card = tk.Frame(self, bg=COLOR_CARD, padx=24, pady=20,
                        highlightbackground=COLOR_BORDER, highlightthickness=1)
        card.pack(fill="x", padx=20, pady=(20, 0))

        # — Port COM —
        tk.Label(card, text="Port Serial (COM)", font=("Segoe UI", 9, "bold"),
                 fg=COLOR_MUTED, bg=COLOR_CARD).grid(row=0, column=0, sticky="w", pady=(0, 4))

        port_frame = tk.Frame(card, bg=COLOR_CARD)
        port_frame.grid(row=1, column=0, sticky="ew", pady=(0, 16))
        card.columnconfigure(0, weight=1)

        self._port_var = tk.StringVar()
        self._port_combo = ttk.Combobox(port_frame, textvariable=self._port_var,
                                         width=30, state="readonly")
        self._port_combo.pack(side="left", fill="x", expand=True)

        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TCombobox",
                         fieldbackground=COLOR_ACCENT,
                         background=COLOR_ACCENT,
                         foreground=COLOR_TEXT,
                         arrowcolor=COLOR_GREEN,
                         selectbackground=COLOR_ACCENT,
                         selectforeground=COLOR_TEXT)

        tk.Button(port_frame, text="⟳ Refresh", command=self._refresh_ports,
                  bg=COLOR_ACCENT, fg=COLOR_GREEN, activebackground=COLOR_BG,
                  activeforeground=COLOR_GREEN, relief="flat", padx=10, cursor="hand2",
                  font=("Segoe UI", 9)).pack(side="left", padx=(8, 0))

        # — Serial Number —
        tk.Label(card, text="Serial Number yang akan disuntikkan",
                 font=("Segoe UI", 9, "bold"), fg=COLOR_MUTED, bg=COLOR_CARD
                 ).grid(row=2, column=0, sticky="w", pady=(0, 4))

        sn_frame = tk.Frame(card, bg=COLOR_CARD)
        sn_frame.grid(row=3, column=0, sticky="ew")

        self._sn_var = tk.StringVar(value="DONGLE-001")
        sn_entry = tk.Entry(sn_frame, textvariable=self._sn_var,
                            font=("Courier New", 14, "bold"),
                            bg=COLOR_ACCENT, fg=COLOR_GREEN, insertbackground=COLOR_GREEN,
                            relief="flat", width=20)
        sn_entry.pack(side="left", fill="x", expand=True, ipady=8, padx=(0, 8))

        # Tombol +/- untuk auto increment
        tk.Button(sn_frame, text="▲ +1", command=self._increment_sn,
                  bg=COLOR_ACCENT, fg=COLOR_MUTED, activebackground=COLOR_BG,
                  relief="flat", padx=8, cursor="hand2",
                  font=("Segoe UI", 9)).pack(side="left", padx=(0, 4))

        # — Tombol Injeksi —
        self._btn_inject = tk.Button(card, text="⚡  INJEKSI SERIAL NUMBER",
                                      command=self._on_inject_click,
                                      bg=COLOR_GREEN, fg="#000000",
                                      activebackground="#00aa88", activeforeground="#000000",
                                      font=("Segoe UI", 11, "bold"),
                                      relief="flat", padx=20, pady=12, cursor="hand2")
        self._btn_inject.grid(row=4, column=0, sticky="ew", pady=(20, 0))

        # === Status Badge ===
        status_frame = tk.Frame(self, bg=COLOR_BG)
        status_frame.pack(fill="x", padx=20, pady=12)

        self._status_canvas = tk.Canvas(status_frame, width=14, height=14,
                                         bg=COLOR_BG, highlightthickness=0)
        self._status_canvas.pack(side="left")
        self._status_dot = self._status_canvas.create_oval(2, 2, 12, 12, fill=COLOR_MUTED)

        self._status_label = tk.Label(status_frame, text="Siap — Pilih port dan masukkan SN",
                                       font=("Segoe UI", 9), fg=COLOR_MUTED, bg=COLOR_BG)
        self._status_label.pack(side="left", padx=8)

        # === Log Terminal ===
        tk.Label(self, text="Log Komunikasi Serial", font=("Segoe UI", 9, "bold"),
                 fg=COLOR_MUTED, bg=COLOR_BG).pack(anchor="w", padx=20)

        self._log = scrolledtext.ScrolledText(self, height=14, wrap="word",
                                               bg=COLOR_CARD, fg=COLOR_TEXT,
                                               font=("Courier New", 9),
                                               relief="flat", padx=10, pady=10,
                                               insertbackground=COLOR_TEXT,
                                               state="disabled",
                                               highlightbackground=COLOR_BORDER,
                                               highlightthickness=1)
        self._log.pack(fill="both", expand=True, padx=20, pady=(4, 16))

        tk.Button(self, text="Bersihkan Log", command=self._clear_log,
                  bg=COLOR_BG, fg=COLOR_MUTED, activebackground=COLOR_ACCENT,
                  relief="flat", font=("Segoe UI", 8), cursor="hand2"
                  ).pack(anchor="e", padx=20, pady=(0, 12))

    # ── Helper: Refresh Port COM ──────────────────────────
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self._port_combo["values"] = ports
        if ports:
            self._port_combo.current(0)
            self._log_append(f"Port tersedia: {', '.join(ports)}")
        else:
            self._port_var.set("")
            self._log_append("Tidak ada port serial yang terdeteksi.")

    # ── Helper: Auto-increment SN (misal 001 → 002) ───────
    def _increment_sn(self):
        sn = self._sn_var.get().strip()
        # Cari angka di bagian paling kanan
        i = len(sn)
        while i > 0 and sn[i - 1].isdigit():
            i -= 1
        prefix = sn[:i]
        num_str = sn[i:]
        if num_str:
            new_num = int(num_str) + 1
            new_sn = f"{prefix}{str(new_num).zfill(len(num_str))}"
            self._sn_var.set(new_sn)
        else:
            self._sn_var.set(sn + "-001")

    # ── Helper: Tulis ke log terminal ─────────────────────
    def _log_append(self, text: str):
        self._log.configure(state="normal")
        timestamp = time.strftime("%H:%M:%S")
        self._log.insert("end", f"[{timestamp}] {text}\n")
        self._log.see("end")
        self._log.configure(state="disabled")

    def _clear_log(self):
        self._log.configure(state="normal")
        self._log.delete("1.0", "end")
        self._log.configure(state="disabled")

    # ── Helper: Set status badge ───────────────────────────
    def _set_status(self, text: str, color: str):
        self._status_canvas.itemconfig(self._status_dot, fill=color)
        self._status_label.config(text=text, fg=color)

    # ── Aksi: Klik tombol injeksi ─────────────────────────
    def _on_inject_click(self):
        port = self._port_var.get().strip()
        sn   = self._sn_var.get().strip()

        if not port:
            messagebox.showerror("Pilih Port", "Silakan pilih port COM terlebih dahulu.")
            return
        if not sn:
            messagebox.showerror("SN Kosong", "Masukkan Serial Number yang akan disuntikkan.")
            return
        if len(sn) > 31:
            messagebox.showerror("SN Terlalu Panjang", "Maksimal 31 karakter.")
            return

        # Konfirmasi sebelum eksekusi
        confirm = messagebox.askyesno(
            "Konfirmasi Injeksi",
            f"Anda akan menyuntikkan:\n\n  Serial Number : {sn}\n  Port          : {port}\n\nLanjutkan?"
        )
        if not confirm:
            return

        # Nonaktifkan tombol saat proses berlangsung
        self._btn_inject.config(state="disabled", text="⏳  Menyuntikkan...")
        self._set_status("Menghubungi Dongle...", "#f0a500")
        self._log_append(f"─── Mulai Injeksi: SN={sn} Port={port} ───")

        def _thread_target():
            inject_sn(port, sn,
                      on_log=lambda msg: self.after(0, self._log_append, msg),
                      on_done=lambda ok, msg: self.after(0, self._on_inject_done, ok, msg, sn))

        threading.Thread(target=_thread_target, daemon=True).start()

    # ── Callback: Setelah proses injeksi selesai ──────────
    def _on_inject_done(self, success: bool, message: str, sn: str):
        self._btn_inject.config(state="normal", text="⚡  INJEKSI SERIAL NUMBER")

        if success:
            self._set_status(f"✅  Berhasil: {sn}", COLOR_GREEN)
            self._log_append(f"✅  {message}")
            messagebox.showinfo(
                "Injeksi Berhasil!",
                f"Serial Number berhasil disimpan ke NVS.\n\n"
                f"  SN : {sn}\n\n"
                f"Cabut & pasang ulang Dongle agar perubahan diterapkan.\n"
                f"Klik tombol [▲ +1] jika Anda ingin menaikkan nomor seri untuk alat berikutnya."
            )
        else:
            self._set_status("❌  Injeksi gagal", COLOR_RED)
            self._log_append(f"❌  {message}")
            messagebox.showerror("Injeksi Gagal", message)


# ─────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────
if __name__ == "__main__":
    app = SNInjectorApp()
    app.mainloop()
