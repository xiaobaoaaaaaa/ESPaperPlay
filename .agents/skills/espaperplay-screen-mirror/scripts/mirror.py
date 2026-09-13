#!/usr/bin/env python3
"""ESPaperPlay 屏幕镜像调试客户端（纯 Python stdlib，无第三方依赖）。

用法（详见同目录 SKILL.md）：
  mirror.py login   --host 192.168.4.1 --password xxx       # 登录并缓存 token
  mirror.py snap    --host 192.168.4.1 -o screen.png        # 拉当前画面存 PNG
  mirror.py watch   --host 192.168.4.1 -n 5 -i 1.0 -d out/  # 轮询快照序列
  mirror.py tap     --host 192.168.4.1 400 240              # WS 注入一次点击
  mirror.py touch   --host 192.168.4.1 400 240 down         # 单条触摸帧
  mirror.py key     --host 192.168.4.1 click                # BOOT 键动作
  mirror.py refresh --host 192.168.4.1                      # 请求强制全刷
  mirror.py live    --host 192.168.4.1 -n 3 -d out/         # WS 连收 N 帧

token 缓存在 /tmp/espaperplay_mirror_token_<host>.json；自签证书默认跳过校验。
"""

import argparse
import base64
import json
import os
import socket
import ssl
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

MAGIC = b"EPM1"
FMT_BW = 0
FMT_G4 = 1
GRAY4_SHADE = (255, 170, 85, 0)  # 2bpp: 0=白 1=浅灰 2=深灰 3=黑

TOKEN_CACHE = "/tmp/espaperplay_mirror_token_{host}.json"
DISPLAY_W = 800
DISPLAY_H = 480


def die(msg, code=1):
    print("错误：" + msg, file=sys.stderr)
    sys.exit(code)


def http_context():
    """自签证书：默认跳过 TLS 校验。"""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def split_host(host_arg):
    host, _, port = host_arg.partition(":")
    return host or "192.168.4.1", int(port) if port else 443


# ---------------------------------------------------------------- HTTP ----

def http_request(host_arg, path, token=None, form=None, binary=False, timeout=8):
    host, port = split_host(host_arg)
    url = "https://%s:%d%s" % (host, port, path)
    data = None
    headers = {}
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=http_context()) as resp:
            body = resp.read()
            return resp.status, body if binary else json.loads(body.decode())
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:200]
        die("HTTP %d %s%s（%s）" % (e.code, e.reason, ("：" + detail) if detail else "", url),
            2 if e.code == 401 else 3)
    except (urllib.error.URLError, socket.timeout, OSError) as e:
        die("无法连接设备 %s：%s（设备可能正在浅睡眠，稍后重试或先唤醒）" % (url, e), 4)


def cache_path(host_arg):
    return TOKEN_CACHE.replace("{host}", host_arg.replace(":", "_"))


def get_token(host_arg, args):
    """--token 优先，其次缓存，最后用 --password 登录。"""
    if getattr(args, "token", None):
        return args.token
    cache = cache_path(host_arg)
    if os.path.exists(cache):
        try:
            with open(cache) as f:
                tok = json.load(f).get("token")
            if tok:
                # 校验缓存 token 是否仍有效
                status, data = http_request(host_arg, "/api/auth/status", token=tok)
                if status == 200 and data.get("authenticated"):
                    return tok
        except SystemExit:
            raise
        except Exception:
            pass  # 缓存损坏/失效，走重新登录
    if not getattr(args, "password", None):
        die("没有可用会话：提供 --password 让我登录，或 --token 直接指定会话令牌")
    status, data = http_request(host_arg, "/api/auth/login",
                                form={"password": args.password})
    if not data.get("token"):
        die("登录响应缺少 token：%s" % data)
    with open(cache, "w") as f:
        json.dump(data, f)
    return data["token"]


# ------------------------------------------------------- 帧解析 / PNG ----

