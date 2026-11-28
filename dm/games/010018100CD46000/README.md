# Resident Evil 5 (`010018100CD46000`)

<!-- PEANUTS: Resident Evil 5 profile note -->

## Status

Provisional Android profile for update 1.0.3. Frame pacing is game owned pending timing measurements.

The desktop split found 19,730 shaders. The targeted split scanned four of
17,487 RomFS files and reproduced all four shader tables byte for byte.

The rewritten `main`, `subsdk0` and `sdk` occupy `0x3b90000` bytes after
16 KiB alignment. The host rounds their combined size to 60 MiB, within the
default 64 MiB used-memory allowance. The other memory settings are survey
estimates. The NPDM declares zero system-resource memory.

## Tested on

Planned device is an AYN Thor with QCS8550, Adreno 740, 12 GB RAM and Android 13.

| Driver | Gameplay, frame timing and memory |
| --- | --- |
| Turnip T30 (Mr_Purple), 26.3.0-T30-1.4.359 | TBD |
| Qualcomm system driver | TBD |

## Builds

Update 1.0.3 (v196608) is the package build. Capcom's MT Framework renders
through NVN on SDK 6.5.0. The profile lists build IDs for `main`, `subsdk0`
and `sdk`. The latter two match Resident Evil 6.

## Open

- Run on both Thor drivers and collect gameplay logs, frame timing, game speed, peak memory and pipeline keys.
- Cutscenes need the NV12 movie colour fix. Coordinate it with upstream #566.
- Check companion availability and other game builds.
