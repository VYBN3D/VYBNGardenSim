# Curaleaf Garden — Changelog

Firmware versions for the 4" S3 and 2.8" S3 grow-game boards.
Each tagged version publishes `.bin` files + `version.txt` on the
[Releases page](https://github.com/VYBN3D/curaleaf-garden/releases);
boards update over the air via **Options → CHECK UPDATES → INSTALL**.

## v23 — market shelf picker + send-screen fixes
- Market SELL mode: shelf row with `<` `>` arrows picks the stash strain;
  SELL 1 and SELL ALL now act on the picked strain (cycle the shelf for others)
- Seed catalog: owned count always visible on unlocked cards
  ("You own xN - BUY to restock/start a batch"); the small gold count
  bubble was removed (it overlapped long strain names)
- Seed catalog: BUY 4-PACK button enlarged to full card width
- Send supplies: SEEDS / STASH JAR arrows cycle **all unlocked strains**
  (they previously only cycled strains you hold, so they looked dead
  when you had one stocked strain)
- Send money: keypad rebuilt as two rows of five big keys — the `0`
  no longer falls off the screen; CLR / 000 / DEL and ALL / SEND / BACK
  evenly spaced

## v21 — PEST, CARE and SKIP cheat codes
- PEST: clears the infestation (type + severity)
- CARE: +5 water packs, +5 care kits, +5 pest defense kits
- SKIP: satisfies the open defoliation window without the kill risk so
  growth resumes (no-ops with feedback when nothing is open)

## v20 — button-scale WiFi scan rows
- Network list rows grow from 17 px / font-1 to 24 px / font-2 with a
  24 px tap zone per row (4 per page, `<` `>` page by 4)
- Saved-network marker and RSSI kept; long SSIDs truncate to fit

## v19 — farmer pop-out + overlay repaint fixes
(Folds in the v17/v18 fixes, which were flashed over USB but never tagged)
- Farmer / speech-bubble overlay re-stamps only when something repaints
  underneath — no more continuous per-pass redraw flood
- Pop-outs restore the covered art region through a PSRAM sprite blitted
  in one push (no visible row-by-row rebuild flash)
- Sprite pixels stored in panel wire order — fixes the mismatched-colour
  box the first pop-out blit left on the garden
- No farmer visits during the watering animation or the dying-plant modal

## v16 — baseline feature set
- Per-strain seeds (bought and tracked per strain)
- WiFi login + OTA firmware updates
- Cheat codes: CASH/MONEY, SEEDS, STASH, UNLOCK, HARVEST, TURBO, HYDRO
- Remediation kits (pest / water / care) and catalog-style shops
