# Aius documentation

Three sections, each with its own index.

- [`reference/`](reference/) — what exists now. The signal path, firmware I/O,
  control protocols, the user interface, the board floorplan and the measured
  project scale. Reach for these to understand the system as built.
- [`plans/`](plans/) — design and work records, each carrying an implementation
  status. This is also where the open-work register and the authoritative defect
  ledger live, so it is the place to find out what is still unresolved.
- [`guides/`](guides/) — task how-tos for bench work.

Documents here carry YAML front-matter and are indexed by
`the repo tooling tool doc-index`. On a plan, `status` states how far the work has
got, not how trustworthy the document is; it is updated in the commit that ships
the work it describes.

Operational how-to for building, testing, flashing and the KiCad environment is
in [`CONTRIBUTING.md`](../CONTRIBUTING.md) rather than here.
