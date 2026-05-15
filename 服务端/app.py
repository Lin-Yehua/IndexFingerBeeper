#!/usr/bin/env python3
import hmac
import json
import math
import os
import random
import re
import secrets
import threading
import time
from datetime import datetime, timedelta
from typing import Dict, List, Optional, Tuple

from flask import (
    Flask,
    Response,
    abort,
    jsonify,
    redirect,
    render_template,
    request,
    send_from_directory,
    url_for,
)
from werkzeug.serving import WSGIRequestHandler, make_server

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
WEB_UI_DIR = os.path.join(BASE_DIR, "ui")

if os.name == "nt":
    DEFAULT_DATA_DIR = os.path.join(BASE_DIR, "data")
else:
    DEFAULT_DATA_DIR = "/opt/csv_api/data"

DATA_DIR = os.environ.get("CSV_API_DATA_DIR", DEFAULT_DATA_DIR)
MAIN_FILE = os.path.join(DATA_DIR, "text.csv")
PENDING_FILE = os.path.join(DATA_DIR, "pending.csv")
DEVICE_META_FILE = os.path.join(DATA_DIR, "devices.json")
MESSAGEDATA_DIR = os.environ.get("CSV_API_MESSAGE_DIR", os.path.join(BASE_DIR, "messagedata"))

MAX_TEXT_LEN = 140
PAGE_SIZE = 10
DEVICE_PAGE_SIZE = int(os.environ.get("CSV_API_DEVICE_PAGE_SIZE", "10"))
MAX_BOTTLE_MESSAGE_LEN = 100
MAX_BOTTLE_USERNAME_LEN = 20
MAX_DEVICE_NOTE_LEN = 80
DEVICE_MESSAGE_LIMIT = 20
UUID_LEN = 18

UUID_RE = re.compile(r"^UUID\d{14}$")
MAC_COMPACT_RE = re.compile(r"^[0-9A-F]{12}$")

API_HOST = os.environ.get("CSV_API_HOST", "0.0.0.0")
API_HTTP_PORT = int(os.environ.get("API_HTTP_PORT", "8080"))
WEB_HTTP_PORT = int(os.environ.get("WEB_HTTP_PORT", "80"))
WEB_HTTPS_ENABLED = (os.environ.get("WEB_HTTPS_ENABLED", "0").strip().lower() in {"1", "true", "yes", "on"})
WEB_HTTP_REDIRECT_TO_HTTPS = (
    WEB_HTTPS_ENABLED
    and
    os.environ.get("WEB_HTTP_REDIRECT_TO_HTTPS", "1" if WEB_HTTPS_ENABLED else "0").strip().lower()
    in {"1", "true", "yes", "on"}
)
WEB_PUBLIC_HOST = (os.environ.get("WEB_PUBLIC_HOST") or "").strip()
# Lightweight stable default:
# - WEB_HTTP_PORT serves the actual Web UI.
# - Set WEB_HTTPS_ENABLED=1 to also serve HTTPS directly from Werkzeug.
# - Set WEB_HTTP_REDIRECT_TO_HTTPS=1 only when HTTPS is enabled and healthy.
WEB_HTTPS_PORT = int(os.environ.get("WEB_HTTPS_PORT", "443"))
WEB_HTTPS_CERT_FILE = (os.environ.get("WEB_HTTPS_CERT_FILE") or "").strip()
WEB_HTTPS_KEY_FILE = (os.environ.get("WEB_HTTPS_KEY_FILE") or "").strip()
WEB_HTTPS_USE_ADHOC = (os.environ.get("WEB_HTTPS_USE_ADHOC", "1").strip().lower() in {"1", "true", "yes", "on"})
WEB_AUTH_USER = (os.environ.get("WEB_AUTH_USER", "admin") or "admin").strip()
WEB_AUTH_PASSWORD = (os.environ.get("WEB_AUTH_PASSWORD") or "").strip()
WEB_AUTH_PASSWORD_IS_TEMP = False
if not WEB_AUTH_PASSWORD:
    WEB_AUTH_PASSWORD = secrets.token_urlsafe(18)
    WEB_AUTH_PASSWORD_IS_TEMP = True
CLIENT_SOCKET_TIMEOUT_SEC = float(os.environ.get("CSV_API_CLIENT_TIMEOUT_SEC", "8"))