def parse_frame(buf):
    """解析 EPM1 帧（快照响应与 WS 二进制同格式），返回灰度字节 + 元信息。"""
    if len(buf) < 16 or buf[0:4] != MAGIC:
        die("帧头不合法（期望 'EPM1'，收到 %r）" % buf[:4])
    fmt = buf[5]
    seq = struct.unpack_from("<I", buf, 6)[0]
    w = struct.unpack_from("<H", buf, 10)[0]
    h = struct.unpack_from("<H", buf, 12)[0]
    px = buf[16:]
    if w == 0 or h == 0:
        die("帧尺寸非法 w=%d h=%d" % (w, h))
    gray = bytearray(w * h)
    if fmt == FMT_BW:
        row = w // 8
        if len(px) < row * h:
            die("1bpp 帧数据不足：需 %d，收到 %d" % (row * h, len(px)))
        for y in range(h):
            base = y * row
            di = y * w
            for xb in range(row):
                b = px[base + xb]
                for k in range(8):
                    gray[di + xb * 8 + k] = 255 if (b & (0x80 >> k)) else 0
    elif fmt == FMT_G4:
        row = w // 4
        if len(px) < row * h:
            die("2bpp 帧数据不足：需 %d，收到 %d" % (row * h, len(px)))
        for y in range(h):
            base = y * row
            di = y * w
            for xb in range(row):
                b = px[base + xb]
                for k in range(4):
                    gray[di + xb * 4 + k] = GRAY4_SHADE[(b >> (6 - 2 * k)) & 3]
    else:
        die("未知像素格式 %d" % fmt)
    return {"fmt": fmt, "seq": seq, "w": w, "h": h}, bytes(gray)


def write_png(path, w, h, gray):
    """8-bit 灰度 PNG（stdlib 手写编码器，免 Pillow 依赖）。"""
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + bytes(gray[y * w:(y + 1) * w]) for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def save_frame(buf, out_path):
    meta, gray = parse_frame(buf)
    write_png(out_path, meta["w"], meta["h"], gray)
    return meta


# ------------------------------------------------------------- WS 客户端 ----

def ws_connect(host_arg, token, timeout=8):
    """极简 RFC6455 客户端（TLS）：完成握手，返回 (sock, fd)。

    只实现本协议需要的部分：发 masked TEXT 帧、收 TEXT/BINARY/PING/CLOSE。
    """
    host, port = split_host(host_arg)
    raw = socket.create_connection((host, port), timeout=timeout)
    sock = http_context().wrap_socket(raw, server_hostname=host)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET /api/screen/ws?token=%s HTTP/1.1\r\n"
           "Host: %s:%d\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n"
           % (urllib.parse.quote(token), host, port, key))
    sock.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        part = sock.recv(4096)
        if not part:
            die("WS 握手被设备关闭（token 无效或固件未开 WS 支持）")
        resp += part
        if len(resp) > 65536:
            die("WS 握手响应异常过大")
    head = resp.split(b"\r\n\r\n", 1)[0].decode(errors="replace")
    status_line = head.splitlines()[0] if head else ""
    if " 101 " not in status_line:
        die("WS 握手未升级（101）：服务器响应 %r" % status_line)
    fd = sock.fileno()
    sock.settimeout(timeout)
    return sock, fd


def ws_recv_frame(sock):
    """收一帧；返回 (opcode, payload)。PING 自动回 PONG。"""
    while True:
        hdr = _recv_exact(sock, 2)
        fin_op = hdr[0]
        opcode = fin_op & 0x0F
        masked = hdr[1] & 0x80
        ln = hdr[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", _recv_exact(sock, 2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", _recv_exact(sock, 8))[0]
        mask = _recv_exact(sock, 4) if masked else None
        payload = _recv_exact(sock, ln) if ln else b""
        if mask:
            payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
        if opcode == 0x9:  # PING -> PONG
            _ws_send(sock, 0xA, payload)
            continue
        if opcode == 0x8:  # CLOSE
            raise ConnectionError("设备关闭了 WS 连接")
        return opcode, payload


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        part = sock.recv(n - len(buf))
        if not part:
            raise ConnectionError("WS 连接中断")
        buf += part
    return buf


