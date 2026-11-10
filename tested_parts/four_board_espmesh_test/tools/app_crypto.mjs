import { createDecipheriv, createHmac, hkdfSync, timingSafeEqual } from 'node:crypto';
import { readFileSync } from 'node:fs';

export { decrypt, hmacTag, pairId, sessionKey };

function parseKey(bytes) {
  const key = Buffer.from((bytes.match(/0x[\da-f]{2}\b/gi) ?? []).map(byte => Number(byte)));
  if (key.length !== 16) throw new Error('Expected a 16-byte key in the handset key header');
  return key;
}

function pairId(source, destination) {
  return source < destination ? `${source}-${destination}` : `${destination}-${source}`;
}

export function readKeys(path) {
  const source = readFileSync(path, 'utf8').replace(/\/\/[^\r\n]*/g, '');
  const networkMatch = source.match(/\bnetwork\s*\[16\]\s*=\s*\{([^}]*)\}/);
  if (!networkMatch) throw new Error('No network key declaration in the handset key header');
  const pairs = new Map();
  for (const [, board, peers] of source.matchAll(/E2E_DEVICE_ID\s*==\s*(\d+)\s*[^]*?\bpeer_keys\s*\[\]\s*=\s*\{([^]*?)\};/g)) {
    for (const [, peer, bytes] of peers.matchAll(/\{\s*(\d+)\s*,\s*\{([^}]*)\}\s*\}/g)) {
      const id = pairId(Number(board), Number(peer));
      const key = parseKey(bytes);
      if (pairs.has(id) && !pairs.get(id).equals(key)) throw new Error(`Inconsistent pair ${id} in key header`);
      pairs.set(id, key);
    }
  }
  if (!pairs.size) throw new Error('No pre-provisioned pair keys in the handset key header');
  return { network: parseKey(networkMatch[1]), pairs };
}

function hmacTag(key, data) {
  return createHmac('sha256', key).update(data).digest().subarray(0, 16);
}

function derive(key, salt, info) {
  return Buffer.from(hkdfSync('sha256', key, salt, info, 16));
}

function sessionKey(frame, pair) {
  // A session REPLY or CONFIRM carries both nonces and the pair-auth tag.
  const step = frame[8];
  if (!pair || frame.length !== 73 || (step !== 2 && step !== 3)) return null;
  const ids = Buffer.from(step === 2 ? [frame[3], frame[2]] : [frame[2], frame[3]]);
  const nonces = frame.subarray(9, 41);
  const auth = derive(pair, Buffer.alloc(0), 'SE19 session auth');
  const authData = Buffer.concat([Buffer.from(step === 2 ? 'SE19 reply' : 'SE19 confirm'), ids, nonces]);
  if (!timingSafeEqual(hmacTag(auth, authData), frame.subarray(41, 57))) return null;
  return derive(pair, nonces, Buffer.concat([Buffer.from('SE19 session key'), ids]));
}

function decrypt(frame, key) {
  const nonce = Buffer.alloc(12);
  frame.copy(nonce, 0, 2, 4);
  frame.copy(nonce, 8, 4, 8);
  const decipher = createDecipheriv('aes-128-gcm', key, nonce);
  decipher.setAAD(frame.subarray(0, 8));
  decipher.setAuthTag(frame.subarray(-32, -16));
  const plaintext = decipher.update(frame.subarray(8, -32));
  try {
    return Buffer.concat([plaintext, decipher.final()]);
  } catch {
    return null;
  }
}

