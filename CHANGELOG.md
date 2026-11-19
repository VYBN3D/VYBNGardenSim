# Curaleaf Garden — Changelog

Firmware versions for the 4" S3 and 2.8" S3 grow-game boards.
Each tagged version publishes `.bin` files + `version.txt` on the
[Releases page](https://github.com/VYBN3D/curaleaf-garden/releases);
boards update over the air via **Options → CHECK UPDATES → INSTALL**.

## v28 — OTA busy guard + sell-modal flash fix
- WiFi/UPDATES: tapping INSTALL now swaps the button to a dimmed
  "DOWNLOADING ..." that can't be re-tapped; if the download or flash
  fails the button returns to normal so you can retry
- Sell submenu: arrow taps no longer trigger the full-page tail render,
  which briefly flashed the market page under the modal

## v27 — smooth sell-picker arrows
- Tapping the giant < > arrows in the sell submenu now repaints only the
  strain card and the SELL bar (a full-page render used to flash the
  whole screen on every arrow tap)

## v26 — sell picker uses big strain cards
- The market strain submenu is rebuilt: BACK is now a proper button in the
  top-right of the box; each stashed strain is one giant card flanked by
  full-height < > arrows (cycling stocked strains like the other pickers),
  with the strain name, count and per-unit price on the card
- Full-width SELL bar under the card; the ARE YOU SURE? confirmation and
  its big BACK / YES, SELL IT buttons are unchanged

## v25 — hash jars, sell/send confirm menus, bigger OTA buttons
- Hash is now part of the stash list: the STASH page lists flower jars and
  hash units per strain on the same rows (jars grade | hash grade)
- Hash jars are sendable: the send-supplies screen has FLOWER JAR and
  HASH JAR cards side by side (own strain arrows), and recipients get
  "Received a %s hash jar"
- Market sell cards open a strain submenu (3 big rows per page, < >
  paging) instead of the skinny top shelf bar; each row shows strain,
  count and per-unit price
- One-tap confirmation before anything sells (ARE YOU SURE? with
  YES, SELL IT / BACK) and before any product is sent (SEND IT? /
  BACK) — no more accidental sales or gifts
- WiFi / UPDATES page: YOUR BUILD shrunk to a small text line, buttons
  enlarged; Options menu: RESET GAME and UPDATE are now two big
  side-by-side card buttons
- v24/v23 catalog tweaks carry over (owned-pack pill badge, send-screen
  arrows cycle all unlocked strains, money keypad rebuilt)

## v24 — seed pack pill badge
- Seed catalog: every unlocked card gets a pill badge showing how many
  4-packs you hold ("x1", "x2" — gold when stocked, gray "x0" when not)
- The newest unlocked strain no longer hides the count behind a
  "Fresh unlock - try it!" note; the count pill is always shown

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