def _ws_send(sock, opcode, payload):
    mask = os.urandom(4)
    hdr = bytearray([0x80 | opcode])
    ln = len(payload)
    if ln < 126:
        hdr.append(0x80 | ln)
    elif ln < 65536:
        hdr.append(0x80 | 126)
        hdr += struct.pack(">H", ln)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack(">Q", ln)
    masked = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
    sock.sendall(bytes(hdr) + mask + masked)


def ws_send_json(sock, obj):
    _ws_send(sock, 0x1, json.dumps(obj, separators=(",", ":")).encode())


# ------------------------------------------------------------- 子命令 ----

def cmd_login(args):
    tok = get_token(args.host, args)  # password 缺失时内部报错
    status, data = http_request(args.host, "/api/auth/status", token=tok)
    print("登录成功，token 已缓存（%s）：%s" % (cache_path(args.host), data))


def cmd_snap(args):
    tok = get_token(args.host, args)
    status, body = http_request(args.host, "/api/screen/snapshot", token=tok, binary=True)
    out = args.out or "espaperplay_screen.png"
    meta = save_frame(body, out)
    print("已保存 %s（%dx%d %s seq=%d）" % (
        out, meta["w"], meta["h"], "黑白" if meta["fmt"] == FMT_BW else "四灰", meta["seq"]))


def cmd_watch(args):
    tok = get_token(args.host, args)
    os.makedirs(args.dir, exist_ok=True)
    last_seq = None
    saved = 0
    for i in range(args.count):
        status, data = http_request(args.host, "/api/auth/status", token=tok)  # 顺带心跳保活
        status, body = http_request(args.host, "/api/screen/snapshot", token=tok, binary=True)
        meta, _ = parse_frame(body)
        if meta["seq"] != last_seq:
            out = os.path.join(args.dir, "frame_%03d_seq%06d.png" % (i, meta["seq"]))
            save_frame(body, out)
            print("frame %d/%d: %s（seq=%d %s）" % (
                i + 1, args.count, out, meta["seq"],
                "黑白" if meta["fmt"] == FMT_BW else "四灰"))
            last_seq = meta["seq"]
            saved += 1
        else:
            print("frame %d/%d: 画面未变化（seq=%d），跳过" % (i + 1, args.count, meta["seq"]))
        if i + 1 < args.count:
            time.sleep(args.interval)
    print("共保存 %d 张到 %s" % (saved, args.dir))


def _ws_session(args):
    tok = get_token(args.host, args)
    sock, _ = ws_connect(args.host, tok)
    return sock


def cmd_touch(args):
    sock = _ws_session(args)
    try:
        if args.action == "up":
            ws_send_json(sock, {"t": "touch", "down": False})
        else:
            ws_send_json(sock, {"t": "touch", "x": args.x, "y": args.y, "down": True})
        # 等设备回执（err/pong）；注入成功本身静默
        try:
            op, payload = ws_recv_frame(sock)
            if op == 0x1:
                msg = json.loads(payload.decode())
                if msg.get("t") == "err":
                    die("设备返回错误：%s" % msg.get("msg"))
        except socket.timeout:
            pass  # 无回执 = 注入成功（成功不回复）
    finally:
        sock.close()
    print("已注入 touch %s" % (("up" if args.action == "up" else "%d,%d down" % (args.x, args.y))))


def cmd_tap(args):
    sock = _ws_session(args)
    try:
        ws_send_json(sock, {"t": "touch", "x": args.x, "y": args.y, "down": True})
        time.sleep(0.15)
        ws_send_json(sock, {"t": "touch", "down": False})
        time.sleep(0.3)
        try:
            op, payload = ws_recv_frame(sock)
            if op == 0x1:
                msg = json.loads(payload.decode())
                if msg.get("t") == "err":
                    die("设备返回错误：%s" % msg.get("msg"))
        except socket.timeout:
            pass
    finally:
        sock.close()
    print("已注入点击 (%d, %d)——注意 e-ink 刷新约 0.4~3s 后画面才变化" % (args.x, args.y))


