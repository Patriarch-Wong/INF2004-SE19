import { timingSafeEqual } from 'node:crypto';
import { decrypt, hmacTag, pairId, sessionKey } from './app_crypto.mjs';

const unknown = 'unknown-payload';
const stage6Marker = Buffer.from('SE19OPUS');

function stage6PlainVoice(frame) {
  // The E2E_PLAIN control used an eight-byte header and two zero-filled tags.
  // Its Opus payload has 26 fixed bytes, then the declared packet bytes.
  if (frame[1] !== 2 || frame.length < 67 || frame.length > 146 || !frame.subarray(8, 16).equals(stage6Marker)) return null;
  const sequence = frame.readUInt32LE(16);
  const frameCount = frame.readUInt32LE(20);
  const opusLength = frame.readUInt16LE(32);
  if (sequence >= frameCount || opusLength < 1 || opusLength > 80 || frame.length !== 66 + opusLength ||
      !frame.subarray(-32).every(byte => byte === 0)) return null;
  return { sequence, frameCount };
}

function textCategory(payload, transport) {
  const printable = bytes => bytes.every(byte => byte >= 0x20 && byte <= 0x7e);
  if (transport !== 'lora' && payload.length > 0 && printable(payload)) return 'message';
  if ((transport === 'lora' || transport === 'app') && payload.length >= 2 && payload[1] !== 0) {
    if (payload[0] === 1 && payload.length > 2 && printable(payload.subarray(2))) return 'message';
    if (payload[0] === 2 && payload.length === 2) return 'message-receipt';
  }
  return unknown;
}

function voiceCategory(payload, transport) {
  const [kind, flags] = payload;
  if (kind === 4 && payload.length === 6 && flags >= 1 && flags <= 6 && payload.readUInt32LE(2)) {
    return 'connection-control';
  }
  if (transport === 'lora') return unknown;
  switch (kind) {
    case 1: {
      if (payload.length < 13 || (flags & ~3) || !payload.readUInt32LE(2)) return unknown;
      const length = payload[10];
      if (length < 1 || length > 80 || payload.length < 12 + length) return unknown;
      const previous = payload[11 + length];
      if (previous > 80 || payload.length !== 12 + length + previous) return unknown;
      if (previous && payload.readUInt32LE(6) === 0) return unknown;
      return 'voice';
    }
    case 2:
      return payload.length === 13 && payload.readUInt32LE(1) && payload.readUInt32LE(9) ? 'voice-nack' : unknown;
    case 3:
      return payload.length === 14 && !(flags & ~3) && payload.readUInt32LE(2) ? 'voice-end' : unknown;
    case 5:
      return payload.length === 1 || payload.length === 3 ? 'presence' : unknown;
    default:
      return unknown;
  }
}

// Classify complete SE19 handset application frames in capture order. Use separate
// transport labels for independent sessions. Returned records contain no key or body.
export function createProjectClassifier({ network, pairs = new Map() }) {
  if (!Buffer.isBuffer(network) || network.length !== 16) throw new TypeError('Expected the 16-byte network key');
  const keys = new Map();
  return (frame, { transport = 'app' } = {}) => {
    if (!Buffer.isBuffer(frame)) throw new TypeError('Expected a complete application frame Buffer');
    if (frame.length < 40 || frame.length > 250 || frame[0] !== 1 || frame.readUInt32LE(4) === 0) {
      return { status: 'unknown-format', category: 'unknown', projectConfirmed: false };
    }
    const [, type, source, destination] = frame;
    const metadata = { source, destination, counter: frame.readUInt32LE(4) };
    const plainVoice = stage6PlainVoice(frame);
    if (plainVoice) return { status: 'plaintext-test', category: 'voice', projectConfirmed: false, ...metadata, ...plainVoice };

    const tag = hmacTag(network, frame.subarray(0, -16));
    if (!timingSafeEqual(tag, frame.subarray(-16))) {
      return { status: 'unknown-or-invalid-hmac', category: 'unknown', projectConfirmed: false };
    }

    const result = (status, category, extra) => ({ status, category, projectConfirmed: true, ...metadata, ...extra });
    const pair = pairId(source, destination);
    const sessionId = `${transport}:${pair}`;
    const known = keys.get(sessionId) ?? [];
    if (type === 4) {
      const step = frame[8];
      if (!source || !destination || source === destination) return result('invalid-project-frame', 'session-control');
      if (step === 1 && frame.length === 41) return result('clear-control', 'session-hello', { pairAuth: 'not-applicable' });
      if ((step !== 2 && step !== 3) || frame.length !== 73) return result('invalid-project-frame', 'session-control');
      const configuredPair = pairs.get(pair);
      const key = sessionKey(frame, configuredPair);
      if (key && !known.some(previous => previous.equals(key))) {
        known.push(key);
        keys.set(sessionId, known);
      }
      return result('clear-control', step === 2 ? 'session-reply' : 'session-confirm', {
        pairAuth: key ? 'verified' : configuredPair ? 'failed' : 'unavailable',
      });
    }
    if (type !== 1 && type !== 2) return result('authenticated-unknown', 'unknown');

    const declaredType = type === 1 ? 'message' : 'voice-or-control';
    if (!known.length) return result('encrypted-unavailable', 'unverified', { declaredType });
    for (const key of known) {
      const payload = decrypt(frame, key);
      if (payload === null) continue;
      const category = type === 1 ? textCategory(payload, transport) : voiceCategory(payload, transport);

      // The hop HMAC already passed. Exercise AES-GCM directly so a failed
      // hop check cannot account for either negative control.
      const wrongKey = Buffer.from(key);
      wrongKey[0] ^= 1;
      const wrongKeyRejected = decrypt(frame, wrongKey) === null;
      wrongKey.fill(0);
      const tampered = Buffer.from(frame);
      tampered[8] ^= 1; // First ciphertext byte, or the GCM tag for empty data.
      const tamperRejected = decrypt(tampered, key) === null;
      const hello = type === 1 && payload.equals(Buffer.from(`Hello from Board ${source}`));
      return result('encrypted-verified', category, { declaredType, hello, wrongKeyRejected, tamperRejected });
    }
    return result('encrypted-auth-fail', 'unverified', { declaredType });
  };
}
