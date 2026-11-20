"""接收 F1 板子回报的诊断行。
板子每轮取数后 POST 一行纯文本到 http://192.168.3.189:8080/r，
这里追加写入 f1_report.log，PC 端直接看，不用拍照、不用串口。
"""
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

LOG = r"D:\nav_build\f1_report.log"


class Handler(BaseHTTPRequestHandler):
    def _record(self, text):
        line = text.strip()
        if not line:
            return
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        print("RECV:", line, flush=True)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(n).decode("utf-8", "replace")
        self._record(body)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"ok")

    def do_GET(self):
        # 也收 ?m= 形式，便于手动用浏览器测通
        from urllib.parse import urlparse, parse_qs
        q = parse_qs(urlparse(self.path).query)
        self._record(q.get("m", [""])[0])
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    open(LOG, "w", encoding="utf-8").close()
    print("listening on 0.0.0.0:8080, log ->", LOG, flush=True)
    HTTPServer(("0.0.0.0", 8080), Handler).serve_forever()
