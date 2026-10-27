#!/usr/bin/env python3
"""本地 SIP/RTP + HTTP/WS 黑盒回归；仅标准库，所有端点在 finally 中关闭。"""
import argparse
import base64
import hashlib
import http.server
import json
import math
import pathlib
import queue
import re
import socket
import socketserver
import struct
import threading
import time
import uuid


class ESL:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", 8021), 3)
        self.file = self.sock.makefile("rb")
        self.read()
        self.sock.sendall(b"auth ClueCon\n\n")
        assert "+OK" in self.read()[0].get("Reply-Text", "")

    def read(self):
        headers = {}
        while True:
            line = self.file.readline()
            if not line:
                raise EOFError()
            if not line.strip():
                break
            key, value = line.decode().split(":", 1)
            headers[key] = value.strip()
        return headers, self.file.read(int(headers.get("Content-Length", 0))).decode()

    def api(self, command):
        self.sock.sendall(("api " + command + "\n\n").encode())
        return self.read()[1].strip()

    def close(self):
        self.file.close()
        self.sock.close()


def ws_send(sock, data, opcode=2):
    n = len(data)
    header = bytes([0x80 | opcode])
    header += bytes([n]) if n < 126 else (b"\x7e" + struct.pack("!H", n) if n < 65536 else b"\x7f" + struct.pack("!Q", n))
    sock.sendall(header + data)


def pcm(scenario="normal"):
    # 一秒音频，尾部 80ms 为不同频率，便于发现尾音丢失。
    rate = 24000 if scenario in ("rate24", "url-rate") else 16000
    count = rate if scenario in ("rate24", "url-rate") else (320 if scenario == "short" else (16317 if scenario == "tail" else 16000))
    values = [int(12000 * math.sin(2 * math.pi * (700 if i < count - 1280 else 1200) * i / rate)) for i in range(count)]
    data = struct.pack(f"<{count}h", *values)
    return data + b"\x01" if scenario == "partial" else data


class MediaServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class MediaHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *_):
        pass

    def do_GET(self):
        scenario = self.path.split("?", 1)[0].rsplit("/", 1)[-1]
        data = pcm(scenario)
        chunks = [data[i:i+997] for i in range(0, len(data), 997)]
        def send_chunks(send):
            for i, chunk in enumerate(chunks):
                send(chunk)
                if i == 16 and scenario in ("jitter", "watchdog"):
                    self.server.ready.set()
                    self.server.stop.wait(1.1)
        if self.headers.get("Upgrade", "").lower() == "websocket":
            key = self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", base64.b64encode(hashlib.sha1(key.encode()).digest()).decode())
            self.end_headers()
            try:
                signal = {"type": "speak_start"}
                if scenario in ("conflict", "tail", "rate24"):
                    signal["data"] = {"rate": 24000 if scenario == "rate24" else 16000}
                ws_send(self.connection, json.dumps(signal).encode(), 1)
                if scenario == "url-rate":
                    self.server.ready.set()
                    self.server.stop.wait(0.45)
                send_chunks(lambda chunk: ws_send(self.connection, chunk))
                ws_send(self.connection, b"")  # 零长帧与文本 EOF 同时通知。
                ws_send(self.connection, b'{"type":"speak_done"}', 1)
                self.server.ready.set()
                self.connection.settimeout(0.2)
                while not self.server.stop.is_set():
                    try:
                        if not self.connection.recv(65536):
                            break
                    except socket.timeout:
                        pass
            except (OSError, ConnectionError):
                pass
        else:
            self.send_response(503 if scenario == "http-error" else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                def send(chunk):
                    self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                    self.wfile.flush()
                send_chunks(send)
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            except OSError:
                pass


class SIP:
    def __init__(self, host):
        self.host = host
        self.sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sip.bind((host, 0))
        self.rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rtp.bind((host, 0))
        self.stop = threading.Event()
        self.packets = []
        self.peer = None
        self.codec = 0
        self.threads = [threading.Thread(target=f, daemon=True) for f in (self.signaling, self.media)]
        for t in self.threads:
            t.start()

    def signaling(self):
        self.sip.settimeout(0.1)
        while not self.stop.is_set():
            try:
                raw, addr = self.sip.recvfrom(65536)
            except socket.timeout:
                continue
            text = raw.decode()
            if text.startswith("ACK"):
                continue
            headers = {k.lower(): v.strip() for k, v in re.findall(r"^([^ :\r\n]+):\s*(.+)\r?$", text, re.M)}
            if text.startswith("INVITE"):
                self.peer = (re.search(r"c=IN IP4 ([\d.]+)", text)[1], int(re.search(r"m=audio (\d+)", text)[1]))
                self.codec = int(re.search(r"m=audio \d+ RTP/AVP (\d+)", text)[1])
                name = "PCMU" if self.codec == 0 else "PCMA"
                body = (f"v=0\r\no=test 1 1 IN IP4 {self.host}\r\ns=test\r\nc=IN IP4 {self.host}\r\nt=0 0\r\nm=audio {self.rtp.getsockname()[1]} RTP/AVP {self.codec}\r\na=rtpmap:{self.codec} {name}/8000\r\na=ptime:20\r\na=sendrecv\r\n")
            else:
                body = ""
            response = "SIP/2.0 200 OK\r\n" + "".join(f"{k}: {headers[k.lower()]}\r\n" for k in ("Via", "From", "Call-ID", "CSeq"))
            response += f"To: {headers['to']};tag=localtest\r\nContact: <sip:test@{self.host}:{self.sip.getsockname()[1]}>\r\nContent-Type: application/sdp\r\nContent-Length: {len(body)}\r\n\r\n{body}"
            self.sip.sendto(response.encode(), addr)

    def media(self):
        self.rtp.settimeout(0.005)
        seq, ts, deadline = 0, 0, None
        while not self.stop.is_set():
            now = time.monotonic()
            if self.peer and deadline is None:
                deadline = now
            if self.peer and now >= deadline:
                silence = b"\xff" if self.codec == 0 else b"\xd5"
                self.rtp.sendto(struct.pack("!BBHII", 128, self.codec, seq % 65536, ts % (2**32), 1234) + silence * 160, self.peer)
                seq, ts, deadline = seq + 1, ts + 160, deadline + 0.02
            try:
                data, source = self.rtp.recvfrom(65536)
                if len(data) >= 12:
                    self.packets.append((time.monotonic(), data, source))
            except socket.timeout:
                pass

    def close(self):
        self.stop.set()
        for t in self.threads:
            t.join(2)
        self.sip.close()
        self.rtp.close()


def decode(payload, codec):
    result = []
    for x in payload:
        if codec == 0:
            u = (~x) & 255
            v = (((u & 15) << 3) + 132) << ((u >> 4) & 7)
            result.append(132 - v if u & 128 else v - 132)
        else:
            a = x ^ 0x55
            v = ((a & 15) << 4) + (8 if ((a >> 4) & 7) == 0 else 264)
            if ((a >> 4) & 7) > 1:
                v <<= ((a >> 4) & 7) - 1
            result.append(v if a & 128 else -v)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--host", required=True, help="FreeSWITCH 可访问的本机 IPv4 地址")
    parser.add_argument("--scenarios", default="normal,short,tail,jitter,watchdog,partial,http-error,conflict,rate24,url-rate,default,aliases")
    parser.add_argument("--linear-timestamps", action="store_true", help="仅测试呼叫启用 SEND_LINEAR_TIMESTAMPS，隔离 soft timer 跳步")
    parser.add_argument("--verify", action="store_true", help="对改造后的输出执行验收断言")
    args = parser.parse_args()
    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    api = ESL()
    server = MediaServer(("0.0.0.0", 0), MediaHandler)
    server.stop, server.ready = threading.Event(), threading.Event()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    results = []
    events = ESL()
    events.sock.sendall(b"event json PLAYBACK_STOP CHANNEL_EXECUTE_COMPLETE CUSTOM mod_audio_fork::error\n\n")
    events.read()
    event_queue = queue.Queue()
    def receive_events():
        try:
            while True:
                headers, body = events.read()
                if body:
                    event_queue.put(json.loads(body))
        except (OSError, EOFError):
            pass
    event_worker = threading.Thread(target=receive_events, daemon=True)
    event_worker.start()
    try:
        for codec in ("PCMU", "PCMA"):
          for scenario in args.scenarios.split(","):
            for driver in ("http", "ws"):
                if (scenario == "conflict" and driver == "http") or (scenario == "http-error" and driver == "ws"):
                    continue
                endpoint = SIP(args.host)
                call = str(uuid.uuid4())
                try:
                    rtp_option = ",rtp_manual_rtp_bugs=SEND_LINEAR_TIMESTAMPS" if args.linear_timestamps else ""
                    response = api.api(f"originate {{origination_uuid={call},absolute_codec_string={codec},originate_timeout=5{rtp_option}}}sofia/external/test@{args.host}:{endpoint.sip.getsockname()[1]} &park()")
                    assert "+OK" in response, response
                    base = f"{args.host}:{server.server_port}"
                    if driver == "ws":
                        server.ready.clear()
                        response = api.api(f"uuid_audio_fork {call} start ws://{base}/{scenario} mono 16000")
                        assert "+OK" in response, response
                        assert server.ready.wait(3), "WS audio did not arrive"
                        if scenario not in ("jitter", "watchdog", "url-rate"):
                            time.sleep(0.15)
                        url = f"audio_fork://{call}?rate={'24000' if scenario in ('conflict', 'rate24', 'url-rate') else '16000'}"
                    else:
                        url = f"audio_fork://http://{base}/{scenario}?sampling={24000 if scenario in ('rate24', 'url-rate') else 16000}"
                    if scenario == "default":
                        url = url.split("?", 1)[0]
                    if scenario == "aliases":
                        url += "&rate=24000" if driver == "http" else "&sampling=24000"
                    if scenario == "watchdog":
                        url += "&watchdog=200"
                    started = time.monotonic()
                    response = api.api(f"uuid_broadcast {call} {url} aleg")
                    assert "+OK" in response, response
                    playback_event = None
                    case_events = []
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        try:
                            event = event_queue.get(timeout=max(0.01, deadline-time.monotonic()))
                            if event.get("Unique-ID") == call:
                                case_events.append(event)
                            if event.get("Unique-ID") == call and (event.get("Event-Name") == "PLAYBACK_STOP" or event.get("Application") == "playback"):
                                playback_event = event
                                break
                        except queue.Empty:
                            break
                    elapsed = time.monotonic() - started
                    assert playback_event, "no PLAYBACK_STOP event"
                    time.sleep(0.04)
                    packets = [data for _, data, _ in endpoint.packets if (data[1] & 127) in (0, 8)]
                    samples = [v for data in packets for v in decode(data[12:], data[1] & 127)]
                    active = [i for i in range(0, len(samples)-79, 80) if sum(x*x for x in samples[i:i+80])/80 > 1e6]
                    deltas = [((struct.unpack_from("!I", b, 4)[0] - struct.unpack_from("!I", a, 4)[0]) % 2**32) for a, b in zip(packets, packets[1:])]
                    seq_deltas = [((struct.unpack_from("!H", b, 2)[0] - struct.unpack_from("!H", a, 2)[0]) % 65536) for a, b in zip(packets, packets[1:])]
                    clock_skips = sum(d == 320 and bool(b[1] & 128) for d, b in zip(deltas, packets[1:]))
                    unexpected_timestamps = [d for d, b in zip(deltas, packets[1:]) if d != 160 and not (d == 320 and b[1] & 128)]
                    result = dict(codec=codec, driver=driver, scenario=scenario, call=call, elapsed=round(elapsed, 3),
                                  playback_samples=playback_event.get("variable_playback_last_offset_pos"),
                                  errors=[e.get("_body") for e in case_events if e.get("Event-Subclass") == "mod_audio_fork::error"],
                                  packets=len(packets), active_samples=len(active)*80,
                                  active_span_samples=active[-1]+80-active[0] if active else 0,
                                  marked_clock_skips=clock_skips, unexpected_timestamps=unexpected_timestamps,
                                  sequence_deltas=sorted(set(seq_deltas)),
                                  timestamp_deltas=sorted(set(deltas)), tail_peak=max(map(abs, samples[active[-1]:active[-1]+80])) if active else 0)
                    results.append(result)
                    print(json.dumps(result), flush=True)
                    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                    prefix = f"{driver}-{codec}-{scenario}"
                    with (out / f"{prefix}.rtp").open("wb") as f:
                        for data in packets:
                            f.write(struct.pack("!H", len(data)) + data)
                    with (out / f"{prefix}.pcap").open("wb") as f:
                        f.write(struct.pack("<IHHIIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 101))
                        wall_offset = time.time() - time.monotonic()
                        for when, data, source in endpoint.packets:
                            if (data[1] & 127) not in (0, 8):
                                continue
                            udp = struct.pack("!HHHH", source[1], endpoint.rtp.getsockname()[1], len(data)+8, 0) + data
                            ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, len(udp)+20, 0, 0, 64, 17, 0,
                                             socket.inet_aton(source[0]), socket.inet_aton(args.host))
                            checksum = sum(struct.unpack("!10H", ip))
                            checksum = (checksum & 65535) + (checksum >> 16)
                            checksum = (checksum & 65535) + (checksum >> 16)
                            ip = ip[:10] + struct.pack("!H", (~checksum) & 65535) + ip[12:]
                            packet = ip + udp
                            stamp = when + wall_offset
                            f.write(struct.pack("<IIII", int(stamp), int((stamp % 1)*1e6), len(packet), len(packet)) + packet)
                    (out / f"{prefix}.pcm").write_bytes(struct.pack(f"<{len(samples)}h", *samples))
                    (out / f"{prefix}-event.json").write_text(json.dumps(playback_event, indent=2))
                    if args.verify:
                        if scenario in ("normal", "short", "tail", "jitter", "rate24", "url-rate", "default"):
                            expected = 160 if scenario == "short" else (8159 if scenario == "tail" else 8000)
                            assert abs(result["active_samples"] - expected) <= 160, result
                            assert result["tail_peak"] > 5000, result
                            assert result["sequence_deltas"] == [1] or scenario == "short", result
                            assert not unexpected_timestamps, result
                            if args.linear_timestamps:
                                assert result["timestamp_deltas"] == [160] or scenario == "short", result
                            if scenario not in ("jitter", "url-rate"):
                                assert int(result["playback_samples"]) == expected, result
                            else:
                                assert int(result["playback_samples"]) < 14000, result
                        if scenario in ("partial", "http-error", "conflict", "aliases"):
                            assert not result["active_samples"], result
                        if scenario == "partial" and driver == "ws":
                            assert any("pcm_partial_frame" in str(error) for error in result["errors"]), result
                        if scenario == "watchdog":
                            # 排除 broadcast 排队与起播开销，直接量 RTP 末尾连续静音。
                            missing_ms = (int(result["playback_samples"]) - result["active_span_samples"]) / 8
                            assert 180 <= missing_ms <= 260 and elapsed < 1.2, result
                finally:
                    api.api(f"uuid_kill {call}")
                    endpoint.close()
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    finally:
        server.stop.set()
        server.shutdown()
        server.server_close()
        worker.join(2)
        events.sock.shutdown(socket.SHUT_RDWR)
        event_worker.join(2)
        events.close()
        api.close()


if __name__ == "__main__":
    main()
