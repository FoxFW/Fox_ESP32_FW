# protobufjs (vendored, no CDN)

FoxHUB's AP has no internet passthrough, so the page's old
`<script src="https://cdnjs.cloudflare.com/.../protobufjs/7.4.0/protobuf.min.js">`
tag (still present in the not-yet-rewritten `foxlab_src/foxfw-lab.html` as
of this commit - see task #7 in the FOXLAB_OPTION_B_PLAN project doc)
can't load once a browser joins FoxHUB. `protobuf.min.js` here is that same
dependency, vendored locally instead.

- Version: 7.4.0 (matches the CDN version the page already referenced, so
  the API surface `foxlab_src/foxfw-lab.html`'s JS expects doesn't change).
- Flavor: **light** (`protobufjs/dist/light/protobuf.min.js`, ~64 KiB
  minified) rather than the full build (~76 KiB) - the light build omits
  the runtime `.proto`-text parser, which isn't needed here since the
  schema ships precompiled as `../proto/flipper-pb.json` and is loaded via
  `protobuf.Root.fromJSON()`. Still has full reflection (`lookupType`,
  `.encode()`/`.decode()`/`.verify()`), just not `protobuf.load()`/`.parse()`.
- License: BSD-3-Clause, see `LICENSE.txt` (copied from the npm package).
- Provenance: installed via `npm install protobufjs@7.4.0 --no-save` and
  copied from `node_modules/protobufjs/dist/light/protobuf.min.js`.
  sha256: 1da001f4c01a5cd050ebca0668bcb4541b44b57f944ad4661b8a4ccaaf29958b

`tools/gen_foxlab_page.js` inlines this file's contents into the shipped
page in place of the `<!--VENDOR:protobufjs-->` marker comment.
