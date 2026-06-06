# Contributing to delugemu

Thanks for your interest in improving the Deluge emulator.

## Ground rules

- **License.** All contributions are GPL-2.0-or-later. New source files must
  carry an SPDX header: `// SPDX-License-Identifier: GPL-2.0-or-later`.
- **Coding style.** Device models follow the
  [QEMU coding style](https://www.qemu.org/docs/master/devel/style.html):
  4-space indent, no tabs, 80-column soft limit, `CamelCase` for QOM types and
  `snake_case` for functions. Run `clang-format` (config in `.clang-format`).
- **Keep upstream clean.** Do not commit changes inside `qemu/`. All custom code
  lives in `src/`. Integration with the QEMU tree happens via
  `scripts/integrate.sh`, which symlinks our files in and patches the build.

## Workflow

```sh
./scripts/bootstrap.sh    # one-time: fetch the QEMU submodule
./scripts/integrate.sh    # link src/ into qemu/ and configure the build
./scripts/build.sh        # build qemu-system-arm
./scripts/run.sh fw.elf   # smoke-test
```

## Adding a new peripheral

1. Add the header under `src/include/hw/<category>/<device>.h`.
2. Add the implementation under `src/hw/<category>/<device>.c`.
3. Register the source file in `src/meson.build`.
4. Wire it into the SoC (`src/hw/arm/rza1l_soc.c`) or board
   (`src/hw/arm/deluge.c`) as appropriate.
5. Document its registers/behaviour in `docs/hardware.md` and update
   `docs/memory-map.md` if it occupies address space.

## Commit messages

Use `area: short summary` subjects, e.g. `hw/misc: model PIC button matrix`.
Keep the body wrapped at 72 columns and explain *why*, not just *what*.
