"""Check signed slot extraction and the listening-file conversion without hardware."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import wave

spec = importlib.util.spec_from_file_location('mic_record', Path(__file__).parent / 'scripts/record.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    # Distinct slots and low-bit noise ensure extraction uses the correct word
    # and preserves negative values without byte swapping or sign corruption.
    raw = struct.pack('<8i', -2147483648, 0, -65536 + 255, 0,
                      2147418112 + 255, 0, 0, 0)
    result = module.export_capture(raw, 16000, root / 'capture')
    assert result['left']['min'] == -32768
    assert result['left']['max'] == 32767
    assert result['left']['full_scale_samples'] == 2
    assert result['right']['nonzero_samples'] == 0
    assert result['right']['rms'] == 0
    assert (root / 'capture/raw-i2s.bin').read_bytes() == raw
    with wave.open(str(root / 'capture/stereo-unity.wav')) as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (2, 2, 16000, 4)
        assert struct.unpack('<8h', wav.readframes(4)) == (-32768, 0, -1, 0, 32767, 0, 0, 0)
    with wave.open(str(root / 'capture/right-listen.wav')) as wav:
        assert wav.readframes(4) == bytes(8)
    result = module.export_capture(struct.pack('<8i', *([65536, 65536] * 4)), 48000, root / 'dc')
    assert result['left']['mean'] == 1 and result['left']['ac_rms'] == 0
    with wave.open(str(root / 'dc/left-listen.wav')) as wav:
        assert wav.getframerate() == 48000
        assert wav.readframes(4) == bytes(8)
    for invalid in (b'', b'1234567'):
        try:
            module.export_capture(invalid, 16000, root / 'bad')
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid stereo frame accepted')
    try:
        module.export_capture(raw, 16000, root / 'capture')
    except FileExistsError:
        pass
    else:
        raise AssertionError('Existing capture overwritten')
print('Raw preservation, signed slot extraction, WAV formats, DC removal, silence, and overwrite checks passed.')
