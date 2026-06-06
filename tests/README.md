# Tests

Emulation and regression tests for delugemu.

## `smoke.sh`

The M0 acceptance check. After building (`scripts/build.sh`) it verifies that:

1. `qemu-system-arm` was produced, and
2. the `deluge` machine type is registered and can be instantiated.

```sh
./scripts/bootstrap.sh   # once
./scripts/integrate.sh   # once (re-run after adding/removing source files)
./scripts/build.sh
./tests/smoke.sh
```

It deliberately does **not** boot firmware yet — that arrives with M1 in
[../docs/roadmap.md](../docs/roadmap.md), at which point firmware-boot and
output-assertion tests will be added here.

## Adding tests

Keep tests runnable standalone and CI-friendly: exit non-zero on failure, avoid
interactive prompts, and don't depend on a real Deluge firmware image unless the
test explicitly fetches/locates one and skips cleanly when it is absent.
