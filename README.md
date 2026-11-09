# SFTV TX: Slow Flipper TV for Flipper Zero

SFTV TX sends pictures, notes and whole Flipper animations as near-ultrasound (17–21 kHz) from
the Flipper's speaker. The receiver is [SFTV RX](https://github.com/Negenii/sftv-playdate) on a
Playdate. No pairing, no radio: the receiver just listens.

A picture is 400×240, 1 bit, and takes from about a minute (line art) to six (dithered photos).
An animation goes over compressed, frame by frame, and the receiver plays it as it arrives.

## Using it

Apps → Tools → SFTV TX.

- Image sends a saved picture or any JPEG, PNG or BMP from the SD card. Six pictures come
  with the app. Left on a picture's preview deletes it.
- Animation lists the dolphin animations on the Flipper (`/ext/dolphin` and asset packs).
  Left/Right browse, Up/Down jump by ten.
- Note sends a short text, typed on the Flipper or taken from a `.txt` on the SD card.
- Options → Calibrate shows the signal quality at the receiver: aim for 80 % or more.

Keep the Flipper's speaker 5–15 cm from the receiver's microphone. While sending, Back pauses,
OK goes on, Back again stops.

## Building

The app is built for [Momentum](https://momentum-fw.dev) firmware with `ufbt`:

    ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel release
    tools/flipper_build.sh launch    # build, install and start on a connected Flipper

`tools/flipper_build.sh` bumps the build number in `flipper/BUILD` (shown in Options → About);
plain `cd flipper && ufbt launch` works too. Host tests of the shared protocol code:
`cd tests && make test`.

Layout: `flipper/` the app, `common/` the protocol in portable C (shared with SFTV RX),
`tests/` host tests and WAV tools, `tools/` picture generators, `images/` sample pictures,
`docs/` design notes.

## Protocol

8-FSK, 17.0–20.5 kHz, 3 ms symbols, synced by a tone pattern. 45-byte frames with
Reed-Solomon parity, interleaved in blocks of four, plus an outer Reed-Solomon code across
groups of 108 frames, so a lost frame comes back without a resend. Pictures go raw or with a
lossless line codec, animations with an adaptive binary range coder. The details are in
`docs/` and `common/sstv_params.h`; transmitter and receiver must run the same protocol
revision (the major version, 3.x here).

## Credits and licenses

© 2026 negenii. Released under the GNU General Public License v3.0 (see `LICENSE`).

- JPEG decoding: TJpgDec by ChaN (its own permissive license, in the source).
- PNG decoding: uzlib by Paul Sokolovsky and contributors (zlib license, `flipper/lib/imgdec/LICENSE.uzlib`).
- The dolphin animations the app sends are the ones already on your Flipper; the dolphin art
  in the app comes from the Flipper Zero firmware (GPL-3.0).

If you like it, you can [buy me a coffee](https://buymeacoffee.com/low.effort).
