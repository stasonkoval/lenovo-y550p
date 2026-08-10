# Lenovo IdeaPad Y550P Linux utilities

Small, model-specific Linux utilities for keeping the Lenovo IdeaPad Y550P
usable on modern distributions.

## Components

- [`y550p-fan`](y550p-fan/) — a systemd-managed userspace fan controller that
  uses the Y550P's native embedded-controller command path and validates fan
  rotation with the hardware tachometer.

## Compatibility and safety

The fan controller has been verified on Lenovo machine type `20035`, Compal
NIWBA LA-5371P, BIOS `1ECN33WW(V5.07)`. It performs raw access to
model-specific embedded-controller registers and must not be used on unrelated
hardware. Read the component README before installing it.

## License

Licensed under the [MIT License](LICENSE).
