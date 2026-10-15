# Car Breaker 19 🚗🛡️

> Passive Honda Remote Keyless Entry (RKE) threat-detection companion for the Flipper Zero.

[![Flipper Zero](https://img.shields.io/badge/Flipper%20Zero-Compatible-green.svg)](https://flipperzero.one/)
[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](../../LICENSE)

## 🎯 Overview

Car Breaker 19 is a research-only, receive-only application that samples Honda RKE transmissions and flags risky behaviours such as Rolling-Pwn, RollBack, or static-code regressions. It shares the same UX philosophy as `uid_brute_smarter`: clear feedback, conservative defaults, and safeguards that keep the Flipper in passive sniffing mode at all times.

The 2025 research push also broadens our passive telemetry goals beyond Honda: Car Breaker 19 now parses Mini Cooper R56/R60 Hitag2 frames (CAS3/CAS3+) to surface UID/counter/entropy diagnostics without ever transmitting. See [`RESEARCH.md`](RESEARCH.md#11-passive-cryptographic-analysis-of-mini-cooper-r56-hitag2-systems) for the methodology implemented in the latest release.

> ⚠️ **IMPORTANT**: This tool exists solely to help owners and authorized researchers validate the security posture of vehicles they control. It does **not** transmit, jam, replay, or brute-force any signals and must never be used against systems without explicit, written authorization.

## ⚖️ Legal & Ethics

- Use only on vehicles you own or are contractually permitted to assess.
- Respect local RF regulations—Car Breaker 19 never transmits, but many jurisdictions still restrict passive capture without consent.
- Follow the disclosure guidelines in [`RESEARCH.md`](RESEARCH.md) when reporting findings to OEMs or regulators.
- Momentum Firmware and the authors disclaim liability for misuse; you accept all risk when deploying this app.

## ✨ Feature Highlights

### Passive Capture Stack
- Pre-loaded CC1101 presets (433.92 MHz & 315 MHz, wide/narrow) derived from Honda HND\_1 and HND\_2 register maps.
- Guided prompts ask for the target region (USA 315 MHz vs EU 433.92 MHz) and manufacturer profile (Honda legacy/modern or Mini CAS3) so the right preset is armed automatically.
- Watchdog timers and mutex-protected buffers prevent UI freezes while processing dense pulse trains.

### Detection Engines
- **Rolling-Pwn heuristic** – requires ≥5 unique frames within 2.5 s, each 3–14 ms long (≈40–160 Manchester bits) and waveform-consistent before raising an alert.
- **RollBack suspicion** – triggers when an identical frame reappears after ≥8 s, signalling counter reset or resynchronization abuse.
- **Static-code detection** – flags repeated hashes that are still within the nominal sliding window, indicating CVE-2022-27254-style fixed commands.
- **Mini Hitag2 entropy monitor** – decodes 104-bit Mini frames (UID, CNTRL, MAC) and scores their Shannon entropy so CAS3/CAS3+ owners can spot weak keystream generators or counter regressions with zero RF transmission.
- Status dialog shows the active preset, running frame count, and live alert state without interrupting capture.

### Safer UX
- Splash screen reminds users that this is a defensive research tool.
- Capture dialog is one button (`Stop`) to avoid accidental mode switches.
- Detection logic tolerates RF noise by rejecting frames outside expected Honda RKE timing envelopes before computing hashes.

## 🚀 Quick Start

### Prerequisites
- Flipper Zero running [Momentum Firmware](https://momentum-fw.dev/) (latest build recommended).
- GNU build tooling available via the `./fbt` helper script.
- This repository cloned into `Momentum-Firmware/applications_user/car_breaker19`.

### Install (direct launch)
```bash
git clone https://github.com/Next-Flip/Momentum-Firmware.git
cd Momentum-Firmware
git clone https://github.com/fbettag/car_breaker_19.git applications_user/car_breaker19
./fbt launch APPSRC=applications_user/car_breaker19
```

### Manual build (.fap artifact)
```bash
git clone https://github.com/Next-Flip/Momentum-Firmware.git
cd Momentum-Firmware
git clone https://github.com/fbettag/car_breaker_19.git applications_user/car_breaker19
./fbt fap_car_breaker19
# Copy dist/f7-C/apps/Research/car_breaker19.fap to /ext/apps/Research/ on the Flipper.
```

### Using the App
1. Open `Apps → Sub-GHz → Car Breaker 19`.
2. Select `Start Capture`, then answer the prompts for region (USA 315 MHz or EU 433.92 MHz) and manufacturer profile (Honda legacy/modern or Mini CAS3) so the radio locks to the proper preset.
3. Leave the Flipper near the target vehicle/fob. Alerts appear inline on the scan screen; press `Stop` to exit and reset the session.

## 🧪 Detection Methodology

| Detection | Criteria | Rationale |
| --- | --- | --- |
| Rolling-Pwn | 5 unique hashes within ≤2.5 s, all frames 3–14 ms (≈40–160 bits) and within 18 % of each other | Mimics the consecutive-sequence abuse detailed in CVE-2021-46145 research while filtering ambient noise. |
| RollBack | Identical hash reappears after ≥8 s | Highlights BCM counter resets or persistence of captured “future” frames. |
| Static Code | Identical hash repeats inside the normal window with matching waveform characteristics | Detects CVE-2022-27254-style fixed commands without mistaking jittery noise for a replay. |

All thresholds are derived from the timing, baud-rate, and FSK deviation analysis documented in [`RESEARCH.md`](RESEARCH.md); they are intentionally conservative so that a warning implies a meaningful follow-up investigation.

## 📖 Research Notes

- [`RESEARCH.md`](RESEARCH.md) aggregates the Honda RKE background, preset math, and external references (Rolling-Pwn, RollBack, static code CVEs).
- Section 11 of the research file documents the Mini Cooper (R56/R60) Hitag2 passive-audit workflow implemented here—covering CAS3 architecture, PCF79xx transponders, 104-bit frame parsing, entropy metrics, and a Flipper-based checklist for expanding Car Breaker 19 beyond Honda.
- The app never bundles exploit payloads—only detection baselines aligned with the documented physics layer.

## 🤝 Support & Feedback

- File firmware issues or detection false-positives via the main [Momentum Firmware issue tracker](../../issues).
- Pull requests that improve detection accuracy, documentation, or presets are welcome—please keep the receive-only design.

## 🙏 Credits

- Honda Rolling-Pwn/RollBack researchers: Kevin2600, Wesley Li, Levente Csikor.
- Momentum Firmware maintainers and the broader Flipper Zero community for Sub-GHz tooling inspiration.

## 📄 License

Car Breaker 19 ships as part of Momentum Firmware and inherits the project’s [GPLv3 license](../../LICENSE).
