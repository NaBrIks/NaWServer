# Touch LCD 7

Read and apply docs/ui-principles.md before changing the UI. Preserve the complete
TechnicalShell design copied from showcase: themes, density, fixed shell, task dock,
tables, local icons, help, notifications and RU/EN. Verify in a real browser.
The owner requested one shared frontend on 2026-10-20. Reuse root frontend/;
keep only the project profile, LCD page and display types here. Firmware remains independent. Preserve LVGL frame completion
and VSYNC ownership. Never enable C3 GPIO2/GPIO8 LED or strip drivers on this LCD.
Build with explicit -e s3. Do not flash while the owner has deferred connecting it.
