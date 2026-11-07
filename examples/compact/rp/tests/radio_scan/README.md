# RAM-only radio isolation

This small Pico SDK diagnostic excludes NaWServer, FreeRTOS, TCP and flash writes.
It uses the same private `../../.private/wifi_config.h` only to count configured
SSID matches; it never prints SSIDs, passwords, BSSIDs or API keys.

Configure this directory into `../../build-radio` using the same installed SDK,
Arm compiler, Ninja, picotool and pioasm paths as the compact demo. The target is
`compact_radio_scan`, explicitly built with `pico_set_binary_type(... no_flash)`.
Before loading, `picotool info -a build-radio/compact_radio_scan.elf` must report
binary start `0x20000000`, not a flash address.

After placing the explicitly identified RP in BOOTSEL:

```
picotool load -x build-radio/compact_radio_scan.elf --ser <selected-RP-serial>
```

Open its USB console and send `s`. Three scans run; the middle pass specifically
probes the configured SSID. Output contains total AP count and configured-network
match count/channel/RSSI. The 12-second per-scan wait is bounded. Afterward send
`b` to return to BOOTSEL and run `picotool reboot --ser <selected-RP-serial>` to
return to the existing compact firmware. Loading this ELF does not replace the
partition table, either application slot, filesystem or NVM.

During the 2026-11-03 investigation, three unfiltered RAM-only scans found 5, 5
and 3 APs with zero configured-network matches. The later targeted follow-up's
serial capture did not complete, so no successful targeted-scan result is claimed.
