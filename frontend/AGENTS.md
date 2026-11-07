# Shared demo frontend

Read and apply ui-principles.md before any UI change. Follow root AGENTS.md.
This is the single Vue/TypeScript frontend for the maintained demo projects, based on the
current S3 UI approved by the owner on 2026-10-20. Do not create per-demo copies
of shared components, styles, state, help, transports, icons or package locks.

CBOR uses direct UTF-8 field names, never a numeric key registry. New names must
not require registration. Follow docs/readme_cbor_requirements_ru.md at repo root.

Demo differences belong to examples/<project>/frontend/project.ts and optional
extension components/types/styles in that directory. Keep physical pin mappings,
LCD/touch details and product pages out of shared sources. Read actual device
state for GPIO and capabilities. Separate appearance storage by project profile.

Install once with npm ci at repository root. npm run build there checks and builds
all maintained profiles. Output stays in each demo's frontend/dist for its existing host
and firmware packer. Browser verification and firmware build/flash are separate.
