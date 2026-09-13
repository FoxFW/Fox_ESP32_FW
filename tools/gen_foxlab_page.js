#!/usr/bin/env node
// Builds foxlab_page.h from foxlab_src/foxfw-lab.html (and, since the Paint
// tab, foxlab_src/wallpaper_painter.html too).
//
// The deployed page has to be a single self-contained file (FoxLAB's
// WebServer serves ONE PROGMEM string, and FoxHUB's AP has no internet
// passthrough for a CDN or extra <script src> routes to work) - but the
// *source* is easier to review and edit as separate pieces. This script
// bridges the two: it wraps foxfw-lab.html as the FOXLAB_HTML PROGMEM
// string that fox_lab.cpp's handleLabRoot() serves.
//
// inlineVendorAssets() below still knows how to splice a vendored asset in
// place of a "<!--VENDOR:name-->" marker comment, a leftover from when the
// page needed the vendored protobufjs runtime + a compiled Flipper RPC
// schema for its old protobuf-over-WebSocket transport (see foxlab_src/
// vendor/ - now unreferenced by the page itself, not deleted). The FLPR
// text-line protocol that replaced it (see FlipperRemote in
// foxfw-lab.html, and foxr_companion.c on the Flipper side) needs no
// vendored assets at all, so foxfw-lab.html currently has zero VENDOR
// markers in it - that's fine, this script only requires an all-or-
// nothing match (see the partial-substitution check below), and 0-of-N is
// as "nothing missing" as N-of-N. Kept rather than ripped out in case a
// future asset ever needs the same treatment.
//
// wallpaper_painter.html is a separate case: it's a deliberately-forked,
// FoxLAB-specific copy of FOX_WEB's wallpaper-painter.html (see its own
// file-top comment for what's different and why - short version: no
// protobufjs/Web-Serial, it talks to the Flipper through the Lab page's
// already-open FLPR relay instead), vendored here rather than pulled from
// the FOX_WEB repo at build time since the two repos don't share a build
// pipeline. Keeping it in sync with FOX_WEB's original when that changes
// is a manual, deliberate step, not automatic.
//
// Usage: node tools/gen_foxlab_page.js
// (run from the Fox_ESP32_FW repo root, or anywhere - paths below are
// resolved relative to this script's own location)

'use strict';
const fs = require('fs');
const path = require('path');

const REPO_ROOT = path.resolve(__dirname, '..');
const SRC_HTML = path.join(REPO_ROOT, 'foxlab_src', 'foxfw-lab.html');
const SRC_PAINTER_HTML = path.join(REPO_ROOT, 'foxlab_src', 'wallpaper_painter.html');
const VENDOR_PBJS = path.join(REPO_ROOT, 'foxlab_src', 'vendor', 'protobufjs', 'protobuf.min.js');
const VENDOR_SCHEMA = path.join(REPO_ROOT, 'foxlab_src', 'vendor', 'proto', 'flipper-pb.json');
const OUT_HEADER = path.join(REPO_ROOT, 'foxlab_page.h');

// C++ raw string delimiters (the d-char-sequence between R" and the first
// "(") are capped at 16 characters by the standard - go over and the
// compiler doesn't recognize R"...(" as a raw string literal at all, so
// everything between the intended open/close markers gets parsed as real
// C++ source instead of a string (a wall of "stray '#'"/"missing
// terminating character"/"does not name a type" errors, one per line of
// HTML/CSS/JS). Keep both of these at 16 characters or fewer.
const RAW_STRING_DELIM = 'FOXLABPAGE';
const CLOSE_SEQUENCE = `)${RAW_STRING_DELIM}"`;
const PAINTER_RAW_STRING_DELIM = 'FOXLABPAINTER';
const PAINTER_CLOSE_SEQUENCE = `)${PAINTER_RAW_STRING_DELIM}"`;

function readText(p) {
    if (!fs.existsSync(p)) {
        throw new Error(`missing required source file: ${p}`);
    }
    return fs.readFileSync(p, 'utf8');
}

