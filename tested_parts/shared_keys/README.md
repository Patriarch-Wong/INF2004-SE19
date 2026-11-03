# Shared keys

One key file for every project, so a board has the same keys whichever
project it runs. Boards flashed from different projects stay compatible,
and adding a board needs no key step.

| File | What |
| --- | --- |
| `e2e_keys.h` | Network key and 10 pair keys for boards 1–5. **Committed on purpose** |
| `generate_keys.py` | Extends, checks or replaces `e2e_keys.h` |
| `find_keys.py` | Reports which keys appear in firmware images or logs |

## Keys

| Key | Held by | Purpose |
| --- | --- | --- |
| Network | every board | HMAC hop tag on every frame |
| Pair `a-b` (1-2, 1-3, … 4-5) | boards a and b only | Session handshake between those two boards; the session key seals voice and connection messages |

Each board's build compiles only the block for its `E2E_DEVICE_ID`, which
holds its own 4 pair keys. Two kinds of build get the network key only:

- relay builds (`-DE2E_RELAY`)
- projects that make pair keys by pairing (`-DE2E_NETWORK_KEY_ONLY`)

The network key and the 1–2 pair key are the ones
`mic_speaker_e2e_test` committed on 2026-11-02, so boards flashed with
that project keep working.

## Using it from a project

In the project's `platformio.ini`:

```ini
[env]
build_flags =
    ...
    -I$PROJECT_DIR/../shared_keys
```

Then use `#include "e2e_keys.h"` as before. Don't keep a copy in the
project's `include/`. Each project's `.gitignore` lists
`include/e2e_keys.h`, so a stray copy is never committed.

The file has keys for five boards. A project that lists fewer boards in its
`network_config.h` must skip the keys for boards it doesn't know, rather
than refusing to start. The projects below do this, and log `Pair keys
compiled in: 1 in use, 3 for boards not listed or without a MAC`. The
unused keys are still compiled into the image.

## Commands

```sh
python3 generate_keys.py           # add missing pairs; keeps every existing key
python3 generate_keys.py --check   # all 10 pairs present, both sides match, all distinct
python3 generate_keys.py --boards 8   # more boards; existing keys kept
python3 generate_keys.py --force   # new keys for everything: rebuild and reflash EVERY board of EVERY project
python3 find_keys.py ../*/.pio/build/*/firmware.bin
```

## Projects using it

| Project | Keys it uses |
| --- | --- |
| [handset_integration_test](../handset_integration_test/) | network + all pairs of its board (boards 1–5) |
| [mic_speaker_e2e_test](../mic_speaker_e2e_test/) | network + 1-2 |
| [mic_speaker_e2e_test2](../mic_speaker_e2e_test2/) | network + 1-2 |

Checked 2026-11-03: every environment of these projects builds, and
`find_keys.py` finds the network key plus exactly the 4 pair keys of that
board in each image.

The other key-using projects still have their own key files, gitignored
unless noted: `e2e_security_test`, `e2e_relay_test`, `e2e_pairing_test`,
`e2e_voice_test`, `e2e_opus_test`, `e2e_relay_voice_test` (committed), and
the CCMP and LoRa keys of `espnow_encrypt_test` and `lora_encrypt_test`.
Those projects keep their own keys for now.

**Broadcast is not encrypted** (the user's decision, 2026-11-03), so there is
no group key here. One-to-many voice works like `opus_broadcast_test`: in
the clear, and anyone on the channel can hear it. Unicast between two
boards stays end-to-end encrypted with the pair keys. `e2e_broadcast_test`
(a shared group key) remains as a finished experiment; its key is not
carried forward.

## Security

The file is public: anyone with this repository can derive every session
key and read every board's voice. This is a school project, and the user
asked for the keys to be committed. For a private deployment:

1. Run `--force`.
2. Keep the file out of git.
3. Rebuild and reflash every board of every project.

A lost board exposes its own 4 pairs and the shared network key, not the
other pairs.
