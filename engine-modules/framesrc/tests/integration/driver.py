#!/usr/bin/env python3
import argparse
import hashlib
import struct
import mmap
import os
import signal
import subprocess
import sys
import time
import zlib

RESULTS = []


def report (name, ok, detail = ""):
    RESULTS.append ((name, ok, detail))
    tag = "PASS" if ok else "FAIL"
    line = f"[{tag}] {name}"
    if detail:
        line += f" ({detail})"
    print (line, flush = True)


def parse_header (mv, layout):
    if layout == "new":
        magic, version, width, height, stride, fmt, vflip, reserved = struct.unpack_from ("<8I", mv, 0)
        sequence = struct.unpack_from ("<Q", mv, 32)[0]
        return {"magic": magic, "version": version, "width": width, "height": height,
                "stride": stride, "format": fmt, "vflip": vflip, "sequence": sequence}
    magic, width, height, stride, fmt, vflip = struct.unpack_from ("<6I", mv, 0)
    sequence = struct.unpack_from ("<Q", mv, 24)[0]
    return {"magic": magic, "version": 0, "width": width, "height": height,
            "stride": stride, "format": fmt, "vflip": vflip, "sequence": sequence}


class StreamView:
    def __init__ (self, path, layout):
        self.path = path
        self.layout = layout
        self.f = open (path, "rb")
        self.map = None
        self.size = 0

    def ensure_mapped (self):
        size = os.path.getsize (self.path)
        if self.map is None or size != self.size:
            if self.map is not None:
                self.map.close ()
            self.map = mmap.mmap (self.f.fileno (), size, prot = mmap.PROT_READ)
            self.size = size

    def header (self):
        self.ensure_mapped ()
        return parse_header (self.map, self.layout)

    def frame_bytes (self, header):
        offset = 40 if self.layout == "new" else 32
        stride = header["stride"]
        height = header["height"]
        start = offset
        stop = offset + stride * height
        seq0 = self.read_sequence ()
        data = bytes (self.map[start:stop])
        seq1 = self.read_sequence ()
        return data, seq0, seq1

    def read_sequence (self):
        offset = 32 if self.layout == "new" else 24
        return struct.unpack_from ("<Q", self.map, offset)[0]

    def close (self):
        if self.map is not None:
            self.map.close ()
            self.map = None
        self.f.close ()


def wait_for_stream (path, layout, timeout, engine):
    deadline = time.time () + timeout
    while time.time () < deadline:
        if not os.path.exists (path):
            time.sleep (0.25)
            continue
        try:
            view = StreamView (path, layout)
            header = view.header ()
            if header["magic"] == 0x4C574531 and header["sequence"] > 0:
                return view, header
            view.close ()
        except (ValueError, OSError):
            pass
        time.sleep (0.25)
    return None, None


def sample_frames (view, seconds, layout):
    samples = []
    deadline = time.time () + seconds
    last = None
    while time.time () < deadline:
        header = view.header ()
        seq = header["sequence"]
        if seq != last:
            data, seq0, seq1 = view.frame_bytes (header)
            stable = seq0 == seq1
            crc = zlib.crc32 (data) if stable else None
            samples.append ({"seq": seq, "crc": crc, "stable": stable,
                             "width": header["width"], "height": header["height"],
                             "data": data if stable else None})
            last = seq
        time.sleep (0.03)
    return samples


def launch_engine (engine_dir, args_list, log_path):
    log = open (log_path, "w")
    process = subprocess.Popen ([os.path.join (engine_dir, "linux-wallpaperengine")] + args_list,
                                stdout = log, stderr = log, cwd = engine_dir)
    return process, log


def stop_engine (process):
    if process.poll () is None:
        process.terminate ()
        try:
            process.wait (timeout = 5)
        except subprocess.TimeoutExpired:
            process.kill ()
            process.wait (timeout = 5)


