# Files shared between krdp (server) and krdp-client

Some small, pure headers are copied by hand between this repo and the client repo
(`~/dev/krdp-client`) because the wire logic must be identical on both ends. There is no shared
submodule, so drift is checked by a script instead.

## The check

```bash
scripts/check-shared-headers.sh [path-to-krdp-client]   # or KRDP_CLIENT_SRC=...
```

Read-only. It normalises both sides (namespace `KRdp` / `KrdpClient` -> `@NS@`, the client's `core/` and
`wayland/` include prefixes dropped, the client's "Copy of ..." provenance comments dropped, trailing
whitespace ignored), prints a unified diff for every pair that differs, and exits 0 (equal), 1 (drift or a
listed file missing) or 77 (client repo absent, so a CTest registration can skip). The table of pairs, and
each pair's allow-list regex, is inside the script.

Fix a drift by making the edit in BOTH repos (the server copy is the original, the client copy is the one
with the provenance comment), not by widening the allow-list. Add a new copy by adding one row to the table.

Not yet registered with CTest. The two lines for `autotests/CMakeLists.txt`:

```cmake
add_test(NAME SharedHeadersDriftCheck COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-shared-headers.sh)
set_tests_properties(SharedHeadersDriftCheck PROPERTIES SKIP_RETURN_CODE 77)
```

(set `KRDP_CLIENT_SRC` in the test environment if the client checkout is not at `../krdp-client`).

## Checked pairs (server path -> client path)

| Server | Client |
|---|---|
| `src/ClipboardFile{Descriptor,Limits,Names,Pacing,Request,Serve,Staging}.h` | `src/core/` same names |
| `src/ClipboardHtml.h`, `src/ClipboardImage.h` | `src/core/` same names |
| `server/PngMimeData.h`, `server/SelectionReaderFd.h` | `src/core/` same names |
| `server/SelectionReader.{h,cpp}` | `src/wayland/` same names |
| `protocols/ext-data-control-v1.xml` | `protocols/` same name |
| `server/kwin/protocols/pointer-constraints-unstable-v1.xml` | `protocols/` same name |

The matching `autotests/*Test.cpp` files are NOT checked: several differ by more than namespace and includes
(for example `ClipboardFileNamesTest.cpp` by 21 lines), so a normalising diff would need per-test rules.

## Intentionally different (not checked)

These share a name or an ancestor but are separate implementations; do not "fix" them to match.

| Server | Client | Differing lines (2026-11-11) | Why |
|---|---|---|---|
| `src/CodecPolicy.h` | `src/core/CodecPolicy.h` | 1614 | server chooses encoder and link response; client decides what to request and decode |
| `src/CodecSelection.h` | `src/core/CodecSelection.h` | 534 | each side's own selection rules over the shared codec ids |
| `server/DeskActivity.h` | `src/core/DeskActivity.h` | 374 | server detects desk input; client handles the notices |
| `server/HostFiles.h` | `src/core/ClipboardLocalFiles.h` | 75 | same file walk, but the client version serves the local file manager's files for client-to-host copy (OPT-078 phase 3); the "keep the two in step" comment there is about the walk and reads, which are reviewed by hand |

Everything else with a common basename (`main.cpp`, `CMakeLists.txt`, `copyright`, `package-farside.sh`,
the `*Test.cpp` files above) is unrelated or out of scope.
