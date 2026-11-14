#!/usr/bin/env python3
"""Mock PuppyPi robot, so a controller app can be tested without the real robot (Python 3 stdlib only).

  python3 mock_robot.py [--host 127.0.0.1] [--robot-src ".../PuppyPi_VR_ROS2/ros2_ws/src/puppy_vr_bridge"]

Every server binds on --host:
  UDP 5001  Protocol v2 control + ACK   real V2Session (protocol_v2.py) and PathFollower (path_follower.py)
  UDP 5002  discovery                   real make_reply() from discovery.py (broadcasts arrive only with --host 0.0.0.0)
  TCP 8090  media_server JSON           /api/media /api/map /api/status /api/path  (+ a tiny viewer at /)
            mock test hooks             /mock/obstacle?x=&y=&r=  /mock/obstacles  /mock/obstacles/clear  /mock/pose
  TCP 8080  web_video_server MJPEG      /stream and /snapshot, topic=/usb_cam/image_raw or /lidar_map/image
  TCP 9090  rosbridge v2 subset         call_service, advertise/publish, subscribe/unsubscribe (plain WebSocket),
                                        subscribe "compression":"png" like rosbridge_library (needs Pillow)

The robot pose (map frame, +x forward, +y left, origin = start) integrates the commanded velocity at 50 Hz
inside a fixed fake 6.5 x 4 m room with one box. /scan ray-casts the room, the box and the dynamic obstacles
added with /mock/obstacle; /map (OccupancyGrid, latched, 0.5 Hz) has the room and the box only.
JPEGs come from mock_assets.py (Pillow in tools/.venv, else sips).
This is a simulation: passing a test against it says nothing about the real robot, Quest or Wi-Fi.
Ctrl+C stops it.
"""

import argparse
import base64
import hashlib
import io
import json
import math
import os
import signal
import socket
import socketserver
import sys
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

sys.dont_write_bytecode = True  # never write __pycache__ into the (read-only) robot working copy

import mock_assets  # noqa: E402  (tools/mock_assets.py, next to this file)

# 로봇 저장소(github.com/Ksyear/PuppyPI_Cpp_ROS2)의 ros2_ws/src/puppy_vr_bridge. --robot-src 또는 환경 변수로 지정
DEFAULT_ROBOT_SRC = os.environ.get("PUPPYPI_ROBOT_SRC", "")
V2_PORT, DISCOVERY_PORT, VIDEO_PORT, MEDIA_PORT, ROSBRIDGE_PORT = 5001, 5002, 8080, 8090, 9090
ROBOT_NAME = "puppypi-mock"
ROBOT_ID = hashlib.sha256(b"puppypi-mock").hexdigest()[:32]
MEDIA = "puppypi-media/1"
CAMERA_TOPIC, MAP_TOPIC = "/usb_cam/image_raw", "/lidar_map/image"
STOP = (0.0, 0.0, 0.0)

# receiver_node.py parameter defaults (the robot's config/vr_bridge.yaml may override them)
MAX_FORWARD_CM_S, MAX_REVERSE_CM_S, MAX_YAW_RAD_S = 25.0, 5.0, 0.28
COMMAND_TIMEOUT_S, LEASE_TIMEOUT_S = 0.4, 1.0

# Fake world in the map frame (metres): inner wall faces of the room and one box obstacle
ROOM = (-1.25, -2.0, 5.25, 2.0)
BOX = (2.4, 0.5, 2.9, 1.0)
ROBOT_RADIUS = 0.15
# Map image geometry: fixed, the fake room counts as fully explored
MAP_W, MAP_H, MAP_LEFT, MAP_BOTTOM, MAP_PPM = 300, 200, -1.75, -2.5, 40.0
# LaserScan like the LD06 driver: 450 beams, counter-clockwise, 0 rad = robot forward
SCAN_BEAMS, SCAN_ANGLE_MIN, SCAN_INC = 450, -math.pi, 2 * math.pi / 450
SCAN_RANGE_MIN, SCAN_RANGE_MAX = 0.02, 12.0
# /map OccupancyGrid like lidar_mapper: 600 x 600 cells of 0.05 m, origin (-15, -15), row 0 at the origin's y
GRID_W, GRID_H, GRID_RES, GRID_X0, GRID_Y0, GRID_WALL_M = 600, 600, 0.05, -15.0, -15.0, 0.1
OBSTACLE_R = 0.12  # default radius of a /mock/obstacle

ACTION_SECONDS = 2.0
BLOCKED_ACTIONS = {"2_legs_stand"}
POSE_FIELDS = ("roll", "pitch", "yaw", "height", "x_shift", "stance_x", "stance_y", "run_time")
STAND = dict(roll=0.0, pitch=0.0, yaw=0.0, height=-10.0, x_shift=-0.5, stance_x=0.0, stance_y=0.0, run_time=500)

TOPICS = [("/scan", "sensor_msgs/msg/LaserScan"), ("/lidar_map/pose", "geometry_msgs/msg/PoseStamped"),
          ("/map", "nav_msgs/msg/OccupancyGrid"),
          ("/puppy_control/status", "std_msgs/msg/String"), ("/puppy_control/arbiter_state", "std_msgs/msg/String"),
          ("/puppy_control/pose", "puppy_control/msg/Pose")]
LATCHED = ("/puppy_control/status", "/puppy_control/arbiter_state", "/map")  # sent once right after subscribe
SERVICES = ["/puppy_control/go_home", "/puppy_control/runActionGroup", "/puppy_control/emergency_stop",
            "/puppy_control/reset_estop", "/lidar_mapper/save_map", "/lidar_mapper/reset_map",
            "/rosapi/topics", "/rosapi/services"]

_log_lock = threading.Lock()


def log(msg):
    now = time.time()
    with _log_lock:
        print("%s.%03d %s" % (time.strftime("%H:%M:%S", time.localtime(now)), int(now * 1000) % 1000, msg),
              flush=True)


def load_robot_modules(src):
    """Import protocol_v2, path_follower and discovery straight from the robot source (no ROS package)."""
    if not src:
        raise SystemExit("robot source not set: clone https://github.com/Ksyear/PuppyPI_Cpp_ROS2 and pass "
                         "--robot-src <clone>/ros2_ws/src/puppy_vr_bridge (or set PUPPYPI_ROBOT_SRC)")
    pkg = src if os.path.isfile(os.path.join(src, "protocol_v2.py")) else os.path.join(src, "puppy_vr_bridge")
    if not os.path.isfile(os.path.join(pkg, "protocol_v2.py")):
        raise SystemExit("protocol_v2.py not found under --robot-src %r" % src)
    sys.path.insert(0, pkg)
    import path_follower
    import protocol_v2
    try:
        import discovery
    except ImportError:  # 공개 로봇 저장소에 discovery.py 가 없으면 문서의 응답 형식으로 대신한다
        discovery = _SpecDiscovery
    return protocol_v2, path_follower, discovery


