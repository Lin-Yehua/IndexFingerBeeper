from __future__ import annotations

import ctypes
import json
import os
import queue
import shutil
import string
import subprocess
import sys
import threading
import time
from pathlib import Path
from tkinter import filedialog, messagebox
import tkinter as tk
from tkinter import ttk


MAIN_ENV = "4d_systems_esp32s3_gen4_r8n16"
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0
AUTO_PORT_LABEL = "自动选择"
BAUD_RATES = ("921600", "460800", "115200")


def runtime_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


APP_DIR = runtime_dir()
SOURCE_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SOURCE_DIR.parent
CONFIG_PATH = APP_DIR / "downloader_config.json"
RESOURCE_DIR = Path(getattr(sys, "_MEIPASS", SOURCE_DIR))

DRIVE_TYPE_NAMES = {
    0: "未知",
    1: "无根目录",
    2: "可移动盘",
    3: "本地磁盘",
    4: "网络盘",
    5: "光驱",
    6: "RAM盘",
}


def is_windows() -> bool:
    return os.name == "nt"


def default_main_firmware() -> str:
    packaged = APP_DIR / "firmware.bin"
    if packaged.exists():
        return "firmware.bin"
    build = PROJECT_DIR / ".pio" / "build" / MAIN_ENV / "firmware.bin"
    if build.exists():
        return str(build)
    return "firmware.bin"


def default_hardware_firmware() -> str:
    for name in ("hardware_test.bin", "hardware_test.bin.bin", "hardware.bin", "test_firmware.bin"):
        if (APP_DIR / name).exists():
            return name
    return "hardware_test.bin"


def default_update_dir() -> str:
    packaged = APP_DIR / "Update"
    if packaged.exists():
        return "Update"
    source = PROJECT_DIR / "FATFSdownload" / "Update"
    if source.exists():
        return str(source)
    return "Update"


def support_bin_path(name: str) -> Path:
    external = APP_DIR / name
    if external.exists():
        return external
    bundled = RESOURCE_DIR / name
    if bundled.exists():
        return bundled
    build_dir = PROJECT_DIR / ".pio" / "build" / MAIN_ENV
    if name in ("bootloader.bin", "partitions.bin"):
        return build_dir / name
    if name == "boot_app0.bin":
        return Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / name
    return external


def default_config() -> dict:
    return {
        "serial_port": AUTO_PORT_LABEL,
        "auto_serial": True,
        "upload_baud": "921600",
        "monitor_baud": "115200",
        "main_firmware": default_main_firmware(),
        "hardware_firmware": default_hardware_firmware(),
        "app_offset": "0x10000",
        "update_dir": default_update_dir(),
        "usb_drive": "",
        "usb_label": "TESTTFT",
        "format_before_copy": True,
    }


def load_config() -> dict:
    data = default_config()
    if CONFIG_PATH.exists():
        try:
            with CONFIG_PATH.open("r", encoding="utf-8") as handle:
                saved = json.load(handle)
            if isinstance(saved, dict):
                data.update(saved)
        except Exception:
            pass
    return data


def save_config(data: dict) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with CONFIG_PATH.open("w", encoding="utf-8") as handle:
        json.dump(data, handle, ensure_ascii=False, indent=2)


def resolve_app_path(value: str) -> Path:
    path = Path(value.strip().strip('"'))
    if not path.is_absolute():
        path = APP_DIR / path
    return path


def normalize_drive(value: str) -> str:
    value = value.strip().replace("/", "\\")
    if not value:
        return ""
    if "[" in value and ":" in value:
        value = value.split()[0]
    if len(value) == 1 and value.isalpha():
        return f"{value.upper()}:\\"
    if len(value) >= 2 and value[1] == ":":
        return f"{value[0].upper()}:\\"
    return value


def drive_for_format(value: str) -> str:
    drive = normalize_drive(value)
    if len(drive) >= 2 and drive[1] == ":":
        return drive[:2]
    return drive


def system_drive() -> str:
    return os.environ.get("SystemDrive", "C:").upper()


def get_drive_type(root: str) -> int:
    if not is_windows():
        return 0
    return int(ctypes.windll.kernel32.GetDriveTypeW(root))


def get_volume_label(root: str) -> str:
    if not is_windows():
        return ""
    name = ctypes.create_unicode_buffer(261)
    fs_name = ctypes.create_unicode_buffer(261)
    serial = ctypes.c_uint(0)
    max_component = ctypes.c_uint(0)
    flags = ctypes.c_uint(0)
    ok = ctypes.windll.kernel32.GetVolumeInformationW(
        root,
        name,
        len(name),
        ctypes.byref(serial),
        ctypes.byref(max_component),
        ctypes.byref(flags),
        fs_name,
        len(fs_name),
    )
    return name.value if ok else ""


def port_sort_key(port: str) -> tuple[str, int | str]:
    prefix = "".join(ch for ch in port if not ch.isdigit()).upper()
    digits = "".join(ch for ch in port if ch.isdigit())
    return prefix, int(digits) if digits else port.upper()


