# Compact production-style demo

Read the inherited repository AGENTS.md and [ui-principles.md](ui-principles.md).
Keep this demo independent from the large diagnostic showcases. One API key,
two status streams, small settings, native streamed A/B firmware updates. Keep
platform code below this directory and reuse the canonical library unchanged.
Do not copy the Vue frontend, install dependencies implicitly or format storage.

Owner update 2026-11-03: S3 firmware installation is authorized for this task.
Identify the board and write ranges; retain every existing FS and NVS byte and
preserve their partition bounds. Do not format or replace the shared FS image.
Identify C3/RP before programming. First USB installation
of the A/B table is distinct from routine HTTP OTA. Preserve current NVS/FS
addresses and unrelated files. Pico uses only the dedicated `.ota.uf2` artifact;
the SDK's ordinary UF2 can contain an absolute block outside the image slots.
Secrets, generated output, dumps and measurements belong in ignored directories.
Do not commit credentials or the installed device keys.
