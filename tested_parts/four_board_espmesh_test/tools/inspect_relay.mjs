import { timingSafeEqual } from 'node:crypto';
import { pathToFileURL } from 'node:url';
import { hmacTag, pairId, readKeys } from './app_crypto.mjs';
import { openMeshLink } from './mesh_link_open.mjs';
import { createProjectClassifier } from './project_classifier.mjs';

const macPattern = /^(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$/i;
const stockMacs = ['e0:72:a1:d8:e2:10', 'e2:72:a1:d9:a4:94', 'e0:72:a1:d7:fa:b8'];
const minimumFrameBytes = 40;
const maximumFrameBytes = 250;
const hopTagBytes = 16;

// Scan past any mesh headers and recognize only complete hop-authenticated
// application frames for Board 1 and Board 2. Include handshake messages.
function* findFrames(bytes, network) {
  for (let offset = 0; offset + minimumFrameBytes <= bytes.length; offset++) {
    const [version, type, source, destination] = bytes.subarray(offset, offset + 4);
    if (version !== 1 || ![1, 4].includes(type) || pairId(source, destination) !== '1-2'
        || bytes.readUInt32LE(offset + 4) === 0) continue;

    const frame = bytes.subarray(offset, offset + maximumFrameBytes);
    const handshakeSize = frame[8] === 1 ? 41 : 73;
    const firstSize = type === 4 ? handshakeSize : minimumFrameBytes;
    const lastSize = type === 4 ? Math.min(handshakeSize, frame.length) : frame.length;
    for (let size = firstSize; size <= lastSize; size++) {
      const tagStart = size - hopTagBytes;
      if (timingSafeEqual(hmacTag(network, frame.subarray(0, tagStart)), frame.subarray(tagStart, size))) {
        yield frame.subarray(0, size);
        offset += size - 1;
        break;
      }
    }
  }
}

export function inspectRelay(frames, keys, [board1, relay, board2]) {
  const classify = createProjectClassifier(keys);
  const directions = [
    { source: 1, destination: 2, origin: board1, target: board2, ingress: 0, egress: 0, proof: null },
    { source: 2, destination: 1, origin: board2, target: board1, ingress: 0, egress: 0, proof: null },
  ];
  const ingressByBytes = new Map();
  let verified = 0;
  let wrongKeyRejected = 0;
  let tamperRejected = 0;
  let missingSession = 0;
  let invalidTag = 0;

  for (const { number, bytes, transmitter, receiver } of frames) {
    if (transmitter !== relay && receiver !== relay) continue;
    for (const frame of findFrames(bytes, keys.network)) {
      const direction = directions[frame[2] - 1];
      const ingress = transmitter === direction.origin && receiver === relay;
      const egress = transmitter === relay && receiver === direction.target;
      if (!ingress && !egress) continue;

      const result = classify(frame, { transport: 'mesh' });
      if (result.status === 'encrypted-unavailable') missingSession++;
      if (result.status === 'encrypted-auth-fail') invalidTag++;
      if (result.status !== 'encrypted-verified' || !result.hello) continue;

      verified++;
      if (result.wrongKeyRejected) wrongKeyRejected++;
      if (result.tamperRejected) tamperRejected++;

      // Match every byte of the sealed application frame, including the
      // endpoints, counter and tags. Never print the bytes or plaintext.
      const exactBytes = frame.toString('hex');
      if (ingress) {
        direction.ingress++;
        if (!ingressByBytes.has(exactBytes)) ingressByBytes.set(exactBytes, number);
      } else {
        direction.egress++;
        const first = ingressByBytes.get(exactBytes);
        if (!direction.proof && first !== undefined && first < number) {
          direction.proof = { first, second: number };
        }
      }
    }
  }

  return { directions, verified, wrongKeyRejected, tamperRejected, missingSession, invalidTag };
}

function main(args) {
  const [capture, keyFile, ...macArgs] = args;
  const macs = macArgs.length ? macArgs.map(mac => mac.toLowerCase()) : stockMacs;
  if (!capture?.toLowerCase().endsWith('.pcap') || !keyFile || macs.length !== 3
      || !macs.every(mac => macPattern.test(mac)) || new Set(macs).size !== macs.length) {
    throw new Error('Usage: node inspect_relay.mjs <capture.pcap> <e2e_keys.h> [Board1-STA Board3-SoftAP Board2-STA]');
  }

  const keys = readKeys(keyFile);
  if (!keys.pairs.has('1-2')) throw new Error('The key file lacks the pair key for Boards 1 and 2.');
  const link = openMeshLink(capture, keys.network);
  const result = inspectRelay(link.frames, keys, macs);
  console.log(`Wi-Fi capture: ${link.protectedCount} protected frames, ${link.eapolCount} WPA2 handshake frames.`);
  console.log(`WPA2: ${link.frames.length} protected frames opened with the matching network key.`);
  for (const direction of result.directions) {
    const { source, destination, ingress, egress, proof } = direction;
    if (proof) {
      console.log(`Board ${source} → 3 → ${destination}: identical AES-GCM-authenticated Hello in PCAP frames ${proof.first} and ${proof.second}.`);
    } else {
      console.log(`Board ${source} → 3 → ${destination}: ${ingress} inbound and ${egress} outbound authenticated Hello copies, no ordered exact match.`);
    }
  }
  const { verified, wrongKeyRejected, tamperRejected, missingSession, invalidTag } = result;
  console.log(`AES-GCM controls: ${wrongKeyRejected}/${verified} wrong keys rejected, ${tamperRejected}/${verified} altered ciphertexts rejected.`);
  if (missingSession || invalidTag) {
    console.log(`Additional app frames without captured session: ${missingSession}. Failed AES-GCM: ${invalidTag}.`);
  }
  if (verified !== wrongKeyRejected || verified !== tamperRejected) {
    console.error('FAIL: An AES-GCM negative control unexpectedly passed.');
    return 1;
  }
  if (result.directions.every(direction => direction.proof)) {
    console.log('PASS: Authenticated Hello crossed both WPA2 radio hops through Board 3 in both directions.');
    return 0;
  }
  console.log('INCONCLUSIVE: Capture both handset joins and both wireless hops, then send Hello from each board over ESP-MESH.');
  return 2;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    process.exitCode = main(process.argv.slice(2));
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
