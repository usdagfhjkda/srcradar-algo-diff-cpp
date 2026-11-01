#!/usr/bin/env python3
"""
ci_mock_server.py — CI 用的 mock HTTP server.

httpx -l 输入是 host:port, 不带 path. 所以 7 case 不能共享 1 server.
本 server 接受 2 个 arg:
   port: 监听端口
   status_code: 响应码 (200 / 404 / 500 ...)

CI 启 3 个 server:
   port 18085 status 200  (case 5 期望 200 → A)
   port 18086 status 200  (case 6 期望 200 → A)
   port 18087 status 404  (case 7 期望 404 → 8)

跑法:
   python3 ci_mock_server.py <port> <status_code> &
"""
import http.server
import socketserver
import sys


class MockHandler(http.server.BaseHTTPRequestHandler):
    status_code = 200  # overridden by main()
    body = b""

    def do_GET(self):
        self.send_response(self.status_code)
        if self.body:
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(self.body)))
        else:
            self.send_header("Content-Length", "0")
        self.end_headers()
        if self.body:
            self.wfile.write(self.body)

    def log_message(self, format, *args):
        pass  # silent


def main():
    if len(sys.argv) < 3:
        print(f"用法: {sys.argv[0]} <port> <status_code>", file=sys.stderr)
        return 1
    port = int(sys.argv[1])
    code = int(sys.argv[2])

    MockHandler.status_code = code
    MockHandler.body = (
        f"<html>case response from port {port}, status {code}</html>".encode("utf-8")
        if code == 200 else b""
    )

    server = socketserver.ThreadingTCPServer(("127.0.0.1", port), MockHandler)
    server.daemon_threads = True
    print(f"[ci_mock_server] listening on 127.0.0.1:{port}, status={code}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())