NOTICE_LEVELS = {"info", "ok", "warn", "error"}

_PATH_LOCKS: Dict[str, threading.RLock] = {}
_PATH_LOCKS_GUARD = threading.Lock()
_DEVICE_META_LOCK = threading.RLock()
_MAIN_FILE_ABS = os.path.abspath(MAIN_FILE)
_MAIN_CACHE_LOCK = threading.RLock()
_MAIN_CACHE_MTIME_NS = -1
_MAIN_CACHE_LINES: List[str] = []

api_app = Flask("csv_api_http")
web_http_app = Flask("csv_api_web_http_redirect")
web_app = Flask("csv_api_web", template_folder=WEB_UI_DIR, static_folder=None)
PROTECTED_WEB_ENDPOINTS = {
    "admin_page",
    "admin_add",
    "admin_update",
    "admin_delete",
    "admin_device_note",
    "admin_device_bottle",
    "review_page",
    "approve",
    "reject",
}


class TimeoutWSGIRequestHandler(WSGIRequestHandler):
    def setup(self) -> None:
        super().setup()
        try:
            self.connection.settimeout(CLIENT_SOCKET_TIMEOUT_SEC)
        except OSError:
            pass


def _force_close_connection(response):
    # Keep long-running service stable: avoid keep-alive socket accumulation.
    response.headers["Connection"] = "close"
    return response


api_app.after_request(_force_close_connection)
web_http_app.after_request(_force_close_connection)
web_app.after_request(_force_close_connection)


def _normalize_path(path: str) -> str:
    return os.path.abspath(path)


def _is_main_file(path: str) -> bool:
    return _normalize_path(path) == _MAIN_FILE_ABS


def _get_path_lock(path: str) -> threading.RLock:
    normalized = _normalize_path(path)
    with _PATH_LOCKS_GUARD:
        lock = _PATH_LOCKS.get(normalized)
        if lock is None:
            lock = threading.RLock()
            _PATH_LOCKS[normalized] = lock
        return lock


def _invalidate_main_lines_cache() -> None:
    global _MAIN_CACHE_MTIME_NS, _MAIN_CACHE_LINES
    with _MAIN_CACHE_LOCK:
        _MAIN_CACHE_MTIME_NS = -1
        _MAIN_CACHE_LINES = []


def ensure_data_dir() -> None:
    os.makedirs(DATA_DIR, exist_ok=True)
    os.makedirs(MESSAGEDATA_DIR, exist_ok=True)
    for path in [MAIN_FILE, PENDING_FILE]:
        if not os.path.exists(path):
            with open(path, "a", encoding="utf-8"):
                pass
    if not os.path.exists(DEVICE_META_FILE):
        with open(DEVICE_META_FILE, "w", encoding="utf-8") as f:
            json.dump({}, f, ensure_ascii=False)


def _read_nonempty_lines_unlocked(file_path: str) -> List[str]:
    lines: List[str] = []
    if not os.path.isfile(file_path):
        return lines

    with open(file_path, "r", encoding="utf-8-sig") as f:
        for line in f:
            text = line.strip()
            if text:
                lines.append(text)
    return lines


def read_nonempty_lines(file_path: str) -> List[str]:
    lock = _get_path_lock(file_path)
    with lock:
        return _read_nonempty_lines_unlocked(file_path)


def append_line(file_path: str, text: str) -> None:
    safe_text = text.replace("\r", " ").replace("\n", " ").strip()
    lock = _get_path_lock(file_path)
    with lock:
        with open(file_path, "a", encoding="utf-8", newline="") as f:
            f.write(safe_text + "\n")
    if _is_main_file(file_path):
        _invalidate_main_lines_cache()


def _write_lines_unlocked(file_path: str, lines: List[str]) -> None:
    with open(file_path, "w", encoding="utf-8", newline="") as f:
        for line in lines:
            safe_text = line.replace("\r", " ").replace("\n", " ").strip()
            if safe_text:
                f.write(safe_text + "\n")


def write_lines(file_path: str, lines: List[str]) -> None:
    lock = _get_path_lock(file_path)
    with lock:
        _write_lines_unlocked(file_path, lines)
    if _is_main_file(file_path):
        _invalidate_main_lines_cache()