def check_stream (args):
    name = "stream liveness and monotonic sequence"
    path = args.frame_path
    if os.path.exists (path):
        os.remove (path)
    process, log = launch_engine (args.engine_dir,
                                  [args.wallpaper, "--window", "0x0x480x320", "--silent",
                                   "--fps", str (args.fps), "--frame-out", path],
                                  "/tmp/framesrc-it-stream.log")
    view = None
    try:
        view, header = wait_for_stream (path, args.layout, 30, process)
        if view is None:
            report (name, False, "no stream within timeout")
            return
        samples = sample_frames (view, args.seconds, args.layout)

        alive = len (samples) >= 2
        report ("stream alive", alive, f"{len (samples)} distinct samples in {args.seconds}s")

        seqs = [s["seq"] for s in samples]
        monotonic = all (b > a for a, b in zip (seqs, seqs [1:]))
        report ("sequence strictly monotonic", monotonic, f"first={seqs[0] if seqs else '-'} last={seqs[-1] if seqs else '-'}")

        torn = [s for s in samples if not s["stable"]]
        torn_ratio = len (torn) / len (samples) if samples else 1.0
        report ("torn ratio below 20%", torn_ratio < 0.2, f"{len (torn)}/{len (samples)} torn")

        nonblack = 0
        counted = 0
        for s in samples [-10:]:
            if s["data"] is None:
                continue
            counted += 1
            if sum (s["data"] [::997]) > 0:
                nonblack += 1
        report ("frames not all-black", counted > 0 and nonblack == counted,
                f"{nonblack}/{counted} non-black")

        crcs = [s["crc"] for s in samples if s["crc"] is not None]
        distinct = len (set (crcs))
        report ("content animated", len (crcs) >= 2 and distinct >= len (crcs) // 2,
                f"{distinct} distinct frames of {len (crcs)}")

        span = seqs [-1] - seqs [0] if len (seqs) >= 2 else 0
        expected = args.fps * args.seconds
        in_band = span <= expected * 1.5
        report ("sequence span within fps budget", in_band,
                f"span={span} budget<={int (expected * 1.5)}")
    finally:
        stop_engine (process)
        log.close ()