class _SpecDiscovery:
    """PUPPYPI_DISCOVER/1|<32hex> → PUPPYPI_HERE/1|<nonce>|<id>|<url-encoded name>|<port>|2 (robot doc 06 §1)."""

    @staticmethod
    def make_reply(packet, name, identity, control_port):
        prefix = b"PUPPYPI_DISCOVER/1|"
        nonce = packet[len(prefix):]
        if not packet.startswith(prefix) or len(nonce) != 32 or any(c not in b"0123456789abcdef" for c in nonce):
            return None
        from urllib.parse import quote
        return ("PUPPYPI_HERE/1|%s|%s|%s|%d|2" % (nonce.decode("ascii"), identity, quote(name, safe=""), control_port)).encode("ascii")


# ── fake world ──

def wrap(a):
    return math.atan2(math.sin(a), math.cos(a))


def touching(x, y, obstacles=()):
    """What the robot disc at (x, y) would touch: "wall", "box", "obstacle", or None when it is free there."""
    x0, y0, x1, y1 = ROOM
    r = ROBOT_RADIUS
    if not (x0 + r <= x <= x1 - r and y0 + r <= y <= y1 - r):
        return "wall"
    dx = max(BOX[0] - x, 0.0, x - BOX[2])
    dy = max(BOX[1] - y, 0.0, y - BOX[3])
    if math.hypot(dx, dy) < r:
        return "box"
    for ox, oy, orad in obstacles:
        if math.hypot(x - ox, y - oy) < r + orad:
            return "obstacle"
    return None


def ray_range(px, py, ang, obstacles=()):
    """Distance from (px, py) inside the room to the first wall, box face or obstacle circle along ang."""
    dx, dy = math.cos(ang), math.sin(ang)
    t = math.inf
    for ox, oy, r in obstacles:  # nearest ray/circle hit in front of the sensor (a sensor inside a circle ignores it)
        fx, fy = px - ox, py - oy
        b = fx * dx + fy * dy
        disc = b * b - (fx * fx + fy * fy - r * r)
        if disc >= 0 and -b - math.sqrt(disc) > 0:
            t = min(t, -b - math.sqrt(disc))
    x0, y0, x1, y1 = ROOM
    if dx > 1e-12:
        t = min(t, (x1 - px) / dx)
    elif dx < -1e-12:
        t = min(t, (x0 - px) / dx)
    if dy > 1e-12:
        t = min(t, (y1 - py) / dy)
    elif dy < -1e-12:
        t = min(t, (y0 - py) / dy)
    tmin, tmax = 0.0, math.inf  # slab test against the box
    for p, d, lo, hi in ((px, dx, BOX[0], BOX[2]), (py, dy, BOX[1], BOX[3])):
        if abs(d) < 1e-12:
            if p < lo or p > hi:
                return t
            continue
        a, b = sorted(((lo - p) / d, (hi - p) / d))
        tmin, tmax = max(tmin, a), min(tmax, b)
        if tmin > tmax:
            return t
    return min(t, tmin) if tmin > 0 else t


def occupancy_grid():
    """/map data, row-major from the origin corner: room inside 0 (seen, free), walls and box 100, rest -1."""
    data = [-1] * (GRID_W * GRID_H)

    def fill(x0, y0, x1, y1, value):  # cells covering [x0, x1) x [y0, y1); the room edges fall on cell edges
        c0, c1 = (int(round((v - GRID_X0) / GRID_RES)) for v in (x0, x1))
        r0, r1 = (int(round((v - GRID_Y0) / GRID_RES)) for v in (y0, y1))
        for row in range(r0, r1):
            data[row * GRID_W + c0:row * GRID_W + c1] = [value] * (c1 - c0)

    x0, y0, x1, y1 = ROOM
    fill(x0 - GRID_WALL_M, y0 - GRID_WALL_M, x1 + GRID_WALL_M, y1 + GRID_WALL_M, 100)
    fill(x0, y0, x1, y1, 0)
    fill(*BOX, 100)
    return data


def ros_stamp(t=None):
    t = time.time() if t is None else t
    return {"sec": int(t), "nanosec": int((t % 1.0) * 1e9)}


# ── the robot ──

class Feed:
    """Latest JPEG of one video topic, rendered lazily (only while someone is watching)."""

    def __init__(self, key_fn, render_fn):
        self.key_fn, self.render_fn = key_fn, render_fn
        self.lock = threading.Lock()
        self.key, self.jpeg, self.stamp = None, None, 0.0

    def latest(self):
        key = self.key_fn()
        with self.lock:
            if self.jpeg is None or key != self.key:
                self.jpeg, self.key, self.stamp = self.render_fn(key), key, time.time()
            return self.key, self.stamp, self.jpeg