function inlineVendorAssets(html) {
    const markers = {
        '<!--VENDOR:protobufjs-->': () => {
            const js = readText(VENDOR_PBJS);
            return `<script>\n${js}\n</script>`;
        },
        '<!--VENDOR:flipper-pb-json-->': () => {
            // Stored pretty-printed in the repo for readability/diffing;
            // minify it here purely to save flash - same content either way.
            const json = JSON.parse(readText(VENDOR_SCHEMA));
            return `<script>\nconst FLIPPER_PB_JSON = ${JSON.stringify(json)};\n</script>`;
        },
    };

    let out = html;
    let substitutions = 0;
    for (const [marker, build] of Object.entries(markers)) {
        if (out.includes(marker)) {
            out = out.split(marker).join(build());
            substitutions++;
        }
    }
    return { html: out, substitutions, totalMarkers: Object.keys(markers).length };
}

function main() {
    for (const delim of [RAW_STRING_DELIM, PAINTER_RAW_STRING_DELIM]) {
        if (delim.length > 16) {
            // See the comment above these constants - a delimiter this long
            // silently produces a header that isn't a valid C++ raw string
            // at all, not a script error, so this has to be caught here
            // rather than left to show up as a wall of compiler errors.
            throw new Error(
                `raw-string delimiter ${JSON.stringify(delim)} is ${delim.length} characters - ` +
                `C++ caps raw-string delimiters at 16, pick a shorter one`
            );
        }
    }

    const srcHtml = readText(SRC_HTML);

    if (srcHtml.includes(CLOSE_SEQUENCE)) {
        // Would prematurely terminate the C++ raw string literal below.
        throw new Error(
            `foxfw-lab.html contains the raw-string terminator sequence ${JSON.stringify(CLOSE_SEQUENCE)} - ` +
            `pick a different delimiter in this script and in foxlab_page.h`
        );
    }

    const { html, substitutions, totalMarkers } = inlineVendorAssets(srcHtml);

    if (substitutions > 0 && substitutions < totalMarkers) {
        // Partial substitution almost certainly means a marker was typo'd -
        // fail loudly rather than silently shipping a half-wired page.
        throw new Error(
            `only ${substitutions}/${totalMarkers} vendor markers were found/replaced in foxfw-lab.html - ` +
            `check the marker comments match exactly`
        );
    }

    const painterHtml = readText(SRC_PAINTER_HTML);
    if (painterHtml.includes(PAINTER_CLOSE_SEQUENCE)) {
        throw new Error(
            `wallpaper_painter.html contains the raw-string terminator sequence ${JSON.stringify(PAINTER_CLOSE_SEQUENCE)} - ` +
            `pick a different delimiter in this script and in foxlab_page.h`
        );
    }

    const header = `#pragma once

// foxfw-lab.html and foxlab_src/wallpaper_painter.html, compiled from
// foxlab_src/ and embedded verbatim (see FoxFW2.0's FoxLAB Flipper app for
// the toggle that starts/stops the FoxHUB AP + this page's web server).
//
// DO NOT EDIT THIS FILE BY HAND - it is generated. Edit
// foxlab_src/foxfw-lab.html and/or foxlab_src/wallpaper_painter.html and
// regenerate with:
//   node tools/gen_foxlab_page.js
const char FOXLAB_HTML[] PROGMEM = R"${RAW_STRING_DELIM}(
${html}
)${RAW_STRING_DELIM}";

// Served at GET /wallpaper-painter.html (see fox_lab.cpp) - the Lab page's
// Paint tab iframe (and its "open in its own tab" link) both point at that
// same relative path already, so no change was needed on that side once
// this route existed to actually answer it instead of falling through to
// FOXLAB_HTML via onNotFound().
const char FOXLAB_PAINTER_HTML[] PROGMEM = R"${PAINTER_RAW_STRING_DELIM}(
${painterHtml}
)${PAINTER_RAW_STRING_DELIM}";
`;

    fs.writeFileSync(OUT_HEADER, header, 'utf8');
    const bytes = Buffer.byteLength(header, 'utf8');
    console.log(`wrote ${OUT_HEADER} (${bytes} bytes, ${(bytes / 1024).toFixed(1)} KiB)`);
    console.log(`vendor markers substituted: ${substitutions}/${totalMarkers}`);
}

main();
