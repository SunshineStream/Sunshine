# Sunshine #5942: Windows AMD64 diagnostic A/B

This is a **source/build kit, not a pair of compiled builds**. Neither Windows runtime behavior nor the persistence hypothesis has been validated. Do not distribute or use it as a fix.

## Build inputs

- Both variants start at Sunshine beta `v2026.1118.30014`, commit `37ee7d549391938ab5dc468783fc6dadc23dbc7e`.
- Both use the commit's recursive submodule pins, including libdisplaydevice `a2afc0430d44223c25a2ebc598000f78b338aa52`.
- A uses the released source (`save_to_database = false`). B applies `persistence.patch` (`true`). No other source file changes. Both are built in the same checkout, build directory, and toolchain.
- This tests the constructor policy as a whole. With this pin, B uses a different topology API branch (the remembered-topology route with a saved-config fallback) as well as adding persistence flags for mode/primary changes. If B works, that does **not** prove that Windows reapplied its saved layout, nor isolate a single Windows API flag.

## Build both portable ZIPs

Prerequisites: a Windows AMD64 machine with MSYS2 UCRT64, Git, Ninja, unzip, and the toolchain/dependencies from the pinned [Windows CI workflow](https://github.com/SunshineStream/Sunshine/blob/37ee7d549391938ab5dc468783fc6dadc23dbc7e/.github/workflows/ci-windows.yml). Use native Windows Node.js LTS, Python 3.14, and uv (MSYS2 Node is incompatible with the web build); ensure its directory is on the UCRT64 PATH. Prepare the dependencies once before running the script. Python tools, if required, belong in a virtual environment, following the checkout's locked uv guidance. Use a fresh, short, local build path to avoid Windows path-length limits. The helper neither changes system packages nor installs/runs Sunshine.

From a Windows terminal, with this kit as the current directory:

```cmd
C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "bash ./build-ab.sh"
```

If native Node is not inherited, add the trusted Node installation directory to PATH in the UCRT64 shell before invoking the script. The script verifies the tag and submodules, builds and ZIP-tests A and B, records build inputs and logs, hashes the archives and their Sunshine executables, and rejects identical executable hashes. The native dependency manifest and logs are traceability, not a promise of bit-for-bit reproduction of the official release. The output ZIPs are unsigned diagnostic artifacts, not installers. Do not distribute them without authorization.

## Test protocol

1. Test only on a machine with local console access and a working physical monitor. Don't use an unattended or remote-only host. Keep the normal Sunshine version/config available and back up its config and display state. Record physical/VDD, primary, resolution, refresh and HDR before testing; verify the Windows console can restore a known-good layout manually. B can persist layout across sessions and could regress the normal dock/undock behavior (#5625).
2. Extract A and B to separate directories. Use equivalent copies of the original Sunshine configuration and app settings, each with separate runtime/display-state files, and confirm from startup logs that the intended configuration is actually loaded and state is separate. Keep client/target, graphics driver, adapter, timing, and Windows settings unchanged. Run only a single Sunshine instance; verify that the installed service or a session-switch restart does not substitute the normal binary for the intended variant. If the portable launch method changes the original reproduction, stop and preserve the build that reproduces it rather than drawing a negative conclusion.
3. Reproduce the same RDP-to-console transition and launch delay from the original report. For A, then B, preferably then A again, run at least two cycles. Restore the same baseline topology and saved Windows layout between cycles; simply switching binaries is insufficient because B can alter persistent Windows state. Abort if the normal layout does not restore cleanly, or if consistent starting state cannot be verified. Don't erase Windows display settings or registry state.
4. Record actual Windows topology and Sunshine's selected output, mode and HDR after probe/start, during stream, after disconnect, and after the next reconnect. Expected failure pattern is 1080p SDR/physical instead of 3840×2160 HDR/VDD. Save only redacted debug logs (from console switch/startup through disconnect), package/hash and version, and observations; do not post complete config/state files or credentials. Test display-already-awake as a control where feasible, and keep the original issue's display-off/TV-wake trigger as a separate test, not a proven equivalent.
5. Stop the diagnostic build and restore the known-good display layout from the local console. Resume the normal Sunshine binary and configuration and confirm normal capture/reconnect. If A reproduced and B didn't, this supports the save-enabled policy as the relevant difference for that setup; it doesn't establish a safe production change. If both pass, the setup failed to reproduce the regression; if both fail, persistence alone isn't sufficient.

Reporter: [#5942 exact stable/beta comparison](https://github.com/SunshineStream/Sunshine/issues/5942#issuecomment-6999485693).
