# Third-party notices

## JUCE 8

This project is built with **JUCE 8**, vendored under `JUCE/`.

JUCE is dual-licensed under the **GNU Affero General Public License v3.0** and a
commercial license. This repository is distributed under the AGPL-3.0, which is
one of the licenses offered for JUCE. If you distribute a modified version, the
AGPL obligations apply. If you need to distribute the application under
different terms, obtain a commercial JUCE licence from Raw Material Software.

<https://juce.com/legal/juce-8-licence/>

## SQLite

The library database uses the system SQLite library (`libsqlite3`), which is in
the public domain.

<https://www.sqlite.org/copyright.html>

## Test fixtures

The headless tests generate their audio fixtures with **FFmpeg** from synthetic
lavfi sources. FFmpeg is a build/test dependency only; it is not linked into or
distributed with the application.

## QR assets

The QR code images under `docs/network/` were generated with `qrencode`.
