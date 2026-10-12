# Retired hardware support

This directory preserves the retired board's build configuration, bench programs,
peripheral implementations, KiCad project, wiring documents and measurements.
Paths mirror the former project layout. The original generated CubeMX export is
in `../CubeMX`; the former application is in `../application`.

Nothing here is selected by the active build. The current H755 target has separate
IO/M4 and UI/M7 firmware. Restore and adapt individual peripherals deliberately;
these old pin assignments, timer clocks and linker layouts are not an H755 port.
Shared device algorithms and Linux regression tests remain in the active project.
Component datasheets remain in the repository's root `docs/` directory.
