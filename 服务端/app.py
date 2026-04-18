#!/usr/bin/env python3
import math
import os
import random
import re
import threading
import time
from typing import Dict, List, Optional, Tuple

from flask import (
    Flask,
    abort,
    jsonify,
    redirect,
    render_template,
    request,
    send_from_directory,
    url_for,
)
from werkzeug.serving import make_server

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
WEB_UI_DIR = os.path.join(BASE_DIR, "ui")

if os.name == "nt":
    DEFAULT_DATA_DIR = os.path.join(BASE_DIR, "data")
else:
    DEFAULT_DATA_DIR = "/opt/csv_api/data"

DATA_DIR = os.environ.get("CSV_API_DATA_DIR", DEFAULT_DATA_DIR)
MAIN_FILE = os.path.join(DATA_DIR, "text.csv")
PENDING_FILE = os.path.join(DATA_DIR, "pending.csv")
MESSAGEDATA_DIR = os.environ.get("CSV_API_MESSAGE_DIR", os.path.join(BASE_DIR, "messagedata"))

MAX_TEXT_LEN = 140
PAGE_SIZE = 10
MAX_BOTTLE_MESSAGE_LEN = 100
MAX_BOTTLE_USERNAME_LEN = 20
DEVICE_MESSAGE_LIMIT = 20
UUID_LEN = 18

UUID_RE = re.compile(r"^UUID\d{14}$")
MAC_COMPACT_RE = re.compile(r"^[0-9A-F]{12}$")

API_HOST = os.environ.get("CSV_API_HOST", "0.0.0.0")
API_HTTP_PORT = int(os.environ.get("API_HTTP_PORT", "80"))
# HTTP-only mode:
# - prefer WEB_HTTP_PORT
# - fallback to legacy WEB_HTTPS_PORT for compatibility
WEB_HTTP_PORT = int(os.environ.get("WEB_HTTP_PORT", os.environ.get("WEB_HTTPS_PORT", "8080")))

NOTICE_LEVELS = {"info", "ok", "warn", "error"}

api_app = Flask("csv_api_http")
web_app = Flask("csv_api_web", template_folder=WEB_UI_DIR, static_folder=None)


def ensure_data_dir() -> None:
    os.makedirs(DATA_DIR, exist_ok=True)
    os.makedirs(MESSAGEDATA_DIR, exist_ok=True)
    for path in [MAIN_FILE, PENDING_FILE]:
        if not os.path.exists(path):
            with open(path, "a", encoding="utf-8"):
                pass


def read_nonempty_lines(file_path: str) -> List[str]:
    lines: List[str] = []
    if not os.path.isfile(file_path):
        return lines

    with open(file_path, "r", encoding="utf-8-sig") as f:
        for line in f:
            text = line.strip()
            if text:
                lines.append(text)
    return lines


def append_line(file_path: str, text: str) -> None:
    safe_text = text.replace("\r", " ").replace("\n", " ").strip()
    with open(file_path, "a", encoding="utf-8", newline="") as f:
        f.write(safe_text + "\n")


def write_lines(file_path: str, lines: List[str]) -> None:
    with open(file_path, "w", encoding="utf-8", newline="") as f:
        for line in lines:
            safe_text = line.replace("\r", " ").replace("\n", " ").strip()
            if safe_text:
                f.write(safe_text + "\n")


def normalize_uuid(raw: Optional[str]) -> str:
    value = (raw or "").strip().upper()
    if UUID_RE.fullmatch(value):
        return value
    return ""


def normalize_mac_compact(raw: Optional[str]) -> str:
    value = (raw or "").strip().upper()
    if not value:
        return ""
    compact = "".join(ch for ch in value if ch not in ":- ")
    if MAC_COMPACT_RE.fullmatch(compact):
        return compact
    return ""


def format_mac_compact_to_colon(mac_compact: str) -> str:
    if not MAC_COMPACT_RE.fullmatch(mac_compact):
        return ""
    return ":".join(mac_compact[i : i + 2] for i in range(0, 12, 2))


def build_device_file_name(uuid: str, mac_compact: str) -> str:
    return f"{uuid}+{mac_compact}.csv"