def get_main_lines_cached() -> List[str]:
    global _MAIN_CACHE_MTIME_NS, _MAIN_CACHE_LINES
    ensure_data_dir()
    lock = _get_path_lock(MAIN_FILE)
    with lock:
        try:
            stat = os.stat(MAIN_FILE)
            mtime_ns = getattr(stat, "st_mtime_ns", int(stat.st_mtime * 1_000_000_000))
        except FileNotFoundError:
            mtime_ns = -1

        with _MAIN_CACHE_LOCK:
            if _MAIN_CACHE_MTIME_NS == mtime_ns:
                return list(_MAIN_CACHE_LINES)
            lines = _read_nonempty_lines_unlocked(MAIN_FILE)
            _MAIN_CACHE_LINES = list(lines)
            _MAIN_CACHE_MTIME_NS = mtime_ns
            return lines


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


def device_key(uuid: str, mac_compact: str) -> str:
    return f"{uuid}+{mac_compact}"


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
        "key": device_key(uuid, mac_compact),
        "file_name": file_name,
        "path": os.path.join(MESSAGEDATA_DIR, file_name),
    }


def _load_device_meta_unlocked() -> Dict[str, Dict[str, object]]:
    if not os.path.isfile(DEVICE_META_FILE):
        return {}
    try:
        with open(DEVICE_META_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(data, dict):
        return {}
    cleaned: Dict[str, Dict[str, object]] = {}
    for key, value in data.items():
        if isinstance(key, str) and isinstance(value, dict):
            cleaned[key] = dict(value)
    return cleaned


def _write_device_meta_unlocked(meta: Dict[str, Dict[str, object]]) -> None:
    tmp_path = DEVICE_META_FILE + ".tmp"
    with open(tmp_path, "w", encoding="utf-8", newline="") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp_path, DEVICE_META_FILE)


def get_device_meta_snapshot() -> Dict[str, Dict[str, object]]:
    ensure_data_dir()
    with _DEVICE_META_LOCK:
        return _load_device_meta_unlocked()


def format_timestamp(ts: Optional[float]) -> str:
    if ts is None:
        return ""
    try:
        return datetime.fromtimestamp(float(ts)).strftime("%Y-%m-%d %H:%M:%S")
    except (OSError, OverflowError, TypeError, ValueError):
        return ""


def touch_device_seen(uuid: str, mac_compact: str) -> None:
    if not uuid or not mac_compact:
        return
    ensure_data_dir()
    now = time.time()
    key = device_key(uuid, mac_compact)
    with _DEVICE_META_LOCK:
        meta = _load_device_meta_unlocked()
        item = meta.get(key)
        if not isinstance(item, dict):
            item = {}
        item["uuid"] = uuid
        item["mac_compact"] = mac_compact
        item["last_seen_ts"] = now
        item["last_seen"] = format_timestamp(now)
        meta[key] = item
        _write_device_meta_unlocked(meta)


def set_device_note(uuid: str, mac_compact: str, note: str) -> None:
    ensure_data_dir()
    key = device_key(uuid, mac_compact)
    with _DEVICE_META_LOCK:
        meta = _load_device_meta_unlocked()
        item = meta.get(key)
        if not isinstance(item, dict):
            item = {"uuid": uuid, "mac_compact": mac_compact}
        item["uuid"] = uuid
        item["mac_compact"] = mac_compact
        item["note"] = note
        meta[key] = item
        _write_device_meta_unlocked(meta)


def parse_date_start(raw: Optional[str]) -> Optional[float]:
    value = (raw or "").strip()
    if not value:
        return None
    try:
        return datetime.strptime(value, "%Y-%m-%d").timestamp()
    except ValueError:
        return None


def parse_date_end(raw: Optional[str]) -> Optional[float]:
    value = (raw or "").strip()
    if not value:
        return None
    try:
        dt = datetime.strptime(value, "%Y-%m-%d") + timedelta(days=1)
        return dt.timestamp()
    except ValueError:
        return None


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


def find_known_device(uuid: str, mac_compact: str) -> Optional[Dict[str, str]]:
    key = device_key(uuid, mac_compact)
    for item in list_known_devices():
        if item["key"] == key:
            return item
    return None


