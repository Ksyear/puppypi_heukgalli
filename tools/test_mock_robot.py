#!/usr/bin/env python3
"""Self-test for mock_robot.py (stdlib unittest). Starts the mock on 127.0.0.1 in a subprocess.

  python3 test_mock_robot.py -v
  MOCK_ROBOT_LOG=1 python3 test_mock_robot.py -v     # also print the mock's own log at the end
  MOCK_JPEG=sips python3 test_mock_robot.py -v       # force a JPEG backend (auto|pillow|sips)
"""

import base64
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
import unittest
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = "127.0.0.1"


def http_json(path, port=8090):
    with urllib.request.urlopen("http://%s:%d%s" % (HOST, port, path), timeout=3) as resp:
        return resp.headers, json.loads(resp.read())


class V2Client:
    """Minimal Protocol v2 sender: one UUID, increasing sequence, waits for the matching ACK."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.connect((HOST, 5001))
        self.sock.settimeout(1.0)
        self.client_id = str(uuid.uuid4())
        self.seq = 0

    def send(self, kind, **fields):
        self.seq += 1
        pkt = {"protocol": 2, "type": kind, "client_id": self.client_id, "sequence": self.seq, **fields}
        self.sock.send(json.dumps(pkt).encode())
        while True:
            ack = json.loads(self.sock.recv(4096))
            if ack["sequence"] == self.seq:
                return ack

    def command(self, active, forward=0.0, turn=0.0):
        return self.send("command", control_active=active, forward=forward, turn=turn)

    def close(self):
        try:
            self.send("disconnect")  # release ownership so the next test can take over at once
        except (OSError, ValueError):
            pass
        self.sock.close()


class WsClient:
    """Minimal masked-frame WebSocket client for the rosbridge test."""

    def __init__(self, port=9090):
        self.sock = socket.create_connection((HOST, port), timeout=3)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(("GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (HOST, port, key)).encode())
        self.rfile = self.sock.makefile("rb")
        self.status_line = self.rfile.readline().decode()
        while self.rfile.readline() not in (b"\r\n", b""):
            pass

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81, 0x80 | n]) if n < 126 else bytes([0x81, 0x80 | 126]) + n.to_bytes(2, "big")
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self):
        b0, b1 = self.rfile.read(2)
        n = b1 & 0x7F
        if n == 126:
            n = int.from_bytes(self.rfile.read(2), "big")
        elif n == 127:
            n = int.from_bytes(self.rfile.read(8), "big")
        return b0 & 0x0F, self.rfile.read(n)

    def recv_json(self, pred, timeout=3.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            op, data = self.recv()
            if op == 0x1:
                msg = json.loads(data)
                if pred(msg):
                    return msg
        raise AssertionError("no matching rosbridge message")

    def close(self):
        self.sock.close()


class MockRobotTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmd = [sys.executable, "-B", os.path.join(HERE, "mock_robot.py"), "--host", HOST,
               "--jpeg", os.environ.get("MOCK_JPEG", "auto")]
        cls.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        cls.log = []
        threading.Thread(target=lambda: cls.log.extend(iter(cls.proc.stdout.readline, "")), daemon=True).start()
        end = time.monotonic() + 60  # first sips run converts the cached camera frames
        while time.monotonic() < end:
            if cls.proc.poll() is not None:
                raise RuntimeError("mock exited early:\n" + "".join(cls.log))
            try:
                http_json("/api/media")
                return
            except OSError:
                time.sleep(0.2)
        raise RuntimeError("mock did not start:\n" + "".join(cls.log))

    @classmethod
    def tearDownClass(cls):
        cls.proc.send_signal(signal.SIGINT)
        code = cls.proc.wait(timeout=10)
        time.sleep(0.1)
        if os.environ.get("MOCK_ROBOT_LOG"):
            sys.stderr.write("\n----- mock_robot.py log -----\n" + "".join(cls.log))
        assert code == 0, "mock exit code %s after SIGINT" % code

    def test_1_neutral_command_ack_idle(self):
        c = V2Client()
        try:
            ack = c.command(False)
            self.assertEqual(set(ack), {"protocol", "type", "client_id", "sequence", "accepted", "reason",
                                        "emergency_stop", "state"})
            self.assertEqual((ack["accepted"], ack["reason"], ack["state"]), (True, "ok", "idle"))
            self.assertEqual(ack["client_id"], c.client_id)
        finally:
            c.close()

    def test_2_active_command_moves_pose(self):
        c = V2Client()
        try:
            self.assertEqual(c.command(False)["state"], "idle")
            x0 = http_json("/api/status")[1]["map"]["robot"]["x_m"]
            states = set()
            end = time.monotonic() + 1.6
            while time.monotonic() < end:  # 20 Hz like the Quest app
                states.add(c.command(True, 1.0, 0.0)["state"])
                time.sleep(0.05)
            for _ in range(24):  # neutral for 1.2 s: the map pose in /api/status refreshes at 1 Hz
                c.command(True, 0.0, 0.0)
                time.sleep(0.05)
            x1 = http_json("/api/status")[1]["map"]["robot"]["x_m"]
            self.assertEqual(states, {"active"})
            self.assertGreater(x1 - x0, 0.2, "pose x %.3f -> %.3f" % (x0, x1))  # 25 cm/s for 1.6 s ~ 0.4 m
        finally:
            c.close()

    def test_3_estop_latches(self):
        c = V2Client()
        try:
            c.command(False)
            ack = c.send("emergency_stop")
            self.assertTrue(ack["accepted"])
            self.assertTrue(ack["emergency_stop"])
            self.assertEqual(ack["state"], "emergency_stop")
            c.command(False)
            ack = c.command(True, 1.0, 0.0)
            self.assertEqual((ack["accepted"], ack["reason"], ack["state"]), (False, "emergency_stop", "emergency_stop"))
            time.sleep(0.6)
            self.assertTrue(http_json("/api/status")[1]["arbiter"]["estop"])
            ack = c.send("clear_emergency")
            self.assertFalse(ack["emergency_stop"])
            self.assertEqual(c.command(False)["state"], "idle")
        finally:
            c.close()

    def test_4_discovery_reply(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(2)
        try:
            nonce = uuid.uuid4().hex
            s.sendto(("PUPPYPI_DISCOVER/1|" + nonce).encode(), (HOST, 5002))
            data, src = s.recvfrom(1024)
        finally:
            s.close()
        fields = data.decode("ascii").split("|")
        self.assertEqual(src[1], 5002)
        self.assertTrue(80 <= len(data) <= 640, len(data))
        self.assertEqual(len(fields), 6)
        self.assertEqual((fields[0], fields[1], fields[4], fields[5]), ("PUPPYPI_HERE/1", nonce, "5001", "2"))
        self.assertRegex(fields[2], r"\A[0-9a-f]{32}\Z")

    def test_5_api_map_json(self):
        headers, m = http_json("/api/map")
        self.assertEqual(headers["Access-Control-Allow-Origin"], "*")
        for k in ("media", "frame", "image_seq", "stamp_unix", "age_s", "width_px", "height_px", "left_m",
                  "bottom_m", "px_per_m", "resolution_m", "robot", "session", "scans", "integrations", "map_size_m"):
            self.assertIn(k, m)
        r = m["robot"]
        u = (r["x_m"] - m["left_m"]) * m["px_per_m"]
        v = m["height_px"] - (r["y_m"] - m["bottom_m"]) * m["px_per_m"]
        self.assertLessEqual(abs(u - r["u_px"]), 1.5)
        self.assertLessEqual(abs(v - r["v_px"]), 1.5)

    def test_6_mjpeg_part(self):
        for topic in ("/usb_cam/image_raw", "/lidar_map/image"):
            with self.subTest(topic=topic):
                s = socket.create_connection((HOST, 8080), timeout=5)
                try:
                    s.sendall(("GET /stream?topic=%s&type=ros_compressed HTTP/1.1\r\nHost: %s\r\n\r\n"
                               % (topic, HOST)).encode())
                    f = s.makefile("rb")
                    self.assertTrue(f.readline().startswith(b"HTTP/1.0 200"))
                    headers = {}
                    while (line := f.readline()) not in (b"\r\n", b""):
                        k, _, v = line.decode().partition(":")
                        headers[k.strip().lower()] = v.strip()
                    self.assertEqual(headers["content-type"], "multipart/x-mixed-replace;boundary=boundarydonotcross")
                    self.assertEqual(f.readline(), b"--boundarydonotcross\r\n")
                    part = {}
                    while (line := f.readline()) not in (b"\r\n", b""):
                        k, _, v = line.decode().partition(":")
                        part[k.strip()] = v.strip()
                    self.assertEqual(part["Content-type"], "image/jpeg")
                    float(part["X-Timestamp"])
                    jpeg = f.read(int(part["Content-Length"]))
                    self.assertEqual(jpeg[:2], b"\xff\xd8")
                    self.assertEqual(jpeg[-2:], b"\xff\xd9")
                    self.assertEqual(f.readline(), b"\r\n")
                    self.assertEqual(f.readline(), b"--boundarydonotcross\r\n")
                finally:
                    s.close()

    def test_7_rosbridge_topics_and_scan(self):
        ws = WsClient()
        try:
            self.assertIn(" 101 ", ws.status_line)
            ws.send({"op": "call_service", "service": "/rosapi/topics", "type": "rosapi/Topics", "args": {},
                     "id": "t1"})
            resp = ws.recv_json(lambda m: m.get("op") == "service_response")
            self.assertEqual((resp["service"], resp["id"], resp["result"]), ("/rosapi/topics", "t1", True))
            self.assertIn("/scan", resp["values"]["topics"])
            self.assertEqual(len(resp["values"]["topics"]), len(resp["values"]["types"]))
            ws.send({"op": "subscribe", "topic": "/scan", "type": "sensor_msgs/LaserScan", "throttle_rate": 0})
            scan = ws.recv_json(lambda m: m.get("op") == "publish" and m.get("topic") == "/scan")["msg"]
            self.assertEqual(len(scan["ranges"]), 450)
            self.assertAlmostEqual(scan["angle_min"], -3.141592653589793)
            self.assertAlmostEqual(scan["angle_increment"], 2 * 3.141592653589793 / 450)
            self.assertEqual(scan["range_max"], 12.0)
            self.assertTrue(all(0.02 <= r <= 12.0 for r in scan["ranges"]))
        finally:
            ws.close()


if __name__ == "__main__":
    unittest.main()
