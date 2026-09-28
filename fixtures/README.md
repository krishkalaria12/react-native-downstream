# Streaming fixtures

Each `.md` file is the exact canonical source. `manifest.json` gives an ordered
array of chunks whose UTF-8 bytes concatenate to that file. A checkpoint's
`afterChunk` is one-based, and its `prefix` is the complete source received by
then. Playback must preserve these boundaries, including the split CRLF in
`unicode-crlf.md`.

The paired `.expected.html` file is the HTML fragment from a fresh md4c 0.6.0
`md2html --gfm` parse of the **finished source**. It is a parser oracle, not
the native display or a safe HTML renderer. In particular, md4c emits raw
`<b>`, `<img>`, and a `javascript:` link in three fixtures. The first release
displays raw HTML as text, images as alt text without fetching them, and unsafe
links as plain labels. Math stays literal text.

`expectedPreview` records the visible policy at selected chunk boundaries.
`visibleText` is the text a user sees, with block boundaries represented by
`\n`, CRLF normalized to `\n`, and a terminal source newline omitted. Keys
other than `visibleText` describe the expected block, style, or allowed action.
An absent key leaves that part of the preview unspecified. `enabledLinks` and
`imageRequests` are complete lists at that checkpoint, not deltas. Temporary
styles and block classification may change as more chunks arrive. They never
change the canonical source or the final parse result.

To rebuild the oracle binary and validate the files:

```sh
git clone https://github.com/mity/md4c.git /tmp/md4c-fixtures
git -C /tmp/md4c-fixtures checkout 902b1bc9172ab45a7ea2539c9db4496ead77daa8
cd /tmp/md4c-fixtures
cc -O2 -DMD_VERSION_MAJOR=0 -DMD_VERSION_MINOR=6 -DMD_VERSION_RELEASE=0 -Isrc \
  src/md4c.c src/md4c-html.c src/entity.c md2html/md2html.c md2html/cmdline.c \
  -o /tmp/md2html-fixtures
cd -
python3 fixtures/validate.py --md2html /tmp/md2html-fixtures
```

The validator checks every chunk and checkpoint against the source, then
compares each expected HTML file byte for byte with a fresh parser run.
