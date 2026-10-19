"""Encode a video as length-prefixed JPEG frames for the ESP32 LCD player."""

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path, default=Path("media/video.lcdv"))
    parser.add_argument("--fps", type=int, default=12)
    parser.add_argument("--quality", type=int, default=6)
    args = parser.parse_args()
    if not 1 <= args.fps <= 30 or not 2 <= args.quality <= 31:
        parser.error("fps must be 1..30 and quality must be 2..31")

    width, height = 240, 136
    filters = (
        f"fps={args.fps},"
        f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
        f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2,setsar=1"
    )
    with tempfile.TemporaryDirectory() as temp_dir:
        subprocess.run(
            [
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-i",
                str(args.source), "-an", "-vf", filters, "-c:v", "mjpeg",
                "-q:v", str(args.quality), "-pix_fmt", "yuvj420p",
                str(Path(temp_dir) / "%06d.jpg"),
            ],
            check=True,
        )
        frames = sorted(Path(temp_dir).glob("*.jpg"))
        if not frames:
            raise ValueError("Source has no video frames")
        max_frame = max(frame.stat().st_size for frame in frames)
        if max_frame > 32768:
            raise ValueError("Frame exceeds 32768 bytes; lower JPEG quality")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        staging = args.output.with_suffix(".tmp")
        with staging.open("wb") as output:
            output.write(struct.pack(
                "<8sHHHHII", b"LCDV0001", width, height, args.fps, 0,
                len(frames), max_frame,
            ))
            for frame in frames:
                jpeg = frame.read_bytes()
                if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(
                    b"\xff\xd9"
                ):
                    raise ValueError(f"Invalid JPEG: {frame.name}")
                output.write(struct.pack("<I", len(jpeg)))
                output.write(jpeg)
        staging.replace(args.output)
    size = args.output.stat().st_size
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    print(f"{args.output}: {width}x{height}, {args.fps} fps, "
          f"{len(frames)} frames, {len(frames) / args.fps:.2f} seconds")
    print(f"{size:,} bytes ({size / 1048576:.2f} MiB); "
          f"largest JPEG {max_frame:,} bytes")
    print(f"SHA256 {digest}")


if __name__ == "__main__":
    main()
