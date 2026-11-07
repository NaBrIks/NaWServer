# Showcase UI

UI source is the shared root frontend/ (owner decision 2026-10-20). Follow its
AGENTS.md; local frontend/ holds only the project profile, commands and tests.

Read and apply [docs/ui-principles.md](docs/ui-principles.md) before any UI change.
It contains the owner's full mandatory UI standard. Keep related working data
visible together, stable numeric columns, minimal row height, local Tabler icons,
centralized styling and real browser verification. Also follow the repository's
root AGENTS.md and C3-only build/identity/flash boundaries.

The task inspector is a persistent right dock with independent scrolling and
bounded, saved width. Keep CPU and memory scales fixed; never use recent extrema
for either full charts or sparklines. Never revert tables to row cards on narrow
screens. Preserve horizontal scrolling when the table cannot fit.
