# Security Policy

## Supported versions

| Version | Security fixes |
| --- | --- |
| 1.x | Supported |
| 0.x | Unsupported pre-release line |

Security fixes are released on the latest 1.x line. A fix that cannot be made
compatibly may require a new major version or immediate disabling of unsafe
behavior.

## Reporting

Do not publish exploit details, sensitive deployment data, credentials, device
paths, or proprietary workloads in a public issue. Use a private GitHub
Security Advisory for this repository when available. Otherwise contact the
repository owner privately through GitHub and provide a minimal description so
a private reporting channel can be established.

Include the affected RTFW version and commit, operating system/toolchain,
configuration, reproduction steps, impact, and whether untrusted input or
native callbacks are involved. Do not attach confidential hardware evidence to
a public report.

## Boundaries

RTFW is an in-process native library. It does not sandbox callbacks, plugins,
host job systems, CUDA contexts, XDMA devices, or caller-owned buffers. Only
trusted native code and explicitly authorized hardware should be attached.
Release checksums and SBOMs improve artifact inspection but are not code
signatures or proof of reproducible builds.
