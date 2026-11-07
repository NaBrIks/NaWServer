# Pico 2 W demo scope

The owner currently permits changes only inside this demo directory. Do not edit
NaWServer core sources, public headers, root CMake, shared frontend, or other demos.
Compile the canonical library sources unchanged. Keep experimental RP adapters
and compatibility glue here and record required library changes in compatibility.md.

Follow repository architecture/code style and the owner's UI principles at
`frontend/ui-principles.md`. Reuse the shared ESP Vue frontend read-only;
do not copy or fork its maintained HTML/CSS/JS. Generated assets belong in build/.
The full UI standard is retained in `frontend/ui-principles.md`; read and apply it.

No Python automation. Dependencies, builds, generated assets, secrets, and logs
stay inside this demo's ignored directories. No commit, push, or root edits.
Require an explicit, verified RP2350 USB serial; never flash the connected ESP32 boards.
No separate flash verify/readback or broad/load/soak testing for this task.
