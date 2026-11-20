#!/usr/bin/env python3
"""Full-view USB CCTV stream on port 8081, independent of inspection/motors.

Examples:
    python3 cctv_server.py  # added 8Mega CCTV camera by default
    python3 cctv_server.py --list
    python3 cctv_server.py --camera /dev/v4l/by-id/SECOND_CAMERA-video-index0
"""
from __future__ import annotations

import argparse
import importlib
import logging
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, cast
from urllib.parse import urlsplit


DEFAULT_CAMERA = "/dev/v4l/by-id/usb-Generic_8Mega_autofous_webcamera_0001-video-index0"


@dataclass(frozen=True)
class FrameSnapshot:
    sequence: int
    jpeg: bytes | None
    reason: str
    closed: bool


class FrameStore:
    """Keep one current frame, shared by all viewers without a growing queue."""

    def __init__(self, stale_seconds: float = 3.0) -> None:
        self._cv = threading.Condition()
        self._sequence = 0
        self._jpeg: bytes | None = None
        self._reason = "Camera is starting or unavailable"
        self._closed = False
        self._updated = 0.0
        self._stale_seconds = stale_seconds

    def publish(self, jpeg: bytes) -> None:
        with self._cv:
            if self._closed:
                return
            self._jpeg = jpeg
            self._reason = ""
            self._updated = time.monotonic()
            self._sequence += 1
            self._cv.notify_all()

    def unavailable(self, reason: str) -> None:
        with self._cv:
            self._jpeg = None
            self._reason = reason
            self._sequence += 1
            self._cv.notify_all()

    def wait_next(self, after: int, timeout: float = 1.0) -> FrameSnapshot:
        with self._cv:
            self._cv.wait_for(
                lambda: self._sequence != after or self._closed, timeout=timeout
            )
            if self._jpeg and time.monotonic() - self._updated > self._stale_seconds:
                self._jpeg = None
                self._reason = "Camera frame timed out"
                self._sequence += 1
                self._cv.notify_all()
            return FrameSnapshot(
                self._sequence, self._jpeg, self._reason, self._closed
            )

    def close(self) -> None:
        with self._cv:
            self._closed = True
            self._jpeg = None
            self._sequence += 1
            self._cv.notify_all()


class CctvHandler(BaseHTTPRequestHandler):
    """Serve one MJPEG stream per viewer from the shared capture worker."""

    def do_GET(self) -> None:
        self.connection.settimeout(3.0)
        route = urlsplit(self.path).path
        if route == "/":
            body = (
                b'<!doctype html><meta charset="utf-8">'
                b'<title>MobleKurly CCTV</title>'
                b'<style>body{margin:0;background:#161a22;color:white;'
                b'font-family:sans-serif}h1{font-size:18px;padding:12px}'
                b'img{display:block;max-width:100%;margin:auto}</style>'
                b'<h1>MobleKurly - full-view CCTV</h1><img src="/video_feed">'
            )
            self._text(200, body, "text/html; charset=utf-8")
            return
        if route != "/video_feed":
            self.send_error(404)
            return
        frames = cast(CctvServer, self.server).frames
        snapshot = frames.wait_next(-1, timeout=0)
        if snapshot.closed or snapshot.jpeg is None:
            self._text(503, (snapshot.reason + "\n").encode(), "text/plain; charset=utf-8")
            return
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        previous = -1
        try:
            while not snapshot.closed and snapshot.jpeg is not None:
                if snapshot.sequence != previous:
                    jpeg = snapshot.jpeg
                    self.wfile.write(
                        b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                        + str(len(jpeg)).encode("ascii")
                        + b"\r\n\r\n"
                        + jpeg
                        + b"\r\n"
                    )
                    self.wfile.flush()
                    previous = snapshot.sequence
                snapshot = frames.wait_next(previous, timeout=1.0)
        except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
            pass
        finally:
            self.close_connection = True

    def _text(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        logging.debug("HTTP %s", format % args)


class CctvServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False

    def __init__(self, address: tuple[str, int], frames: FrameStore) -> None:
        self.frames = frames
        super().__init__(address, CctvHandler)


def camera_source(value: str) -> int | str:
    """Accept an index or a stable /dev/v4l/by-id path."""
    return int(value) if value.isdecimal() else value


def positive_port(value: str) -> int:
    port = int(value)
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="List devices without opening cameras")
    parser.add_argument("--camera", type=camera_source, default=DEFAULT_CAMERA,
                        help="CCTV camera index or path (default: %(default)s)")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=positive_port, default=8081)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--fps", type=int, default=8)
    parser.add_argument("--quality", type=int, default=75)
    args = parser.parse_args(argv)
    if not (160 <= args.width <= 1920 and 120 <= args.height <= 1080):
        parser.error("supported size: width 160..1920, height 120..1080")
    if not 1 <= args.fps <= 30 or not 30 <= args.quality <= 95:
        parser.error("fps must be 1..30 and quality must be 30..95")
    return args