class MockRobot:
    def __init__(self, host, protocol_v2, path_follower, discovery, encoder):
        self.host = host
        self.pv2, self.pf, self.disc = protocol_v2, path_follower, discovery
        self.encoder = encoder
        self.stop = threading.Event()
        self.lock = threading.RLock()  # one lock for session, follower, pose and arbiter
        self.session = protocol_v2.V2Session(MAX_FORWARD_CM_S, MAX_REVERSE_CM_S, MAX_YAW_RAD_S,
                                             COMMAND_TIMEOUT_S, LEASE_TIMEOUT_S, accept_estop_from_any_client=True)
        self.follower = path_follower.PathFollower(path_follower.FollowerConfig())
        self.target = (0.0, 0.0)  # output (x cm/s, yaw rad/s), what puppy_control would receive
        self.pose = (0.0, 0.0, 0.0)  # simulated true pose (x, y, yaw) in the map frame
        self.lidar_pose, self.lidar_pose_t = None, 0.0  # /lidar_map/pose at 10 Hz
        self.scan, self.scan_pts, self.scan_t = None, None, 0.0
        self.trail = [(0.0, 0.0)]
        self.bumped = False
        self.collided, self.collided_with = False, None  # any contact since start or the last reset_map
        self.obstacles = []  # dynamic (x, y, r) circles in the map frame: in /scan and collisions, not in /map
        self.grid = occupancy_grid()  # /map data, fixed
        self.pil = mock_assets.load_pillow()  # PIL.Image for rosbridge png compression, or None
        self.estop_sources = set()  # "vr" (Protocol v2 event) and/or "service" (/puppy_control/emergency_stop)
        self.action, self.action_until = None, 0.0
        self.preempt_seq, self.preempt_reason = 0, None
        self.posture = dict(STAND)
        self.map_session = time.strftime("%Y%m%d_%H%M%S")
        self.scans = self.integrations = 0
        self._integrated_at = self.pose
        self.map_seq, self.map_info, self.map_info_rx = 0, None, -1e9
        self.map_frame = (0, self.pose, list(self.trail))
        self._v2_key = self._path_key = None
        self._moving = False
        self._last_reject_log = 0.0
        self.ws_conns, self.ws_lock = set(), threading.Lock()
        self.camera = mock_assets.CameraFrames(encoder) if encoder else None
        self.mapper = mock_assets.MapDrawer(MAP_W, MAP_H, MAP_LEFT, MAP_BOTTOM, MAP_PPM, ROOM, [BOX])
        self.camera_count = 0
        self.feeds = {CAMERA_TOPIC: Feed(lambda: int(time.monotonic() * 10), self._render_camera),
                      MAP_TOPIC: Feed(lambda: self.map_frame[0], self._render_map)}
        self.threads, self.servers, self.sockets = [], [], []

    # ── lifecycle ──

    def start(self):
        self.v2_sock = self._udp(V2_PORT)  # no SO_REUSEADDR, like v2_server.py / discovery.py
        self.disc_sock = self._udp(DISCOVERY_PORT)
        self._serve(ThreadingHTTPServer((self.host, MEDIA_PORT), make_media_handler(self)), "media 8090")
        self._serve(ThreadingHTTPServer((self.host, VIDEO_PORT), make_video_handler(self)), "video 8080")
        ws = RosbridgeServer((self.host, ROSBRIDGE_PORT), WsHandler)
        ws.robot = self
        self._serve(ws, "rosbridge 9090")
        for name, fn in (("v2", self._v2_loop), ("discovery", self._discovery_loop), ("tick", self._tick_loop),
                         ("rosbridge-pub", self._publish_loop)):
            self._thread(name, fn)
        log("mock PuppyPi on %s: v2 udp %d, discovery udp %d, media http %d, video http %d, rosbridge ws %d"
            % (self.host, V2_PORT, DISCOVERY_PORT, MEDIA_PORT, VIDEO_PORT, ROSBRIDGE_PORT))
        log("v2 limits: forward %.1f cm/s, reverse %.1f cm/s, yaw %.2f rad/s, watchdog %.2f s, lease %.1f s"
            % (MAX_FORWARD_CM_S, MAX_REVERSE_CM_S, MAX_YAW_RAD_S, COMMAND_TIMEOUT_S, LEASE_TIMEOUT_S))
        log("jpeg backend: %s" % (self.encoder.name if self.encoder else "none (video endpoints answer 503)"))
        log("rosbridge png compression: %s" % ("pillow" if self.pil else "unavailable, Pillow missing (sent uncompressed)"))

    def shutdown(self):
        self.stop.set()
        for srv in self.servers:
            srv.shutdown()
            srv.server_close()
        with self.ws_lock:
            conns = list(self.ws_conns)
        for c in conns:
            c.close()
        for s in self.sockets:
            s.close()
        for t in self.threads:
            t.join(timeout=1.0)

    def _udp(self, port):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sockets.append(s)
        s.bind((self.host, port))
        s.settimeout(0.1)
        return s

    def _serve(self, srv, name):
        srv.daemon_threads = True
        self.servers.append(srv)
        self._thread(name, srv.serve_forever)

    def _thread(self, name, fn):
        t = threading.Thread(target=fn, name=name, daemon=True)
        self.threads.append(t)
        t.start()

    # ── receiver_node.py callbacks (call with self.lock held) ──

    def _on_output(self, velocity, reason):
        x, _y, yaw = velocity
        if self.follower.active:
            if x == 0.0 and yaw == 0.0 and reason == "command":
                return  # neutral heartbeat while following a path
            # joystick, watchdog/lease expiry, estop, arbiter block -> cancel the path
            self.follower.cancel("수동 조작" if (x or yaw) else reason)
        self.target = (x, yaw)

    def _on_path(self, path, owner, request_client_id=None, request_sequence=None):
        if path == ():
            self.follower.cancel("VR 취소")
        else:
            self.follower.set_path(path, owner, time.monotonic(), request_client_id, request_sequence)
            self.target = (0.0, 0.0)

    def _set_estop_source(self, source, latched):
        if latched:
            self.estop_sources.add(source)
        else:
            self.estop_sources.discard(source)
        self._sync_estop()

    def _sync_estop(self):
        """Mirror the arbiter estop into the v2 session like receiver_node._on_arbiter does."""
        latched = bool(self.estop_sources)
        before = self.session.emergency_stop
        self.session.set_external_estop(latched)
        if before != self.session.emergency_stop:
            self._on_output(STOP, "외부 비상정지 %s" % ("latch" if latched else "해제"))
        if latched and self.action:
            log("action group %s aborted by emergency stop" % self.action)
            self._end_action()

    def _end_action(self):
        self.action = None
        self.session.set_motion_permitted(True)

    def _vr_moving(self):
        return self.target != (0.0, 0.0) or self.follower.active

    # ── UDP 5001: Protocol v2 (same flow as v2_server.V2UdpServer._handle) ──

    def _v2_loop(self):
        while not self.stop.is_set():
            try:
                data, addr = self.v2_sock.recvfrom(self.pv2.MAX_PACKET_BYTES + 1)
            except socket.timeout:
                continue
            except OSError:
                if self.stop.is_set():
                    break
                continue
            try:
                self._v2_handle(data, addr)
            except Exception:
                log("v2: packet handling error, stopping\n%s" % traceback.format_exc())
                with self.lock:
                    self.session.force_stop()
                    self._on_output(STOP, "패킷 처리 오류")

    def _v2_handle(self, data, addr):
        with self.lock:
            now = time.monotonic()
            result = self.session.handle_datagram(data, addr, now)
            ack = result.ack
            owner = self.session.owner
            request = (ack["client_id"], ack["sequence"]) if result.path else (None, None)
            if result.velocity is not None:
                self._on_output(result.velocity, result.event or "command")
            if result.path is not None:
                self._on_path(result.path, owner, *request)
            if result.estop_change is not None:
                self._set_estop_source("vr", result.estop_change)
            # log under the lock so the event line comes before the tick thread's state line
            where = "%s:%d seq=%s" % (addr[0], addr[1], ack["sequence"])
            if not ack["accepted"]:
                if now - self._last_reject_log >= 1.0:  # rate limited like v2_server.py
                    self._last_reject_log = now
                    log("v2 reject %s: %s (%s)" % (where, ack["reason"], result.event or "-"))
            elif result.event:
                log("v2 %s: %s" % (where, result.event))
            self._log_v2_state()
        try:
            self.v2_sock.sendto(self.pv2.encode_ack(ack), addr)
        except (OSError, ValueError):
            pass

    def _log_v2_state(self):
        with self.lock:
            s = self.session
            key = (s.state, s.owner, s.emergency_stop, s.motion_permitted)
            if key == self._v2_key:
                return
            self._v2_key = key
        log("v2 state: %s owner=%s estop=%s motion_permitted=%s" % key)

    # ── UDP 5002: discovery (discovery.py make_reply, unicast reply from port 5002) ──

    def _discovery_loop(self):
        window, replies = time.monotonic(), 0
        while not self.stop.is_set():
            try:
                packet, peer = self.disc_sock.recvfrom(1024)
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                break
            reply = self.disc.make_reply(packet, ROBOT_NAME, ROBOT_ID, V2_PORT)
            if reply is None:
                log("discovery: ignored %d bytes from %s:%d" % (len(packet), peer[0], peer[1]))
                continue
            now = time.monotonic()
            if now - window >= 1:
                window, replies = now, 0
            if replies >= 20:
                continue
            replies += 1
            try:
                self.disc_sock.sendto(reply, peer)
                log("discovery: reply to %s:%d" % peer)
            except OSError:
                pass

    # ── 50 Hz tick: watchdog, path follower, pose integration, sensors ──

    def _tick_loop(self):
        period = 1.0 / 50.0
        last = nxt = time.monotonic()
        while not self.stop.wait(max(0.0, nxt - time.monotonic())):
            now = time.monotonic()
            dt, last = min(0.1, now - last), now
            nxt += period
            if nxt < now:
                nxt = now + period
            try:
                self._tick(now, dt)
            except Exception:
                log("tick error\n%s" % traceback.format_exc())

    def _tick(self, now, dt):
        path_log = None
        with self.lock:
            events = self.session.check_timeouts(now)
            if events:
                self._on_output(STOP, "; ".join(events))
                log("v2: %s; stop" % "; ".join(events))
            if self.action and now >= self.action_until:
                log("action group %s finished" % self.action)
                self._end_action()
            f = self.follower  # same checks as receiver_node._tick
            if f.active:
                state = self.session.state
                if state not in ("idle", "active"):
                    f.cancel("세션 %s" % state)
                elif not self.session.motion_permitted:
                    f.cancel("이동 차단(중재기)")
                elif self.session.owner != f.owner:
                    f.cancel("조종자 바뀜")
                if f.active:
                    self.target = f.update(now, self.lidar_pose, self.lidar_pose_t, self.scan_pts, self.scan_t)
                else:
                    self.target = (0.0, 0.0)
            key = (f.state, f.index, f.waiting, f.request_client_id, f.request_sequence)
            if key != self._path_key:
                self._path_key = key
                if not f.active and f.state in ("done", "cancelled"):
                    self.target = (0.0, 0.0)
                path_log = "path: %s %d/%d request=%s/%s waiting=%s reason=%s" % (
                    f.state, f.index, len(f.points), f.request_client_id, f.request_sequence, f.waiting, f.reason)
            self._integrate(dt)
            if now - self.lidar_pose_t >= 0.1:
                self._sense(now)
            if now - self.map_info_rx >= 1.0:
                self._update_map_info(now)
        if path_log and self._path_key[0] != "idle":
            log(path_log)
        self._log_v2_state()

    def _integrate(self, dt):
        x_cm, yaw_rate = self.target
        moving = (x_cm, yaw_rate) != (0.0, 0.0)
        if moving != self._moving:
            self._moving = moving
            if moving:
                log("sim: moving x=%.1f cm/s yaw=%.3f rad/s" % (x_cm, yaw_rate))
            else:
                log("sim: stopped at x=%.3f y=%.3f yaw=%.3f" % self.pose)
        if not moving:
            return
        x, y, th = self.pose
        mid = th + yaw_rate * dt / 2
        v = x_cm / 100.0
        nx, ny = x + v * math.cos(mid) * dt, y + v * math.sin(mid) * dt
        hit = touching(nx, ny, self.obstacles) if v else None
        if hit:
            if not self.bumped:
                log("sim: touching the %s at x=%.2f y=%.2f, translation blocked" % (hit, x, y))
            self.bumped, self.collided, self.collided_with = True, True, hit
            nx, ny = x, y
        elif v:
            self.bumped = False
        self.pose = (nx, ny, wrap(th + yaw_rate * dt))

    def _sense(self, now):
        """10 Hz: /lidar_map/pose (= true pose, no estimation error) and /scan by ray casting."""
        x, y, th = self.pose
        self.lidar_pose, self.lidar_pose_t = self.pose, now
        ranges = []
        for i in range(SCAN_BEAMS):
            r = ray_range(x, y, th + SCAN_ANGLE_MIN + i * SCAN_INC, self.obstacles)
            ranges.append(r if r <= SCAN_RANGE_MAX else 0.0)
        self.scan, self.scan_t = ranges, now
        self.scan_pts = self.pf.scan_to_points(ranges, SCAN_ANGLE_MIN, SCAN_INC, SCAN_RANGE_MIN, SCAN_RANGE_MAX)
        self.scans += 1
        tx, ty = self.trail[-1]
        if math.hypot(x - tx, y - ty) >= 0.03:
            self.trail.append((x, y))
            del self.trail[:-3000]
        ix, iy, ith = self._integrated_at
        if math.hypot(x - ix, y - iy) >= 0.05 or abs(wrap(th - ith)) >= 0.05:
            self.integrations += 1
            self._integrated_at = self.pose

    def _update_map_info(self, now):
        """1 Hz, like /lidar_map/info that comes with every map image."""
        self.map_seq += 1
        x, y, th = self.lidar_pose or self.pose
        u, v = self.mapper.to_px(x, y)
        self.map_info = {
            "media": MEDIA, "frame": "map", "image_seq": self.map_seq, "stamp_unix": round(time.time(), 3),
            "width_px": MAP_W, "height_px": MAP_H, "left_m": MAP_LEFT, "bottom_m": MAP_BOTTOM,
            "px_per_m": MAP_PPM, "resolution_m": 0.05,
            "robot": {"x_m": round(x, 3), "y_m": round(y, 3), "yaw_rad": round(th, 3),
                      "u_px": int(round(u)), "v_px": int(round(v))},
            "session": self.map_session, "scans": self.scans, "integrations": self.integrations, "map_size_m": 30.0,
        }
        self.map_info_rx = now
        self.map_frame = (self.map_seq, (x, y, th), list(self.trail))

    def _reset_map(self):
        self.pose = (0.0, 0.0, 0.0)
        self.trail = [(0.0, 0.0)]
        self._integrated_at = self.pose
        self.scans = self.integrations = 0
        self.map_session = time.strftime("%Y%m%d_%H%M%S")
        self.map_info_rx = -1e9  # new map info on the next tick
        self.bumped, self.collided, self.collided_with = False, False, None

    # ── test hooks (HTTP 8090 /mock/*), the real robot has none of these ──

    def obstacle_list(self):
        with self.lock:
            return {"frame": "map", "obstacles": [{"x": x, "y": y, "r": r} for x, y, r in self.obstacles]}

    def add_obstacle(self, x, y, r):
        with self.lock:
            self.obstacles.append((x, y, r))
            n = len(self.obstacles)
            on_robot = touching(self.pose[0], self.pose[1], [(x, y, r)]) == "obstacle"
        log("mock: obstacle %d added at x=%.2f y=%.2f r=%.2f%s" % (
            n, x, y, r, " (overlaps the robot: it can only turn until cleared)" if on_robot else ""))
        return self.obstacle_list()

    def clear_obstacles(self):
        with self.lock:
            n = len(self.obstacles)
            self.obstacles = []
        log("mock: %d obstacle(s) cleared" % n)
        return dict(self.obstacle_list(), removed=n)

    def mock_pose(self):
        """True simulated pose (not /lidar_map/pose) and the contact flags."""
        with self.lock:
            x, y, th = self.pose
            return {"frame": "map", "x": round(x, 4), "y": round(y, 4), "yaw": round(th, 4),
                    "collided": self.collided, "collided_with": self.collided_with, "touching": self.bumped,
                    "mock": True}

    # ── state views (JSON shapes of media_server.py / receiver_node.py) ──

    def arbiter_state(self):
        s = self.session
        return {"owner": "vr" if self._vr_moving() else None, "estop": bool(self.estop_sources),
                "estop_sources": sorted(self.estop_sources), "engine": "ready",
                "vr_permitted": s.motion_permitted, "vr_block_reason": s.snapshot()["blocked_reason"],
                "vr_preempt_seq": self.preempt_seq, "preempt_seq": self.preempt_seq,
                "last_preempt_reason": self.preempt_reason, "action": self.action, "mock": True}

    def robot_status(self):
        x, yaw = self.target
        return {"engine": "ready", "estop": bool(self.estop_sources), "owner": "vr" if self._vr_moving() else None,
                "action": self.action, "posture": dict(self.posture),
                "output": {"x_cm_s": x, "yaw_rad_s": yaw}, "mock": True}

    def api_map(self):
        with self.lock:
            return dict(self.map_info, age_s=round(time.monotonic() - self.map_info_rx, 2))

    def api_status(self):
        with self.lock:
            m = self.api_map()
            return {"media": MEDIA, "arbiter": dict(self.arbiter_state(), age_s=0.0),
                    "map": {"robot": m["robot"], "age_s": m["age_s"]}}

    def api_path(self):
        with self.lock:
            now = time.monotonic()
            p = self.lidar_pose
            pose = None if p is None else {"x_m": round(p[0], 3), "y_m": round(p[1], 3),
                                           "yaw_rad": round(p[2], 4), "age_s": round(now - self.lidar_pose_t, 2)}
            return {"media": MEDIA, "path": self.follower.status(now, p), "pose": pose,
                    "session_state": self.session.state, "emergency_stop": self.session.emergency_stop,
                    "age_s": 0.0}

    # ── video frames ──

    def _render_camera(self, _key):
        self.camera_count += 1
        return self.camera.frame(self.camera_count)

    def _render_map(self, _seq):
        seq, pose, trail = self.map_frame
        return self.encoder.encode(self.mapper.draw(pose, trail), "map_live")

    # ── rosbridge services and topics ──

    def call_service(self, service, args):
        """Returns (result, values) for a rosbridge service_response."""
        with self.lock:
            if service == "/puppy_control/go_home":  # std_srvs/Empty
                if self.estop_sources:
                    log("go_home ignored: emergency stop latched")
                    return True, {}
                if self._vr_moving():  # VR owns the motion: preempt, the session needs a new neutral
                    self.session.preempt()
                    self.preempt_seq, self.preempt_reason = self.preempt_seq + 1, "go_home"
                    self._on_output(STOP, "선점 정지: go_home")
                self.posture = dict(STAND)
                log("go_home: posture Stand")
                return True, {}
            if service == "/puppy_control/runActionGroup":
                name = str(args.get("name", ""))
                base = name[:-5] if name.endswith(".d6ac") else name
                if not base:
                    return True, {"success": False, "message": "empty action group name"}
                if base in BLOCKED_ACTIONS:
                    log("runActionGroup %s rejected (blocked_action_groups)" % name)
                    return True, {"success": False, "message": "blocked action group: %s" % base}
                if self.estop_sources:
                    return True, {"success": False, "message": "emergency stop latched"}
                if self.action:
                    return True, {"success": False, "message": "action group %s is running" % self.action}
                self.action, self.action_until = name, time.monotonic() + ACTION_SECONDS
                if self.session.set_motion_permitted(False, "동작 그룹 실행 중: %s" % name):
                    self._on_output(STOP, "중재기: 동작 그룹 %s" % name)
                log("action group %s running for %.1f s (v2 motion blocked)" % (name, ACTION_SECONDS))
                return True, {"success": True, "message": "running %s" % name}
            if service == "/puppy_control/emergency_stop":  # std_srvs/Trigger
                self._set_estop_source("service", True)
                log("robot emergency stop latched (service)")
                return True, {"success": True, "message": "emergency stop latched"}
            if service == "/puppy_control/reset_estop":
                self.estop_sources.clear()
                self._sync_estop()
                log("robot emergency stop reset (all sources)")
                return True, {"success": True, "message": "emergency stop cleared"}
            if service == "/lidar_mapper/save_map":
                log("save_map: mock, nothing written")
                return True, {"success": True, "message": "mock: map not written to disk"}
            if service == "/lidar_mapper/reset_map":
                self._reset_map()
                log("reset_map: pose back to origin, trail and collided flag cleared, session %s" % self.map_session)
                return True, {"success": True, "message": "map reset, session %s" % self.map_session}
            if service == "/rosapi/topics":
                return True, {"topics": [t for t, _ in TOPICS], "types": [k for _, k in TOPICS]}
            if service == "/rosapi/services":
                return True, {"services": list(SERVICES)}
        return False, "Service %s does not exist" % service

    def set_posture(self, msg):
        with self.lock:
            if self.estop_sources:
                log("/puppy_control/pose ignored: emergency stop latched")
                return
            for k in POSE_FIELDS:
                if isinstance(msg.get(k), (int, float)) and not isinstance(msg.get(k), bool):
                    self.posture[k] = msg[k]
            posture = dict(self.posture)
        log("/puppy_control/pose <- %s" % json.dumps(posture))

    def topic_messages(self, topics, slow, map_due=False):
        """Current message for each wanted topic. slow=True adds the 2 Hz String topics, map_due=True /map."""
        out = {}
        with self.lock:
            stamp = ros_stamp()
            if map_due and "/map" in topics:
                out["/map"] = {
                    "header": {"stamp": stamp, "frame_id": "map"},
                    "info": {"map_load_time": stamp, "resolution": GRID_RES, "width": GRID_W, "height": GRID_H,
                             "origin": {"position": {"x": GRID_X0, "y": GRID_Y0, "z": 0.0},
                                        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0}}},
                    "data": self.grid}
            if "/scan" in topics and self.scan:
                out["/scan"] = {
                    "header": {"stamp": stamp, "frame_id": "laser"},
                    "angle_min": SCAN_ANGLE_MIN, "angle_max": SCAN_ANGLE_MIN + SCAN_INC * (SCAN_BEAMS - 1),
                    "angle_increment": SCAN_INC, "time_increment": 0.1 / SCAN_BEAMS, "scan_time": 0.1,
                    "range_min": SCAN_RANGE_MIN, "range_max": SCAN_RANGE_MAX,
                    "ranges": [round(r, 4) for r in self.scan], "intensities": []}
            if "/lidar_map/pose" in topics and self.lidar_pose:
                x, y, th = self.lidar_pose
                out["/lidar_map/pose"] = {
                    "header": {"stamp": stamp, "frame_id": "map"},
                    "pose": {"position": {"x": x, "y": y, "z": 0.0},
                             "orientation": {"x": 0.0, "y": 0.0, "z": math.sin(th / 2), "w": math.cos(th / 2)}}}
            if slow and "/puppy_control/status" in topics:
                out["/puppy_control/status"] = {"data": json.dumps(self.robot_status(), ensure_ascii=False)}
            if slow and "/puppy_control/arbiter_state" in topics:
                out["/puppy_control/arbiter_state"] = {"data": json.dumps(self.arbiter_state(), ensure_ascii=False)}
        return out

    def _publish_loop(self):
        n, nxt = 0, time.monotonic()
        while not self.stop.wait(max(0.0, nxt - time.monotonic())):
            nxt += 0.1
            n += 1
            with self.ws_lock:
                conns = list(self.ws_conns)
            wanted = set()
            for c in conns:
                wanted |= c.topics()
            if not wanted:
                continue
            msgs = self.topic_messages(wanted, slow=(n % 5 == 0), map_due=(n % 20 == 0))  # /map every 2.0 s
            texts = {t: json.dumps({"op": "publish", "topic": t, "msg": m}) for t, m in msgs.items()}
            for c in conns:
                c.offer(texts)


