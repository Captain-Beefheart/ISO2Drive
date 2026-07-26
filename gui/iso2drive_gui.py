#!/usr/bin/env python3
"""
ISO2Drive GUI — a thin, Etcher-style front-end over the `iso2drive` CLI.

It only orchestrates the command-line tool: pick an image, pick a drive, choose a
mode, preview the exact command (dry-run), then Flash (with --commit). All the real
work, safety guards, and both backends live in the CLI.
"""
import os
import re
import sys
import glob
import shutil
import threading
import queue
import subprocess
import tkinter as tk
from tkinter import filedialog, messagebox

# ---- palette (matches the CLI's teal) --------------------------------------
BG    = "#16181A"
PANEL = "#1E2124"
TEAL  = "#2AD1EA"
FG    = "#E6E6E6"
DIM   = "#9AA0A6"
WARN  = "#E0B341"
ERR   = "#E0574B"
OK    = "#57C77E"
CMD   = "#8AB4F8"

ANSI = re.compile(r"\x1b\[[0-9;]*m")
WIN_ROW = re.compile(r"^\s*(\d+)\s+(\d+)\s*MiB\s+(\S+)\s+(\S+)\s+(.*)$")
PCT = re.compile(r"(\d+)%")

MODES = [
    ("raw",       "Bootable USB — raw (dd)"),
    ("copy",      "Bootable USB — file-copy"),
    ("provision", "Install to disk — frugal (ERASES disk)"),
]


def find_binary():
    # exact names first, then any versioned release binary (highest name wins)
    pats = (["iso2drive.exe", "iso2drive-*.exe"] if os.name == "nt"
            else ["iso2drive", "iso2drive-*", "ISO2Drive-*.AppImage"])
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (here, os.path.dirname(here), os.getcwd()):
        for pat in pats:
            for p in sorted(glob.glob(os.path.join(d, pat)), reverse=True):
                if os.path.isfile(p):
                    return p
    for n in (["iso2drive.exe", "iso2drive"] if os.name == "nt" else ["iso2drive"]):
        w = shutil.which(n)
        if w:
            return w
    return None


def is_elevated():
    try:
        if os.name == "nt":
            import ctypes
            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        return os.geteuid() == 0
    except Exception:
        return False


def _run_capture(args):
    try:
        return subprocess.run(args, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=60).stdout
    except Exception:
        return ""


def drives_windows(binary, show_fixed):
    out = _run_capture([binary, "list-disks"])
    drives = []
    for line in out.splitlines():
        m = WIN_ROW.match(ANSI.sub("", line))
        if not m:
            continue
        num, mib, rem, sysf, model = m.groups()
        system = sysf.upper() == "YES"
        removable = rem.lower() == "yes"
        if system:
            continue  # never offer the Windows system disk
        if not removable and not show_fixed:
            continue
        gib = int(mib) / 1024
        drives.append({
            "arg": num,
            "label": f"[{num}] {model.strip() or 'disk'} — {gib:.1f} GiB"
                     f" ({'removable' if removable else 'fixed'})",
            "removable": removable,
        })
    return drives


def drives_linux(show_fixed):
    out = _run_capture(["lsblk", "-dn", "-o", "NAME,SIZE,TRAN,HOTPLUG,MODEL", "-P"])
    root_disk = ""
    try:
        src = subprocess.run(["findmnt", "-no", "SOURCE", "/"], capture_output=True,
                             text=True, timeout=10).stdout.strip()
        if src:
            root_disk = subprocess.run(["lsblk", "-no", "PKNAME", src], capture_output=True,
                                       text=True, timeout=10).stdout.split("\n")[0].strip()
    except Exception:
        pass
    drives = []
    for line in out.splitlines():
        kv = dict(re.findall(r'(\w+)="([^"]*)"', line))
        name = kv.get("NAME", "")
        if not name or name == root_disk:
            continue
        removable = kv.get("HOTPLUG") == "1" or kv.get("TRAN") == "usb"
        if not removable and not show_fixed:
            continue
        model = kv.get("MODEL", "").strip() or name
        drives.append({
            "arg": f"/dev/{name}",
            "label": f"/dev/{name}  {model} — {kv.get('SIZE','?')}"
                     f" ({'removable' if removable else 'fixed'})",
            "removable": removable,
        })
    return drives


