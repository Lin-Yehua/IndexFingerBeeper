#!/usr/bin/env python3
import math
import os
import random
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

MAX_TEXT_LEN = 140
PAGE_SIZE = 10

API_HOST = os.environ.get("CSV_API_HOST", "0.0.0.0")
API_HTTP_PORT = int(os.environ.get("API_HTTP_PORT", "80"))
WEB_HTTPS_PORT = int(os.environ.get("WEB_HTTPS_PORT", "443"))
SSL_CERT_FILE = os.environ.get("WEB_SSL_CERT_FILE", "").strip()
SSL_KEY_FILE = os.environ.get("WEB_SSL_KEY_FILE", "").strip()

NOTICE_LEVELS = {"info", "ok", "warn", "error"}

api_app = Flask("csv_api_http")
web_app = Flask("csv_api_web", template_folder=WEB_UI_DIR, static_folder=None)


def ensure_data_dir() -> None:
    os.makedirs(DATA_DIR, exist_ok=True)
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


def resolve_ssl_context():
    if SSL_CERT_FILE and SSL_KEY_FILE:
        if not os.path.isfile(SSL_CERT_FILE):
            raise FileNotFoundError(f"SSL cert file not found: {SSL_CERT_FILE}")
        if not os.path.isfile(SSL_KEY_FILE):
            raise FileNotFoundError(f"SSL key file not found: {SSL_KEY_FILE}")
        return SSL_CERT_FILE, SSL_KEY_FILE

    return "adhoc"


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
            "web_https_port": WEB_HTTPS_PORT,
        }
    ), 200


@api_app.route("/random", methods=["GET"])
def random_text():
    ensure_data_dir()
    lines = read_nonempty_lines(MAIN_FILE)

    if not lines:
        return jsonify({"ok": False, "error": "No approved text found"}), 404

    return jsonify({"ok": True, "text": random.choice(lines)}), 200


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

    ssl_context = resolve_ssl_context()

    api_server = ServerThread(api_app, API_HOST, API_HTTP_PORT)
    web_server = ServerThread(web_app, API_HOST, WEB_HTTPS_PORT, ssl_context=ssl_context)

    api_server.start()
    web_server.start()

    print(f"[API ] http://{API_HOST}:{API_HTTP_PORT}")
    print(f"[WEB ] https://{API_HOST}:{WEB_HTTPS_PORT}")

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