# ── HTTP 8090: media_server ──

VIEWER = """<!doctype html><html><head><meta charset="utf-8"><title>PuppyPi mock</title></head>
<body style="background:#111;color:#ddd;font:14px sans-serif;margin:16px">
<h3>PuppyPi mock robot (simulation)</h3><img id="c"> <img id="m"><pre id="s"></pre>
<script>
fetch('/api/media').then(r => r.json()).then(d => { c.src = d.camera.stream; m.src = d.map.stream; });
setInterval(async () => { s.textContent = JSON.stringify(await (await fetch('/api/path')).json(), null, 1); }, 1000);
</script></body></html>"""


def descriptor(host, video_port=VIDEO_PORT, media_port=MEDIA_PORT):
    v, m = "http://%s:%d" % (host, video_port), "http://%s:%d" % (host, media_port)
    q = lambda topic: "topic=%s&type=ros_compressed" % topic  # noqa: E731
    return {
        "media": MEDIA,
        "robot": {"name": ROBOT_NAME},
        "camera": {"stream": "%s/stream?%s" % (v, q(CAMERA_TOPIC)), "snapshot": "%s/snapshot?%s" % (v, q(CAMERA_TOPIC)),
                   "format": "multipart/x-mixed-replace, image/jpeg parts", "nominal_fps": 10,
                   "note": "mock camera %dx%d test pattern" % (mock_assets.CAM_W, mock_assets.CAM_H)},
        "map": {"stream": "%s/stream?%s" % (v, q(MAP_TOPIC)), "snapshot": "%s/snapshot?%s" % (v, q(MAP_TOPIC)),
                "info": "%s/api/map" % m, "format": "multipart/x-mixed-replace, image/jpeg parts", "nominal_fps": 1,
                "pixel_from_map": "u = (x - left_m) * px_per_m ; v = height_px - (y - bottom_m) * px_per_m"},
        "status": "%s/api/status" % m,
        "viewer": "%s/" % m,
    }