def device_file_path(uuid: str, mac_compact: str) -> str:
    return os.path.join(MESSAGEDATA_DIR, build_device_file_name(uuid, mac_compact))


def parse_device_file_name(file_name: str) -> Optional[Dict[str, str]]:
    if not file_name.lower().endswith(".csv"):
        return None
    stem = file_name[:-4]
    if "+" not in stem:
        return None
    uuid_part, mac_part = stem.split("+", 1)
    uuid = normalize_uuid(uuid_part)
    mac_compact = normalize_mac_compact(mac_part)
    if not uuid or not mac_compact:
        return None
    return {
        "uuid": uuid,
        "mac_compact": mac_compact,
        "mac": format_mac_compact_to_colon(mac_compact),
        "file_name": file_name,
        "path": os.path.join(MESSAGEDATA_DIR, file_name),
    }


def list_known_devices() -> List[Dict[str, str]]:
    ensure_data_dir()
    devices: List[Dict[str, str]] = []
    try:
        names = sorted(os.listdir(MESSAGEDATA_DIR))
    except FileNotFoundError:
        return devices

    for name in names:
        parsed = parse_device_file_name(name)
        if not parsed:
            continue
        devices.append(parsed)
    return devices


def collect_device_overview() -> List[Dict[str, object]]:
    devices = list_known_devices()
    overview: List[Dict[str, object]] = []
    for item in devices:
        messages = read_nonempty_lines(item["path"])
        overview.append(
            {
                "uuid": item["uuid"],
                "mac": item["mac"],
                "file_name": item["file_name"],
                "queue_count": len(messages),
                "messages": messages,
            }
        )
    return overview


def ensure_device_file(uuid: str, mac_compact: str) -> str:
    ensure_data_dir()
    path = device_file_path(uuid, mac_compact)
    if not os.path.exists(path):
        with open(path, "a", encoding="utf-8"):
            pass
    return path


def append_device_message(path: str, message: str) -> None:
    lines = read_nonempty_lines(path)
    lines.append(message)
    if len(lines) > DEVICE_MESSAGE_LIMIT:
        lines = lines[-DEVICE_MESSAGE_LIMIT:]
    write_lines(path, lines)


def pop_oldest_device_message(path: str) -> Optional[str]:
    if not os.path.isfile(path):
        return None
    lines = read_nonempty_lines(path)
    if not lines:
        return None
    oldest = lines[0]
    write_lines(path, lines[1:])
    return oldest


def parse_positive_int(raw_value: Optional[str], default: int = 1) -> int:
    try:
        value = int((raw_value or "").strip())
        return value if value > 0 else default
    except (TypeError, ValueError):
        return default


def paginate(items: List[str], page: int, page_size: int) -> Tuple[List[str], int, int, int]:
    total_items = len(items)
    total_pages = max(1, math.ceil(total_items / page_size))
    current_page = min(max(page, 1), total_pages)
    start_index = (current_page - 1) * page_size
    end_index = start_index + page_size
    return items[start_index:end_index], current_page, total_pages, start_index


def notice_from_request() -> Optional[Dict[str, str]]:
    text = (request.args.get("msg") or "").strip()
    if not text:
        return None

    level = (request.args.get("level") or "info").strip().lower()
    if level not in NOTICE_LEVELS:
        level = "info"

    return {"text": text, "level": level}


def redirect_with_notice(
    endpoint: str,
    *,
    page: Optional[int] = None,
    msg: str = "",
    level: str = "info",
    anchor: Optional[str] = None,
):
    query: Dict[str, str] = {}
    if page is not None:
        query["page"] = str(page)

    clean_msg = msg.strip()
    if clean_msg:
        query["msg"] = clean_msg
        query["level"] = level if level in NOTICE_LEVELS else "info"

    target = url_for(endpoint, **query)
    if anchor:
        target = f"{target}#{anchor}"

    return redirect(target)


class ServerThread(threading.Thread):
    def __init__(self, app: Flask, host: str, port: int, ssl_context=None):
        super().__init__(daemon=True)
        self._server = make_server(host, port, app, ssl_context=ssl_context)

    def run(self) -> None:
        self._server.serve_forever()

    def shutdown(self) -> None:
        self._server.shutdown()