def list_devices() -> None:
    """Only inspect device names/symlinks; leave the existing camera untouched."""
    print("USB camera device paths (use the ADDED camera, not inspection):")
    found = False
    for directory in (Path("/dev/v4l/by-id"), Path("/dev/v4l/by-path")):
        if directory.exists():
            for path in sorted(directory.glob("*video-index0")):
                print(f"  {path} -> {path.resolve()}")
                found = True
    for path in sorted(Path("/dev").glob("video[0-9]*")):
        name_file = Path("/sys/class/video4linux") / path.name / "name"
        index_file = Path("/sys/class/video4linux") / path.name / "index"
        try:
            name = name_file.read_text().strip()
            index = index_file.read_text().strip()
        except OSError:
            name, index = "USB video device", "?"
        print(f"  {path}: {name} (device index {index})")
        found = True
    if not found:
        print("  No /dev/video* cameras found. Run this command on the Raspberry Pi.")
    print("Some /dev/video nodes carry metadata, not images. Prefer a video-index0 path.")


def capture_loop(args: argparse.Namespace, frames: FrameStore,
                 stopping: threading.Event, cv2: Any) -> None:
    """Open only the selected camera and reconnect it after read failures."""
    last_problem = ""
    while not stopping.is_set():
        camera = None
        try:
            camera = cv2.VideoCapture(args.camera, cv2.CAP_V4L2)
            if not camera.isOpened():
                raise RuntimeError(f"Camera open failed: {args.camera}; use --list to check devices and --camera to select CCTV")
            # MJPG reduces USB transfer volume for two independent USB cameras.
            camera.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
            camera.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
            camera.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
            camera.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            camera.set(cv2.CAP_PROP_FPS, args.fps)
            logging.info("CCTV camera opened: %s", args.camera)
            last_problem = ""
            while not stopping.is_set():
                started = time.monotonic()
                ok, image = camera.read()
                if not ok or image is None:
                    raise RuntimeError("Camera read failed; check the CCTV USB cable")
                ok, jpeg = cv2.imencode(".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, args.quality])
                if not ok:
                    raise RuntimeError("Camera JPEG encoding failed")
                frames.publish(jpeg.tobytes())
                stopping.wait(max(0.0, 1.0 / args.fps - (time.monotonic() - started)))
        except Exception as exc:
            problem = str(exc)
            frames.unavailable(problem)
            if problem != last_problem:
                logging.warning("%s; reconnecting in 2 seconds", problem)
                last_problem = problem
        finally:
            if camera is not None:
                camera.release()
        stopping.wait(2.0)


def main(argv: list[str] | None = None) -> int:
    args = parse_arguments(argv)
    if args.list:
        list_devices()
        return 0
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        cv2 = importlib.import_module("cv2")
    except ImportError:
        print("OpenCV (cv2) is missing. Run with the same Python/virtual environment as pi_server.py.")
        return 1
    frames = FrameStore()
    stopping = threading.Event()
    try:
        server = CctvServer((args.host, args.port), frames)
    except OSError as exc:
        logging.error("Cannot start CCTV HTTP server on port %s: %s", args.port, exc)
        return 1
    capture = threading.Thread(target=capture_loop, args=(args, frames, stopping, cv2),
                               daemon=True, name="cctv-capture")
    capture.start()
    logging.info("CCTV HTTP ready on port %s; path /video_feed", args.port)
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        stopping.set()
        frames.close()
        server.server_close()
        capture.join(timeout=3)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