def make_media_handler(robot):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype="application/json; charset=utf-8"):
            data = body.encode("utf-8") if isinstance(body, str) else json.dumps(body, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            host = (self.headers.get("Host") or "127.0.0.1").rsplit(":", 1)[0].strip("[]")
            path = self.path.split("?", 1)[0]
            if path == "/":
                return self._send(200, VIEWER, "text/html; charset=utf-8")
            if path == "/api/media":
                return self._send(200, descriptor(host))
            if path == "/api/map":
                return self._send(200, robot.api_map())
            if path == "/api/status":
                return self._send(200, robot.api_status())
            if path == "/api/path":
                return self._send(200, robot.api_path())
            if path == "/mock/obstacle":
                q = parse_qs(urlsplit(self.path).query)
                try:
                    x, y = float(q["x"][0]), float(q["y"][0])
                    r = float(q.get("r", [OBSTACLE_R])[0])
                    if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(r) and r > 0):
                        raise ValueError
                except (KeyError, ValueError):
                    return self._send(400, {"error": "need x=<m>&y=<m> (map frame) and optional r=<m> > 0, "
                                                     "default r %.2f" % OBSTACLE_R})
                return self._send(200, robot.add_obstacle(x, y, r))
            if path == "/mock/obstacles":
                return self._send(200, robot.obstacle_list())
            if path == "/mock/obstacles/clear":
                return self._send(200, robot.clear_obstacles())
            if path == "/mock/pose":
                return self._send(200, robot.mock_pose())
            return self._send(404, {"error": "not found", "see": "/api/media"})
    return H


