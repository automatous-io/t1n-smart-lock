# Contributing

**[README](../README.md)** > **Contributing** · [Report an issue](../../../issues/new)

Contributions are welcome. This is a solo-maintained project, so
response times vary, but every issue and PR gets read.

## What's in scope

The kinds of contributions most likely to land:

- Bug reports with reproducible serial logs
- Documentation fixes, clarifications, and typo catches
- Hardware compatibility reports for variants I haven't tested
  (different Sprinter year, region, or trim)
- Small, focused PRs that fix a specific issue

The kinds that are harder to land:

- Large refactors without a discussed-in-advance design
- Changes that weaken or remove the safeguards
  (door-open check, asymmetric pulse rule, CTM blackout windows)
- Architectural changes to the doors module or the CTM model

If you want to work on something larger, open an issue first to
talk through the approach. Saves both of us time.

## Reporting issues

Open a [GitHub issue](../../../issues/new) with:

- **Vehicle:** Year, make, and model.
- **Hardware:** T1N Smart Lock PCB revision (silkscreened on the
  board) and the firmware version you're running.
- **Ecosystem:** Apple Home, Google Home, Home Assistant, etc.,
  and the Thread Border Router(s) on your network.
- **What happened:** What you expected, what actually occurred,
  and steps to reproduce.
- **Logs:** Serial monitor output at 115200 baud, especially
  anything around the failure point.

If your device won't commission, won't boot, or behaves
unexpectedly after flashing, serial logs are usually the fastest
path to a diagnosis.

## Pull requests

Before opening a PR for anything beyond a typo or a small bug fix,
open an issue describing what you want to change and why. This
avoids merge conflicts with work in progress and saves you from
writing code that won't land for architectural reasons.

When the PR is ready:

- Keep it focused on one change. Multiple unrelated changes
  should be separate PRs.
- Match the existing code style and naming conventions.
- Include a brief test plan in the PR description: what hardware
  scenarios did you verify, and what was the outcome.
- Update relevant docs in the same PR if behavior changes.

## Contribution licensing

Contributions are accepted under the same license as the part of
the project they touch. Firmware contributions (anything in
`firmware/`) are provided under Apache 2.0, which follows from the
Apache 2.0 submission terms: unless you state otherwise, what you
submit for inclusion is licensed under Apache 2.0. Hardware and
enclosure contributions (anything in `hardware/` or `enclosure/`)
are provided under CC BY 4.0, the same license as those design
files. By submitting a contribution you confirm it is your own work
and that you have the right to submit it under these terms.

## Communication

GitHub issues and PRs are the only supported channels. Email and
DMs aren't monitored for project work.

## Code of conduct

Be straightforward, technical, and direct. Disagreement is fine;
hostility is not.

## Related documentation

- [README](../README.md) — project overview and quick start
- [FIRMWARE.md](FIRMWARE.md) — firmware architecture and behavior
- [HARDWARE.md](HARDWARE.md) — PCB design, BOM, and ordering
- [ENCLOSURE.md](ENCLOSURE.md) — 3D printed enclosure
- [BUILDING.md](BUILDING.md) — building and flashing the firmware
- [INSTALL.md](INSTALL.md) — commissioning and van installation
- [SAFETY.md](SAFETY.md) — electrical and operational safety
- [CERTIFICATION.md](CERTIFICATION.md) — Matter/Thread certification status
- [LICENSING.md](LICENSING.md) — license terms