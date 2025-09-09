# Threat Model

This document outlines potential threats to the runtime and mitigation
strategies.

- **Plugin misuse**: Untrusted plugins could attempt to exploit the process.
  Only load plugins from trusted sources and verify ABI versions.
- **Resource exhaustion**: Plugins may allocate excessive memory or CPU. Use
  runtime limits and monitoring to detect and contain abuses.
- **Denial of Service**: Malformed inputs may trigger unexpected states.
  Validate inputs and employ watchdog timers.