def collect_device_overview(
    start_ts: Optional[float] = None,
    end_ts: Optional[float] = None,
) -> List[Dict[str, object]]:
    devices = list_known_devices()
    meta = get_device_meta_snapshot()
    overview: List[Dict[str, object]] = []
    for item in devices:
        item_meta = meta.get(item["key"], {})
        raw_last_seen_ts = item_meta.get("last_seen_ts") if isinstance(item_meta, dict) else None
        try:
            last_seen_ts = float(raw_last_seen_ts) if raw_last_seen_ts is not None else None
        except (TypeError, ValueError):
            last_seen_ts = None

        if start_ts is not None and (last_seen_ts is None or last_seen_ts < start_ts):
            continue
        if end_ts is not None and (last_seen_ts is None or last_seen_ts >= end_ts):
            continue

        messages = read_nonempty_lines(item["path"])
        overview.append(
            {
                "key": item["key"],
                "anchor": f"{item['uuid']}-{item['mac_compact']}",
                "uuid": item["uuid"],
                "mac": item["mac"],
                "mac_compact": item["mac_compact"],
                "file_name": item["file_name"],
                "note": str(item_meta.get("note") or "") if isinstance(item_meta, dict) else "",
                "last_seen_ts": last_seen_ts,
                "last_seen_text": format_timestamp(last_seen_ts) or "未知",
                "queue_count": len(messages),
                "messages": messages,
            }
        )
    overview.sort(key=lambda item: item["last_seen_ts"] if item["last_seen_ts"] is not None else -1, reverse=True)
    return overview


def ensure_device_file(uuid: str, mac_compact: str) -> str:
    ensure_data_dir()
    path = device_file_path(uuid, mac_compact)
    if not os.path.exists(path):
        with open(path, "a", encoding="utf-8"):
            pass
    return path


def append_device_message(path: str, message: str) -> None:
    lock = _get_path_lock(path)
    with lock:
        lines = _read_nonempty_lines_unlocked(path)
        lines.append(message)
        if len(lines) > DEVICE_MESSAGE_LIMIT:
            lines = lines[-DEVICE_MESSAGE_LIMIT:]
        _write_lines_unlocked(path, lines)


def pop_oldest_device_message(path: str) -> Optional[str]:
    lock = _get_path_lock(path)
    with lock:
        if not os.path.isfile(path):
            return None
        lines = _read_nonempty_lines_unlocked(path)
        if not lines:
            return None
        oldest = lines[0]
        _write_lines_unlocked(path, lines[1:])
        return oldest


def parse_positive_int(raw_value: Optional[str], default: int = 1) -> int:
    try:
        value = int((raw_value or "").strip())
        return value if value > 0 else default
    except (TypeError, ValueError):
        return default


def paginate(items: List[object], page: int, page_size: int) -> Tuple[List[object], int, int, int]:
    total_items = len(items)
    total_pages = max(1, math.ceil(total_items / page_size))
    current_page = min(max(page, 1), total_pages)
    start_index = (current_page - 1) * page_size
    end_index = start_index + page_size
    return items[start_index:end_index], current_page, total_pages, start_index


def admin_device_query_from_mapping(values) -> Dict[str, str]:
    return {
        "device_page": str(parse_positive_int(values.get("device_page"), 1)),
        "device_from": (values.get("device_from") or "").strip(),
        "device_to": (values.get("device_to") or "").strip(),
    }


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
    extra_query: Optional[Dict[str, str]] = None,
):
    query: Dict[str, str] = {}
    if extra_query:
        query.update({k: v for k, v in extra_query.items() if v is not None and str(v) != ""})
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
        self._server = make_server(
            host,
            port,
            app,
            threaded=True,
            request_handler=TimeoutWSGIRequestHandler,
            ssl_context=ssl_context,
        )
        if hasattr(self._server, "daemon_threads"):
            self._server.daemon_threads = True

    def run(self) -> None:
        self._server.serve_forever()

    def shutdown(self) -> None:
        self._server.shutdown()


