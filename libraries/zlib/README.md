# zlib 1.3.2 (vendored, unmodified)

The subset of zlib that `baseLogger/Helpers/gzip_body.h` links: raw deflate, raw inflate, CRC-32 and
the Adler-32 that deflate and inflate reference even for raw streams. Nothing here is edited; the
gzip file-I/O sources (`gz*.c`), examples and build files are left out.

- Source: <https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz>
- SHA-256 of the tarball: `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`, the same on
  <https://zlib.net/> and the GitHub release.
- Signature: `zlib-1.3.2.tar.gz.asc`, good signature from Mark Adler,
  key `5ED4 6A67 21D3 6558 7791 E2AA 783F CD8E 58BC AFBA`.
- Licence: zlib, see [`LICENSE`](LICENSE).

`SHA256SUMS` lists every file copied from the tarball; `tests/run_gzip_container_checks.py` fails if a
file drifts from it or an unlisted source appears. To upgrade, replace the files from a verified tarball,
regenerate `SHA256SUMS`, and update the version checks in `gzip_body.h`.

`library.json` builds it with `-DNO_GZIP` (we write the gzip container ourselves) and repeats
`-mfix-esp32-psram-cache-issue`: the compressor state lives in PSRAM.
