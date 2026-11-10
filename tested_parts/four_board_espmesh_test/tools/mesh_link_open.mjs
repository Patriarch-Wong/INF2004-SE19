import { spawnSync } from 'node:child_process';
import { createHmac, pbkdf2Sync } from 'node:crypto';
import { existsSync, mkdtempSync, realpathSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';

const CONFIG_PREFIX = 'se19-tshark-';
const PASSWORD_LABEL = 'SE19 mesh soft-AP password';
const MAX_TSHARK_OUTPUT = 128 * 1024 * 1024;
const PROTECTED_DATA = 'wlan.fc.type == 2 && wlan.fc.protected == 1';

function tsharkDefault() {
  if (process.platform === 'win32') {
    const standardPath = join(process.env.ProgramW6432 ?? process.env.ProgramFiles ?? 'C:/Program Files',
      'Wireshark', 'tshark.exe');
    if (existsSync(standardPath)) return standardPath;
  }
  return 'tshark';
}

function runTshark(tsharkPath, pcapPath, profile, args) {
  const result = spawnSync(tsharkPath, ['-n', '-r', pcapPath, ...args], {
    encoding: 'utf8',
    env: { ...process.env, WIRESHARK_CONFIG_DIR: profile, LC_ALL: 'C', LANG: 'C' },
    maxBuffer: MAX_TSHARK_OUTPUT,
    windowsHide: true,
  });
  if (result.error?.code === 'ENOENT') {
    throw new Error('TShark is unavailable. Install Wireshark with TShark or specify its path.');
  }
  if (result.error) throw new Error(`TShark could not read the capture: ${result.error.message}`);
  if (result.status !== 0) {
    throw new Error(`TShark could not process the capture (exit ${result.status}).`);
  }
  return result.stdout;
}

function readWifiMetadata(text) {
  const ssids = new Set();
  const addresses = new Map();
  let eapolCount = 0;
  for (const line of text.split(/\r?\n/)) {
    if (!line) continue;
    const [number, ssid = '', eapolType, transmitter, receiver] = line.split('\t');
    const hex = ssid.replaceAll(':', '').toLowerCase();
    if (/^(?:[0-9a-f]{2}){1,32}$/.test(hex)) ssids.add(hex);
    if (eapolType?.trim() === '3') eapolCount++;
    if (transmitter && receiver) {
      addresses.set(Number(number), { transmitter: transmitter.toLowerCase(), receiver: receiver.toLowerCase() });
    }
  }
  return { ssids, eapolCount, addresses };
}

function readDecryptedBlocks(text) {
  const frames = [];
  let protectedCount = 0;
  let number;
  let block;
  let used = 0;

  function finishBlock() {
    if (!block) return;
    if (used !== block.length) throw new Error(`Incomplete decrypted data for Wi-Fi frame ${number}`);
    frames.push({ number, bytes: block });
    block = undefined;
    used = 0;
  }

  for (const line of text.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (/^\d+$/.test(trimmed)) {
      finishBlock();
      number = Number(trimmed);
      protectedCount++;
      continue;
    }
    const heading = /^Decrypted (?:CCMP|GCMP) data \((\d+) bytes\):$/.exec(trimmed);
    if (heading) {
      finishBlock();
      if (number === undefined) throw new Error('Missing frame number before decrypted TShark data');
      const length = Number(heading[1]);
      if (length > 65535) throw new Error(`Invalid decrypted length for Wi-Fi frame ${number}`);
      block = Buffer.alloc(length);
      continue;
    }
    if (!block || !trimmed) continue;
    const row = /^([0-9a-f]+)\s+((?:[0-9a-f]{2})(?:\s+[0-9a-f]{2})*)$/i.exec(trimmed);
    if (!row) {
      finishBlock();
      continue;
    }
    const offset = Number.parseInt(row[1], 16);
    const bytes = Buffer.from(row[2].replace(/\s+/g, ''), 'hex');
    if (offset !== used || used + bytes.length > block.length) {
      throw new Error(`Invalid decrypted hex offsets for Wi-Fi frame ${number}`);
    }
    bytes.copy(block, used);
    used += bytes.length;
    if (used === block.length) finishBlock();
  }
  finishBlock();
  return { protectedCount, frames };
}

/**
 * Open the WPA2 Wi-Fi layer. The returned buffers still contain the mesh
 * wrapper and encrypted application bytes. No key or temporary profile is
 * included in the result.
 */
export function openMeshLink(pcapPath, networkKey, options = {}) {
  const tsharkPath = options.tsharkPath ?? tsharkDefault();
  const capture = resolve(pcapPath);
  if (options.passphrase === undefined && !(networkKey instanceof Uint8Array && networkKey.length === 16)) {
    throw new Error('Expected the 16-byte SE19 network key');
  }
  // A passphrase override is useful for testing with synthetic WPA2 captures.
  const passphrase = options.passphrase === undefined
    ? createHmac('sha256', networkKey).update(PASSWORD_LABEL, 'ascii').digest().subarray(0, 16).toString('hex')
    : options.passphrase;
  if (typeof passphrase !== 'string' || !/^[ -~]{8,63}$/.test(passphrase)) {
    throw new Error('Expected a WPA2 passphrase of 8 to 63 ASCII characters');
  }
  const tempRoot = realpathSync(tmpdir());
  const profile = mkdtempSync(join(tempRoot, CONFIG_PREFIX));
  try {
    const metadata = runTshark(tsharkPath, capture, profile, [
      '-Y', `wlan.ssid || eapol.type == 3 || (${PROTECTED_DATA})`,
      '-T', 'fields', '-E', 'occurrence=f', '-e', 'frame.number',
      '-e', 'wlan.ssid', '-e', 'eapol.type', '-e', 'wlan.ta', '-e', 'wlan.ra',
    ]);
    const { ssids, eapolCount, addresses } = readWifiMetadata(metadata);
    if (ssids.size > 64) {
      throw new Error('The capture contains over 64 SSIDs. Capture only the handset traffic and retry.');
    }
    const pmks = Array.from(ssids, (ssid) =>
      pbkdf2Sync(passphrase, Buffer.from(ssid, 'hex'), 4096, 32, 'sha1').toString('hex'));
    if (pmks.length) {
      const keys = pmks.map((pmk) => `"wpa-psk","${pmk}"\n`).join('');
      writeFileSync(join(profile, '80211_keys'), keys, { encoding: 'ascii', mode: 0o600 });
    }
    const hexdump = runTshark(tsharkPath, capture, profile, [
      '-o', 'wlan.enable_decryption:TRUE', '-o', 'gui.column.format:"No.","%m"',
      '-P', '--hexdump', 'all', '--hexdump', 'noascii', '-Y', PROTECTED_DATA,
    ]);
    const { protectedCount, frames } = readDecryptedBlocks(hexdump);
    for (const frame of frames) Object.assign(frame, addresses.get(frame.number));
    return { ssidCount: ssids.size, eapolCount, protectedCount, frames };
  } finally {
    if (dirname(profile) === tempRoot && basename(profile).startsWith(CONFIG_PREFIX)) {
      rmSync(profile, { recursive: true, force: true });
    }
  }
}
