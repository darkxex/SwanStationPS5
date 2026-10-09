# Contributing

Bug reports, fixes and testing are all welcome.

**Reporting a bug**

- Include your firmware, how you loaded etaHEN and kstuff, and the game (its name and serial, for example `SLUS-00594`).
- Attach `/data/SwanStationPS5/logs/SwanStationPS5.log`. After a crash, the log has a `crash:` section with addresses. Those map to functions with the `SwanStationPS5-symbols` artifact of the same build.

**Changing the code**

- Every push is built by GitHub Actions. A change should build there: the PS5 app and the desktop compile check.
- Keep the SPDX header (`GPL-3.0-or-later`) on new source files.
- Never commit BIOS files, games, keys, `.env`, `build/` or `dist/`.
- Release tags are `vMAJOR.MINOR.PATCH`, matching `SwanStationPS5_VERSION` in `src/SwanStationPS5.h`.
