# Contributing to OBC-Zero

Thank you for considering a contribution. This project values evidence over
volume: a single well-documented failed test is worth more here than a large
untested feature.

## Ground rules

By contributing, you agree that your contribution is licensed under the
Apache License 2.0, as stated in `LICENSE`.

Do not contribute code, data, or documentation that is subject to export control
(ITAR, EAR, EU dual-use regulation) or to a third-party confidentiality
agreement. If you are unsure whether something is restricted, do not submit it.

## Getting set up

Ubuntu 24.04 or equivalent:

```bash
sudo apt install gcc-riscv64-unknown-elf qemu-system-misc make python3-venv
python3 -m venv .venv && source .venv/bin/activate
pip install -e ./harness[dev]
make build && make test
```

If `make test` does not pass on a clean checkout, open an issue before doing
anything else.

## What is welcome

- Fault injectors that break the system in a way it does not currently survive.
- Recovery logic backed by a test that fails without it.
- Test reports, including negative results.
- Portability work toward other RV32 targets.
- Documentation and architecture decision records.

## What is not welcome

- Dynamic allocation, recursion, or unbounded loops in `flight/`.
- Dependencies added to the flight core. It stays freestanding.
- Large refactors submitted without prior discussion in an issue.
- Changes that weaken an assertion in order to make a test pass.
- Edits or deletions to existing files under `docs/reports/`.

## Coding standards

Flight code (`flight/`):

- C11, freestanding, no dynamic allocation, no recursion.
- Fixed-width integer types only.
- Every fallible function returns a status enum.
- Every return value is checked.
- Every loop has a provable bound.

Host code (`harness/`):

- Python 3.11+, type hints on public functions.
- `ruff check` and `ruff format` clean.
- Fault injectors are deterministic given a seed.

## Commits and pull requests

- Branch from `main` using `feat/`, `fix/`, `test/`, or `docs/`.
- Conventional commits, imperative mood, subject under 72 characters.
- One logical change per commit.
- Rebase rather than merge when updating a branch.

A pull request should state:

1. What problem it solves.
2. How it was tested, with the command used and the seed if applicable.
3. Which rule in this document it might sit close to, if any.

Pull requests touching `flight/core/` must include a fault injection case.

## Reporting a fault-tolerance defect

If you find a fault the system does not survive, open an issue with:

- The commit hash.
- The exact command and seed used to reproduce it.
- The captured serial output.
- The observed recovery behaviour versus the expected one.

Reproducible fault reports are the most valuable contribution to this project.

## Architecture decision records

Non-obvious technical choices are recorded in `docs/adr/` as numbered files.
ADRs are immutable once merged. To change a decision, add a new ADR that
supersedes the old one and update the old one's status line only.

## Security

Do not open a public issue for a vulnerability in the recovery or command
validation path. Contact the maintainer directly.

## Code of conduct

Be precise, be direct, and assume competence. Disagreement about engineering is
expected and welcome. Personal hostility is not.