def resolve_web_ssl_context():
    cert_file = WEB_HTTPS_CERT_FILE
    key_file = WEB_HTTPS_KEY_FILE

    if cert_file or key_file:
        if not cert_file or not key_file:
            raise RuntimeError("Both WEB_HTTPS_CERT_FILE and WEB_HTTPS_KEY_FILE must be set.")
        if not os.path.isfile(cert_file):
            raise RuntimeError(f"WEB_HTTPS_CERT_FILE not found: {cert_file}")
        if not os.path.isfile(key_file):
            raise RuntimeError(f"WEB_HTTPS_KEY_FILE not found: {key_file}")
        return cert_file, key_file

    if WEB_HTTPS_USE_ADHOC:
        # Development fallback: Werkzeug generates a temporary self-signed cert.
        return "adhoc"

    raise RuntimeError(
        "HTTPS is required for Web UI. Set WEB_HTTPS_CERT_FILE and WEB_HTTPS_KEY_FILE,"
        " or enable WEB_HTTPS_USE_ADHOC=1."
    )


def web_auth_failed_response() -> Response:
    return Response(
        "Authentication required",
        401,
        {"WWW-Authenticate": 'Basic realm="CSV Admin", charset="UTF-8"'},
    )


def is_web_auth_ok() -> bool:
    auth = request.authorization
    if not auth or (auth.type or "").lower() != "basic":
        return False

    username = auth.username or ""
    password = auth.password or ""
    return hmac.compare_digest(username, WEB_AUTH_USER) and hmac.compare_digest(password, WEB_AUTH_PASSWORD)


@web_app.before_request
def protect_admin_review_routes():
    if request.endpoint not in PROTECTED_WEB_ENDPOINTS:
        return None
    if is_web_auth_ok():
        return None
    return web_auth_failed_response()


def _host_without_port(raw_host: str) -> str:
    host = (raw_host or "").strip()
    if not host:
        return ""
    if host.startswith("["):
        close_idx = host.find("]")
        return host if close_idx < 0 else host[: close_idx + 1]
    if ":" in host:
        return host.split(":", 1)[0]
    return host


def build_https_redirect_target(path: str) -> str:
    preferred_host = WEB_PUBLIC_HOST or request.host
    host = _host_without_port(preferred_host)
    if not host:
        host = "localhost"
    if WEB_HTTPS_PORT == 443:
        host_part = host
    else:
        host_part = f"{host}:{WEB_HTTPS_PORT}"

    target = f"https://{host_part}{path}"
    if request.query_string:
        target += "?" + request.query_string.decode("utf-8", errors="ignore")
    return target


@web_http_app.route("/", defaults={"subpath": ""}, methods=["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"])
@web_http_app.route("/<path:subpath>", methods=["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"])
def web_http_redirect(subpath: str):
    if not WEB_HTTP_REDIRECT_TO_HTTPS:
        return Response("Web HTTP redirect disabled.", 404)
    path = f"/{subpath}" if subpath else "/"
    return redirect(build_https_redirect_target(path), code=308)


@api_app.route("/", methods=["GET"])
def api_index():
    return jsonify(
        {
            "ok": True,
            "service": "csv_api",
            "api": ["/random", "/health"],
            "web": ["/admin", "/submit", "/review", "/bottle"],
            "api_http_port": API_HTTP_PORT,
            "web_http_port": WEB_HTTP_PORT,
            "web_https_enabled": WEB_HTTPS_ENABLED,
            "web_https_port": WEB_HTTPS_PORT,
            "web_http_redirect_to_https": WEB_HTTP_REDIRECT_TO_HTTPS,
        }
    ), 200


@api_app.route("/random", methods=["GET"])
def random_text():
    ensure_data_dir()
    req_uuid = normalize_uuid(request.args.get("uuid"))
    req_mac_compact = normalize_mac_compact(request.args.get("mac"))
    if req_uuid and req_mac_compact:
        touch_device_seen(req_uuid, req_mac_compact)
        device_path = ensure_device_file(req_uuid, req_mac_compact)
        queued = pop_oldest_device_message(device_path)
        if queued:
            return jsonify({"ok": True, "text": queued, "source": "device_queue"}), 200

    lines = get_main_lines_cached()
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

    approved_lines = get_main_lines_cached()
    pending_count = len(read_nonempty_lines(PENDING_FILE))
    device_from = (request.args.get("device_from") or "").strip()
    device_to = (request.args.get("device_to") or "").strip()
    device_start_ts = parse_date_start(device_from)
    device_end_ts = parse_date_end(device_to)
    total_devices = len(list_known_devices())
    filtered_devices = collect_device_overview(device_start_ts, device_end_ts)
    requested_device_page = parse_positive_int(request.args.get("device_page"), 1)
    device_page_items, device_page, device_total_pages, _ = paginate(
        filtered_devices,
        requested_device_page,
        max(1, DEVICE_PAGE_SIZE),
    )

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
        device_overview=device_page_items,
        total_devices=total_devices,
        filtered_device_count=len(filtered_devices),
        device_page=device_page,
        device_total_pages=device_total_pages,
        device_page_size=max(1, DEVICE_PAGE_SIZE),
        device_from=device_from,
        device_to=device_to,
        max_bottle_message_len=MAX_BOTTLE_MESSAGE_LEN,
        max_device_note_len=MAX_DEVICE_NOTE_LEN,
        main_file=MAIN_FILE,
        notice=notice_from_request(),
    )


