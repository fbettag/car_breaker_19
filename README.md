# Car Breaker 19 🚗🛡️

> Passive Honda Remote Keyless Entry (RKE) threat-detection companion for the Flipper Zero.

[![Flipper Zero](https://img.shields.io/badge/Flipper%20Zero-Compatible-green.svg)](https://flipperzero.one/)
[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](../../LICENSE)

## 🎯 Overview

Car Breaker 19 is a research application that samples Honda RKE transmissions and flags risky behaviours such as Rolling-Pwn, RollBack, or static-code regressions. It shares the same UX philosophy as `uid_brute_smarter`: clear feedback, conservative defaults, and safeguards. Passive analysis remains the default mode; the optional **Guided Audit** additionally lets owners verify their own vehicle by replaying signals captured moments earlier in the same session.

The 2025 research push also broadens our passive telemetry goals beyond Honda: Car Breaker 19 now parses Mini Cooper R56/R60 Hitag2 frames (CAS3/CAS3+) to surface UID/counter/entropy diagnostics without ever transmitting. See [`RESEARCH.md`](RESEARCH.md#11-passive-cryptographic-analysis-of-mini-cooper-r56-hitag2-systems) for the methodology implemented in the latest release.

> ⚠️ **IMPORTANT**: This tool exists solely to help owners and authorized researchers validate the security posture of vehicles they control. Passive capture never transmits. The Guided Audit transmits **only** frames captured in the current session, requires an explicit authorization acknowledgment, bounds every replay count, and never jams. It must never be used against systems without explicit, written authorization.

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

### 🧪 Guided Audit (opt-in transmit)

Three owner-side tests mirror published attack mechanisms so a vehicle owner can determine whether their own car is affected. Every test replays **only** raw frames captured earlier in the same session and requires an explicit authorization acknowledgment first.

| Test | Procedure | Finding |
| --- | --- | --- |
| Static Code | Capture one used unlock, enforced 30 s settle delay, single replay | Car reacts → fixed codes (CVE-2022-27254 class) |
| RollBack | Capture ~5 normally used codes, consecutive replay burst of the oldest 3 | Car resyncs → replay-induced counter rollback (arXiv 2210.11923) |
| Rolling-Pwn | Capture ~10 presses out of vehicle range, replay unused codes one at a time | Any reaction → previous-cycle codes accepted (CVE-2021-46145 class) |

Safeguards: authorization gate, session-only frames (no file import), bounded replay counts (max 5), per-test frequency confirmation display, no jamming, and a verdict screen with an exportable audit log (`/ext/apps_data/car_breaker19/`). Replays that the firmware's regional TX lock refuses are reported as blocked, not bypassed.

### 📚 Known Issues Lookup

A metadata table of documented RKE weaknesses (Rolling-Pwn, RollBack, Hitag2 cryptanalysis, telematics API flaws) from the 2025 systematization of knowledge (arXiv 2505.02713), browsable per manufacturer so owners can check whether their model generation is already on record.

### 💾 Session Export

Every retained raw frame can be written as a stock-compatible `.sub` file (custom CC1101 preset included) for offline analysis or playback in the stock Sub-GHz app.

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
4. For an active self-audit, select `Guided Audit (TX)` instead, pick a test, and follow the step-by-step prompts. The verdict and full audit log land in `/ext/apps_data/car_breaker19/`.
5. `Known Issues` browses documented manufacturer weaknesses; `Export Session` writes all retained raw frames as `.sub` files.

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
