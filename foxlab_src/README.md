# FoxLAB page source

Editable source for `foxlab_page.h` (the PROGMEM page FoxLAB's
`labStart()` serves over the FoxHUB WiFi AP - see `fox_lab.cpp`'s
`handleLabRoot()`). The deployed page has to be a single self-contained
HTML file (no CDN, no extra ESP32 routes/ports for separate JS assets -
FoxHUB's AP has no internet passthrough, and `foxlab_page.h` is one
PROGMEM string), but that's an awkward format to hand-edit directly, so
the actual source lives here instead and gets compiled into
`../foxlab_page.h` by `../tools/gen_foxlab_page.js`.

## Layout

- `foxfw-lab.html` - the page itself (HTML/CSS/JS, single file). Two
  marker comments in it get replaced at build time:
  - `<!--VENDOR:protobufjs-->` -> inlined `<script>` with the vendored
    protobufjs runtime (`vendor/protobufjs/`)
  - `<!--VENDOR:flipper-pb-json-->` -> inlined `<script>` defining
    `const FLIPPER_PB_JSON = {...}` from the compiled Flipper RPC schema
    (`vendor/proto/flipper-pb.json`)

  (As of this commit these markers aren't in the file yet - it still has
  its original `<script src="https://cdnjs.../protobuf.min.js">` +
  `<script src="assets/js/flipper-rpc.js">` tags from when the page ran
  over USB/Web Serial. Wiring in the markers + the new WebSocket-based
  Flipper RPC client is task #7 of the Option-B rewrite - see the
  `FOXLAB_OPTION_B_PLAN` project doc.)
- `vendor/protobufjs/` - vendored protobufjs runtime, see its own README.
- `vendor/proto/` - Flipper RPC `.proto` schema (mirrored from FoxFW2.0)
  + the compiled JSON descriptor, see its own README.

## Building

```sh
node tools/gen_foxlab_page.js
```

Run from anywhere (paths are resolved relative to the script, not the
cwd). Regenerates `../foxlab_page.h` from this directory's contents; the
generated file says as much at the top and shouldn't be hand-edited.
