# Runtime Profile Fixtures

`example-linux.json` is a schema-valid RTFW 1.1+ profile used to demonstrate the
installed profile shape. It is consumed by the strict C++ runtime-profile
loader, but it is not an autotune recommendation, latency result, hardware
qualification record, or claim about every Linux host.

Create deployment candidates with `tools/autotune/make_config.py` or
`tools/autotune/install_profile.py`, then validate them against the real
embedding workload. See
[the runtime profile contract](../docs/runtime_profiles.md).