@api_app.route("/", methods=["GET"])
def api_index():
    return jsonify(
        {
            "ok": True,
            "service": "csv_api",
            "api": ["/random", "/health"],
            "web": ["/admin", "/submit", "/review", "/bottle"],
            "web_http_port": WEB_HTTP_PORT,
        }
    ), 200


@api_app.route("/random", methods=["GET"])
def random_text():
    ensure_data_dir()
    req_uuid = normalize_uuid(request.args.get("uuid"))
    req_mac_compact = normalize_mac_compact(request.args.get("mac"))
    if req_uuid and req_mac_compact:
        device_path = ensure_device_file(req_uuid, req_mac_compact)
        queued = pop_oldest_device_message(device_path)
        if queued:
            return jsonify({"ok": True, "text": queued, "source": "device_queue"}), 200

    lines = read_nonempty_lines(MAIN_FILE)
    if not lines:
        return jsonify({"ok": False, "error": "No approved text found"}), 404

    return jsonify({"ok": True, "text": random.choice(lines), "source": "main_random"}), 200


@api_app.route("/health", methods=["GET"])
def health():
    return jsonify({"ok": True}), 200


@web_app.route("/assets/<path:filename>", methods=["GET"])
def assets(filename: str):
    if not filename.endswith((".css", ".js")):
        abort(404)
    return send_from_directory(WEB_UI_DIR, filename)


@web_app.route("/", methods=["GET"])
def home_page():
    return redirect(url_for("admin_page"))


@web_app.route("/admin", methods=["GET"])
def admin_page():
    ensure_data_dir()

    approved_lines = read_nonempty_lines(MAIN_FILE)
    pending_count = len(read_nonempty_lines(PENDING_FILE))
    device_overview = collect_device_overview()

    requested_page = parse_positive_int(request.args.get("page"), 1)
    page_lines, page, total_pages, start_index = paginate(approved_lines, requested_page, PAGE_SIZE)

    rows = []
    for offset, line in enumerate(page_lines):
        global_index = start_index + offset
        rows.append(
            {
                "global_index": global_index,
                "display_index": global_index + 1,
                "text": line,
            }
        )

    return render_template(
        "admin.html",
        max_text_len=MAX_TEXT_LEN,
        page=page,
        total_pages=total_pages,
        rows=rows,
        total_approved=len(approved_lines),
        pending_count=pending_count,
        device_overview=device_overview,
        main_file=MAIN_FILE,
        notice=notice_from_request(),
    )