@web_app.route("/admin-add", methods=["POST"])
def admin_add():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    device_query = admin_device_query_from_mapping(request.form)
    text = (request.form.get("text") or "").strip()

    if not text:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Text cannot be empty.",
            level="warn",
            extra_query=device_query,
        )

    if len(text) > MAX_TEXT_LEN:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg=f"Single text length must be <= {MAX_TEXT_LEN}.",
            level="warn",
            extra_query=device_query,
        )

    append_line(MAIN_FILE, text)

    approved_count = len(get_main_lines_cached())
    target_index = max(approved_count - 1, 0)
    target_page = (target_index // PAGE_SIZE) + 1

    return redirect_with_notice(
        "admin_page",
        page=target_page,
        msg="Added to approved library.",
        level="ok",
        extra_query=device_query,
        anchor=f"item-{target_index}",
    )


@web_app.route("/admin-update", methods=["POST"])
def admin_update():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    device_query = admin_device_query_from_mapping(request.form)
    approved_lines = get_main_lines_cached()

    try:
        index = int((request.form.get("index") or "").strip())
    except ValueError:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Invalid index.",
            level="warn",
            extra_query=device_query,
        )

    text = (request.form.get("text") or "").strip()
    if not text:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Text cannot be empty.",
            level="warn",
            extra_query=device_query,
        )

    if len(text) > MAX_TEXT_LEN:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg=f"Single text length must be <= {MAX_TEXT_LEN}.",
            level="warn",
            extra_query=device_query,
        )

    if not (0 <= index < len(approved_lines)):
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Target text does not exist.",
            level="warn",
            extra_query=device_query,
        )

    approved_lines[index] = text
    write_lines(MAIN_FILE, approved_lines)

    target_page = (index // PAGE_SIZE) + 1
    return redirect_with_notice(
        "admin_page",
        page=target_page,
        msg="Update saved.",
        level="ok",
        extra_query=device_query,
        anchor=f"item-{index}",
    )


@web_app.route("/admin-delete", methods=["POST"])
def admin_delete():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    device_query = admin_device_query_from_mapping(request.form)
    approved_lines = get_main_lines_cached()

    try:
        index = int((request.form.get("index") or "").strip())
    except ValueError:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Invalid index.",
            level="warn",
            extra_query=device_query,
        )

    if not (0 <= index < len(approved_lines)):
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Target text does not exist.",
            level="warn",
            extra_query=device_query,
        )

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
        extra_query=device_query,
        anchor=anchor,
    )


@web_app.route("/admin-device-note", methods=["POST"])
def admin_device_note():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    device_query = admin_device_query_from_mapping(request.form)
    uuid = normalize_uuid(request.form.get("uuid"))
    mac_compact = normalize_mac_compact(request.form.get("mac"))
    note = (request.form.get("note") or "").strip()

    if not uuid or not mac_compact or not find_known_device(uuid, mac_compact):
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Device not found.",
            level="warn",
            extra_query=device_query,
        )
    if len(note) > MAX_DEVICE_NOTE_LEN:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg=f"Device note length must be <= {MAX_DEVICE_NOTE_LEN}.",
            level="warn",
            extra_query=device_query,
            anchor=f"device-{uuid}-{mac_compact}",
        )

    set_device_note(uuid, mac_compact, note)
    anchor = f"device-{uuid}-{mac_compact}"
    return redirect_with_notice(
        "admin_page",
        page=current_page,
        msg="Device note saved.",
        level="ok",
        extra_query=device_query,
        anchor=anchor,
    )