class App(tk.Tk):
    def __init__(self, binary):
        super().__init__()
        self.binary = binary
        self.q = queue.Queue()
        self.proc = None
        self.drives = []
        self.running = False

        self.title("ISO2Drive")
        self.configure(bg=BG)
        self.geometry("820x780")
        self.minsize(720, 680)

        self._build_header()
        self._build_statusbar()          # reserve the bottom BEFORE the expanding body
        body = tk.Frame(self, bg=BG)
        body.pack(fill="both", expand=True, padx=18, pady=(4, 8))
        self._build_image(body)
        self._build_drive(body)
        self._build_mode(body)
        self._build_options(body)
        self._build_actions(body)
        self._build_progress(body)
        self._build_log(body)

        self.refresh_drives()
        self.on_mode_change()
        self.after(60, self._poll)

    # ---- widgets -----------------------------------------------------------
    def _label(self, parent, text, **kw):
        kw.setdefault("bg", BG); kw.setdefault("fg", FG)
        return tk.Label(parent, text=text, **kw)

    def _build_header(self):
        h = tk.Frame(self, bg=BG)
        h.pack(fill="x", padx=18, pady=(10, 2))
        tk.Label(h, text="ISO2Drive", bg=BG, fg=TEAL,
                 font=("Segoe UI", 18, "bold")).pack(side="left")
        tk.Label(h, text="  flash an ISO to a drive, boot it anywhere",
                 bg=BG, fg=DIM, font=("Segoe UI", 10)).pack(side="left", anchor="s", pady=(0, 4))
        strip = tk.Frame(self, bg=BG)
        strip.pack(fill="x", padx=18)
        tk.Label(strip, text="①  IMAGE      ›      ②  DRIVE      ›      ③  FLASH",
                 bg=BG, fg=TEAL, font=("Segoe UI", 11, "bold")).pack(anchor="w", pady=(0, 6))

    def _card(self, parent, title):
        f = tk.Frame(parent, bg=PANEL, highlightbackground="#2A2E31", highlightthickness=1)
        f.pack(fill="x", pady=2)
        inner = tk.Frame(f, bg=PANEL)
        inner.pack(fill="x", padx=12, pady=4)
        tk.Label(inner, text=title, bg=PANEL, fg=DIM,
                 font=("Segoe UI", 9, "bold")).pack(anchor="w")
        return inner

    def _build_image(self, parent):
        c = self._card(parent, "① IMAGE")
        row = tk.Frame(c, bg=PANEL); row.pack(fill="x", pady=(4, 0))
        self.iso_var = tk.StringVar(value="(no image selected)")
        tk.Entry(row, textvariable=self.iso_var, state="readonly", readonlybackground=BG,
                 fg=FG, relief="flat", font=("Segoe UI", 10)).pack(side="left", fill="x", expand=True, ipady=4)
        tk.Button(row, text="Browse…", command=self.browse_iso, bg="#2A2E31", fg=FG,
                  activebackground="#3A3F43", relief="flat", padx=12).pack(side="left", padx=(8, 0))
        self.distro_var = tk.StringVar(value="")
        tk.Label(c, textvariable=self.distro_var, bg=PANEL, fg=DIM,
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(4, 0))

    def _build_drive(self, parent):
        c = self._card(parent, "② DRIVE")
        row = tk.Frame(c, bg=PANEL); row.pack(fill="x", pady=(4, 0))
        self.drive_var = tk.StringVar(value="(no drives)")
        self.drive_menu = tk.OptionMenu(row, self.drive_var, "(no drives)")
        self.drive_menu.configure(bg=BG, fg=FG, activebackground="#2A2E31", relief="flat",
                                  highlightthickness=0, anchor="w")
        self.drive_menu["menu"].configure(bg=PANEL, fg=FG, activebackground=TEAL, activeforeground=BG)
        self.drive_menu.pack(side="left", fill="x", expand=True, ipady=2)
        tk.Button(row, text="↻", command=self.refresh_drives, bg="#2A2E31", fg=FG,
                  activebackground="#3A3F43", relief="flat", width=3).pack(side="left", padx=(8, 0))
        self.show_fixed = tk.BooleanVar(value=False)
        tk.Checkbutton(c, text="show non-removable (internal) disks", variable=self.show_fixed,
                       command=self.refresh_drives, bg=PANEL, fg=DIM, selectcolor=BG,
                       activebackground=PANEL, activeforeground=FG,
                       font=("Segoe UI", 9)).pack(anchor="w", pady=(6, 0))

    def _build_mode(self, parent):
        c = self._card(parent, "③ MODE")
        self.mode = tk.StringVar(value="raw")
        for val, text in MODES:
            tk.Radiobutton(c, text=text, value=val, variable=self.mode, command=self.on_mode_change,
                           bg=PANEL, fg=FG, selectcolor=BG, activebackground=PANEL,
                           activeforeground=TEAL, font=("Segoe UI", 10)).pack(anchor="w", pady=1)

    def _build_options(self, parent):
        self.opt = self._card(parent, "OPTIONS")
        self.verify = tk.BooleanVar(value=False)
        self.fs = tk.StringVar(value="fat32")
        self.persist_on = tk.BooleanVar(value=False)
        self.persist_size = tk.StringVar(value="4G")

        self.opt_raw = tk.Frame(self.opt, bg=PANEL)
        tk.Checkbutton(self.opt_raw, text="verify after writing (read back + compare)",
                       variable=self.verify, bg=PANEL, fg=FG, selectcolor=BG,
                       activebackground=PANEL, font=("Segoe UI", 10)).pack(anchor="w")

        self.opt_copy = tk.Frame(self.opt, bg=PANEL)
        tk.Label(self.opt_copy, text="filesystem:", bg=PANEL, fg=FG,
                 font=("Segoe UI", 10)).pack(side="left")
        for f in ("fat32", "exfat", "ntfs"):
            tk.Radiobutton(self.opt_copy, text=f, value=f, variable=self.fs, bg=PANEL, fg=FG,
                           selectcolor=BG, activebackground=PANEL, activeforeground=TEAL,
                           font=("Segoe UI", 10)).pack(side="left", padx=6)

        self.opt_prov = tk.Frame(self.opt, bg=PANEL)
        tk.Checkbutton(self.opt_prov, text="add persistence (Ubuntu/Debian), size:",
                       variable=self.persist_on, bg=PANEL, fg=FG, selectcolor=BG,
                       activebackground=PANEL, font=("Segoe UI", 10)).pack(side="left")
        tk.Entry(self.opt_prov, textvariable=self.persist_size, width=6, bg=BG, fg=FG,
                 relief="flat", justify="center").pack(side="left", padx=6, ipady=2)

    def _build_actions(self, parent):
        row = tk.Frame(parent, bg=BG); row.pack(fill="x", pady=(10, 4))
        self.btn_flash = tk.Button(row, text="⚡  Flash", command=self.flash, bg=TEAL, fg="#0B1416",
                                   activebackground="#3FE0F2", relief="flat", padx=24, pady=7,
                                   font=("Segoe UI", 11, "bold"))
        self.btn_plan = tk.Button(row, text="Show plan  (dry-run)", command=self.show_plan,
                                  bg="#2A2E31", fg=FG, activebackground="#3A3F43",
                                  relief="flat", padx=14, pady=7, font=("Segoe UI", 10))
        self.btn_cancel = tk.Button(row, text="Cancel", command=self.cancel, bg="#3A2A2A", fg=FG,
                                    activebackground="#4A3535", relief="flat", padx=14, pady=7,
                                    state="disabled")
        self.btn_flash.pack(side="left")
        self.btn_plan.pack(side="left", padx=10)
        self.btn_cancel.pack(side="left")

    def _build_progress(self, parent):
        from tkinter import ttk
        style = ttk.Style(self)
        try:
            style.theme_use("default")
        except Exception:
            pass
        style.configure("teal.Horizontal.TProgressbar", troughcolor=PANEL,
                        background=TEAL, bordercolor=PANEL, lightcolor=TEAL, darkcolor=TEAL)
        self.pbar = ttk.Progressbar(parent, style="teal.Horizontal.TProgressbar",
                                    mode="determinate", maximum=100)
        self.pbar.pack(fill="x", pady=(6, 2))

    def _build_log(self, parent):
        wrap = tk.Frame(parent, bg=BG); wrap.pack(fill="both", expand=True)
        self.log = tk.Text(wrap, bg="#0F1112", fg=FG, insertbackground=FG, relief="flat",
                           font=("Consolas", 9), height=5, width=10, wrap="word")
        sb = tk.Scrollbar(wrap, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.log.pack(side="left", fill="both", expand=True)
        for tag, col in (("info", FG), ("warn", WARN), ("err", ERR),
                         ("cmd", CMD), ("dim", DIM), ("ok", OK)):
            self.log.tag_configure(tag, foreground=col)
        self.log.configure(state="disabled")

    def _build_statusbar(self):
        bar = tk.Frame(self, bg=PANEL); bar.pack(fill="x", side="bottom")
        elev = is_elevated()
        txt = f"engine: {os.path.basename(self.binary)}"
        txt += "   •   elevated ✓" if elev else "   •   NOT elevated — flashing needs admin/root"
        tk.Label(bar, text=txt, bg=PANEL, fg=(OK if elev else WARN),
                 font=("Segoe UI", 9)).pack(side="left", padx=12, pady=4)
        if not elev and os.name == "nt":
            tk.Button(bar, text="Relaunch as Administrator", command=self.relaunch_admin,
                      bg="#2A2E31", fg=FG, activebackground="#3A3F43", relief="flat",
                      font=("Segoe UI", 9)).pack(side="right", padx=8, pady=3)

    # ---- behavior ----------------------------------------------------------
    def browse_iso(self):
        p = filedialog.askopenfilename(title="Select an ISO",
                                       filetypes=[("Disc images", "*.iso *.img"), ("All files", "*.*")])
        if not p:
            return
        self.iso_var.set(p)
        self.distro_var.set("detecting…")
        threading.Thread(target=self._detect, args=(p,), daemon=True).start()

    def _detect(self, path):
        out = _run_capture([self.binary, "detect", path])
        m = re.search(r"detected:\s*(.+)", ANSI.sub("", out))
        self.after(0, self.distro_var.set,
                   ("✓ " + m.group(1).strip()) if m else "unknown family (will rely on loopback.cfg)")

    def refresh_drives(self):
        self.drives = (drives_windows(self.binary, self.show_fixed.get()) if os.name == "nt"
                       else drives_linux(self.show_fixed.get()))
        menu = self.drive_menu["menu"]
        menu.delete(0, "end")
        if not self.drives:
            self.drive_var.set("(no drives found — plug one in, or ↻)")
        else:
            for d in self.drives:
                menu.add_command(label=d["label"], command=lambda v=d["label"]: self.drive_var.set(v))
            self.drive_var.set(self.drives[0]["label"])

    def on_mode_change(self):
        for f in (self.opt_raw, self.opt_copy, self.opt_prov):
            f.pack_forget()
        m = self.mode.get()
        {"raw": self.opt_raw, "copy": self.opt_copy, "provision": self.opt_prov}[m].pack(
            anchor="w", pady=(4, 0), fill="x")

    def _selected_drive(self):
        for d in self.drives:
            if d["label"] == self.drive_var.get():
                return d
        return None

    def build_command(self, commit):
        iso = self.iso_var.get()
        if not iso or not os.path.isfile(iso):
            messagebox.showwarning("ISO2Drive", "Select an ISO image first."); return None
        drv = self._selected_drive()
        if not drv:
            messagebox.showwarning("ISO2Drive", "Select a target drive first."); return None
        m = self.mode.get()
        args = []
        if m == "raw":
            args = ["write-usb", iso, drv["arg"]]
            if self.verify.get():
                args.append("--verify")
            if not drv["removable"]:
                args.append("--force")
        elif m == "copy":
            args = ["copy-usb", iso, drv["arg"], "--fs", self.fs.get()]
            if not drv["removable"]:
                args.append("--force")
        elif m == "provision":
            args = ["provision", drv["arg"], iso]
            if self.persist_on.get() and self.persist_size.get().strip():
                args += ["--persist", self.persist_size.get().strip()]
        if commit:
            args.append("--commit")
        return args

    def show_plan(self):
        args = self.build_command(commit=False)
        if args:
            self._run(args)

    def flash(self):
        args = self.build_command(commit=True)
        if not args:
            return
        drv = self._selected_drive()
        if not messagebox.askyesno(
                "Confirm flash",
                f"This will ERASE:\n\n    {drv['label']}\n\nand run:\n\n    iso2drive "
                + " ".join(args) + "\n\nContinue?", icon="warning", default="no"):
            return
        self._run(args)

    def cancel(self):
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
            except Exception:
                pass

    # ---- subprocess streaming ---------------------------------------------
    def _run(self, args):
        if self.running:
            return
        self._log_clear()
        self._append(f"$ {os.path.basename(self.binary)} " + " ".join(args) + "\n", "cmd")
        try:
            self.proc = subprocess.Popen([self.binary] + args, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, text=True,
                                         encoding="utf-8", errors="replace", bufsize=0)
        except Exception as e:
            self._append(f"failed to launch engine: {e}\n", "err"); return
        self._set_running(True)
        self.pbar.configure(mode="indeterminate"); self.pbar.start(12)
        threading.Thread(target=self._reader, args=(self.proc,), daemon=True).start()

    def _reader(self, proc):
        buf = ""
        while True:
            chunk = proc.stdout.read(256)
            if not chunk:
                break
            buf += chunk
            parts = re.split(r"[\r\n]", buf)
            buf = parts.pop()
            for p in parts:
                self.q.put(("line", p))
        if buf:
            self.q.put(("line", buf))
        self.q.put(("done", proc.wait()))

    def _poll(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "line":
                    self._handle_line(val)
                elif kind == "done":
                    self._finish(val)
        except queue.Empty:
            pass
        self.after(60, self._poll)

    def _handle_line(self, line):
        clean = ANSI.sub("", line)
        m = PCT.search(clean)
        if m and ("writing" in clean or "verify" in clean or "%" in clean):
            try:
                self.pbar.configure(mode="determinate")
                self.pbar.stop()
                self.pbar["value"] = int(m.group(1))
            except Exception:
                pass
        if not clean.strip():
            return
        tag = "info"
        s = clean.lstrip()
        if s.startswith("[warn]"):
            tag = "warn"
        elif s.startswith("[err"):
            tag = "err"
        elif s.startswith("$ ") or s.startswith("+ "):
            tag = "cmd"
        elif s.startswith("(dry-run)"):
            tag = "dim"
        self._append(clean + "\n", tag)

    def _finish(self, rc):
        self.pbar.stop()
        self.pbar.configure(mode="determinate")
        if rc == 0:
            self.pbar["value"] = 100
            self._append("\n✓ done (exit 0)\n", "ok")
        else:
            self.pbar["value"] = 0
            self._append(f"\n✗ engine exited with code {rc}\n", "err")
        self._set_running(False)

    # ---- helpers -----------------------------------------------------------
    def _set_running(self, on):
        self.running = on
        self.btn_flash.configure(state="disabled" if on else "normal")
        self.btn_plan.configure(state="disabled" if on else "normal")
        self.btn_cancel.configure(state="normal" if on else "disabled")

    def _append(self, text, tag="info"):
        self.log.configure(state="normal")
        self.log.insert("end", text, tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _log_clear(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def relaunch_admin(self):
        try:
            import ctypes
            params = " ".join(f'"{a}"' for a in sys.argv)
            ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, params, None, 1)
            self.destroy()
        except Exception as e:
            messagebox.showerror("ISO2Drive", f"Could not relaunch elevated: {e}")


def main():
    binary = find_binary()
    if not binary:
        root = tk.Tk(); root.withdraw()
        messagebox.showerror(
            "ISO2Drive",
            "Could not find the iso2drive engine.\n\nBuild it (make), or put the release\n"
            "binary next to this script or on your PATH.")
        return 1
    App(binary).mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
