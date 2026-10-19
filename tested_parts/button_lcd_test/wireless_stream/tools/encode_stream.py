"""Encode 240x136/12 fps constrained-baseline H.264 + 16 kHz mono AAC."""

import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zlib

HEADER = struct.Struct("<8s4H8I")
RECORD = struct.Struct("<4I")
RATE = 16000
FPS = 12


def run(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", *args],
                   check=True)


def split_video(data):
    starts = [m.start() for m in re.finditer(b"\x00\x00\x00\x01\x09", data)]
    if not starts or starts[0] != 0:
        raise ValueError("H.264 stream must start with an AUD")
    starts.append(len(data))
    return [data[a:b] for a, b in zip(starts, starts[1:])]


def split_audio(data):
    frames = []
    offset = 0
    while offset < len(data):
        p = data[offset:offset + 7]
        if len(p) != 7 or p[:2] != b"\xff\xf1":
            raise ValueError("Invalid ADTS header")
        size = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5)
        if size < 8 or offset + size > len(data):
            raise ValueError("Truncated AAC frame")
        frames.append(data[offset:offset + size])
        offset += size
    return frames


def validate(data):
    if len(data) < HEADER.size:
        raise ValueError("Truncated header")
    (magic, width, height, fps, reserved, count, rate, samples, skip,
     records, maximum, size, duration) = HEADER.unpack_from(data)
    if (magic != b"LCDSTR01" or (width, height, fps, rate) !=
            (240, 136, FPS, RATE) or reserved or not 1 <= count <= 7200
            or samples != (count * rate + fps // 2) // fps or skip != 1024
            or records != count + (samples + skip + 1023) // 1024
            or not 8 <= maximum <= 65536 or size != len(data)
            or size > 4 * 1024 * 1024
            or duration != count * 1000000 // fps):
        raise ValueError("Unsupported stream header")
    offset, videos, audios, previous = HEADER.size, 0, 0, 0
    for _ in range(records):
        if offset + RECORD.size > size:
            raise ValueError("Missing record header")
        kind, stamp, length, crc = RECORD.unpack_from(data, offset)
        offset += RECORD.size
        payload = data[offset:offset + length]
        if (not 8 <= length <= maximum or len(payload) != length
                or zlib.crc32(payload) != crc or stamp < previous):
            raise ValueError("Invalid record or checksum")
        if kind == 1:
            if (stamp != (videos * rate + fps // 2) // fps
                    or not payload.startswith(b"\x00\x00\x00\x01\x09")):
                raise ValueError("Invalid video access unit")
            videos += 1
        elif kind == 2:
            if (stamp != max(0, (audios - 1) * 1024)
                    or len(split_audio(payload)) != 1
                    or payload[2] >> 6 != 1
                    or (payload[2] >> 2) & 15 != 8
                    or ((payload[2] & 1) << 2 | payload[3] >> 6) != 1
                    or payload[6] & 3):
                raise ValueError("Invalid AAC frame")
            audios += 1
        else:
            raise ValueError("Unknown record type")
        previous = stamp
        offset += length
    if offset != size or videos != count or audios != records - count:
        raise ValueError("Incorrect record count or trailing data")
    return count, samples / rate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("audio", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path("media/clip.lcdstream"))
    parser.add_argument("--crf", type=int, default=28)
    args = parser.parse_args()
    if not 18 <= args.crf <= 40:
        parser.error("crf must be 18..40")
    with tempfile.TemporaryDirectory() as temporary:
        video_path = Path(temporary) / "video.h264"
        audio_path = Path(temporary) / "audio.aac"
        run("-i", str(args.video), "-an", "-vf",
            "fps=12,scale=240:136:force_original_aspect_ratio=decrease,"
            "pad=240:136:(ow-iw)/2:(oh-ih)/2,setsar=1",
            "-c:v", "libx264", "-profile:v", "baseline", "-preset",
            "medium", "-crf", str(args.crf), "-pix_fmt", "yuv420p",
            "-bf", "0", "-refs", "1", "-x264-params",
            "aud=1:repeat-headers=1:keyint=12:min-keyint=12:scenecut=0",
            "-f", "h264", str(video_path))
        video = split_video(video_path.read_bytes())
        samples = (len(video) * RATE + FPS // 2) // FPS
        run("-i", str(args.audio), "-vn", "-ac", "1", "-ar", str(RATE),
            "-af", f"aresample={RATE},apad,atrim=end_sample={samples}",
            "-c:a", "aac", "-profile:a", "aac_low", "-b:a", "32k",
            "-f", "adts", str(audio_path))
        audio = split_audio(audio_path.read_bytes())
        # FFmpeg's native AAC encoder adds one 1024-sample priming frame.
        if len(audio) != (samples + 1024 + 1023) // 1024:
            raise ValueError("Unexpected AAC priming/padding")
        records = [(max(0, (i - 1) * 1024), 2, p)
                   for i, p in enumerate(audio)]
        records += [((i * RATE + FPS // 2) // FPS, 1, p)
                    for i, p in enumerate(video)]
        records.sort(key=lambda r: (r[0], -r[1]))
        payload = b"".join(RECORD.pack(kind, stamp, len(p), zlib.crc32(p))
                           + p for stamp, kind, p in records)
        maximum = max(len(p) for _, _, p in records)
        header = HEADER.pack(b"LCDSTR01", 240, 136, FPS, 0, len(video),
                             RATE, samples, 1024, len(records), maximum,
                             HEADER.size + len(payload),
                             len(video) * 1000000 // FPS)
        content = header + payload
        validate(content)
        # Decode both elementary streams on the host before publishing.
        run("-i", str(video_path), "-f", "null", "-")
        run("-i", str(audio_path), "-f", "null", "-")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        staging = args.output.with_suffix(".tmp")
        staging.write_bytes(content)
        staging.replace(args.output)
    print(f"{args.output}: {len(content):,} bytes; 240x136, 12 fps; "
          f"{len(video)} frames; {samples / RATE:.3f} s")
    print(f"AAC-LC mono 16 kHz / 32 kbit/s; largest record {maximum:,} bytes")


if __name__ == "__main__":
    main()
