# Changelog

## 0.1.0 - 2026-11-18 (tested in game: TEST-RESULT-0.1.0.txt)
### Added
- Replace = animation -> new [| new...], When, Priority, Chance: animations replaced as they start (Montage_PlayInternal hook); sequences inside dynamic montages, montage assets by montage.
- Console anim.log, anim.list, anim.reload.
### Fixed
- Conditions compiled at map load, not at start-up (this mod loads before NorseConditions).
