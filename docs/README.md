# Documentation

## Using the library

| Guide | Subject |
| --- | --- |
| [Shared handles](SHARED.md) | Mounted owner, file objects, positions, leases, and lifetime |
| [API and sector contract](API.md) | Mount, providers, snapshot compatibility, and status codes |
| [Build](BUILD.md) | Core toolchain, object/library outputs, and verification |
| [Linking](LINKING.md) | Function selection and consumer replacements |
| [Streams](STREAMING.md) | Reusable cluster maps and bounded range reads |
| [Ordering](ORDERING.md) | Staged dependency phases and explicit physical commit |
| [Checking](CHECKING.md) | Scoped diagnostics and checked/adaptive reads |
| [Recovery](RECOVERY.md) | Candidate FAT/BPB views and bounded read-only salvage |
| [Policy](POLICY.md) | Optional retained findings and caller-selected restrictions |
| [Formatting](FORMATTING.md) | Geometry planning, formatting, and verification |
| [Large clusters](CLUSTER64.md) | Representation, supported sizes, and consistent views |

## Developing and validating

- [Implementation](DEVELOPING.md): source structure, register policy, and invariants.
- [Validation](VALIDATION.md): measured coverage, code sizes, and limits.
- [Examples](../example/README.md): integration code and its guides.
- [Tests](../tests/README.md): fixtures and platform harnesses.

Public assembly declarations remain beside the library in the repository root.
Use their parameter contracts together with the feature guides.
