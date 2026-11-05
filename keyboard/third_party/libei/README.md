# Pinned libei client

Client and shared utility sources from libei 1.6.0, MIT licensed. Upstream source
files are unchanged. The server, portal helper, tools, tests and documentation
are excluded. CMake builds the client statically for both host and ARM64, with
GNU/Linux configuration in `config.h`. Other application dependencies retain
their existing system linkage.
Upstream protocol assertions remain enabled in release builds.

- Upstream: https://gitlab.freedesktop.org/libinput/libei
- Tag: `1.6.0`
- Commit: `8a46bf3d4b6af7f25a61be53386cf618218114ef`
- Archive: https://gitlab.freedesktop.org/libinput/libei/-/archive/1.6.0/libei-1.6.0.tar.gz
- Archive SHA-256: `81b4b967f0e5938492434a04f7ea2e6c6d96d3e6c44700bc691821f82281e602`

The Frame's libei 1.4.1 dereferences an absent connection when an endpoint closes
during negotiation. Upstream excludes `EI_STATE_BACKEND` from connection cleanup
in 1.5.0 and later. Pinning 1.6.0 also matches the existing host protocol-test
baseline. This keeps the fix independent of SteamOS updates and avoids changing
libraries used by Steam or other apps. Native key state and socket reconnection
remain owned by `EiSink`.

`provenance.json` records hashes of the upstream files and generated snapshots.
`generated/` contains architecture-independent protocol stubs. To regenerate
them, install Python's Jinja2 module and run from this directory:

```sh
python3 proto/ei-scanner --component=brei --output=generated/brei-proto.h proto/protocol.xml src/brei-proto.h.tmpl
python3 proto/ei-scanner --component=ei --output=generated/ei-proto.h proto/protocol.xml src/ei-proto.h.tmpl
python3 proto/ei-scanner --component=ei --output=generated/ei-proto.c proto/protocol.xml '--jinja-extra-data={"headerfile":"ei-proto.h"}' src/ei-proto.c.tmpl
```

Normal builds use the checked-in snapshots and do not run the scanner.
