# Flipper RPC protobuf schema (vendored)

`*.proto` in this directory are mirrored verbatim from FoxFW2.0's
`assets/protobuf/` (the Flipper's own RPC schema - `flipper.proto` is the
top-level file, importing the other 8). They are the source of truth for
what the Flipper's Expansion-protocol RPC session can carry; FoxLAB's
`fox_rpc_bridge` on the ESP32 side never parses this (it just relays opaque
Data-frame bytes, see `Fox_ESP32_FW/fox_rpc_bridge.h`'s header comment) -
only the browser page needs the schema, to encode/decode protobuf messages
with vendored protobufjs.

`flipper-pb.json` is the compiled form: a protobufjs "static JSON
descriptor" (`pbjs -t json`), produced from these `.proto` files with
`protobufjs-cli`. The browser loads this JSON directly via
`protobuf.Root.fromJSON()` - no `.proto` text parsing happens at runtime,
which is why the vendored runtime (`../protobufjs/protobuf.min.js`) is the
"light" build (no parser, ~64 KiB minified) rather than the full ~200+ KiB
build the page's old CDN tag pointed at. `tools/gen_foxlab_page.js` inlines
both into the shipped page in place of `<!--VENDOR:flipper-pb-json-->` /
`<!--VENDOR:protobufjs-->` marker comments.

## Regenerating flipper-pb.json

If the Flipper's protobuf schema changes upstream (new/changed RPC
messages in FoxFW2.0), re-sync and recompile:

```sh
# from Fox_ESP32_FW/foxlab_src/vendor/proto/
cp ../../../../FoxFW2.0/assets/protobuf/*.proto .    # adjust path as needed
npm install protobufjs@7.4.0 protobufjs-cli@1.1.3 --no-save   # in a scratch dir
node_modules/.bin/pbjs -t json -p . -o flipper-pb.json flipper.proto
```

Then run `node tools/gen_foxlab_page.js` from the repo root to rebuild
`foxlab_page.h`. `flipper-pb.json` is kept pretty-printed here for
readability/diffing - the generator minifies it when inlining, so there's
no need to hand-minify this copy.