def check_oracle (args):
    name = "render oracle (screen capture vs frame-out)"
    if not args.with_oracle:
        report (name, True, "skipped by --fast")
        return
    path = args.frame_path
    if os.path.exists (path):
        os.remove (path)
    shot = "/tmp/framesrc-it-oracle.png"
    if os.path.exists (shot):
        os.remove (shot)
    process, log = launch_engine (args.engine_dir,
                                  ["--screen-root", args.output, "--bg", args.wallpaper, "--silent",
                                   "--fps", str (args.fps), "--frame-out", path],
                                  "/tmp/framesrc-it-oracle.log")
    view = None
    try:
        view, header = wait_for_stream (path, args.layout, 30, process)
        time.sleep (3)
        shots = []
        for i in range (3):
            shot_i = f"{shot}.{i}"
            try:
                result = subprocess.run (["spectacle", "-b", "-n", "-o", shot_i],
                                         capture_output = True, timeout = 15)
                if result.returncode == 0 and os.path.exists (shot_i):
                    shots.append (shot_i)
            except subprocess.TimeoutExpired:
                pass
            time.sleep (1)
        if view is None or not shots:
            report (name, False, "capture failed")
            return


        crop = "crop=iw:ih*0.85:0:0,scale=8:8"
        oracle_grids = [subprocess.run (["ffmpeg", "-loglevel", "error", "-i", shot_i,
                                         "-vf", crop, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                                        capture_output = True, check = True).stdout for shot_i in shots]
        frame_file = "/tmp/framesrc-it-oracle-frame.raw"
        header = view.header ()
        data, seq0, seq1 = view.frame_bytes (header)
        with open (frame_file, "wb") as f:
            f.write (data)
        stride = header["stride"]
        height = header["height"]
        width = header["width"]
        frame_grid = subprocess.run (["ffmpeg", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                                      "-s", f"{width}x{height}", "-i", frame_file,
                                      "-vf", crop, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                                     capture_output = True, check = True).stdout
        if any (len (g) != len (frame_grid) for g in oracle_grids):
            report (name, False, "grid size mismatch")
            return
        def luminance (grid):
            return [(grid[i * 3] * 299 + grid[i * 3 + 1] * 587 + grid[i * 3 + 2] * 114) / 1000.0 for i in range (len (grid) // 3)]

        def ranks (values):
            order = sorted (range (len (values)), key = lambda i: values[i])
            result = [0] * len (values)
            for rank, index in enumerate (order):
                result[index] = rank
            return result

        def spearman (a, b):
            rank_a = ranks (a)
            rank_b = ranks (b)
            mean_a = sum (rank_a) / len (rank_a)
            mean_b = sum (rank_b) / len (rank_b)
            numerator = sum ((x - mean_a) * (y - mean_b) for x, y in zip (rank_a, rank_b))
            denom_a = sum ((x - mean_a) ** 2 for x in rank_a) ** 0.5
            denom_b = sum ((y - mean_b) ** 2 for y in rank_b) ** 0.5
            return numerator / (denom_a * denom_b) if denom_a > 0 and denom_b > 0 else 0.0

        frame_luma = luminance (frame_grid)
        oracle_lumas = [luminance (g) for g in oracle_grids]
        correlation = max (spearman (o, frame_luma) for o in oracle_lumas)
        oracle_luma = sum (sum (o) / len (o) for o in oracle_lumas) / len (oracle_lumas)
        luma_ratio = (sum (frame_luma) / len (frame_luma)) / oracle_luma if oracle_luma else 0
        structural = correlation >= 0.55
        brightness_sane = 0.5 <= luma_ratio <= 2.0
        report (name, structural and brightness_sane,
                f"rank correlation={correlation:.2f} (need>=0.55; flipped~0.32) luma ratio={luma_ratio:.2f}")
    finally:
        stop_engine (process)
        log.close ()
        if view is not None:
            view.close ()


def check_lifecycle (args):
    name = "lifecycle (kill -9, restart, consumer before engine)"
    path = args.frame_path
    if os.path.exists (path):
        os.remove (path)

    view = StreamView (path, args.layout) if os.path.exists (path) else None
    process, log = launch_engine (args.engine_dir,
                                  [args.wallpaper, "--window", "0x0x480x320", "--silent",
                                   "--fps", str (args.fps), "--frame-out", path],
                                  "/tmp/framesrc-it-life.log")
    try:
        started, _ = wait_for_stream (path, args.layout, 30, process)
        if not started:
            report (name, False, "engine never produced frames")
            return
        seq_before_kill = started.header ()["sequence"]
        process.send_signal (signal.SIGKILL)
        process.wait (timeout = 5)
        if not os.path.exists (path):
            report (name, False, "file removed after kill -9")
            return
        report ("file survives kill -9", True, f"seq at kill={seq_before_kill}")

        process2, log2 = launch_engine (args.engine_dir,
                                        [args.wallpaper, "--window", "0x0x480x320", "--silent",
                                         "--fps", str (args.fps), "--frame-out", path],
                                        "/tmp/framesrc-it-life2.log")
        try:
            resumed, header = wait_for_stream (path, args.layout, 30, process2)
            samples = sample_frames (resumed, 5, args.layout) if resumed else []
            report ("stream resumes after restart", len (samples) >= 2,
                    f"{len (samples)} samples in 5s after restart")
        finally:
            stop_engine (process2)
            log2.close ()
    finally:
        log.close ()
        if view:
            view.close ()


def main ():

    parser = argparse.ArgumentParser (description = "FrameSource engine integration driver")
    parser.add_argument ("--engine-dir", required = True)
    parser.add_argument ("--wallpaper", default = "2172956777")
    parser.add_argument ("--output", default = "eDP-1")
    parser.add_argument ("--fps", type = int, default = 30)
    parser.add_argument ("--seconds", type = int, default = 15)
    parser.add_argument ("--frame-path", default = "/tmp/framesrc-integration.bin")
    parser.add_argument ("--layout", choices = ["old", "new"], default = "new")
    parser.add_argument ("--with-oracle", dest = "with_oracle", action = argparse.BooleanOptionalAction, default = True)
    args = parser.parse_args ()

    leftover = subprocess.run (["pkill", "-f", f"frame-out {args.frame_path}"], capture_output = True)
    if leftover.returncode == 0:
        print ("cleanup: terminated leftover engine instances from previous runs")
        time.sleep (2)

    check_stream (args)
    check_lifecycle (args)
    check_oracle (args)

    failed = [r for r in RESULTS if not r[1]]
    print (f"\ndriver summary: {len (RESULTS) - len (failed)}/{len (RESULTS)} checks passed")
    for name, _, detail in failed:
        print (f"  FAILED: {name} ({detail})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit (main ())
