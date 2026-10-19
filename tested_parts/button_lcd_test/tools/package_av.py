"""Bundle LCDV video and 16 kHz mono mu-law audio for SD/ESP-NOW playback."""

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audio", type=Path)
    parser.add_argument("--video", type=Path, default=Path("media/video.lcdv"))
    parser.add_argument("--output", type=Path, default=Path("media/clip.lcdav"))
    args = parser.parse_args()
    video = args.video.read_bytes()
    if len(video) < 24 or video[:8] != b"LCDV0001":
        parser.error("Expected an LCDV0001 video")
    fps = struct.unpack_from("<H", video, 12)[0]
    frames = struct.unpack_from("<I", video, 16)[0]
    if not fps or not frames:
        parser.error("Video has no frames or frame rate")
    rate = 16000
    samples = round(frames * rate / fps)
    with tempfile.TemporaryDirectory() as temporary:
        audio_path = Path(temporary) / "audio.mulaw"
        subprocess.run([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-i",
            str(args.audio), "-vn", "-ac", "1", "-ar", str(rate),
            "-af", f"aresample={rate},apad,atrim=end_sample={samples}",
            "-c:a", "pcm_mulaw", "-f", "mulaw", str(audio_path),
        ], check=True)
        audio = audio_path.read_bytes()
    if len(audio) != samples:
        raise ValueError("Audio length does not match video duration")
    header = struct.pack("<8s6I", b"LCDAV001", 32, 32 + len(audio),
                         32, len(audio), rate, len(video))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + audio + video)
    content = args.output.read_bytes()
    print(f"{args.output}: {len(content):,} bytes, {samples / rate:.3f}s")
    print("Audio: 16 kHz mono G.711 mu-law, 8 bits per sample")
    print(f"SHA256 {hashlib.sha256(content).hexdigest()}")


if __name__ == "__main__":
    main()