@web_app.route("/admin-device-bottle", methods=["POST"])
def admin_device_bottle():
    ensure_data_dir()

    current_page = parse_positive_int(request.form.get("page"), 1)
    device_query = admin_device_query_from_mapping(request.form)
    uuid = normalize_uuid(request.form.get("uuid"))
    mac_compact = normalize_mac_compact(request.form.get("mac"))
    message = (request.form.get("message") or "").strip()

    target = find_known_device(uuid, mac_compact) if uuid and mac_compact else None
    if not target:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Device not found.",
            level="warn",
            extra_query=device_query,
        )
    if not message:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg="Bottle message cannot be empty.",
            level="warn",
            extra_query=device_query,
            anchor=f"device-{target['uuid']}-{target['mac_compact']}",
        )
    if len(message) > MAX_BOTTLE_MESSAGE_LEN:
        return redirect_with_notice(
            "admin_page",
            page=current_page,
            msg=f"Bottle message length must be <= {MAX_BOTTLE_MESSAGE_LEN}.",
            level="warn",
            extra_query=device_query,
            anchor=f"device-{target['uuid']}-{target['mac_compact']}",
        )

    path = ensure_device_file(target["uuid"], target["mac_compact"])
    append_device_message(path, message)
    anchor = f"device-{target['uuid']}-{target['mac_compact']}"
    return redirect_with_notice(
        "admin_page",
        page=current_page,
        msg=f"Delivered to {target['uuid']} / {target['mac']}.",
        level="ok",
        extra_query=device_query,
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
    approved_count = len(get_main_lines_cached())

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
        final_message = f"[{username}] {message}"

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
                return redirect_with_notice("bottle_page", msg="UUID does not exist.", level="warn")
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


def validate_server_ports() -> None:
    ports = {
        "API_HTTP_PORT": API_HTTP_PORT,
        "WEB_HTTP_PORT": WEB_HTTP_PORT,
    }
    if WEB_HTTPS_ENABLED:
        ports["WEB_HTTPS_PORT"] = WEB_HTTPS_PORT
    seen: Dict[int, str] = {}
    for name, port in ports.items():
        if port in seen:
            raise RuntimeError(f"Port conflict: {name} and {seen[port]} both use {port}.")
        seen[port] = name


def run_servers() -> None:
    ensure_data_dir()
    validate_server_ports()
    web_ssl_context = resolve_web_ssl_context() if WEB_HTTPS_ENABLED else None

    api_server = ServerThread(api_app, API_HOST, API_HTTP_PORT)
    http_web_app = web_http_app if WEB_HTTP_REDIRECT_TO_HTTPS else web_app
    web_http_server = ServerThread(http_web_app, API_HOST, WEB_HTTP_PORT)
    web_server = (
        ServerThread(web_app, API_HOST, WEB_HTTPS_PORT, ssl_context=web_ssl_context)
        if WEB_HTTPS_ENABLED
        else None
    )

    api_server.start()
    web_http_server.start()
    if web_server:
        web_server.start()

    print(f"[API ] http://{API_HOST}:{API_HTTP_PORT}")
    if WEB_HTTP_REDIRECT_TO_HTTPS:
        print(f"[WEB ] http://{API_HOST}:{WEB_HTTP_PORT} -> https://{WEB_PUBLIC_HOST or API_HOST}:{WEB_HTTPS_PORT}")
    else:
        print(f"[WEB ] http://{API_HOST}:{WEB_HTTP_PORT}")
    if WEB_HTTPS_ENABLED:
        print(f"[WEB ] https://{API_HOST}:{WEB_HTTPS_PORT}")
    else:
        print("[WEB ] HTTPS disabled (set WEB_HTTPS_ENABLED=1 to enable)")
    if WEB_AUTH_PASSWORD_IS_TEMP:
        print(f"[WEB ] WEB_AUTH_USER={WEB_AUTH_USER}")
        print(f"[WEB ] WEB_AUTH_PASSWORD={WEB_AUTH_PASSWORD} (temporary, set WEB_AUTH_PASSWORD to override)")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Shutting down...")
    finally:
        api_server.shutdown()
        web_http_server.shutdown()
        if web_server:
            web_server.shutdown()


if __name__ == "__main__":
    run_servers()