@web_app.route("/admin-add", methods=["POST"])
def admin_add():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    text = (request.form.get("text") or "").strip()

    if not text:
        return redirect_with_notice("admin_page", page=current_page, msg="Text cannot be empty.", level="warn")

    if len(text) > MAX_TEXT_LEN:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg=f"Single text length must be <= {MAX_TEXT_LEN}.",
            level="warn",
        )

    append_line(MAIN_FILE, text)

    approved_count = len(read_nonempty_lines(MAIN_FILE))
    target_index = max(approved_count - 1, 0)
    target_page = (target_index // PAGE_SIZE) + 1

    return redirect_with_notice(
        "admin_page",
        page=target_page,
        msg="Added to approved library.",
        level="ok",
        anchor=f"item-{target_index}",
    )


@web_app.route("/admin-update", methods=["POST"])
def admin_update():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    approved_lines = read_nonempty_lines(MAIN_FILE)

    try:
        index = int((request.form.get("index") or "").strip())
    except ValueError:
        return redirect_with_notice("admin_page", page=current_page, msg="Invalid index.", level="warn")

    text = (request.form.get("text") or "").strip()
    if not text:
        return redirect_with_notice("admin_page", page=current_page, msg="Text cannot be empty.", level="warn")

    if len(text) > MAX_TEXT_LEN:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg=f"Single text length must be <= {MAX_TEXT_LEN}.",
            level="warn",
        )

    if not (0 <= index < len(approved_lines)):
        return redirect_with_notice("admin_page", page=current_page, msg="Target text does not exist.", level="warn")

    approved_lines[index] = text
    write_lines(MAIN_FILE, approved_lines)

    target_page = (index // PAGE_SIZE) + 1
    return redirect_with_notice(
        "admin_page",
        page=target_page,
        msg="Update saved.",
        level="ok",
        anchor=f"item-{index}",
    )


@web_app.route("/admin-delete", methods=["POST"])
def admin_delete():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    approved_lines = read_nonempty_lines(MAIN_FILE)

    try:
        index = int((request.form.get("index") or "").strip())
    except ValueError:
        return redirect_with_notice("admin_page", page=current_page, msg="Invalid index.", level="warn")

    if not (0 <= index < len(approved_lines)):
        return redirect_with_notice("admin_page", page=current_page, msg="Target text does not exist.", level="warn")

    del approved_lines[index]
    write_lines(MAIN_FILE, approved_lines)

    if approved_lines:
        focus_index = min(index, len(approved_lines) - 1)
        target_page = (focus_index // PAGE_SIZE) + 1
        anchor = f"item-{focus_index}"
    else:
        target_page = 1
        anchor = None

    return redirect_with_notice(
        "admin_page",
        page=target_page,
        msg="Text deleted.",
        level="ok",
        anchor=anchor,
    )


@web_app.route("/submit", methods=["GET"])
def submit_page():
    ensure_data_dir()
    return render_template(
        "submit.html",
        max_text_len=MAX_TEXT_LEN,
        notice=notice_from_request(),
    )


@web_app.route("/add-pending", methods=["POST"])
def add_pending():
    ensure_data_dir()

    text = (request.form.get("text") or "").strip()
    if not text:
        return redirect_with_notice("submit_page", msg="Text cannot be empty.", level="warn")

    if len(text) > MAX_TEXT_LEN:
        return redirect_with_notice(
            "submit_page",
            msg=f"Single text length must be <= {MAX_TEXT_LEN}.",
            level="warn",
        )

    append_line(PENDING_FILE, text)
    return redirect_with_notice("submit_page", msg="Submitted. Waiting for review.", level="ok")


@web_app.route("/review", methods=["GET"])
def review_page():
    ensure_data_dir()

    pending_lines = read_nonempty_lines(PENDING_FILE)
    approved_count = len(read_nonempty_lines(MAIN_FILE))

    requested_page = parse_positive_int(request.args.get("page"), 1)
    page_lines, page, total_pages, start_index = paginate(pending_lines, requested_page, PAGE_SIZE)

    rows = []
    for offset, line in enumerate(page_lines):
        global_index = start_index + offset
        rows.append(
            {
                "global_index": global_index,
                "display_index": global_index + 1,
                "text": line,
            }
        )

    return render_template(
        "review.html",
        max_text_len=MAX_TEXT_LEN,
        page=page,
        total_pages=total_pages,
        rows=rows,
        pending_count=len(pending_lines),
        approved_count=approved_count,
        notice=notice_from_request(),
    )


@web_app.route("/bottle", methods=["GET"])
def bottle_page():
    ensure_data_dir()
    return render_template(
        "bottle.html",
        max_message_len=MAX_BOTTLE_MESSAGE_LEN,
        max_username_len=MAX_BOTTLE_USERNAME_LEN,
        uuid_len=UUID_LEN,
        notice=notice_from_request(),
    )


@web_app.route("/bottle-send", methods=["POST"])
def bottle_send():
    ensure_data_dir()

    target_mode = (request.form.get("targetMode") or "specified").strip().lower()
    target_input = (request.form.get("target") or "").strip()
    username = (request.form.get("username") or "").strip()
    message = (request.form.get("message") or "").strip()

    if not message:
        return redirect_with_notice("bottle_page", msg="Message cannot be empty.", level="warn")
    if len(message) > MAX_BOTTLE_MESSAGE_LEN:
        return redirect_with_notice(
            "bottle_page",
            msg=f"Message length must be <= {MAX_BOTTLE_MESSAGE_LEN}.",
            level="warn",
        )
    if len(username) > MAX_BOTTLE_USERNAME_LEN:
        return redirect_with_notice(
            "bottle_page",
            msg=f"Username length must be <= {MAX_BOTTLE_USERNAME_LEN}.",
            level="warn",
        )

    final_message = message
    if username:
        final_message = f"[{username}]：{message}"

    devices = list_known_devices()
    target: Optional[Dict[str, str]] = None

    if target_mode == "random":
        if not devices:
            return redirect_with_notice("bottle_page", msg="No known device available.", level="warn")
        target = random.choice(devices)
    else:
        if not target_input:
            return redirect_with_notice("bottle_page", msg="Target cannot be empty.", level="warn")

        target_uuid = normalize_uuid(target_input)
        target_mac = normalize_mac_compact(target_input)
        if target_uuid:
            uuid_matches = [item for item in devices if item["uuid"] == target_uuid]
            if not uuid_matches:
                return redirect_with_notice("bottle_page", msg="UUID不存在", level="warn")
            target = uuid_matches[0]
        elif target_mac:
            mac_matches = [item for item in devices if item["mac_compact"] == target_mac]
            if not mac_matches:
                return redirect_with_notice("bottle_page", msg="MAC does not exist.", level="warn")
            target = mac_matches[0]
        else:
            return redirect_with_notice(
                "bottle_page",
                msg=f"Invalid target. Enter a {UUID_LEN}-char UUID or MAC.",
                level="warn",
            )

    if not target:
        return redirect_with_notice("bottle_page", msg="Target device not found.", level="warn")

    path = ensure_device_file(target["uuid"], target["mac_compact"])
    append_device_message(path, final_message)
    return redirect_with_notice(
        "bottle_page",
        msg=f"Delivered: {target['uuid']} / {target['mac']}",
        level="ok",
    )


@web_app.route("/approve", methods=["POST"])
def approve():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    pending_lines = read_nonempty_lines(PENDING_FILE)

    try:
        index = int((request.form.get("index") or "").strip())
    except ValueError:
        return redirect_with_notice("review_page", page=current_page, msg="Invalid index.", level="warn")

    if not (0 <= index < len(pending_lines)):
        return redirect_with_notice("review_page", page=current_page, msg="Target text does not exist.", level="warn")

    text = pending_lines[index]
    append_line(MAIN_FILE, text)
    del pending_lines[index]
    write_lines(PENDING_FILE, pending_lines)

    if pending_lines:
        focus_index = min(index, len(pending_lines) - 1)
        target_page = (focus_index // PAGE_SIZE) + 1
        anchor = f"pending-{focus_index}"
    else:
        target_page = 1
        anchor = None

    return redirect_with_notice(
        "review_page",
        page=target_page,
        msg="Approved and moved to library.",
        level="ok",
        anchor=anchor,
    )


@web_app.route("/reject", methods=["POST"])
def reject():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    pending_lines = read_nonempty_lines(PENDING_FILE)

    try:
        index = int((request.form.get("index") or "").strip())
    except ValueError:
        return redirect_with_notice("review_page", page=current_page, msg="Invalid index.", level="warn")

    if not (0 <= index < len(pending_lines)):
        return redirect_with_notice("review_page", page=current_page, msg="Target text does not exist.", level="warn")

    del pending_lines[index]
    write_lines(PENDING_FILE, pending_lines)

    if pending_lines:
        focus_index = min(index, len(pending_lines) - 1)
        target_page = (focus_index // PAGE_SIZE) + 1
        anchor = f"pending-{focus_index}"
    else:
        target_page = 1
        anchor = None

    return redirect_with_notice(
        "review_page",
        page=target_page,
        msg="Rejected.",
        level="ok",
        anchor=anchor,
    )


def run_servers() -> None:
    ensure_data_dir()

    api_server = ServerThread(api_app, API_HOST, API_HTTP_PORT)
    web_server = ServerThread(web_app, API_HOST, WEB_HTTP_PORT)

    api_server.start()
    web_server.start()

    print(f"[API ] http://{API_HOST}:{API_HTTP_PORT}")
    print(f"[WEB ] http://{API_HOST}:{WEB_HTTP_PORT}")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Shutting down...")
    finally:
        api_server.shutdown()
        web_server.shutdown()


if __name__ == "__main__":
    run_servers()
