#!/usr/bin/env python3
"""Tests for tools/analyse_run.py on generated logs in the firmware's format."""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import analyse_run  # noqa: E402

FRAMES = analyse_run.FRAMES


class Run:
    """Three boards' logs for a run: 1->2, 2->1, then both at once."""

    def __init__(self):
        self.logs = {1: [], 2: [], "relay": []}
        self.counters = {(1, 2): 0, (2, 1): 0}
        for board in (1, 2):
            self.add(board, 1.0, "Build mode: encrypted handset via relay")
            self.add(board, 2.0, f"SESSION established with {3 - board} | "
                     "session 1234ABCD | initiator")
        self.add("relay", 0.5, "Build mode: relay (network key only)")
        self.add("relay", 0.6, "Compiled-in keys: network only | "
                 "Pair keys held: 0")
        for board in (1, 2):
            self.data(board, 3 - board, 1, 3.0)

    def add(self, board, time, text):
        self.logs[board].append((time, text))

    def data(self, src, dst, kind, time, forward=True):
        self.counters[(src, dst)] += 1
        counter = self.counters[(src, dst)]
        self.add(src, time, f"TX_DATA {kind} {src} {dst} {counter} 600 0")
        if forward:
            self.add("relay", time + 0.001,
                     f"FWD {kind} {src} {dst} {counter} 130 ok 2500")
        self.add(dst, time + 0.002, f"RX_DATA {kind} {src} {counter} 480")

    def stream(self, src, start, stream_id, crc_of=None, late=0):
        dst = 3 - src
        self.add(src, start, f"STREAM_TX_START {stream_id} to {dst} frames "
                 f"{FRAMES} 1")
        self.add(dst, start + 0.0025, f"STREAM_LOCK {stream_id} from {src} "
                 f"frames {FRAMES} anchor 0")
        for seq in range(FRAMES):
            time = start + seq * 0.02
            crc = f"{seq:08X}"
            self.add(src, time, f"OPUS_TX {seq} 60 {crc} 400 7000 8000 1")
            self.data(src, dst, 2, time)
            received = crc_of(seq) if crc_of else crc
            self.add(dst, time + 0.003,
                     f"OPUS_RX {seq} 60 {received} {seq * 20000} 0")
            self.add(dst, time + 0.2, f"OPUS_PLAY {seq} 0 2500 2100")
        end = start + FRAMES * 0.02 + 0.5
        self.add(src, end, f"TX_REPORT {stream_id} to {dst} frames {FRAMES} "
                 f"sent {FRAMES} send_errors 0 build_errors 0 echoes 600")
        self.drops(src, end)
        self.report(dst, end + 8, stream_id, src, late)

    def report(self, board, time, stream_id, src, late, snr=26.73):
        lines = [
            f"RX_REPORT {stream_id} from {src} anchor 0 frames {FRAMES} "
            f"received {FRAMES} duplicates 0 reordered 0 foreign 0 stale 0 "
            f"wrong_frames 0 played {FRAMES} abandoned 0",
            f"PLAYOUT 200000 on_time {FRAMES - late} late {late} lost 0 "
            "jitter 900",
            f"QUALITY live 200000 decoded {FRAMES} concealed 0 decode_failed "
            f"0 failed 0 corr 0.9989 snr {snr} worst 25.04 windows 60",
            "LIVE_MISSED 0",
            f"QUALITY in_order 4294967295 decoded {FRAMES} concealed 0 "
            f"decode_failed 0 failed 0 corr 0.9989 snr {snr} worst 25.04 "
            "windows 60"]
        for line in lines:
            self.add(board, time, line)
        self.drops(board, time)
        self.add(board, time, f"RX_REPORT_END {stream_id}")

    def drops(self, board, time, timer=0):
        self.add(board, time, f"VOICE_DROPS timer {timer} radio 0 encode 0 "
                 "codec 0 play_log 0")

    def relay_status(self, queue_drops=0):
        self.add("relay", 400.0, "Status | forwarded 1 | no ack 0 | send "
                 "errors 0 | dropped 0 | pairing ignored 0 | queue drops "
                 f"{queue_drops} | unknown MAC drops 0")

    def analyse(self):
        with tempfile.TemporaryDirectory() as folder:
            paths = {}
            for name, lines in self.logs.items():
                path = Path(folder) / f"{name}.txt"
                path.write_text("".join(f"{t:7.3f} {text}\n"
                                        for t, text in sorted(lines,
                                                              key=lambda x: x[0])))
                paths[name] = path
            return analyse_run.analyse(paths[1], paths[2], paths["relay"])