# ── HTTP 8080: web_video_server ──

BOUNDARY = "boundarydonotcross"


def make_video_handler(robot):
    class H(BaseHTTPRequestHandler):
        server_version, sys_version = "web_video_server", ""
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def _headers(self, ctype, extra=()):
            self.send_response(200)
            self.send_header("Connection", "close")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate, pre-check=0, post-check=0, max-age=0")
            self.send_header("Pragma", "no-cache")
            self.send_header("Content-type", ctype)
            self.send_header("Access-Control-Allow-Origin", "*")
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()

        def _error(self, code, text):
            data = text.encode()
            self.send_response(code)
            self.send_header("Content-type", "text/plain")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urlsplit(self.path)
            topic = parse_qs(url.query).get("topic", [""])[0]
            feed = robot.feeds.get(topic)
            if url.path not in ("/stream", "/snapshot") or feed is None:
                return self._error(404, "unknown path or topic; topics: %s %s" % (CAMERA_TOPIC, MAP_TOPIC))
            if robot.encoder is None:
                return self._error(503, "no JPEG encoder: install Pillow into tools/.venv")
            if url.path == "/snapshot":
                _key, stamp, jpeg = feed.latest()
                self._headers("image/jpeg", (("X-Timestamp", "%.6f" % stamp), ("Content-Length", str(len(jpeg)))))
                self.wfile.write(jpeg)
                return
            peer = "%s:%d" % self.client_address[:2]
            log("video: stream %s -> %s opened" % (topic, peer))
            self._headers("multipart/x-mixed-replace;boundary=" + BOUNDARY)
            sent, last = 0, None
            try:
                self.wfile.write(("--%s\r\n" % BOUNDARY).encode())
                while not robot.stop.is_set():
                    key, stamp, jpeg = feed.latest()
                    if key != last:
                        last = key
                        head = "Content-type: image/jpeg\r\nX-Timestamp: %.6f\r\nContent-Length: %d\r\n\r\n" % (
                            stamp, len(jpeg))
                        self.wfile.write(head.encode() + jpeg + ("\r\n--%s\r\n" % BOUNDARY).encode())
                        sent += 1
                    time.sleep(0.02)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            log("video: stream %s -> %s closed after %d frames" % (topic, peer, sent))
    return H