def cmd_key(args):
    sock = _ws_session(args)
    try:
        ws_send_json(sock, {"t": "key", "action": args.action})
        time.sleep(0.3)
        try:
            op, payload = ws_recv_frame(sock)
            if op == 0x1:
                msg = json.loads(payload.decode())
                if msg.get("t") == "err":
                    die("设备返回错误：%s" % msg.get("msg"))
        except socket.timeout:
            pass
    finally:
        sock.close()
    print("已注入 BOOT 键动作 %s" % args.action)


def cmd_refresh(args):
    sock = _ws_session(args)
    try:
        ws_send_json(sock, {"t": "refresh"})
        time.sleep(0.3)
        try:
            op, payload = ws_recv_frame(sock)
            if op == 0x1:
                msg = json.loads(payload.decode())
                if msg.get("t") == "err":
                    die("设备拒绝全刷：%s" % msg.get("msg"))
        except socket.timeout:
            pass
    finally:
        sock.close()
    print("已请求强制全刷（FULL_FORCE 约 1.7s 或四灰约 2.5s）")


def cmd_live(args):
    os.makedirs(args.dir, exist_ok=True)
    sock = _ws_session(args)
    got = 0
    deadline = time.time() + args.timeout
    try:
        while got < args.count:
            remaining = deadline - time.time()
            if remaining <= 0:
                print("提示：等待 %ds 只收到 %d/%d 帧——期间设备没有刷新。"
                      "设备只在刷屏时推帧，可先 tap 触发界面变化再重试，"
                      "或改用 snap 拉当前画面。" % (args.timeout, got, args.count))
                break
            sock.settimeout(min(remaining, 5.0))
            try:
                op, payload = ws_recv_frame(sock)
            except TimeoutError:
                continue
            if op != 0x2:
                continue
            got += 1
            out = os.path.join(args.dir, "live_%02d.png" % got)
            meta = save_frame(payload, out)
            print("live %d/%d: %s（seq=%d %s）" % (
                got, args.count, out, meta["seq"],
                "黑白" if meta["fmt"] == FMT_BW else "四灰"))
    finally:
        sock.close()
    if got:
        print("共保存 %d 张到 %s" % (got, args.dir))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", required=True, help="设备地址（如 192.168.4.1 或 10.0.0.7:8443）")
    p.add_argument("--password", help="控制台密码（登录用；成功后缓存 token）")
    p.add_argument("--token", help="已有会话 token（跳过登录）")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("login", help="登录并缓存 token")

    s = sub.add_parser("snap", help="拉取当前画面存 PNG")
    s.add_argument("-o", "--out", help="输出 PNG 路径")

    s = sub.add_parser("watch", help="轮询快照（画面未变自动跳过）")
    s.add_argument("-n", "--count", type=int, default=5)
    s.add_argument("-i", "--interval", type=float, default=1.0)
    s.add_argument("-d", "--dir", default="espaperplay_frames")

    s = sub.add_parser("tap", help="注入一次点击（down+150ms+up）")
    s.add_argument("x", type=int)
    s.add_argument("y", type=int)

    s = sub.add_parser("touch", help="注入单条触摸帧")
    s.add_argument("x", type=int, nargs="?")
    s.add_argument("y", type=int, nargs="?")
    s.add_argument("action", choices=["down", "up"])

    s = sub.add_parser("key", help="注入 BOOT 键动作")
    s.add_argument("action", choices=["press_down", "press_up", "click", "double_click",
                                      "long_press_start", "long_press_hold", "long_press_up"])

    sub.add_parser("refresh", help="请求强制全刷清残影")

    s = sub.add_parser("live", help="WS 连接连收 N 帧（等待设备刷新）")
    s.add_argument("-n", "--count", type=int, default=3)
    s.add_argument("-d", "--dir", default="espaperplay_live")
    s.add_argument("-t", "--timeout", type=float, default=60.0,
                   help="总等待预算秒数（设备只在刷屏时推帧）")

    args = p.parse_args()
    fn = {
        "login": cmd_login, "snap": cmd_snap, "watch": cmd_watch, "tap": cmd_tap,
        "touch": cmd_touch, "key": cmd_key, "refresh": cmd_refresh, "live": cmd_live,
    }[args.cmd]
    fn(args)


if __name__ == "__main__":
    main()