def list_serial_port_infos() -> list[dict]:
    try:
        from serial.tools import list_ports

        infos = []
        for port in list_ports.comports():
            infos.append(
                {
                    "device": port.device,
                    "description": port.description or "",
                    "hwid": port.hwid or "",
                    "manufacturer": port.manufacturer or "",
                    "product": port.product or "",
                }
            )
        if infos:
            return sorted(infos, key=lambda item: port_sort_key(item["device"]))
    except Exception:
        pass

    ports: list[str] = []
    if is_windows():
        try:
            import winreg

            key_path = r"HARDWARE\DEVICEMAP\SERIALCOMM"
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path) as key:
                index = 0
                while True:
                    try:
                        _, value, _ = winreg.EnumValue(key, index)
                    except OSError:
                        break
                    if isinstance(value, str):
                        ports.append(value)
                    index += 1
        except OSError:
            pass
    return [{"device": port, "description": "", "hwid": "", "manufacturer": "", "product": ""} for port in sorted(set(ports), key=port_sort_key)]


def list_serial_ports() -> list[str]:
    return [item["device"] for item in list_serial_port_infos()]


def list_candidate_drives(include_fixed: bool = False) -> list[dict]:
    if not is_windows():
        return []
    drives: list[dict] = []
    for letter in string.ascii_uppercase:
        root = f"{letter}:\\"
        drive_type = get_drive_type(root)
        if drive_type == 1 or not os.path.exists(root):
            continue
        if root[:2].upper() == system_drive():
            continue
        if drive_type == 2 or (include_fixed and drive_type == 3):
            label = get_volume_label(root)
            type_name = DRIVE_TYPE_NAMES.get(drive_type, "未知")
            drives.append(
                {
                    "root": root,
                    "label": label,
                    "type": drive_type,
                    "display": f"{root}  {label or '未命名'}  [{type_name}]",
                }
            )
    return drives


def esptool_command_base() -> list[str]:
    if getattr(sys, "frozen", False):
        return [sys.executable, "--esptool"]
    return [sys.executable, str(Path(__file__).resolve()), "--esptool"]


def run_esptool_cli(argv: list[str]) -> int:
    import esptool

    try:
        result = esptool.main(argv)
        return int(result or 0)
    except SystemExit as exc:
        return int(exc.code or 0)


class QueueWriter:
    def __init__(self, log_queue: queue.Queue[str]) -> None:
        self.log_queue = log_queue
        self.pending = ""

    def write(self, text: str) -> int:
        if not text:
            return 0
        self.pending += text
        while "\n" in self.pending:
            line, self.pending = self.pending.split("\n", 1)
            self.log_queue.put(line + "\n")
        return len(text)

    def flush(self) -> None:
        if self.pending:
            self.log_queue.put(self.pending)
            self.pending = ""


class EspDownloaderApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.config = load_config()
        self.log_queue: queue.Queue[str] = queue.Queue()
        self.current_process: subprocess.Popen | None = None
        self.monitor_thread: threading.Thread | None = None
        self.monitor_stop = threading.Event()
        self.esptool_lock = threading.Lock()
        self.busy = False
        self.task_buttons: list[ttk.Button] = []

        self.root.title("ESP 独立烧录工具")
        self.root.geometry("1000x720")
        self.root.minsize(860, 600)

        self.make_variables()
        self.build_ui()
        self.refresh_ports()
        self.refresh_drives()
        self.poll_log_queue()
        self.log("ESP 独立烧录工具已启动。")
        self.log(f"运行目录: {APP_DIR}")
        self.log("不依赖 PlatformIO；请把 firmware.bin、hardware_test.bin 和 Update 文件夹放在程序目录。")
        self.log("烧录会自动写入 bootloader、分区表、boot_app0 和所选程序 bin。")

    def make_variables(self) -> None:
        self.port_var = tk.StringVar(value=str(self.config.get("serial_port", "")))
        self.auto_serial_var = tk.BooleanVar(value=bool(self.config.get("auto_serial", True)))
        self.upload_baud_var = tk.StringVar(value=str(self.config.get("upload_baud", "921600")))
        self.monitor_baud_var = tk.StringVar(value=str(self.config.get("monitor_baud", "115200")))
        self.main_firmware_var = tk.StringVar(value=str(self.config.get("main_firmware", default_main_firmware())))
        self.hardware_firmware_var = tk.StringVar(value=str(self.config.get("hardware_firmware", default_hardware_firmware())))
        self.app_offset_var = tk.StringVar(value=str(self.config.get("app_offset", "0x10000")))
        self.update_dir_var = tk.StringVar(value=str(self.config.get("update_dir", default_update_dir())))
        self.usb_drive_var = tk.StringVar(value=str(self.config.get("usb_drive", "")))
        self.usb_label_var = tk.StringVar(value=str(self.config.get("usb_label", "TESTTFT")))
        self.format_before_copy_var = tk.BooleanVar(value=bool(self.config.get("format_before_copy", True)))

    def build_ui(self) -> None:
        style = ttk.Style()
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("Primary.TButton", font=("Microsoft YaHei UI", 11, "bold"), padding=(14, 9))
        style.configure("Danger.TButton", foreground="#9f1239")
        style.configure("Header.TLabel", font=("Microsoft YaHei UI", 16, "bold"))

        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(2, weight=1)

        header = ttk.Frame(self.root, padding=(14, 12, 14, 6))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        ttk.Label(header, text="ESP 固件烧录与 MSD FatFS 写入", style="Header.TLabel").grid(row=0, column=0, sticky="w")
        self.status_var = tk.StringVar(value="空闲")
        ttk.Label(header, textvariable=self.status_var).grid(row=0, column=1, sticky="e")

        notebook = ttk.Notebook(self.root)
        notebook.grid(row=1, column=0, sticky="ew", padx=14, pady=(0, 8))
        main_tab = ttk.Frame(notebook, padding=12)
        settings_tab = ttk.Frame(notebook, padding=12)
        advanced_tab = ttk.Frame(notebook, padding=12)
        notebook.add(main_tab, text="下载")
        notebook.add(settings_tab, text="设置")
        notebook.add(advanced_tab, text="高级功能")

        self.build_main_tab(main_tab)
        self.build_settings_tab(settings_tab)
        self.build_advanced_tab(advanced_tab)
        self.build_log_panel()

    def build_main_tab(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(1, weight=1)

        ttk.Label(parent, text="串口").grid(row=0, column=0, sticky="w", padx=(0, 8), pady=4)
        self.port_combo = ttk.Combobox(parent, textvariable=self.port_var, width=18)
        self.port_combo.grid(row=0, column=1, sticky="w", pady=4)
        ttk.Checkbutton(parent, text="自动", variable=self.auto_serial_var, command=self.on_auto_serial_changed).grid(
            row=0, column=2, sticky="w", padx=(8, 0), pady=4
        )
        ttk.Button(parent, text="自动选择", command=self.select_serial_port_now).grid(row=0, column=3, sticky="w", padx=8, pady=4)
        ttk.Button(parent, text="刷新串口", command=self.refresh_ports).grid(row=0, column=4, sticky="w", padx=(0, 8), pady=4)

        ttk.Label(parent, text="烧录波特率").grid(row=1, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Combobox(parent, textvariable=self.upload_baud_var, values=BAUD_RATES, width=12, state="readonly").grid(
            row=1, column=1, sticky="w", pady=4
        )

        ttk.Label(parent, text="监视波特率").grid(row=1, column=2, sticky="e", padx=(16, 8), pady=4)
        ttk.Combobox(parent, textvariable=self.monitor_baud_var, values=BAUD_RATES, width=12, state="readonly").grid(
            row=1, column=3, sticky="w", pady=4
        )

        actions = ttk.Frame(parent)
        actions.grid(row=2, column=0, columnspan=7, sticky="ew", pady=(12, 4))
        actions.columnconfigure((0, 1), weight=1)
        btn_main = ttk.Button(actions, text="下载主程序", style="Primary.TButton", command=self.start_download_main)
        btn_test = ttk.Button(actions, text="下载硬件测试程序", style="Primary.TButton", command=self.start_download_hardware)
        btn_main.grid(row=0, column=0, sticky="ew", padx=(0, 8))
        btn_test.grid(row=0, column=1, sticky="ew", padx=(8, 0))
        self.task_buttons.extend([btn_main, btn_test])

        monitors = ttk.Frame(parent)
        monitors.grid(row=3, column=0, columnspan=7, sticky="ew", pady=(8, 0))
        ttk.Button(monitors, text="启动串口监视器", command=self.start_serial_monitor).grid(row=0, column=0, padx=(0, 8))
        ttk.Button(monitors, text="停止串口监视器", command=self.stop_serial_monitor).grid(row=0, column=1, padx=(0, 8))
        ttk.Button(monitors, text="停止当前任务", command=self.stop_current_task).grid(row=0, column=2, padx=(0, 8))
        ttk.Button(monitors, text="清空日志", command=self.clear_log).grid(row=0, column=3, padx=(0, 8))
        ttk.Button(monitors, text="保存日志", command=self.save_log).grid(row=0, column=4, padx=(0, 8))

        info = (
            "主程序下载：烧录主程序 bin，然后提示连接 USB MSD，格式化为 FAT32 并复制 Update 文件夹。\n"
            "硬件测试程序下载：只烧录硬件测试 bin，不写入 MSD。"
        )
        ttk.Label(parent, text=info, wraplength=900, foreground="#475569").grid(
            row=4, column=0, columnspan=7, sticky="ew", pady=(12, 0)
        )

    def build_settings_tab(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(1, weight=1)
        parent.columnconfigure(4, weight=1)

        self.add_path_row(parent, 0, "主程序 bin", self.main_firmware_var, self.pick_main_firmware)
        self.add_path_row(parent, 1, "硬件测试 bin", self.hardware_firmware_var, self.pick_hardware_firmware)
        self.add_path_row(parent, 2, "Update 文件夹", self.update_dir_var, self.pick_update_dir)
        self.add_entry(parent, 3, 0, "程序偏移", self.app_offset_var)
        self.add_baud_row(parent, 3, 3, "烧录波特率", self.upload_baud_var)
        self.add_baud_row(parent, 4, 0, "监视波特率", self.monitor_baud_var)
        self.add_entry(parent, 4, 3, "MSD卷标", self.usb_label_var)

        ttk.Checkbutton(parent, text="自动选择串口", variable=self.auto_serial_var, command=self.on_auto_serial_changed).grid(
            row=5, column=0, columnspan=2, sticky="w", pady=(8, 4)
        )
        ttk.Button(parent, text="立即自动选择", command=self.select_serial_port_now).grid(row=5, column=2, sticky="w", padx=8, pady=(8, 4))

        ttk.Label(parent, text="MSD盘符").grid(row=6, column=0, sticky="w", pady=6, padx=(0, 8))
        self.usb_combo = ttk.Combobox(parent, textvariable=self.usb_drive_var)
        self.usb_combo.grid(row=6, column=1, sticky="ew", pady=6)
        ttk.Button(parent, text="刷新MSD", command=self.refresh_drives).grid(row=6, column=2, sticky="w", padx=8, pady=6)
        ttk.Checkbutton(parent, text="主程序下载后格式化MSD并复制Update", variable=self.format_before_copy_var).grid(
            row=7, column=0, columnspan=4, sticky="w", pady=(8, 4)
        )

        note = "默认从 exe 所在目录读取 firmware.bin、hardware_test.bin、Update；也可以在这里选择绝对路径。"
        ttk.Label(parent, text=note, wraplength=880, foreground="#475569").grid(
            row=8, column=0, columnspan=5, sticky="ew", pady=(8, 4)
        )

        buttons = ttk.Frame(parent)
        buttons.grid(row=9, column=0, columnspan=5, sticky="e", pady=(12, 0))
        ttk.Button(buttons, text="保存设置", command=self.save_settings).grid(row=0, column=0, padx=(0, 8))
        ttk.Button(buttons, text="重新载入设置", command=self.reload_settings).grid(row=0, column=1)

    def build_advanced_tab(self, parent: ttk.Frame) -> None:
        parent.columnconfigure((0, 1), weight=1)
        btn_erase = ttk.Button(parent, text="擦 Flash", style="Danger.TButton", command=self.start_erase_flash)
        btn_format = ttk.Button(parent, text="格式化 MSD 为 FATFS", command=self.start_format_msd)
        btn_erase.grid(row=0, column=0, sticky="ew", padx=(0, 8), pady=(0, 10))
        btn_format.grid(row=0, column=1, sticky="ew", padx=(8, 0), pady=(0, 10))
        self.task_buttons.extend([btn_erase, btn_format])

        note = "高级功能只保留擦 Flash 和格式化 MSD。格式化会清空选中盘符，请确认盘符无误。"
        ttk.Label(parent, text=note, wraplength=880, foreground="#475569").grid(row=1, column=0, columnspan=2, sticky="ew")

    def build_log_panel(self) -> None:
        frame = ttk.Frame(self.root, padding=(14, 0, 14, 14))
        frame.grid(row=2, column=0, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(1, weight=1)
        ttk.Label(frame, text="监视器 / 日志输出").grid(row=0, column=0, sticky="w", pady=(0, 6))
        self.log_text = tk.Text(
            frame,
            height=18,
            wrap="word",
            state="disabled",
            bg="#111827",
            fg="#e5e7eb",
            insertbackground="#e5e7eb",
            font=("Consolas", 10),
            relief="flat",
            padx=10,
            pady=8,
        )
        scrollbar = ttk.Scrollbar(frame, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scrollbar.set)
        self.log_text.grid(row=1, column=0, sticky="nsew")
        scrollbar.grid(row=1, column=1, sticky="ns")

    def add_entry(self, parent: ttk.Frame, row: int, column: int, label: str, variable: tk.StringVar) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=column, sticky="w", pady=6, padx=(0, 8))
        ttk.Entry(parent, textvariable=variable).grid(row=row, column=column + 1, sticky="ew", pady=6, padx=(0, 8))

    def add_baud_row(self, parent: ttk.Frame, row: int, column: int, label: str, variable: tk.StringVar) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=column, sticky="w", pady=6, padx=(0, 8))
        ttk.Combobox(parent, textvariable=variable, values=BAUD_RATES, width=12, state="readonly").grid(
            row=row, column=column + 1, sticky="w", pady=6, padx=(0, 8)
        )

    def add_path_row(self, parent: ttk.Frame, row: int, label: str, variable: tk.StringVar, command) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=6, padx=(0, 8))
        ttk.Entry(parent, textvariable=variable).grid(row=row, column=1, columnspan=3, sticky="ew", pady=6)
        ttk.Button(parent, text="浏览", command=command).grid(row=row, column=4, sticky="e", padx=(8, 0), pady=6)

    def collect_config_from_ui(self) -> dict:
        self.config.update(
            {
                "serial_port": self.port_var.get().strip(),
                "auto_serial": bool(self.auto_serial_var.get()),
                "upload_baud": self.normalize_baud(self.upload_baud_var.get(), "921600"),
                "monitor_baud": self.normalize_baud(self.monitor_baud_var.get(), "115200"),
                "main_firmware": self.main_firmware_var.get().strip() or "firmware.bin",
                "hardware_firmware": self.hardware_firmware_var.get().strip() or "hardware_test.bin",
                "app_offset": self.app_offset_var.get().strip() or "0x10000",
                "update_dir": self.update_dir_var.get().strip() or "Update",
                "usb_drive": normalize_drive(self.usb_drive_var.get().strip()),
                "usb_label": self.usb_label_var.get().strip() or "TESTTFT",
                "format_before_copy": bool(self.format_before_copy_var.get()),
            }
        )
        return dict(self.config)

    def apply_config_to_ui(self) -> None:
        self.port_var.set(str(self.config.get("serial_port", AUTO_PORT_LABEL)))
        self.auto_serial_var.set(bool(self.config.get("auto_serial", True)))
        self.upload_baud_var.set(self.normalize_baud(str(self.config.get("upload_baud", "921600")), "921600"))
        self.monitor_baud_var.set(self.normalize_baud(str(self.config.get("monitor_baud", "115200")), "115200"))
        self.main_firmware_var.set(str(self.config.get("main_firmware", default_main_firmware())))
        self.hardware_firmware_var.set(str(self.config.get("hardware_firmware", default_hardware_firmware())))
        self.app_offset_var.set(str(self.config.get("app_offset", "0x10000")))
        self.update_dir_var.set(str(self.config.get("update_dir", default_update_dir())))
        self.usb_drive_var.set(str(self.config.get("usb_drive", "")))
        self.usb_label_var.set(str(self.config.get("usb_label", "TESTTFT")))
        self.format_before_copy_var.set(bool(self.config.get("format_before_copy", True)))

    def normalize_baud(self, value: str, fallback: str) -> str:
        value = str(value).strip()
        return value if value in BAUD_RATES else fallback

    def pick_main_firmware(self) -> None:
        self.pick_bin(self.main_firmware_var, "选择主程序 firmware.bin")

    def pick_hardware_firmware(self) -> None:
        self.pick_bin(self.hardware_firmware_var, "选择硬件测试 bin")

    def pick_bin(self, variable: tk.StringVar, title: str) -> None:
        path = filedialog.askopenfilename(title=title, filetypes=[("BIN 文件", "*.bin"), ("所有文件", "*.*")], initialdir=str(APP_DIR))
        if path:
            variable.set(self.relative_or_absolute(path))

    def pick_update_dir(self) -> None:
        path = filedialog.askdirectory(title="选择 Update 文件夹", initialdir=str(APP_DIR))
        if path:
            variable_path = self.relative_or_absolute(path)
            self.update_dir_var.set(variable_path)

    def relative_or_absolute(self, path: str) -> str:
        resolved = Path(path).resolve()
        try:
            return str(resolved.relative_to(APP_DIR))
        except ValueError:
            return str(resolved)

    def save_settings(self) -> None:
        save_config(self.collect_config_from_ui())
        self.log(f"设置已保存: {CONFIG_PATH}")
        messagebox.showinfo("设置", "设置已保存。")

    def reload_settings(self) -> None:
        self.config = load_config()
        self.apply_config_to_ui()
        self.refresh_ports()
        self.refresh_drives()
        self.log("设置已重新载入。")

    def refresh_ports(self) -> None:
        ports = list_serial_ports()
        self.port_combo["values"] = [AUTO_PORT_LABEL] + ports
        if self.auto_serial_var.get() and self.port_var.get() not in ports:
            self.port_var.set(AUTO_PORT_LABEL)
        elif not self.port_var.get() and ports:
            self.port_var.set(ports[0])
        self.log(f"串口刷新完成: {', '.join(ports) if ports else '未发现串口'}")

    def on_auto_serial_changed(self) -> None:
        if self.auto_serial_var.get():
            self.port_var.set(AUTO_PORT_LABEL)
            self.log("已启用自动串口选择。")
        else:
            ports = list_serial_ports()
            if self.port_var.get() == AUTO_PORT_LABEL and ports:
                self.port_var.set(ports[0])
            self.log("已关闭自动串口选择。")

    def select_serial_port_now(self) -> None:
        self.collect_config_from_ui()
        try:
            port = self.auto_select_serial_port()
        except RuntimeError as exc:
            messagebox.showwarning("自动选择串口", str(exc))
            return
        messagebox.showinfo("自动选择串口", f"已选择 {port}")

    def auto_select_serial_port(self) -> str:
        infos = list_serial_port_infos()
        if not infos:
            raise RuntimeError("未发现可用串口。")
        scored = sorted(infos, key=lambda item: (-self.serial_score(item), port_sort_key(item["device"])))
        selected = scored[0]
        score = self.serial_score(selected)
        if len(scored) > 1:
            details = ", ".join(f"{item['device']}({self.serial_score(item)})" for item in scored)
            self.log(f"自动串口候选: {details}")
        port = selected["device"]
        desc = selected.get("description") or selected.get("product") or selected.get("hwid") or "无描述"
        if len(scored) > 1 and score == 0:
            self.log(f"未识别到明显ESP串口，按端口顺序选择: {port}")
        else:
            self.log(f"自动选择串口: {port} - {desc}")
        self.config["serial_port"] = port
        self.call_on_ui(lambda: self.port_var.set(port))
        return port

    def serial_score(self, info: dict) -> int:
        text = " ".join(
            str(info.get(key, "")) for key in ("device", "description", "hwid", "manufacturer", "product")
        ).lower()
        score = 0
        weighted_keywords = {
            "esp32": 100,
            "esp": 90,
            "jtag": 80,
            "usb serial": 70,
            "uart": 65,
            "cp210": 60,
            "ch340": 60,
            "ch910": 60,
            "wch": 55,
            "silicon labs": 55,
            "ftdi": 50,
            "prolific": 45,
            "usb": 20,
        }
        for keyword, weight in weighted_keywords.items():
            if keyword in text:
                score += weight
        return score

    def get_serial_port_for_task(self) -> str:
        configured = self.config.get("serial_port", "").strip()
        if self.config.get("auto_serial", True) or not configured or configured == AUTO_PORT_LABEL:
            return self.auto_select_serial_port()
        available = list_serial_ports()
        if configured not in available:
            self.log(f"配置串口 {configured} 当前不可用，尝试自动选择。")
            return self.auto_select_serial_port()
        return configured

    def refresh_drives(self) -> None:
        drives = list_candidate_drives(include_fixed=False)
        values = [drive["display"] for drive in drives]
        self.usb_combo["values"] = values
        current = normalize_drive(self.usb_drive_var.get())
        if current:
            for drive in drives:
                if drive["root"].upper() == current.upper():
                    self.usb_drive_var.set(drive["display"])
                    break
        elif len(drives) == 1:
            self.usb_drive_var.set(drives[0]["display"])
        self.log(f"MSD刷新完成: {', '.join(values) if values else '未发现可移动MSD'}")

    def log(self, message: str) -> None:
        self.log_queue.put(f"[{time.strftime('%H:%M:%S')}] {message}\n")

    def poll_log_queue(self) -> None:
        try:
            while True:
                message = self.log_queue.get_nowait()
                self.log_text.configure(state="normal")
                self.log_text.insert("end", message)
                self.log_text.see("end")
                self.log_text.configure(state="disabled")
        except queue.Empty:
            pass
        self.root.after(80, self.poll_log_queue)

    def clear_log(self) -> None:
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def save_log(self) -> None:
        path = filedialog.asksaveasfilename(
            title="保存日志",
            defaultextension=".txt",
            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")],
        )
        if not path:
            return
        Path(path).write_text(self.log_text.get("1.0", "end-1c"), encoding="utf-8")
        self.log(f"日志已保存: {path}")

    def call_on_ui(self, func):
        if threading.current_thread() is threading.main_thread():
            return func()
        result: dict = {}
        event = threading.Event()

        def wrapped() -> None:
            try:
                result["value"] = func()
            except BaseException as exc:
                result["error"] = exc
            finally:
                event.set()

        self.root.after(0, wrapped)
        event.wait()
        if "error" in result:
            raise result["error"]
        return result.get("value")

    def set_busy(self, busy: bool, status: str = "空闲") -> None:
        self.busy = busy
        state = "disabled" if busy else "normal"
        for button in self.task_buttons:
            button.configure(state=state)
        self.status_var.set(status)

    def start_task(self, title: str, worker) -> None:
        if self.busy:
            messagebox.showwarning("任务正在运行", "请等待当前任务结束，或先停止当前任务。")
            return
        self.collect_config_from_ui()
        self.set_busy(True, title)
        self.log(f"开始任务: {title}")
        thread = threading.Thread(target=self.task_wrapper, args=(title, worker), daemon=True)
        thread.start()

    def task_wrapper(self, title: str, worker) -> None:
        try:
            worker()
            self.log(f"任务完成: {title}")
        except RuntimeError as exc:
            self.log(f"任务失败: {exc}")
            self.call_on_ui(lambda: messagebox.showerror("任务失败", str(exc)))
        except Exception as exc:
            self.log(f"任务异常: {exc}")
            self.call_on_ui(lambda: messagebox.showerror("任务异常", str(exc)))
        finally:
            self.current_process = None
            self.root.after(0, lambda: self.set_busy(False, "空闲"))

    def start_download_main(self) -> None:
        self.start_task("下载主程序", self.download_main_worker)

    def start_download_hardware(self) -> None:
        self.start_task("下载硬件测试程序", self.download_hardware_worker)

    def start_erase_flash(self) -> None:
        if not messagebox.askyesno("擦Flash确认", "将擦除芯片 Flash，是否继续？"):
            return
        self.start_task("擦 Flash", self.erase_flash_worker)

    def start_format_msd(self) -> None:
        self.start_task("格式化 MSD 为 FATFS", self.format_msd_worker)

    def stop_current_task(self) -> None:
        process = self.current_process
        if process and process.poll() is None:
            self.log("正在停止当前任务...")
            process.terminate()
        else:
            self.log("当前没有可强制停止的外部任务；烧录任务会在当前步骤结束后返回。")

    def run_process(self, args: list[str], title: str, check: bool = True, input_text: str | None = None) -> int:
        self.log(f"{title}: {subprocess.list2cmdline(args)}")
        try:
            process = subprocess.Popen(
                args,
                cwd=str(APP_DIR),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                stdin=subprocess.PIPE if input_text is not None else subprocess.DEVNULL,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=CREATE_NO_WINDOW,
            )
        except FileNotFoundError as exc:
            raise RuntimeError(f"找不到命令: {args[0]}") from exc

        self.current_process = process
        if input_text is not None and process.stdin is not None:
            try:
                process.stdin.write(input_text)
                process.stdin.close()
            except OSError:
                pass
        assert process.stdout is not None
        for line in process.stdout:
            self.log_queue.put(line)
        rc = process.wait()
        self.current_process = None
        if check and rc != 0:
            raise RuntimeError(f"{title}失败，退出码 {rc}")
        return rc

    def esptool(self, args: list[str], title: str) -> None:
        import esptool

        self.log(f"{title}: esptool {subprocess.list2cmdline(args)}")
        writer = QueueWriter(self.log_queue)
        with self.esptool_lock:
            old_stdout = sys.stdout
            old_stderr = sys.stderr
            sys.stdout = writer
            sys.stderr = writer
            try:
                result = esptool.main(args)
                rc = int(result or 0)
            except SystemExit as exc:
                rc = int(exc.code or 0)
            finally:
                writer.flush()
                sys.stdout = old_stdout
                sys.stderr = old_stderr
        if rc != 0:
            raise RuntimeError(f"{title}失败，退出码 {rc}")

    def flash_pairs_for_firmware(self, firmware: Path) -> list[str]:
        app_offset = self.config.get("app_offset", "0x10000").strip() or "0x10000"
        pairs = [
            ("0x0000", support_bin_path("bootloader.bin")),
            ("0x8000", support_bin_path("partitions.bin")),
            ("0xe000", support_bin_path("boot_app0.bin")),
            (app_offset, firmware),
        ]
        args: list[str] = []
        for offset, image in pairs:
            if not image.exists():
                raise RuntimeError(f"缺少启动烧录文件: {image}")
            args.extend([offset, str(image)])
        return args

    def burn_firmware(self, firmware_value: str, title: str) -> None:
        port = self.get_serial_port_for_task()
        firmware = resolve_app_path(firmware_value)
        if not firmware.exists():
            raise RuntimeError(f"找不到固件文件: {firmware}")
        baud = self.normalize_baud(self.config.get("upload_baud", "921600"), "921600")
        self.stop_serial_monitor()
        flash_args = self.flash_pairs_for_firmware(firmware)
        self.esptool(
            [
                "--chip",
                "esp32s3",
                "--port",
                port,
                "--baud",
                baud,
                "--before",
                "default_reset",
                "--after",
                "hard_reset",
                "write_flash",
                "-z",
                *flash_args,
            ],
            title,
        )

    def download_main_worker(self) -> None:
        self.burn_firmware(self.config.get("main_firmware", "firmware.bin"), "烧录主程序 bin")
        if not self.config.get("format_before_copy", True):
            return
        should_continue = self.call_on_ui(
            lambda: messagebox.askyesno(
                "连接MSD",
                "主程序烧录完成。\n\n请让设备进入USB MSD模式并连接到电脑，然后继续格式化并复制 Update 文件夹。",
            )
        )
        if not should_continue:
            self.log("用户跳过 MSD 写入。")
            return
        drive = self.choose_msd_drive()
        if not drive:
            self.log("未选择 MSD，跳过 Update 写入。")
            return
        confirmed = self.call_on_ui(
            lambda: messagebox.askyesno("确认格式化", f"即将格式化 {drive} 为 FAT32，盘内数据会被清空。是否继续？")
        )
        if not confirmed:
            self.log("用户取消 MSD 格式化。")
            return
        self.format_drive(drive)
        self.copy_update_to_drive(drive)
        self.call_on_ui(lambda: messagebox.showinfo("完成", "Update 文件夹复制完成，可以拔出MSD。"))

    def download_hardware_worker(self) -> None:
        self.burn_firmware(self.config.get("hardware_firmware", "hardware_test.bin"), "烧录硬件测试 bin")

    def erase_flash_worker(self) -> None:
        port = self.get_serial_port_for_task()
        baud = self.normalize_baud(self.config.get("upload_baud", "921600"), "921600")
        self.stop_serial_monitor()
        self.esptool(
            [
                "--chip",
                "esp32s3",
                "--port",
                port,
                "--baud",
                baud,
                "--before",
                "default_reset",
                "--after",
                "hard_reset",
                "erase_flash",
            ],
            "擦除芯片 Flash",
        )

    def format_msd_worker(self) -> None:
        drive = self.choose_msd_drive()
        if not drive:
            self.log("未选择 MSD。")
            return
        confirmed = self.call_on_ui(
            lambda: messagebox.askyesno("确认格式化", f"即将格式化 {drive} 为 FAT32，盘内数据会被清空。是否继续？")
        )
        if not confirmed:
            self.log("用户取消 MSD 格式化。")
            return
        self.format_drive(drive)

    def choose_msd_drive(self) -> str:
        configured = normalize_drive(self.config.get("usb_drive", ""))
        if configured and os.path.exists(configured):
            return configured
        drives = list_candidate_drives(include_fixed=False)
        if len(drives) == 1:
            drive = drives[0]["root"]
            ok = self.call_on_ui(lambda: messagebox.askyesno("选择MSD", f"检测到 {drives[0]['display']}，是否使用？"))
            return drive if ok else ""
        if not drives:
            retry = self.call_on_ui(lambda: messagebox.askretrycancel("未发现MSD", "未发现可移动MSD。请连接后点击重试。"))
            if retry:
                return self.choose_msd_drive()
            return ""
        return self.call_on_ui(lambda: self.drive_dialog(drives))

    def drive_dialog(self, drives: list[dict]) -> str:
        dialog = tk.Toplevel(self.root)
        dialog.title("选择MSD")
        dialog.transient(self.root)
        dialog.grab_set()
        dialog.resizable(False, False)
        dialog.columnconfigure(0, weight=1)
        ttk.Label(dialog, text="请选择要格式化的MSD盘符：").grid(row=0, column=0, sticky="w", padx=14, pady=(14, 6))
        selected = tk.StringVar(value=drives[0]["display"])
        combo = ttk.Combobox(dialog, textvariable=selected, values=[d["display"] for d in drives], width=54, state="readonly")
        combo.grid(row=1, column=0, sticky="ew", padx=14, pady=6)
        result = {"drive": ""}

        def ok() -> None:
            for drive in drives:
                if drive["display"] == selected.get():
                    result["drive"] = drive["root"]
                    break
            dialog.destroy()

        def cancel() -> None:
            dialog.destroy()

        buttons = ttk.Frame(dialog)
        buttons.grid(row=2, column=0, sticky="e", padx=14, pady=(8, 14))
        ttk.Button(buttons, text="确定", command=ok).grid(row=0, column=0, padx=(0, 8))
        ttk.Button(buttons, text="取消", command=cancel).grid(row=0, column=1)
        dialog.bind("<Return>", lambda _event: ok())
        dialog.bind("<Escape>", lambda _event: cancel())
        dialog.wait_window()
        return result["drive"]

    def validate_drive_is_safe(self, drive: str) -> str:
        drive = normalize_drive(drive)
        if not drive or len(drive) < 3:
            raise RuntimeError("MSD盘符无效。")
        if drive[:2].upper() == system_drive():
            raise RuntimeError("拒绝操作系统盘。")
        if not os.path.exists(drive):
            raise RuntimeError(f"MSD不存在: {drive}")
        drive_type = get_drive_type(drive)
        if drive_type != 2:
            ok = self.call_on_ui(
                lambda: messagebox.askyesno(
                    "磁盘类型确认",
                    f"{drive} 不是可移动盘，类型为 {DRIVE_TYPE_NAMES.get(drive_type, '未知')}。\n仍然继续吗？",
                )
            )
            if not ok:
                raise RuntimeError("用户取消非可移动盘操作。")
        return drive

    def format_drive(self, drive: str) -> None:
        if not is_windows():
            raise RuntimeError("自动格式化当前只支持 Windows。")
        drive = self.validate_drive_is_safe(drive)
        label = self.config.get("usb_label", "TESTTFT").strip() or "TESTTFT"
        current_label = get_volume_label(drive)
        args = ["format.com", drive_for_format(drive), "/FS:FAT32", "/Q", "/Y", f"/V:{label}"]
        self.run_process(args, f"格式化 {drive} 为 FAT32", input_text=f"{current_label}\n")

    def copy_update_to_drive(self, drive: str) -> None:
        drive = self.validate_drive_is_safe(drive)
        update_dir = resolve_app_path(self.config.get("update_dir", "Update"))
        if not update_dir.exists() or not update_dir.is_dir():
            raise RuntimeError(f"找不到 Update 文件夹: {update_dir}")
        destination = Path(drive) / "Update"
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(update_dir, destination)
        self.log(f"已复制 Update: {update_dir} -> {destination}")

    def start_serial_monitor(self) -> None:
        if self.monitor_thread and self.monitor_thread.is_alive():
            messagebox.showinfo("串口监视器", "串口监视器已经在运行。")
            return
        self.collect_config_from_ui()
        try:
            port = self.get_serial_port_for_task()
        except RuntimeError as exc:
            messagebox.showwarning("串口监视器", str(exc))
            return
        try:
            baud = int(self.normalize_baud(self.config.get("monitor_baud", "115200"), "115200"))
        except ValueError:
            messagebox.showwarning("串口监视器", "监视波特率无效。")
            return
        self.monitor_stop.clear()
        self.monitor_thread = threading.Thread(target=self.monitor_worker, args=(port, baud), daemon=True)
        self.monitor_thread.start()
        self.status_var.set("串口监视中")
        self.log(f"启动串口监视器: {port} @ {baud}")

    def monitor_worker(self, port: str, baud: int) -> None:
        try:
            import serial

            with serial.Serial(port, baudrate=baud, timeout=0.2) as ser:
                while not self.monitor_stop.is_set():
                    data = ser.readline()
                    if data:
                        self.log_queue.put(data.decode("utf-8", errors="replace"))
        except Exception as exc:
            self.log(f"串口监视器异常: {exc}")
        finally:
            self.root.after(0, lambda: self.status_var.set("空闲" if not self.busy else self.status_var.get()))
            self.log("串口监视器已停止。")

    def stop_serial_monitor(self) -> None:
        if self.monitor_thread and self.monitor_thread.is_alive():
            self.monitor_stop.set()
            self.log("正在停止串口监视器...")
        else:
            self.log("串口监视器未运行。")


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "--esptool":
        return run_esptool_cli(sys.argv[2:])

    root = tk.Tk()
    app = EspDownloaderApp(root)

    def on_close() -> None:
        if app.busy and not messagebox.askyesno("退出", "任务仍在运行，确定要退出吗？"):
            return
        app.stop_serial_monitor()
        process = app.current_process
        if process and process.poll() is None:
            process.terminate()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