# ── TCP 9090: rosbridge v2 over a plain RFC 6455 WebSocket ──

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_MAX_MESSAGE = 1 << 20


def png_message(image_module, text):
    """rosbridge_library pngcompression.encode: the UTF-8 JSON text as the pixels of an RGB PNG, padded with
    b"\\n", base64 (standard) in {"op": "png", "data": ...}."""
    raw = text.encode("utf-8")
    width = math.floor(math.sqrt(len(raw) / 3.0))
    height = math.ceil((len(raw) / 3.0) / width)
    raw += b"\n" * (width * height * 3 - len(raw))
    buf = io.BytesIO()
    image_module.frombytes("RGB", (width, height), raw).save(buf, "png")
    return json.dumps({"op": "png", "data": base64.standard_b64encode(buf.getvalue()).decode("ascii")})


class RosbridgeServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class WsHandler(socketserver.BaseRequestHandler):
    def handle(self):
        WsConn(self.request, self.client_address, self.server.robot).run()


class WsConn:
    def __init__(self, sock, addr, robot):
        self.sock, self.robot = sock, robot
        self.peer = "%s:%d" % addr[:2]
        self.rfile = sock.makefile("rb")
        self.send_lock = threading.Lock()
        self.subs, self.subs_lock = {}, threading.Lock()  # topic -> [throttle_s, last_sent, compression]

    # framing

    def _read(self, n):
        data = self.rfile.read(n)
        if data is None or len(data) < n:
            raise ConnectionError("closed")
        return data

    def _read_frame(self):
        b0, b1 = self._read(2)
        fin, op, n = b0 & 0x80, b0 & 0x0F, b1 & 0x7F
        if n == 126:
            n = int.from_bytes(self._read(2), "big")
        elif n == 127:
            n = int.from_bytes(self._read(8), "big")
        if n > WS_MAX_MESSAGE:
            raise ValueError("frame too large")
        if not b1 & 0x80:
            raise ValueError("client frame not masked")  # RFC 6455 5.1: the server must close
        mask = self._read(4)
        data = self._read(n)
        if n:
            data = (int.from_bytes(data, "big") ^ int.from_bytes((mask * (n // 4 + 1))[:n], "big")).to_bytes(n, "big")
        return fin, op, data

    def _recv_message(self):
        """Next text/binary message; handles ping, pong, close and fragmentation. Returns (op, data)."""
        op, parts = None, []
        while True:
            fin, fop, data = self._read_frame()
            if fop == 0x9:
                self.send_frame(0xA, data)
                continue
            if fop == 0xA:
                continue
            if fop == 0x8:
                self.send_frame(0x8, data[:2])
                return 0x8, data
            if fop in (0x1, 0x2):
                op, parts = fop, [data]
            elif fop == 0x0:
                parts.append(data)
            if fin and op is not None:
                return op, b"".join(parts)

    def send_frame(self, op, payload):
        n = len(payload)
        if n < 126:
            head = bytes([0x80 | op, n])
        elif n < 65536:
            head = bytes([0x80 | op, 126]) + n.to_bytes(2, "big")
        else:
            head = bytes([0x80 | op, 127]) + n.to_bytes(8, "big")
        with self.send_lock:
            self.sock.sendall(head + payload)

    def send_json(self, obj):
        self.send_frame(0x1, (obj if isinstance(obj, str) else json.dumps(obj, ensure_ascii=False)).encode("utf-8"))

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    # session

    def _handshake(self):
        request = self.rfile.readline(4096)
        headers = {}
        while True:
            line = self.rfile.readline(4096)
            if line in (b"\r\n", b"\n", b""):
                break
            k, _, v = line.decode("latin-1").partition(":")
            headers[k.strip().lower()] = v.strip()
        key = headers.get("sec-websocket-key")
        if not request.startswith(b"GET ") or "websocket" not in headers.get("upgrade", "").lower() or not key:
            self.sock.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return False
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        return True

    def run(self):
        try:
            if not self._handshake():
                return
        except OSError:
            return
        with self.robot.ws_lock:
            self.robot.ws_conns.add(self)
        log("rosbridge: client %s connected" % self.peer)
        try:
            while not self.robot.stop.is_set():
                op, data = self._recv_message()
                if op == 0x8:
                    break
                if op == 0x1:
                    self._dispatch(data)
        except (OSError, ValueError) as exc:
            if not isinstance(exc, ConnectionError):
                log("rosbridge: client %s error: %s" % (self.peer, exc))
        finally:
            with self.robot.ws_lock:
                self.robot.ws_conns.discard(self)
            self.close()
            log("rosbridge: client %s disconnected" % self.peer)

    def topics(self):
        with self.subs_lock:
            return set(self.subs)

    def offer(self, texts):
        now = time.monotonic()
        with self.subs_lock:
            due = []
            for topic, text in texts.items():
                sub = self.subs.get(topic)
                if sub and now - sub[1] >= sub[0]:
                    sub[1] = now
                    due.append((text, sub[2]))
        try:
            for text, compression in due:
                self.send_json(png_message(self.robot.pil, text) if compression == "png" else text)
        except OSError:
            self.close()

    def _dispatch(self, data):
        try:
            msg = json.loads(data.decode("utf-8"))
            op = msg.get("op")
        except (ValueError, AttributeError):
            log("rosbridge %s: not a JSON object, ignored" % self.peer)
            return
        topic = msg.get("topic")
        if op == "call_service":
            service = msg.get("service")
            args = msg.get("args") if isinstance(msg.get("args"), dict) else {}
            result, values = self.robot.call_service(service, args)
            resp = {"op": "service_response", "service": service}
            if "id" in msg:
                resp["id"] = msg["id"]
            resp.update(values=values, result=result)
            self.send_json(resp)
            log("rosbridge %s: call_service %s %s -> result=%s %s"
                % (self.peer, service, json.dumps(args, ensure_ascii=False), result, json.dumps(values, ensure_ascii=False)))
        elif op == "publish":
            if topic == "/puppy_control/pose" and isinstance(msg.get("msg"), dict):
                self.robot.set_posture(msg["msg"])
            else:
                log("rosbridge %s: publish on %s ignored (no subscriber in the mock)" % (self.peer, topic))
        elif op == "subscribe":
            throttle = msg.get("throttle_rate") or 0
            throttle = throttle / 1000.0 if isinstance(throttle, (int, float)) else 0.0
            compression = msg.get("compression") or "none"
            if compression == "png" and self.robot.pil is None:
                log("rosbridge %s: compression png on %s needs Pillow, sending uncompressed" % (self.peer, topic))
                compression = "none"
            elif compression not in ("none", "png"):
                log("rosbridge %s: compression %r not supported by the mock, sending uncompressed"
                    % (self.peer, compression))
                compression = "none"
            with self.subs_lock:
                self.subs[topic] = [throttle, -1e9, compression]
            known = topic in dict(TOPICS) and topic != "/puppy_control/pose"
            log("rosbridge %s: subscribe %s throttle_rate=%s ms compression=%s%s"
                % (self.peer, topic, msg.get("throttle_rate", 0), compression,
                   "" if known else " (mock never publishes it)"))
            if topic in LATCHED:  # TRANSIENT_LOCAL on the robot: current value right away
                text = json.dumps({"op": "publish", "topic": topic,
                                   "msg": self.robot.topic_messages({topic}, slow=True, map_due=True)[topic]})
                self.offer({topic: text})
        elif op == "unsubscribe":
            with self.subs_lock:
                self.subs.pop(topic, None)
            log("rosbridge %s: unsubscribe %s" % (self.peer, topic))
        elif op in ("advertise", "unadvertise"):
            log("rosbridge %s: %s %s %s" % (self.peer, op, topic, msg.get("type", "")))
        else:
            log("rosbridge %s: op %r not supported by the mock, ignored" % (self.peer, op))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1", help="address every server binds to (0.0.0.0 = all, LAN)")
    ap.add_argument("--robot-src", default=DEFAULT_ROBOT_SRC,
                    help="ros2_ws/src/puppy_vr_bridge of the robot working copy (read only)")
    ap.add_argument("--jpeg", default="auto", choices=["auto", "pillow", "sips", "none"],
                    help="JPEG backend for the video streams (default: Pillow if found, else sips)")
    args = ap.parse_args(argv)
    protocol_v2, path_follower, discovery = load_robot_modules(args.robot_src)
    log("robot modules: %s" % os.path.dirname(protocol_v2.__file__))
    try:
        encoder = mock_assets.make_encoder(args.jpeg)
        robot = MockRobot(args.host, protocol_v2, path_follower, discovery, encoder)
    except Exception as exc:  # missing Pillow/sips for a forced backend, sips failure, ...
        log("cannot prepare JPEG frames: %s" % exc)
        return 1
    # Ctrl+C / SIGINT / SIGTERM all just set the stop flag (also works when started as a background job,
    # where the shell leaves SIGINT ignored and Python would never raise KeyboardInterrupt)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: robot.stop.set())
    try:
        robot.start()
    except OSError as exc:
        log("cannot bind on %s: %s (is another robot or mock running?)" % (args.host, exc))
        robot.shutdown()
        return 1
    while not robot.stop.wait(0.5):
        pass
    log("stopping")
    robot.shutdown()
    log("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
