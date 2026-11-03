"""Tests for tools/check_streams.py with lines in the firmware's format."""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_streams  # noqa: E402

TX = ("12.345 TX_END stream=0A0B0C0D peer=2 frames=500 seconds=10.00 "
      "crc=1234ABCD nacks=3 retransmits=4 send_errors=0 codec_skipped=0 "
      "encode_drops=0 encode_max_us=9000\n")
RX_OK = ("12.900 RX_END stream=0A0B0C0D from=1 frames=500 end_seen=yes "
         "played=500 missing=0 direct=495 redundant=1 retransmit=4 late=0 "
         "duplicate=0 nacks=3 requested=4 crc=1234ABCD sender_crc=1234ABCD "
         "intact=yes underruns=0 decode_errors=0 decode_max_us=2500 "
         "correlation=0.9989 snr_db=26.73 worst_1s_snr_db=20.10\n")


def run(tx: str, rx: str) -> dict:
    with tempfile.TemporaryDirectory() as folder:
        a = Path(folder, "tx.txt")
        b = Path(folder, "rx.txt")
        a.write_text("boot noise\n" + tx)
        b.write_text(rx + ">>> report\n")
        return check_streams.check(check_streams.records(a, "TX_END"),
                                   check_streams.records(b, "RX_END"))


def main() -> int:
    failures = 0

    def expect(condition: bool, name: str) -> None:
        nonlocal failures
        if not condition:
            failures += 1
            print(f"FAIL: {name}")

    good = run(TX, RX_OK)
    expect(good["passed"], "intact stream passes")
    expect(good["streams"][0]["retransmit"] == 4, "fields parsed")
    expect(good["streams"][0]["correlation"] == 0.9989, "quality parsed")
    bad_crc = run(TX, RX_OK.replace("crc=1234ABCD sender", "crc=FFFFFFFF sender"))
    expect(not bad_crc["passed"], "CRC mismatch fails")
    gap = run(TX, RX_OK.replace("played=500 missing=0",
                                "played=499 missing=1").replace(
                                    "intact=yes", "intact=no"))
    expect(not gap["passed"], "missing frame fails")
    expect(not run(TX, "")["passed"], "no receiver line fails")
    expect(not run("", "")["passed"], "no streams fails")
    if failures:
        print(f"{failures} check_streams test(s) failed")
        return 1
    print("All check_streams tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
