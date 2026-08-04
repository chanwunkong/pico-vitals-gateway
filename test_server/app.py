#!/usr/bin/env python3
"""本機測試用伺服器：接收 Pico 中繼裝置上傳的生理資料，並提供簡易網頁檢視。
純標準函式庫實作，不需要額外安裝套件，直接執行：python app.py
"""
import json
import threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DB_LOCK = threading.Lock()
records = []  # list of dict: {patient_id, type, value, received_at_ms, uploaded_at}

HOST = "0.0.0.0"
PORT = 5000

VITAL_TYPE_NAMES = {
    0: "unknown",
    1: "temperature",
    2: "spo2",
    3: "pulse_rate",
    4: "systolic",
    5: "diastolic",
}


def format_received_at(ms):
    if ms is None:
        return ""
    # 校時成功的話 received_at_ms 是真實世界的 epoch ms（一定是很大的數字，
    # 對應到 2020 年以後）；沒校時成功會是 boot-relative ms（開機以來經過的
    # 毫秒數，數值小很多），兩種用大小判斷、分開顯示，別把後者誤標成日期。
    EPOCH_MS_THRESHOLD = 1_500_000_000_000  # 約 2017-07
    if ms >= EPOCH_MS_THRESHOLD:
        return datetime.fromtimestamp(ms / 1000).strftime("%Y-%m-%d %H:%M:%S")
    return f"{ms} (未校時，開機經過時間)"


def render_html():
    rows = ""
    with DB_LOCK:
        count = len(records)
        for r in reversed(records[-100:]):
            rows += (
                f"<tr><td>{r['uploaded_at']}</td><td>{r.get('patient_id', '')}</td>"
                f"<td>{VITAL_TYPE_NAMES.get(r.get('type'), r.get('type'))}</td>"
                f"<td>{r.get('value')}</td><td>{format_received_at(r.get('received_at_ms'))}</td></tr>"
            )
    return f"""<!DOCTYPE html><html><head><meta charset="utf-8">
<meta http-equiv="refresh" content="5">
<title>Pico 中繼裝置 - 測試檢視</title>
<style>
body{{font-family:sans-serif;padding:20px;}}
table{{border-collapse:collapse;width:100%;max-width:800px;}}
th,td{{border:1px solid #ccc;padding:8px;text-align:left;}}
th{{background:#f4f4f4;}}
</style></head><body>
<h2>Pico 中繼裝置 - 收到的生理資料（測試用，每 5 秒自動重新整理）</h2>
<p>目前累計 {count} 筆</p>
<table><tr><th>收到時間(伺服器)</th><th>個案編號</th><th>類型</th><th>數值</th><th>裝置量測時間</th></tr>
{rows}
</table></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print("[server] " + (fmt % args))

    def do_GET(self):
        if self.path == "/":
            body = render_html().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path != "/api/vitals":
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b'{"error":"invalid json"}')
            return

        patient_id = payload.get("patient_id", "")
        readings = payload.get("readings", [])
        now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        with DB_LOCK:
            for reading in readings:
                records.append({
                    "patient_id": patient_id,
                    "type": reading.get("type"),
                    "value": reading.get("value"),
                    "received_at_ms": reading.get("received_at_ms"),
                    "uploaded_at": now,
                })

        print(f"[server] received {len(readings)} reading(s) for patient_id={patient_id}")

        body = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"[server] listening on http://{HOST}:{PORT}  (open http://127.0.0.1:{PORT}/ in a browser)")
    print(f"[server] POST endpoint: http://<this-PC-IP>:{PORT}/api/vitals")
    server.serve_forever()


if __name__ == "__main__":
    main()
