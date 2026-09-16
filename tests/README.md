# Tests

Platform harnesses live in separate subdirectories. [win32/](win32/README.md)
contains the current Windows harness, including its provider, staging buffer,
ABI probes, suite modules, and build rules. Library builds do not select it.

The Python tools here generate disk images and independently inspect their
contents. Their formats and expectations can be reused by additional harnesses.
Host executables and generated images belong under ignored `build/` storage.

| Tool | Purpose |
| --- | --- |
| `images.py` | Deterministic clean and damaged FAT32 volume images |
| `check-images.py` | Manifest/hash checks and library observation profiles |
| `commit-images.py` | Materialize production commit traces and interrupted states |
| `format-images.py`, `check-format-images.py` | Formatter output and independent raw verification |
| `check-boot-images.py` | Explicit backup-BPB candidate views |

From the repository root, after building the selected harness:

```cmd
py tests\images.py --output build\images
py tests\check-images.py build\images\manifest.json
```

`check-images.py --reader <executable>` selects an alternate image reader. Its
arguments are image path, sector bytes, cluster bytes, fault label, and optional
profile (`shared`, `checked`, `views`, `stream`, or `range`; omission selects the
snapshot profile). It returns the JSON observations defined by the fixture
checks. The default is `build/win32/imagecheck.exe`.

Add another platform under `tests/<platform>/`, with its own provider and build
entry point. Reuse image manifests and expected observations; keep platform APIs
out of the public library declarations. The current C suite modules share the
Windows fixture driver and are part of that harness, not a portable test API.

The trace-driven tools currently default to the Windows suite executable.
An optional non-repair fsck comparison uses explicitly supplied tooling; the
independent raw checks do not require it. [Validation](../docs/VALIDATION.md)
distinguishes synthetic tests, recorded physical tests, and untested firmware.
