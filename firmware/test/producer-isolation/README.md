# producer-isolation and stack-bound: the post-link walk

[`aius_walk.py`](aius_walk.py) implements §Walk specification W1 to W6 of
[`docs/plans/aius-sample-rate-following.md`](../../../docs/plans/aius-sample-rate-following.md)
(Phase 4e) over a linked ELF, its kept relocations and its link map.
[`../stack-bound/stack_bound.py`](../stack-bound/stack_bound.py) charges
depth over the same graph.

- **W8 fixtures are linked by the suite**, never committed: [`fixture_build.py`](fixture_build.py)
  compiles `fixtures/*.c` with the PlatformIO `arm-none-eabi` toolchain
  (or `$AIUS_ARM_TOOLCHAIN`) against [`fixtures/two_region.ld`](fixtures/two_region.ld)
  into a temporary directory (`$AIUS_WALK_FIXTURE_DIR` keeps it).
- **The optical-image cases skip until the link carries its artifacts**:
  `firmware.map` (`-Wl,-Map`), relocation sections (`-Wl,--emit-relocs`),
  `.d` files (`-MMD`) and `.su` files (`-fstack-usage`), all parent-wiring's
  `platformio.ini` seam. Tables: [`tables_optical.py`](tables_optical.py),
  [`../stack-bound/tables_stack.py`](../stack-bound/tables_stack.py).
- **Needs** `python3` with `pyelftools` (`elftools`); not `python3 -I`,
  which hides the user site-packages.

## Invocation: for `firmware/scripts/test.sh`

```bash
python3 -m unittest discover -s "$fw_root/test/producer-isolation" -p 'test_*.py'
python3 -m unittest discover -s "$fw_root/test/stack-bound" -p 'test_*.py'
```

`AIUS_PIO_BUILD` (default `firmware/.pio/build`), `AIUS_ISOLATION_ENVS`
(default `teensy41-optical teensy41-optical-slot-a`) and `AIUS_STACK_ENV`
(default `teensy41-optical-slot-a`) select the images.
