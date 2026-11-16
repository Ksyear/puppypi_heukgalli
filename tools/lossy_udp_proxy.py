"""UDP 5001 relay that drops chosen Protocol v2 packets between the app and the mock robot.

Used by the emergency-stop loss test: the app's first two emergency_stop packets are dropped, and the
test passes only if the robot still ends up latched (the app must re-send) within the run.

  python3 tools/lossy_udp_proxy.py --listen 127.0.0.2 --seconds 12
  ./build/console --host 127.0.0.2 --script tests/it_estop_loss.txt     (mock_robot.py on 127.0.0.1)

Exit code 0 if an ACK with emergency_stop true came back after the drops, 1 otherwise.
"""

import argparse
import json
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.2", help="address the app connects to (port 5001)")
    ap.add_argument("--robot", default="127.0.0.1", help="mock robot address (port 5001)")
    ap.add_argument("--drop-type", default="emergency_stop")
    ap.add_argument("--drop-count", type=int, default=2)
    ap.add_argument("--seconds", type=float, default=12.0)
    args = ap.parse_args()

    app_side = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    app_side.bind((args.listen, 5001))
    app_side.settimeout(0.02)
    robot_side = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    robot_side.bind((args.robot, 0))
    robot_side.settimeout(0.02)

    client = None
    dropped = forwarded = 0
    latched_at = None
    start = time.time()
    while time.time() - start < args.seconds:
        try:
            data, client = app_side.recvfrom(65536)
            try:
                kind = json.loads(data).get("type")
            except ValueError:
                kind = None
            if kind == args.drop_type:
                if dropped < args.drop_count:
                    dropped += 1
                    continue
                forwarded += 1
            robot_side.sendto(data, (args.robot, 5001))
        except socket.timeout:
            pass
        try:
            ack, _ = robot_side.recvfrom(65536)
            if client:
                app_side.sendto(ack, client)
            try:
                if dropped >= args.drop_count and json.loads(ack).get("emergency_stop") and latched_at is None:
                    latched_at = time.time() - start
            except ValueError:
                pass
        except socket.timeout:
            pass
    print(f"proxy: dropped {args.drop_type} x{dropped}, forwarded x{forwarded}, robot latched: "
          + (f"yes at {latched_at:.1f} s" if latched_at is not None else "NO"))
    return 0 if latched_at is not None else 1


if __name__ == "__main__":
    sys.exit(main())