def good_run():
    run = Run()
    run.stream(1, 10.0, "AAAA0001")
    run.stream(2, 90.0, "BBBB0001")
    run.stream(1, 170.0, "AAAA0002")
    run.stream(2, 170.5, "BBBB0002")
    run.relay_status()
    return run


def test_good_run_passes():
    report = good_run().analyse()
    assert report["pass"], report["checks"]
    assert report["checks"]["R3_one_way"]["directions"] == ["1->2", "2->1"]
    assert report["checks"]["R4_two_way"]["directions"] == ["1->2", "2->1"]
    streams = {s["id"]: s for s in report["streams"]}
    assert not streams["AAAA0001"]["two_way"]
    assert streams["BBBB0002"]["two_way"]
    assert streams["AAAA0001"]["crc_matched"] == FRAMES
    assert streams["AAAA0001"]["relay"]["voice_forwards"] == FRAMES


def test_frame_not_through_relay_fails_r1():
    run = good_run()
    run.data(1, 2, 1, 300.0, forward=False)
    report = run.analyse()
    assert report["checks"]["R1_relay_path"][
        "accepted_without_relay_forward"] == 1
    assert not report["checks"]["R1_relay_path"]["pass"]
    assert not report["pass"]


def test_direct_frame_reject_fails_r1():
    run = good_run()
    run.add(2, 300.0, "REJECT E0:72:A1:D5:64:0C | 130 bytes | "
            "not via the relay")
    report = run.analyse()
    assert report["checks"]["R1_relay_path"]["rejected_not_via_relay"] == 1
    assert not report["pass"]


def test_relay_with_pair_key_fails_r1():
    run = good_run()
    run.logs["relay"] = [line for line in run.logs["relay"]
                         if "Pair keys held: 0" not in line[1]]
    assert not run.analyse()["checks"]["R1_relay_path"]["pass"]


def test_crc_mismatch_fails_stream():
    run = Run()
    run.stream(1, 10.0, "AAAA0001", crc_of=lambda s: "DEADBEEF"
               if s == 5 else f"{s:08X}")
    run.stream(2, 90.0, "BBBB0001")
    run.stream(1, 170.0, "AAAA0002")
    run.stream(2, 170.5, "BBBB0002")
    run.relay_status()
    report = run.analyse()
    stream = next(s for s in report["streams"] if s["id"] == "AAAA0001")
    assert stream["crc_mismatched"] == 1 and not stream["pass"]
    assert not report["checks"]["R3_one_way"]["pass"]
    assert report["checks"]["R4_two_way"]["pass"]


def test_late_frames_fail_two_way():
    run = Run()
    run.stream(1, 10.0, "AAAA0001")
    run.stream(2, 90.0, "BBBB0001")
    run.stream(1, 170.0, "AAAA0002", late=30)
    run.stream(2, 170.5, "BBBB0002")
    run.relay_status()
    report = run.analyse()
    assert report["checks"]["R3_one_way"]["pass"]
    assert not report["checks"]["R4_two_way"]["pass"]


def test_missing_direction_fails():
    run = Run()
    run.stream(1, 10.0, "AAAA0001")
    run.stream(1, 170.0, "AAAA0002")
    run.stream(2, 170.5, "BBBB0002")
    run.relay_status()
    report = run.analyse()
    assert report["checks"]["R3_one_way"]["directions"] == ["1->2"]
    assert not report["checks"]["R3_one_way"]["pass"]


def test_drops_fail_r5_and_relay_queue_fails_r6():
    run = good_run()
    run.drops(1, 399.0, timer=2)
    run.relay_status(queue_drops=3)
    report = run.analyse()
    assert not report["checks"]["R5_pipelines"]["pass"]
    assert not report["checks"]["R6_relay"]["pass"]


def test_repeated_report_is_ignored():
    run = good_run()
    # The report command prints the last stream again, here with a worse
    # figure; only the first report counts.
    run.report(1, 390.0, "BBBB0002", 2, late=500, snr=3.0)
    report = run.analyse()
    stream = next(s for s in report["streams"] if s["id"] == "BBBB0002")
    assert stream["playout"][200000]["late"] == 0
    assert report["pass"]


def test_summary_nearest_rank():
    assert analyse_run.summary([4, 1, 3, 2]) == {
        "count": 4, "min": 1, "median": 2, "p99": 4, "max": 4}
    assert analyse_run.summary([]) == {"count": 0}


if __name__ == "__main__":
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_")]
    for test in tests:
        test()
    print(f"All {len(tests)} analysis tests passed")
