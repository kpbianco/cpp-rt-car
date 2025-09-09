# Config Hot Reload

This module provides a tiny hot-reload mechanism for the `core::Config` structure.
Configuration is stored on disk in a simple textual format:

```
<major> <minor>
```

`ConfigHotReloader` watches the file's modification time and atomically swaps in
new configuration objects when it changes.  Basic validation ensures the major
version matches the previously loaded configuration.  The hot-reload wrapper is
header-only and has no external dependencies.
