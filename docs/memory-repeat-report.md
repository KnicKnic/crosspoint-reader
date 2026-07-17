# Memory Test Repeat Report

Ten serial boot captures were taken on COM3: five baseline boots, then five PM/auto-light-sleep boots. Settings were enabled from the file in all ten captures.

## Build Footprint

| Image | firmware.bin bytes | PlatformIO RAM used | PlatformIO total image size |
|---|---:|---:|---:|
| baseline | 5,510,384 | 48,572 B | 5,510,226 B |
| pm | 5,520,464 | 48,748 B | 5,520,316 B |
| delta | +10,080 | +176 B | +10,090 B |

## Per-Run Boot/Heap Data

| Image | Run | Path check | Configure start free | Configured free | 10s free | 20s free | 30s free | 30s min free |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| baseline | 1 | PM skipped as expected | 197,268 | 197,268 | 112,844 | 112,844 | 112,844 | 112,784 |
| baseline | 2 | PM skipped as expected | 197,268 | 197,268 | 112,844 | 112,844 | 112,844 | 112,784 |
| baseline | 3 | PM skipped as expected | 197,268 | 197,268 | 112,844 | 112,844 | 112,844 | 112,784 |
| baseline | 4 | PM skipped as expected | 197,268 | 197,268 | 112,844 | 112,844 | 112,844 | 112,784 |
| baseline | 5 | PM skipped as expected | 197,268 | 197,268 | 112,844 | 112,844 | 112,844 | 112,784 |
| pm | 1 | PM lock held; sticky log present | 195,900 | 194,080 | 109,644 | 109,644 | 109,644 | 109,588 |
| pm | 2 | PM lock held; sticky log present | 195,900 | 194,080 | 109,644 | 109,644 | 109,644 | 109,588 |
| pm | 3 | PM lock held; sticky log present | 195,900 | 194,080 | 109,644 | 109,644 | 109,644 | 109,588 |
| pm | 4 | PM lock applied; sticky line serial-corrupted | 195,900 | 194,080 | 109,644 | 109,644 | 109,644 | 109,588 |
| pm | 5 | PM lock held; sticky log present | 195,900 | 194,080 | 109,644 | 109,644 | 109,644 | 109,588 |

## Averages

| Metric | Baseline avg | PM avg | Delta PM-Baseline |
|---|---:|---:|---:|
| Configure start free | 197,268.0 | 195,900.0 | -1,368.0 |
| Configured free | 197,268.0 | 194,080.0 | -3,188.0 |
| 10s free | 112,844.0 | 109,644.0 | -3,200.0 |
| 20s free | 112,844.0 | 109,644.0 | -3,200.0 |
| 30s free | 112,844.0 | 109,644.0 | -3,200.0 |
| 30s min free | 112,784.0 | 109,588.0 | -3,196.0 |

## Notes

- Baseline path check means the settings were on, but the build correctly logged that `CONFIG_PM_ENABLE` is not present.
- PM configured successfully in all five PM samples. All five created the CPU lock, created the no-light-sleep lock, and reached `After PM lock apply`.
- The exact sticky no-light-sleep phrase was present in 4 of 5 PM repeat logs; run 4 shows a serial-corrupted merge at that line, but the lock-apply heap and later telemetry are present.
- COM3 still enumerated after the PM test sequence.

## Raw Logs

- Baseline: `.pio/build/memory_baseline/repeat/baseline-run-01.log` through `baseline-run-05.log`
- PM: `.pio/build/memory_pm/repeat/pm-run-01.log` through `pm-run-05.log`