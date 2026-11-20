/*
  Cannabis Garden — a small touchscreen farming game for the ESP32-2432S028R.

  Libraries:
    - TFT_eSPI by Bodmer
    - XPT2046_Touchscreen by Paul Stoffregen

  Configure TFT_eSPI's active User_Setup.h with the ESP32-2432S028R settings:
    ILI9341_2_DRIVER, 240x320, MOSI 13, MISO 12, SCLK 14, CS 15,
    DC 2, RST -1, BL 21, USE_HSPI_PORT.
  The touchscreen uses its separate CYD pins below.
*/

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <SD.h>
#include <math.h>
#include <limits.h>
#include <ctype.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define TOUCH_IRQ   36
#define TOUCH_MOSI  32
#define TOUCH_MISO  39
#define TOUCH_CLK   25
#define TOUCH_CS    33
#define BOOT_BUTTON_PIN 0

// On this CYD revision the microSD slot uses the default VSPI pins.
#define SD_CS   5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCLK 18

// Harvest-ready indicator LED on the back of the CYD (RGB revision:
// red=GPIO 4, green=GPIO 16, blue=GPIO 17). Set singleColorLedPin to the
// pin of a single-color LED instead, or 255 to disable the LED entirely.
// Verified on hardware: driving 17 LOW lit BLUE, 16 lit GREEN, so the
// standard mapping is correct (red=4). The earlier "red dead on 4" result
// was the death-LED timing bug, not a dead pin.
#define LED_R_PIN 4
#define LED_G_PIN 16
#define LED_B_PIN 17
#define LED_SINGLE_PIN 255   // 255 = RGB LED mode; a pin number = single-color mode

// Draw directly to the TFT. The 320x240 RGB565 sprite used too much
// internal RAM on the CYD and caused unstable display/touch behavior.
TFT_eSPI canvas = TFT_eSPI();
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(TOUCH_CS, TOUCH_IRQ);
Preferences preferences;
bool sdMounted = false;

const int ART_W = 178;
const int ART_H = 144;
uint8_t bmpRow[ART_W * 3 + 4];
uint16_t rgbRow[320];
uint16_t scaledPlantRow[320];
unsigned long deathLedUntil = 0;  // red LED stays lit briefly after a kill

// Settings pages: scroll offset for the big-button list, and drag-gesture state.
int16_t settingsScroll = 0;
uint8_t characterSelectIndex = 0;  // grower shown on the giant select card
// SD file installer (serial-triggered tool): lets a host push BMP files to
// the card without removing it. Send "SDINSTALL\n" at 115200 to enter.
bool sdInstallerMode = false;
char installerBuf[16];
uint8_t installerLen = 0;


// Growth stage currently shown on screen; tick refreshes only redraw stats
// while the stage (and therefore the plant art) is unchanged.
uint8_t lastDrawnStage = 255;

enum Page : uint8_t { GARDEN, SUPPLIES, MARKET, STASH, OPTIONS, GROWER_MENU, HASH_ROOM, HARVEST_SELECT, TRIM_ROOM, PLANT_VIEW, PLANTING, NEARBY, NAME_EDIT, CHARACTER_SELECT, SEED_SHOP, OTA_PAGE, WIFI_LOGIN, CHEAT_CODE };

Page growerMenuReturn = GARDEN;    // where the leaf-menu BACK button returns
bool growerFromMenu = false;       // character select was opened from the leaf menu
Page growerNameReturn = OPTIONS;   // where the name editor returns
Page page = GARDEN;
Page pageAfterPlanting = GARDEN;
Page pageAfterSeedShop = SUPPLIES;

const int SCREEN_W = 320;
const int SCREEN_H = 240;
const int HEADER_H = 28;
const int NAV_Y = 207;
const int NAV_H = 33;
const int ACTION_Y = 174;
const int ACTION_H = 29;
const int PLANTS_PER_ROOM = 4;
const uint16_t HASH_ROOM_COST = 150;

// Firmware identity, shown in the boot log and on the Options page.
#define FW_VERSION "v16"
#define FW_BOARD    "CL"
#define FW_DATE    "Nov 19"
#define FW_VERSION_FULL FW_VERSION "-" FW_BOARD

const uint16_t BG = 0x1082;
const uint16_t PANEL = 0x2125;
const uint16_t PANEL_LIGHT = 0x3187;
const uint16_t TEXT_MAIN = 0xEF7D;
const uint16_t TEXT_DIM = 0x9CF3;
const uint16_t ACCENT = 0x6ECA;
const uint16_t ACCENT_DARK = 0x3D66;
const uint16_t GOLD = 0xFEA0;
const uint16_t RED = 0xE986;

// Small 24x24 alpha-cut mask of the WinFlorida leaf logo.
const uint8_t winLeafLogoMask[72] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0x80,0x01,0xFF,0xE0,
  0x07,0x80,0x78,0x0E,0x00,0x1C,0x1C,0x00,0x0C,0x18,0x00,0xC0,
  0x38,0x1F,0xC0,0x30,0x78,0x80,0x30,0xC1,0x80,0x31,0x81,0x80,
  0x31,0x01,0x80,0x31,0x21,0x00,0x32,0x43,0x00,0x1A,0xC6,0x00,
  0x18,0x8C,0x00,0x1C,0xF8,0x0C,0x0D,0xF0,0x1C,0x03,0x00,0x78,
  0x07,0xFF,0xE0,0x0E,0x7F,0x80,0x00,0x00,0x00,0x00,0x00,0x00
};

uint8_t growth = 0;
uint8_t hydration = 70;
uint8_t vigor = 78;
uint8_t waterPacks = 3;
uint8_t careKits = 2;
uint8_t pestDefenseKits = 2;
uint8_t seedStrain = 0;
uint8_t activeStrain = 0;
uint8_t remediationKits = 1;
uint16_t untrimmedUnits = 0;
uint8_t untrimmedStrain = 0;  // strain of the untrimmed batch waiting in the trim room
uint16_t credits = 18;
uint16_t gameMinutes = 0;
uint16_t harvestCount = 0;
uint8_t lastHarvestUnits = 0;
uint8_t moldLossUnits = 0;
uint8_t moldRescuedUnits = 0;  // units rescued from mold by remediation kits
// WiFi login state (buffers must exist before saveGame below).
char wifiSsid[33] = "";
char wifiPass[65] = "";
char wifiSsidEdit[33] = "";
char wifiPassEdit[65] = "";
uint8_t wifiLoginStage = 0;            // 0 typing network, 1 typing password
char otaStatus[64] = "No WiFi yet - tap WIFI LOGIN";
char otaLatest[16] = "";               // version.txt when a check succeeded
// Cheat console (Options > CHEATS): type a code, tap ACTIVATE.
char cheatEdit[16] = "";
char cheatFeedback[28] = "";
char growerName[12] = "Conor";
char growerNameEdit[12] = "";
uint8_t farmerCharacter = 0;
bool farmerCharacterChosen = false;
bool selectingFarmerFromOptions = false;
uint8_t bongs = 0;
uint8_t lighters = 0;
uint8_t rollingPapers = 0;
uint8_t growthBoostTicks = 0;
uint8_t smokeAccessoryUsed = 0;
uint8_t stonedLevel = 0;  // 0-100, decays one point per game tick
const char *stonedLevelNames[10] = {
  "Sober", "Barely Buzzed", "Nice Mellow", "Feeling Fuzzy", "Giggle Fit",
  "Munchies Soon", "Time Is Fake", "Couch Merged", "Deep Thoughts",
  "Giraffe Pu$$y"
};
uint8_t smokeProductUsed = 0;
uint8_t pestType = 0;
uint8_t pestSeverity = 0;
bool plantActive = true;
bool firstPlantingDone = true;
uint8_t plantedPotMask = 0;
bool seedDragging = false;
int seedDragX = 0;
int seedDragY = 0;
uint8_t gameSpeed = 0;  // 0 = RELAXED, 1 = QUICK, 2 = EXTREME (replaces quickPace/demoMode)
bool displayInverted = false;
bool hashRoomUnlocked = false;
bool resetArmed = false;
bool waterAnimationRequested = false;
bool smokeAnimationRequested = false;
bool hashAnimationRequested = false;
bool flowerHarvestAnimationRequested = false;
bool flowerTrimAnimationRequested = false;
bool moldAnimationRequested = false;
bool marketGearMode = false;
bool stashGearMode = false;

// Defoliation windows, over-defoliation and overwatering (per harvest).
const uint8_t DEFOLIATE_WINDOWS = 4;
const uint8_t defoliateWindowStart[DEFOLIATE_WINDOWS] = {20, 40, 60, 80};
const char *defoliateWindowNames[DEFOLIATE_WINDOWS] = {
  "FIRST DEFOLIATION", "SECOND DEFOLIATION", "THIRD DEFOLIATION", "FINAL DEFOLIATION"
};
uint8_t defoliateWindow = 255;      // window the click count belongs to (255 = none yet)
uint8_t defoliateClicks = 0;        // clicks used in the current window
uint8_t defoliateLimit = 2;         // hidden random limit for the current window
uint8_t defoliatePromptPending = 255;
// Defoliation pacing: after a window opens its button stays gray for this many
// game-minutes (1 per tick: 30 s real on normal speed, 4 s on fast) so the
// windows don't fire back-to-back. While a fresh window is open and untouched,
// growth pauses — defoliation is maintenance you must do, not a growth boost.
const uint8_t DEFOLIATE_COOLDOWN_MINUTES = 4;
uint16_t defoliateReadyAt = 0;      // gameMinutes when the window's button unlocks
uint8_t deathReason = 0;            // 1 = overwatered, 2 = over-defoliated, 3 = dried out
// Wilt timer: below 20% hydration the plant is on borrowed time. If it stays
// that dry for this many consecutive ticks it dies — even before the soil is
// fully bone-dry (the old 0% wither still applies). 20 ticks = 10 real
// minutes at normal speed, 80 s on fast.
const uint8_t DRY_TICKS_LIMIT = 20;
uint8_t dryStreakTicks = 0;         // consecutive ticks the plant has been <20% water
bool plantDeathAnimationRequested = false;
bool pestAnimationRequested = false;

const char DEVICE_BRAND[] = "WinFlorida";
const uint16_t NEARBY_MAGIC = 0xC47A;
const uint8_t PACKET_HELLO = 1;
const uint8_t PACKET_MESSAGE = 2;
const uint8_t PACKET_GIFT = 3;
const uint8_t PACKET_CASH = 4;
const uint8_t MAX_NEARBY_PEERS = 4;
const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct __attribute__((packed)) NearbyPacket {
  uint16_t magic;
  uint8_t type;
  uint8_t item;
  char brand[12];
  char grower[12];
  uint8_t masteryLevel;
  char text[32];
};

struct NearbyPeer {
  bool active;
  uint8_t mac[6];
  char brand[12];
  char grower[12];
  uint8_t masteryLevel;
  unsigned long lastSeen;
};

struct FarmerSaying {
  const char *line1;
  const char *line2;
};

const FarmerSaying farmerSayings[20] = {
  {"Fire!", ""},
  {"Dang, that looks", "Terpy."},
  {"Leaf me to it.", ""},
  {"Roots before shoots.", ""},
  {"Mites better scoot.", ""},
  {"Grow big or go home.", ""},
  {"That canopy is a", "real leaf-er."},
  {"Grower green team!", ""},
  {"Lookin' frosty-ish.", ""},
  {"Stay high on", "good vibes."},
  {"Bud-dy, we did it!", ""},
  {"Photosynthesis:", "nature's snack time."},
  {"This room's a", "total grow-mance."},
  {"More leaves, please.", ""},
  {"That stem has", "main character energy."},
  {"The plants look", "gorgeous today."},
  {"I could leaf here", "all day."},
  {"Buds of steel!", ""},
  {"Smells like a", "harvest party."},
  {"Terp there it is!", ""}
};

NearbyPeer nearbyPeers[MAX_NEARBY_PEERS] = {};
NearbyPacket inboundNearbyPacket = {};
uint8_t inboundNearbyMac[6] = {};
volatile bool inboundNearbyReady = false;
portMUX_TYPE nearbyMux = portMUX_INITIALIZER_UNLOCKED;
bool nearbyReady = false;
uint8_t selectedNearbyPeer = 255;
uint8_t nearbyMode = 0; // 0 list, 1 compose, 2 gift, 3 inbox, 4 cash
char outgoingMessage[28] = "";
char outgoingCash[6] = "";      // money amount being typed (digits only)
uint8_t msgKbMode = 0;           // compose keyboard: 0 lower, 1 upper, 2 symbols
uint8_t nameKbMode = 0;          // name-edit keyboard: 0 lower, 1 upper, 2 symbols
char lastNearbyMessage[52] = "No messages yet";
unsigned long lastNearbyHello = 0;

unsigned long lastGameTick = 0;
// Grow clock: wall seconds since power-on. The ESP32 RTC keeps counting
// through light AND deep sleep, so the grow advances while the screen sleeps.
uint32_t growEpoch = 0;
// Sleep dormancy: ticks that elapse while the screen sleeps are run "dormant"
// — water/vigor/pests/wilt-timer pause, so a plant put to sleep healthy can
// never die in its sleep — while growth keeps advancing on the wall clock.
bool ticksDuringSleep = false;
bool justWokeFromSleep = false;
unsigned long lastFarmerVisitAt = 0;
unsigned long lastSave = 0;
unsigned long resetArmedAt = 0;
bool previousTouch = false;

void render(bool presentNow = true);
void harvestLedRed();
bool wateringWouldDrown();
uint8_t defoliateButtonState();
void resetDefoliationForNewPlant();
void animatePlantDeath();
void showDefoliatePrompt();
void animatePestInvasion();
void drawNearbyKeyboardRow(const char *keys, int count, int startX, int y);
void drawGardenActions();
void waitForCutsceneContinue();
void drawSettingsRow(int y, const char *label, const char *value);

// --- 8-bit soundtrack -------------------------------------------------------
// Square-wave chiptune through the LEDC peripheral. SPEAKER_PIN (GPIO26) is
// hard-wired inside the CYD to the onboard SC8002B audio amplifier, which
// drives the 2-pin SPEAK connector on the back of the board — plug a small
// 8-ohm speaker into it to hear the game. GPIO26 is NOT broken out on any
// header, so it can only feed the onboard amp. The drum voice (kick, snare,
// hat as short square blips) shares that same amp-fed pin by default;
// EXTERNAL_DRUM_BUZZER = 1 routes drums to DRUM_PIN instead for a passive
// piezo wired between GPIO27 (free on the CN1 header) and GND.
// Set a pin to 255 to mute that voice. Other pins in use on this CYD:
// touch 25/32/33/36/39, SD 5/18/19/23, TFT 2/12/13/14/15/21, RGB LED 4/16/17.
#define SPEAKER_PIN 26
#define DRUM_PIN 27
#define EXTERNAL_DRUM_BUZZER 0  // 0 = drums share the onboard speaker (amp)
uint8_t musicVolume = 60;  // 0..100% of max loudness; 0 = music off

struct ChipNote { uint16_t freq; uint8_t steps; };  // freq 0 = rest; steps in 16ths
struct ChipSong { const ChipNote* notes; uint16_t count; uint16_t stepMs; bool loop; };
struct DrumLoop { const ChipNote* notes; uint16_t count; uint16_t stepMs; };

// Indie hip-hop inspired garden theme: laid-back boom-bap groove (~94 BPM,
// 160 ms sixteenths) in A minor — kick/snare bass hits, hat ticks, and a
// sparse pentatonic lead answering the bassline.
const ChipNote gardenThemeNotes[] PROGMEM = {
  // Bar 1: A bass groove
  {110,2},{1047,1},{0,1},{165,2},{1047,1},{0,1},{110,2},{1047,1},{0,1},{165,2},{1047,1},{0,1},
  // Bar 2: drop to G
  {98,2},{1047,1},{0,1},{165,2},{1047,1},{0,1},{98,2},{1047,1},{0,1},{165,2},{1047,1},{0,1},
  // Bar 3: lead answers over the kick
  {131,2},{440,1},{0,1},{165,2},{392,1},{0,1},{110,2},{1047,1},{0,1},{165,2},{440,1},{0,1},
  // Bar 4: resolve and breathe
  {110,2},{392,1},{0,1},{165,2},{330,1},{0,1},{98,2},{1047,1},{0,1},{165,2},{0,2},
};
const ChipSong gardenTheme = {gardenThemeNotes,
    sizeof(gardenThemeNotes) / sizeof(ChipNote), 160, true};

// Plant lost: slow descending minor phrase.
const ChipNote deathJingleNotes[] PROGMEM = {
  {440,2},{330,2},{262,2},{220,3},{196,2},{165,5},{0,2},
};
const ChipSong deathJingle = {deathJingleNotes,
    sizeof(deathJingleNotes) / sizeof(ChipNote), 170, false};

// Harvest: rising fanfare.
const ChipNote harvestFanfareNotes[] PROGMEM = {
  {523,1},{659,1},{784,1},{1047,4},{0,1},{784,2},{1047,5},{0,2},
};
const ChipSong harvestFanfare = {harvestFanfareNotes,
    sizeof(harvestFanfareNotes) / sizeof(ChipNote), 90, false};

// Pest invasion: two-note siren.
const ChipNote pestAlarmNotes[] PROGMEM = {
  {880,2},{659,2},{880,2},{659,2},{880,2},{659,2},{0,2},
};
const ChipSong pestAlarm = {pestAlarmNotes,
    sizeof(pestAlarmNotes) / sizeof(ChipNote), 110, false};

// Buy / sell: classic coin blip.
const ChipNote coinBlipNotes[] PROGMEM = {
  {988,1},{1319,3},{0,1},
};
const ChipSong coinBlip = {coinBlipNotes,
    sizeof(coinBlipNotes) / sizeof(ChipNote), 70, false};

const ChipSong* activeSong = nullptr;
const ChipSong* backgroundSong = &gardenTheme;
uint16_t songPos = 0;
unsigned long noteEnd = 0;

// Drum-voice state (declared early: startSong() silences the groove for jingles)
const DrumLoop* activeDrums = nullptr;
uint16_t drumPos = 0;
unsigned long drumEnd = 0;    // when the sounding hit stops
unsigned long drumNext = 0;   // when the next scheduled hit fires
bool drumPlaying = false;
uint16_t leadFreq = 0;    // lead note currently meant for the amp pin
bool drumOnAmp = false;   // a drum blip has borrowed the amp pin

void chipToneOff() {
  if (SPEAKER_PIN == 255) return;
  if (drumOnAmp) { leadFreq = 0; return; }  // blip owns the pin right now
  ledcWrite(SPEAKER_PIN, 0);
  leadFreq = 0;
}

void chipToneOn(uint16_t freq) {
  if (SPEAKER_PIN == 255 || freq == 0) { chipToneOff(); return; }
  leadFreq = freq;
  if (drumOnAmp) return;  // blip owns the pin; it hands the note back after
  ledcWriteTone(SPEAKER_PIN, freq);
  // Duty scales with musicVolume (100% = the original ~20% duty loudness).
  ledcWrite(SPEAKER_PIN, (uint32_t)51 * musicVolume / 100);
}

void startSong(const ChipSong* song) {
  if (SPEAKER_PIN == 255 || musicVolume == 0) return;
  chipToneOff();
  activeSong = song;
  songPos = 0;
  noteEnd = 0;
  if (song != backgroundSong) {
    drumToneOff();      // jingles play dry; the groove resumes after
    drumPlaying = false;
  }
}

void playCoin() { startSong(&coinBlip); }

// --- Drum track (second LEDC voice) -----------------------------------------
// Drum hits are short square blips whose pitch implies the drum: ~65 Hz kick,
// ~180 Hz snare buzz, ~4.5 kHz hat tick. The pattern runs on the same
// 16th-note clock as the garden theme so the two voices stay in step.
// One 16-step bar of boom-bap, repeated with a bar-4 double-kick fill.
const ChipNote gardenDrumNotes[] PROGMEM = {
  {65,2},{4500,2},{180,2},{4500,2},{65,2},{4500,2},{180,2},{4500,2},   // bars 1-2
  {65,2},{4500,2},{180,2},{4500,2},{65,2},{4500,2},{180,2},{4500,2},
  {65,2},{4500,2},{180,2},{4500,2},{65,2},{4500,2},{180,2},{4500,2},   // bar 3
  {65,2},{4500,2},{180,2},{4500,2},{65,1},{0,1},{65,1},{4500,1},       // bar 4 fill
  {4500,2},{180,2},
};
const DrumLoop gardenDrums = {gardenDrumNotes,
    sizeof(gardenDrumNotes) / sizeof(ChipNote), 160};

void drumToneOff() {
  if (drumOnAmp) {
    // Hand the amp pin back to the lead voice (silently if none is playing).
    drumOnAmp = false;
    ledcWrite(SPEAKER_PIN, 0);
    if (leadFreq) chipToneOn(leadFreq);
    return;
  }
  if (DRUM_PIN == 255) return;
  ledcWrite(DRUM_PIN, 0);
}

void drumHit(uint16_t freq) {
  uint32_t duty = freq > 400 ? 35 : (freq <= 120 ? 140 : 110);
#if EXTERNAL_DRUM_BUZZER
  if (DRUM_PIN == 255) return;
  ledcWriteTone(DRUM_PIN, freq);
  ledcWrite(DRUM_PIN, duty * musicVolume / 100);
#else
  // Borrow the amp-fed speaker pin for the blip, then restore the lead note.
  if (SPEAKER_PIN == 255) return;
  drumOnAmp = true;
  ledcWriteTone(SPEAKER_PIN, freq);
  ledcWrite(SPEAKER_PIN, duty * musicVolume / 100);
#endif
  drumPlaying = true;
  drumEnd = millis() + (freq > 400 ? 25 : (freq <= 120 ? 90 : 60));
}

void startDrums() {
  drumToneOff();
  drumPlaying = false;
  activeDrums = &gardenDrums;
  drumPos = 0;
  drumNext = millis();
}

void updateDrums() {
  if (musicVolume == 0) return;
  if (SPEAKER_PIN == 255 && DRUM_PIN == 255) return;
  unsigned long now = millis();
  if (drumPlaying && (long)(drumEnd - now) <= 0) {
    drumToneOff();
    drumPlaying = false;
  }
  // Drums only run under the background theme; jingles play dry and the
  // groove picks up again (in step) when the theme returns.
  if (!activeSong || activeSong != backgroundSong) return;
  if (!activeDrums) startDrums();
  if ((long)(drumNext - now) > 0) return;
  if (drumPos >= activeDrums->count) drumPos = 0;
  uint16_t freq = pgm_read_word(&activeDrums->notes[drumPos].freq);
  uint8_t steps = pgm_read_byte(&activeDrums->notes[drumPos].steps);
  ++drumPos;
  if (freq) drumHit(freq);
  drumNext = now + (unsigned long)steps * activeDrums->stepMs;
}

void updateMusic() {
  updateDrums();
  if (SPEAKER_PIN == 255 || musicVolume == 0) return;
  if (!activeSong) {
    startSong(backgroundSong);  // auto-resume the theme whenever idle
    if (!activeSong) return;
  }
  unsigned long now = millis();
  if ((long)(noteEnd - now) > 0) return;
  if (songPos >= activeSong->count) {
    if (activeSong->loop) {
      songPos = 0;
    } else {
      // Jingle finished: hand the speaker back to the garden theme.
      const ChipSong* next = activeSong == backgroundSong ? nullptr : backgroundSong;
      activeSong = nullptr;
      chipToneOff();
      if (next) startSong(next);
      return;
    }
  }
  chipToneOff();
  uint16_t freq = pgm_read_word(&activeSong->notes[songPos].freq);
  uint8_t steps = pgm_read_byte(&activeSong->notes[songPos].steps);
  ++songPos;
  if (freq) chipToneOn(freq);
  noteEnd = now + (unsigned long)steps * activeSong->stepMs;
}

// Blocking-delay drop-in that keeps the sequencer playing during cutscenes.
void pumpDelay(unsigned long ms) {
  unsigned long end = millis() + ms;
  do {
    updateMusic();
    delay(ms < 12 ? ms : 12);
  } while ((long)(end - millis()) > 0);
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void drawLeafLogo(int x, int y, int scale, uint16_t color) {
  for (int row = 0; row < 24; ++row) {
    for (int col = 0; col < 24; ++col) {
      int bitIndex = row * 24 + col;
      uint8_t packed = pgm_read_byte(&winLeafLogoMask[bitIndex >> 3]);
      if (packed & (0x80 >> (bitIndex & 7))) {
        if (scale == 1) canvas.drawPixel(x + col, y + row, color);
        else canvas.fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

int stageForGrowth() {
  if (growth < 12) return 0;
  if (growth < 32) return 1;
  if (growth < 58) return 2;
  if (growth < 82) return 3;
  return 4;
}

const char *stageName() {
  if (!plantActive) return "EMPTY ROOM";
  switch (stageForGrowth()) {
    case 0: return "SEEDLING";
    case 1: return "YOUNG PLANT";
    case 2: return "GROWING";
    case 3: return "FLOWERING";
    default: return "MATURE";
  }
}

const uint8_t STRAIN_COUNT = 10;
// Seeds are held per strain: buy (or receive) different strains and pick
// which one to plant on the planting screen.
uint8_t seedsByStrain[STRAIN_COUNT] = {0};
uint8_t plantStrain = 0;            // strain chosen on the planting screen
uint8_t sendSeedStrain = 0;         // strain picked in the nearby send-supplies flow

uint16_t totalSeeds() {
  uint16_t t = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) t += seedsByStrain[i];
  return t;
}

uint8_t countSeededStrains() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) if (seedsByStrain[i]) ++n;
  return n;
}

// Next/prev strain (wrapping) that you actually hold seeds of.
uint8_t nextSeededStrain(uint8_t from, int dir) {
  for (uint8_t step = 0; step < STRAIN_COUNT; ++step) {
    from = (from + STRAIN_COUNT + dir) % STRAIN_COUNT;
    if (seedsByStrain[from]) return from;
  }
  return from;
}

const char *strainNames[STRAIN_COUNT] = {
  "Sour Diesel", "Blue Dream", "OG Kush", "Northern Lights", "White Widow",
  "Granddaddy Purple", "Jack Herer", "Girl Scout Cookies", "Gelato", "Toad Venom"
};
const char *strainDifficulty[STRAIN_COUNT] = {
  "HARD", "MEDIUM", "HARD", "EASY", "MEDIUM", "MEDIUM", "MEDIUM", "HARD", "HARD", "EXPERT"
};
const int8_t strainPaceModifier[STRAIN_COUNT] = {-1, 0, -1, 1, 0, 0, 0, -1, -1, -2};
const uint8_t strainHydrationNeed[STRAIN_COUNT] = {24, 18, 24, 14, 18, 18, 18, 24, 24, 32};
const uint8_t strainPrices[STRAIN_COUNT] = {18, 12, 18, 8, 12, 12, 12, 18, 18, 30};
// Seed catalog runs easy -> hard: EASY first (the starting strain you can
// afford), then MEDIUM, HARD, EXPERT. Data stays keyed by strain index;
// these tables only order the catalog, buying, and unlock progression.
// Order: NL(3) $8 EASY | Blue Dream(1), White Widow(4), GDP(5), Jack(6) $12
// MEDIUM | Sour Diesel(0), OG Kush(2), GSC(7), Gelato(8) $18 HARD | Toad(9)
const uint8_t strainCatalogOrder[STRAIN_COUNT] = {3, 1, 4, 5, 6, 0, 2, 7, 8, 9};
const uint8_t strainCatalogPos[STRAIN_COUNT]  = {5, 1, 6, 0, 2, 3, 4, 7, 8, 9};
// Market values scale with grow difficulty: EASY $8 seeds -> $10 flower per
// unit, MEDIUM $12 -> $14, HARD $18 -> $20, EXPERT $30 -> $32. Hash runs at
// roughly 2.5x the flower value of the strain it was made from.
const uint8_t strainFlowerValue[STRAIN_COUNT] = {20, 14, 20, 10, 14, 14, 14, 20, 20, 32};
const uint8_t strainHashValue[STRAIN_COUNT] = {50, 35, 50, 25, 35, 35, 35, 50, 50, 80};
// Stash and hash are tracked per strain so their sell prices match difficulty.
uint16_t stashByStrain[STRAIN_COUNT] = {0};
uint16_t hashByStrain[STRAIN_COUNT] = {0};
// Harvest quality: the plant's vigor when harvested grades the batch and sets
// what it sells for. Quality (0-100 = vigor at harvest) is stored per stash
// jar and weighted-blended when batches of the same jar merge. Jars saved by
// older firmware default to 70 = full price.
uint8_t stashQualityByStrain[STRAIN_COUNT] = {70, 70, 70, 70, 70, 70, 70, 70, 70, 70};
uint8_t hashQualityByStrain[STRAIN_COUNT]  = {70, 70, 70, 70, 70, 70, 70, 70, 70, 70};
uint8_t untrimmedQuality = 70;  // vigor of the plant the untrimmed batch came from
// Defoliation clicks allowed per window: a random number in [min, max] each time.
// Going past it kills the plant. The button turns red once you reach the minimum.
const uint8_t strainDefoliateMin[STRAIN_COUNT] = {1, 2, 1, 2, 2, 2, 2, 1, 1, 1};
const uint8_t strainDefoliateMax[STRAIN_COUNT] = {3, 3, 2, 4, 3, 3, 3, 2, 3, 2};
// Highest hydration a mature plant survives; younger stages tolerate less.
const uint8_t strainWaterTolerance[STRAIN_COUNT] = {90, 94, 90, 98, 94, 94, 94, 90, 90, 86};
const uint8_t stageWaterPenalty[5] = {14, 8, 4, 0, 0};
const uint8_t WATER_AMOUNT = 32;
const uint8_t WATER_DRAIN_PER_TICK = 3;        // was 1
const uint8_t THIRSTY_STRAIN_EXTRA_DRAIN = 2;  // was 1

uint16_t stashTotal() {
  uint16_t total = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) total += stashByStrain[i];
  return total;
}

uint16_t hashTotal() {
  uint16_t total = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) total += hashByStrain[i];
  return total;
}

// Cheapest non-empty bucket: smoking burns low-grade product first.
int16_t cheapestStashStrain() {
  int16_t best = -1;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i)
    if (stashByStrain[i] && (best < 0 || strainFlowerValue[i] < strainFlowerValue[best])) best = i;
  return best;
}

int16_t cheapestHashStrain() {
  int16_t best = -1;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i)
    if (hashByStrain[i] && (best < 0 || strainHashValue[i] < strainHashValue[best])) best = i;
  return best;
}

// Priciest non-empty bucket: selling one unit nets the top-shelf price.
int16_t priciestStashStrain() {
  int16_t best = -1;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i)
    if (stashByStrain[i] && (best < 0 || strainFlowerValue[i] > strainFlowerValue[best])) best = i;
  return best;
}

int16_t priciestHashStrain() {
  int16_t best = -1;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i)
    if (hashByStrain[i] && (best < 0 || strainHashValue[i] > strainHashValue[best])) best = i;
  return best;
}

// "$X each" when every bucket sells at the same price, "$X-$Y each" otherwise.
void stashPriceText(char *out, size_t len) {
  int16_t lo = -1, hi = -1;
  uint8_t loG = 100, hiG = 100;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    if (!stashByStrain[i]) continue;
    uint16_t v = flowerUnitPrice(i);
    uint8_t g = sellGradeForQuality(stashQualityByStrain[i]);
    if (lo < 0 || v < lo) { lo = v; loG = g; }
    if (hi < 0 || v > hi) { hi = v; hiG = g; }
  }
  if (lo < 0) snprintf(out, len, "-");
  else if (lo == hi) snprintf(out, len, "%s $%u each", gradeName(loG), lo);
  else snprintf(out, len, "%s $%u-$%u each", loG == hiG ? gradeName(loG) : "GRADED", lo, hi);
}

void hashPriceText(char *out, size_t len) {
  int16_t lo = -1, hi = -1;
  uint8_t loG = 100, hiG = 100;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    if (!hashByStrain[i]) continue;
    uint16_t v = hashUnitPrice(i);
    uint8_t g = sellGradeForQuality(hashQualityByStrain[i]);
    if (lo < 0 || v < lo) { lo = v; loG = g; }
    if (hi < 0 || v > hi) { hi = v; hiG = g; }
  }
  if (lo < 0) snprintf(out, len, "-");
  else if (lo == hi) snprintf(out, len, "%s $%u each", gradeName(loG), lo);
  else snprintf(out, len, "%s $%u-$%u each", loG == hiG ? gradeName(loG) : "GRADED", lo, hi);
}
uint16_t harvestedStrainMask = 0;
uint8_t strainUnlockedThrough = 0;

const char *currentStrainName() {
  return strainNames[min((int)activeStrain, (int)STRAIN_COUNT - 1)];
}

const char *masteryNicknameForLevel(uint8_t level) {
  static const char *names[10] = {
    "First Time", "Rookie Grower", "Green Thumb", "Bud Tender", "Harvest Hand",
    "Garden Guide", "Master Grower", "Hash Artisan", "High Roller", "Super Pothead"
  };
  return names[constrain((int)level - 1, 0, 9)];
}

uint8_t currentMasteryLevel() {
  return min(10, (int)(harvestCount / 5) + 1);
}

const char *farmerCharacterName(uint8_t character) {
  if (character == 1) return "Mathias";
  if (character == 2) return "Jay";
  if (character == 3) return "Lindsay";
  if (character == 4) return "Eric";
  if (character == 8) return "Pedro";
  if (character == 5) return "James";
  if (character == 6) return "Sergio";
  if (character == 7) return "Vicente";
  return "Conor";
}

void saveGame() {
  preferences.putUChar("growth", growth);
  preferences.putUChar("water", hydration);
  preferences.putUChar("vigor", vigor);
  preferences.putUChar("waterPk", waterPacks);
  preferences.putUChar("careKit", careKits);
  preferences.putUChar("pestKit", pestDefenseKits);
  preferences.putBytes("seedArr", seedsByStrain, sizeof(seedsByStrain));
  preferences.putUChar("seedType", seedStrain);
  preferences.putUChar("activeType", activeStrain);
  preferences.putUShort("strainMask", harvestedStrainMask);
  preferences.putUChar("strainOpen", strainUnlockedThrough);
  preferences.putBytes("stashArr", stashByStrain, sizeof(stashByStrain));
  preferences.putBytes("hashArr", hashByStrain, sizeof(hashByStrain));
  preferences.putUShort("rawFlower", untrimmedUnits);
  preferences.putUChar("rawStrain", untrimmedStrain);
  preferences.putUChar("stoned", stonedLevel);
  preferences.putUShort("credits", credits);
  preferences.putUShort("minutes", gameMinutes);
  preferences.putUShort("harvests", harvestCount);
  preferences.putString("grower", growerName);
  preferences.putUChar("farmer", farmerCharacter);
  preferences.putBool("farmerChosen", farmerCharacterChosen);
  preferences.putUChar("bongs", bongs);
  preferences.putUChar("lighters", lighters);
  preferences.putUChar("papers", rollingPapers);
  preferences.putUChar("growBoost", growthBoostTicks);
  preferences.putBool("active", plantActive);
  preferences.putBool("plantDone", firstPlantingDone);
  preferences.putUChar("plantMask", plantedPotMask);
  preferences.putUChar("afterPlant", (uint8_t)pageAfterPlanting);
  preferences.putUChar("gspeed", gameSpeed);
  preferences.putBool("invert", displayInverted);
  preferences.putUChar("mvol", musicVolume);
  preferences.putBool("hashRoom", hashRoomUnlocked);
  preferences.putUChar("pest", pestType);
  preferences.putUChar("pestLevel", pestSeverity);
  preferences.putUChar("remKit", remediationKits);
  preferences.putUChar("defWin", defoliateWindow);
  preferences.putUChar("defClicks", defoliateClicks);
  preferences.putUChar("defLimit", defoliateLimit);
  preferences.putUShort("defReady", defoliateReadyAt);
  preferences.putUChar("dryTick", dryStreakTicks);  // wilt timer survives sleep/reboots
  preferences.putBytes("sqArr", stashQualityByStrain, sizeof(stashQualityByStrain));
  preferences.putBytes("hqArr", hashQualityByStrain, sizeof(hashQualityByStrain));
  preferences.putUChar("rawQual", untrimmedQuality);
  // Wall seconds since power-on at save time: on the next boot the grow
  // catches up on everything that happened while the screen slept.
  preferences.putUInt("epoch", (uint32_t)time(nullptr));
  preferences.putString("wSsid", wifiSsid);
  preferences.putString("wPass", wifiPass);
  preferences.putBool("started", true);
  lastSave = millis();
}

void newGame() {
  growth = 0;
  hydration = 70;
  dryStreakTicks = 0;
  vigor = 78;
  waterPacks = 3;
  careKits = 2;
  pestDefenseKits = 2;
  remediationKits = 1;
  // Start with no seeds and the stock strain pointed at Northern Lights so
  // the first seed-catalog visit opens with the EASY starter strain buyable.
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) seedsByStrain[i] = 0;
  plantStrain = strainCatalogOrder[0];
  seedStrain = strainCatalogOrder[0];
  activeStrain = 0;
  harvestedStrainMask = 0;
  strainUnlockedThrough = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    stashByStrain[i] = 0;
    hashByStrain[i] = 0;
  }
  untrimmedUnits = 0;
  untrimmedStrain = 0;
  stonedLevel = 0;
  credits = 18;
  gameMinutes = 0;
  harvestCount = 0;
  lastHarvestUnits = 0;
  moldLossUnits = 0;
  moldRescuedUnits = 0;
  moldAnimationRequested = false;
  strlcpy(growerName, "Conor", sizeof(growerName));
  farmerCharacter = 0;
  farmerCharacterChosen = false;
  selectingFarmerFromOptions = false;
  pageAfterPlanting = GARDEN;
  bongs = 0;
  lighters = 0;
  rollingPapers = 0;
  growthBoostTicks = 0;
  smokeAccessoryUsed = 0;
  smokeProductUsed = 0;
  pestType = 0;
  pestSeverity = 0;
  plantActive = false;
  firstPlantingDone = false;
  plantedPotMask = 0;
  seedDragging = false;
  page = CHARACTER_SELECT;
  gameSpeed = 0;
  displayInverted = false;
  musicVolume = 60;
  hashRoomUnlocked = false;
  saveGame();
}

void loadGame() {
  preferences.begin("cannagame", false);
  if (!preferences.getBool("started", false)) {
    newGame();
    return;
  }
  growth = preferences.getUChar("growth", 0);
  hydration = preferences.getUChar("water", 70);
  vigor = preferences.getUChar("vigor", 78);
  waterPacks = preferences.getUChar("waterPk", 3);
  careKits = preferences.getUChar("careKit", 2);
  pestDefenseKits = preferences.getUChar("pestKit", 2);
  seedStrain = preferences.getUChar("seedType", strainCatalogOrder[0]);
  activeStrain = preferences.getUChar("activeType", seedStrain);
  if (seedStrain >= STRAIN_COUNT) seedStrain = 0;
  if (activeStrain >= STRAIN_COUNT) activeStrain = seedStrain;
  // Per-strain seed pool (v15+). Older saves kept one seed pool tagged with
  // seedStrain: migrate it into the new per-strain slots.
  if (preferences.getBytesLength("seedArr") == sizeof(seedsByStrain)) {
    preferences.getBytes("seedArr", seedsByStrain, sizeof(seedsByStrain));
  } else {
    uint8_t oldSeeds = preferences.getUChar("seeds", 0);
    if (oldSeeds && seedStrain < STRAIN_COUNT) seedsByStrain[seedStrain] = oldSeeds;
  }
  plantStrain = seedStrain;
  if (!seedsByStrain[plantStrain]) plantStrain = nextSeededStrain(plantStrain, 1);
  // One-time migration: saves started before the easy-first catalog received
  // the old Sour Diesel starter pack. If those seeds were never planted,
  // swap the stock to Northern Lights so the EASY strain is buyable first.
  if (!preferences.getBool("migNL", false)) {
    if (seedsByStrain[seedStrain] > 0 && seedStrain == 0 && !plantActive) {
      seedStrain = strainCatalogOrder[0];
      activeStrain = seedStrain;
      preferences.putUChar("seedType", seedStrain);
      preferences.putUChar("activeType", activeStrain);
    }
    preferences.putBool("migNL", true);
  }
  harvestedStrainMask = preferences.getUShort("strainMask", 0) & 0x03FF;
  strainUnlockedThrough = min((int)preferences.getUChar("strainOpen", 0), (int)STRAIN_COUNT - 1);
  if (preferences.getBytes("stashArr", stashByStrain, sizeof(stashByStrain)) !=
      sizeof(stashByStrain)) {
    // Migrate pre-per-strain saves: the old pooled stash keeps its value in
    // the cheapest strain so nothing is inflated.
    stashByStrain[3] = preferences.getUShort("stash", 0);
  }
  if (preferences.getBytes("hashArr", hashByStrain, sizeof(hashByStrain)) !=
      sizeof(hashByStrain)) {
    hashByStrain[3] = preferences.getUShort("hash", 0);
  }
  untrimmedUnits = preferences.getUShort("rawFlower", 0);
  untrimmedStrain = preferences.getUChar("rawStrain", 0);
  stonedLevel = preferences.getUChar("stoned", 0);
  credits = preferences.getUShort("credits", 18);
  gameMinutes = preferences.getUShort("minutes", 0);
  harvestCount = preferences.getUShort("harvests", 0);
  String savedGrowerName = preferences.getString("grower", "Conor");
  savedGrowerName.trim();
  if (savedGrowerName.length()) savedGrowerName.toCharArray(growerName, sizeof(growerName));
  bongs = preferences.getUChar("bongs", 0);
  lighters = preferences.getUChar("lighters", 0);
  rollingPapers = preferences.getUChar("papers", 0);
  growthBoostTicks = preferences.getUChar("growBoost", 0);
  plantActive = preferences.getBool("active", true);
  firstPlantingDone = preferences.getBool("plantDone", true);
  farmerCharacter = preferences.getUChar("farmer", 0);
  if (farmerCharacter > 7) farmerCharacter = 0;
  farmerCharacterChosen = preferences.getBool("farmerChosen", firstPlantingDone);
  plantedPotMask = preferences.getUChar("plantMask", firstPlantingDone ? 0x0F : 0);
  uint8_t afterPlant = preferences.getUChar("afterPlant", (uint8_t)GARDEN);
  pageAfterPlanting = (afterPlant == HASH_ROOM || afterPlant == TRIM_ROOM)
                        ? (Page)afterPlant : GARDEN;
  if (!firstPlantingDone) {
    plantActive = false;
    // Route by what the grower actually has: no grower yet -> picker; grower
    // but no full seed pack -> seed catalog; pack in hand -> planting screen.
    page = !farmerCharacterChosen ? CHARACTER_SELECT
         : totalSeeds() >= PLANTS_PER_ROOM ? PLANTING : SEED_SHOP;
  }
  strlcpy(wifiSsid, preferences.getString("wSsid", "").c_str(), sizeof(wifiSsid));
  strlcpy(wifiPass, preferences.getString("wPass", "").c_str(), sizeof(wifiPass));
  gameSpeed = preferences.getUChar("gspeed", 0);
  if (gameSpeed > 2) gameSpeed = 0;
  displayInverted = preferences.getBool("invert", false);
  musicVolume = preferences.getUChar("mvol", 60);
  if (musicVolume > 100) musicVolume = 60;
  hashRoomUnlocked = preferences.getBool("hashRoom", false);
  pestType = preferences.getUChar("pest", 0);
  pestSeverity = preferences.getUChar("pestLevel", 0);
  remediationKits = preferences.getUChar("remKit", 1);
  if (pestType > 3) { pestType = 0; pestSeverity = 0; }
  defoliateWindow = preferences.getUChar("defWin", 255);
  defoliateClicks = preferences.getUChar("defClicks", 0);
  defoliateLimit = preferences.getUChar("defLimit", 2);
  defoliateReadyAt = preferences.getUShort("defReady", 0);
  dryStreakTicks = preferences.getUChar("dryTick", 0);
  if (preferences.getBytesLength("sqArr") == sizeof(stashQualityByStrain))
    preferences.getBytes("sqArr", stashQualityByStrain, sizeof(stashQualityByStrain));
  if (preferences.getBytesLength("hqArr") == sizeof(hashQualityByStrain))
    preferences.getBytes("hqArr", hashQualityByStrain, sizeof(hashQualityByStrain));
  untrimmedQuality = preferences.getUChar("rawQual", 70);
}

// Advances the grow by the wall time that passed since the last save — what
// keeps the grow running through sleep (the RTC clock never stops) and across
// deep-sleep wakes / reboots while the board stays powered.
void catchUpGrowTime() {
  uint32_t nowE = (uint32_t)time(nullptr);
  uint32_t savedE = preferences.getUInt("epoch", 0);
  if (savedE && savedE < nowE) {
    const uint8_t secPerTick = gameSpeed == 2 ? 4 : 30;
    uint32_t ticks = (nowE - savedE) / secPerTick;
    if (ticks > 2880) {
      ticks = 2880;         // bound pathological catch-ups (~24 h of growth)
      growEpoch = nowE;     // skip anything beyond the bound
    } else {
      growEpoch = savedE + ticks * secPerTick;
    }
    // Boot catch-up is sleep time too: run it dormant so a plant can never
    // die while the board was asleep/reset.
    ticksDuringSleep = true;
    while (ticks--) gameTick();
    ticksDuringSleep = false;
  } else {
    // No usable saved clock (first boot, or the clock restarted after a
    // power cycle): start counting from now.
    growEpoch = nowE;
  }
}

bool addNearbyPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 1;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

void onNearbyData(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || len != (int)sizeof(NearbyPacket)) return;
  NearbyPacket packet;
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != NEARBY_MAGIC) return;
  portENTER_CRITICAL(&nearbyMux);
  inboundNearbyPacket = packet;
  memcpy(inboundNearbyMac, info->src_addr, sizeof(inboundNearbyMac));
  inboundNearbyReady = true;
  portEXIT_CRITICAL(&nearbyMux);
}

void startNearbyRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_err_t result = esp_now_init();
  if (result != ESP_OK && result != ESP_ERR_ESPNOW_EXIST) return;
  esp_now_register_recv_cb(onNearbyData);
  addNearbyPeer(BROADCAST_MAC);
  nearbyReady = true;
  lastNearbyHello = 0;
}

bool sendNearbyPacket(const uint8_t *mac, uint8_t type, uint8_t item,
                      const char *text) {
  if (!nearbyReady || !addNearbyPeer(mac)) return false;
  NearbyPacket packet = {};
  packet.magic = NEARBY_MAGIC;
  packet.type = type;
  packet.item = item;
  strncpy(packet.brand, DEVICE_BRAND, sizeof(packet.brand) - 1);
  strncpy(packet.grower, growerName, sizeof(packet.grower) - 1);
  packet.masteryLevel = currentMasteryLevel();
  if (text) strncpy(packet.text, text, sizeof(packet.text) - 1);
  return esp_now_send(mac, (const uint8_t *)&packet, sizeof(packet)) == ESP_OK;
}

int findOrAddNearbyPeer(const uint8_t *mac, const NearbyPacket &packet) {
  int slot = -1;
  unsigned long oldest = ULONG_MAX;
  int oldestSlot = 0;
  unsigned long now = millis();
  for (int i = 0; i < MAX_NEARBY_PEERS; ++i) {
    if (nearbyPeers[i].active && memcmp(nearbyPeers[i].mac, mac, 6) == 0) {
      slot = i;
      break;
    }
    if (!nearbyPeers[i].active || now - nearbyPeers[i].lastSeen > 20000UL) {
      slot = i;
      break;
    }
    if (nearbyPeers[i].lastSeen < oldest) {
      oldest = nearbyPeers[i].lastSeen;
      oldestSlot = i;
    }
  }
  if (slot < 0) slot = oldestSlot;
  NearbyPeer &peer = nearbyPeers[slot];
  peer.active = true;
  memcpy(peer.mac, mac, 6);
  strncpy(peer.brand, packet.brand[0] ? packet.brand : "Garden", sizeof(peer.brand) - 1);
  peer.brand[sizeof(peer.brand) - 1] = '\0';
  strncpy(peer.grower, packet.grower[0] ? packet.grower : "Grower", sizeof(peer.grower) - 1);
  peer.grower[sizeof(peer.grower) - 1] = '\0';
  peer.masteryLevel = constrain((int)packet.masteryLevel, 1, 10);
  peer.lastSeen = now;
  return slot;
}

// --- Nearby notifications ----------------------------------------------------
// A new message or supply delivery pops a small tappable card over whatever
// page is up: "NEW MESSAGE" + sender. Tapping it opens the NEARBY inbox; it
// also fades out on its own after 12 s. While the NEARBY page itself is open,
// incoming packets only repaint the grower list / inbox text, never the
// whole page.
bool nearbyPopupActive = false;
unsigned long nearbyPopupUntil = 0;
char nearbyPopupFrom[14] = "";
char nearbyPopupTitle[20] = "NEW MESSAGE";

void drawNearbyPopup() {
  canvas.fillRoundRect(168, 197, 147, 39, 8, ACCENT_DARK);
  canvas.drawRoundRect(168, 197, 147, 39, 8, PANEL_LIGHT);
  canvas.setTextColor(TFT_WHITE, ACCENT_DARK);
  canvas.drawCentreString(nearbyPopupTitle, 241, 204, 1);
  canvas.setTextColor(GOLD, ACCENT_DARK);
  canvas.drawCentreString(nearbyPopupFrom, 241, 216, 1);
}

// Repaints just the grower list band. No latest-message line here: messages
// and supply deliveries surface through the popup / inbox instead.
void drawNearbyPeerListOnly() {
  canvas.fillRect(0, 78, SCREEN_W, 100, BG);  // rows + empty-scan box
  int count = 0;
  for (int i = 0; i < MAX_NEARBY_PEERS; ++i) {
    NearbyPeer &peer = nearbyPeers[i];
    if (peer.active && millis() - peer.lastSeen < 20000UL) {
      ++count;
      int y = 80 + i * 23;
      uint16_t fill = (selectedNearbyPeer == i) ? ACCENT_DARK : PANEL;
      canvas.fillRoundRect(13, y, 294, 21, 4, fill);
      canvas.drawRoundRect(13, y, 294, 21, 4, PANEL_LIGHT);
      canvas.setTextColor(TEXT_MAIN, fill);
      canvas.drawString(peer.grower, 20, y + 6, 1);   // just the typed name
      canvas.setTextColor(TEXT_DIM, fill);
      canvas.drawRightString("ONLINE", 298, y + 6, 1);
    } else {
      peer.active = false;
      if (selectedNearbyPeer == i) selectedNearbyPeer = 255;
    }
  }
  if (!count) {
    canvas.fillRoundRect(14, 85, 292, 63, 8, PANEL);
    canvas.drawRoundRect(14, 85, 292, 63, 8, PANEL_LIGHT);
    canvas.setTextColor(TEXT_MAIN, PANEL);
    canvas.drawCentreString(nearbyReady ? "Scanning nearby Wi-Fi devices..." : "Nearby radio unavailable", 160, 103, 2);
    canvas.setTextColor(TEXT_DIM, PANEL);
    canvas.drawCentreString("No router needed; both boards scan together", 160, 127, 1);
  }
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawCentreString(lastNearbyMessage, 160, 171, 1);
}

// Repaints just the inbox message box interior.
void drawNearbyInboxTextOnly() {
  canvas.fillRect(16, 84, 288, 74, PANEL);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString(lastNearbyMessage, 160, 105, 2);
}

// The popup card sits over content that gets partial repaints (garden stats,
// farmer visits, animations) — re-stamp it at the end of each loop pass so it
// always reads as the topmost layer.
void refreshNearbyPopup() {
  if (nearbyPopupActive) drawNearbyPopup();
}

void applyNearbyGift(uint8_t item, uint8_t strain) {
  switch (item) {
    case 1:
      strain = min((int)strain, (int)STRAIN_COUNT - 1);
      seedsByStrain[strain] = min(255, (int)seedsByStrain[strain] + 4);
      seedStrain = strain;
      snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received 4 %s seeds", strainNames[strain]);
      break;
    case 2: pestDefenseKits = min(255, (int)pestDefenseKits + 1); snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received a pest kit"); break;
    case 3: waterPacks = min(255, (int)waterPacks + 2); snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received 2 water packs"); break;
    case 4: careKits = min(255, (int)careKits + 2); snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received 2 care kits"); break;
    default: return;
  }
  saveGame();
}

void serviceNearby() {
  if (!nearbyReady) return;
  unsigned long now = millis();
  if (now - lastNearbyHello >= 2200UL) {
    sendNearbyPacket(BROADCAST_MAC, PACKET_HELLO, 0, "");
    lastNearbyHello = now;
  }

  NearbyPacket packet;
  uint8_t mac[6];
  bool hasPacket = false;
  portENTER_CRITICAL(&nearbyMux);
  if (inboundNearbyReady) {
    packet = inboundNearbyPacket;
    memcpy(mac, inboundNearbyMac, sizeof(mac));
    inboundNearbyReady = false;
    hasPacket = true;
  }
  portEXIT_CRITICAL(&nearbyMux);
  if (!hasPacket) return;

  int peerIndex = findOrAddNearbyPeer(mac, packet);
  // Seed gifts carry the sender's strain in text (one digit). Older senders
  // leave it empty: the receiver then keeps their current strain.
  uint8_t giftStrain = seedStrain;
  if (packet.text[0] >= '0' && packet.text[0] < '0' + STRAIN_COUNT) {
    giftStrain = packet.text[0] - '0';
  }
  if (packet.type == PACKET_MESSAGE) {
    snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "%.25s: %.25s",
             nearbyPeers[peerIndex].grower, packet.text);
  } else if (packet.type == PACKET_GIFT) {
    applyNearbyGift(packet.item, giftStrain);
  } else if (packet.type == PACKET_CASH) {
    uint16_t amount = (uint16_t)atoi(packet.text);
    if (amount) {
      credits = (uint16_t)min(65535, (int)credits + amount);
      snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received $%u", amount);
      saveGame();
    }
  }
  if (page == NEARBY) {
    // Targeted refresh only: keep the header, buttons and layout untouched.
    if (nearbyMode == 0) drawNearbyPeerListOnly();
    else if (nearbyMode == 3) drawNearbyInboxTextOnly();
    return;
  }
  if (packet.type == PACKET_MESSAGE) {
    strlcpy(nearbyPopupTitle, "NEW MESSAGE", sizeof(nearbyPopupTitle));
  } else if (packet.type == PACKET_GIFT) {
    strlcpy(nearbyPopupTitle, "SUPPLIES RECEIVED", sizeof(nearbyPopupTitle));
  } else if (packet.type == PACKET_CASH) {
    strlcpy(nearbyPopupTitle, "MONEY RECEIVED", sizeof(nearbyPopupTitle));
  } else {
    return;   // HELLO beacons and other traffic never pop up
  }
  strlcpy(nearbyPopupFrom, nearbyPeers[peerIndex].grower, sizeof(nearbyPopupFrom));
  nearbyPopupUntil = millis() + 12000UL;
  nearbyPopupActive = true;
  drawNearbyPopup();
}

void sendNearbyHelloNow() {
  if (nearbyReady) sendNearbyPacket(BROADCAST_MAC, PACKET_HELLO, 0, "");
}

void drawButton(int x, int y, int w, int h, const char *label,
                uint16_t fill, uint16_t textColor = TEXT_MAIN) {
  canvas.fillRoundRect(x, y, w, h, 6, fill);
  canvas.drawRoundRect(x, y, w, h, 6, PANEL_LIGHT);
  canvas.setTextColor(textColor, fill);
  int font = (w < 90 || h < 25) ? 1 : 2;
  int textHeight = font == 2 ? 16 : 8;
  canvas.drawCentreString(label, x + w / 2, y + (h - textHeight) / 2, font);
}

void drawFarmerAvatar() {
  uint16_t hair = rgb565(35, 27, 26);
  uint16_t skin = farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143);
  canvas.fillCircle(301, 14, 11, 0x2125);
  if (farmerCharacter == 5) {
    // James: brown-gray, medium-length hair swept up into a pompadour.
    uint16_t jHair = rgb565(124, 110, 97), jLight = rgb565(168, 156, 142);
    canvas.fillRoundRect(294, 6, 14, 13, 5, jHair);
    canvas.fillCircle(301, 14, 6, skin);
    canvas.fillRoundRect(294, 4, 14, 6, 3, jHair);
    canvas.fillCircle(299, 5, 3, jHair);
    canvas.drawFastHLine(296, 4, 8, jLight);
    canvas.fillRect(294, 9, 2, 8, jHair);
    canvas.fillRect(306, 9, 2, 8, jHair);
    canvas.drawPixel(299, 13, TFT_BLACK);
    canvas.drawPixel(303, 13, TFT_BLACK);
    canvas.drawFastHLine(299, 17, 5, rgb565(150, 90, 80));
    canvas.drawFastHLine(299, 16, 5, rgb565(152, 146, 140));   // goatee
    canvas.fillRect(300, 18, 3, 2, rgb565(152, 146, 140));
  } else if (farmerCharacter == 1) {
    canvas.fillRoundRect(293, 4, 16, 8, 5, rgb565(19, 21, 24));
    canvas.fillCircle(301, 14, 7, skin);
    canvas.fillRoundRect(294, 5, 14, 7, 4, rgb565(19, 21, 24));
    canvas.fillRoundRect(295, 12, 5, 4, 1, rgb565(10, 15, 19));
    canvas.fillRoundRect(302, 12, 5, 4, 1, rgb565(10, 15, 19));
    canvas.drawFastHLine(300, 13, 2, rgb565(10, 15, 19));
    canvas.drawFastHLine(298, 17, 7, rgb565(82, 51, 39));
    canvas.fillRect(297, 19, 9, 3, rgb565(205, 207, 202));
  } else if (farmerCharacter == 2) {
    canvas.fillRoundRect(293, 4, 16, 20, 7, hair);
    canvas.fillCircle(301, 14, 6, skin);
    canvas.fillRect(294, 11, 3, 12, hair);
    canvas.fillRect(306, 11, 3, 12, hair);
    canvas.fillRoundRect(295, 11, 5, 4, 1, rgb565(24, 28, 30));
    canvas.fillRoundRect(302, 11, 5, 4, 1, rgb565(24, 28, 30));
    canvas.drawFastHLine(300, 13, 2, rgb565(24, 28, 30));
    canvas.drawFastHLine(297, 17, 8, hair);
    canvas.drawFastHLine(299, 20, 5, TFT_WHITE);
  } else if (farmerCharacter == 4) {
    canvas.fillRoundRect(293, 4, 16, 20, 7, hair);
    canvas.fillCircle(301, 13, 6, skin);
    canvas.fillRoundRect(295, 5, 12, 7, 5, hair);
    canvas.fillRect(294, 11, 3, 12, hair);
    canvas.fillRect(306, 11, 3, 12, hair);
    canvas.fillRoundRect(295, 11, 5, 4, 1, rgb565(10, 13, 16));
    canvas.fillRoundRect(302, 11, 5, 4, 1, rgb565(10, 13, 16));
    canvas.drawFastHLine(300, 13, 2, rgb565(10, 13, 16));
    canvas.drawFastHLine(298, 18, 7, rgb565(82, 51, 39));
  } else if (farmerCharacter == 3) {
    hair = rgb565(210, 176, 104);
    canvas.fillRoundRect(293, 4, 16, 20, 7, hair);
    canvas.fillCircle(301, 13, 6, skin);
    canvas.fillRoundRect(295, 5, 12, 7, 5, hair);
    canvas.drawFastVLine(294, 12, 9, rgb565(239, 216, 151));
    canvas.drawFastVLine(307, 12, 9, rgb565(239, 216, 151));
    canvas.drawPixel(299, 12, TFT_BLACK);
    canvas.drawPixel(303, 12, TFT_BLACK);
    canvas.drawFastHLine(298, 18, 7, rgb565(167, 83, 78));
    canvas.drawFastHLine(299, 19, 5, TFT_WHITE);
  } else if (farmerCharacter == 8) {
    // Pedro: short black hair, dark shades, full beard.
    canvas.fillRoundRect(294, 5, 14, 7, 4, rgb565(30, 26, 24));    // hair
    canvas.fillCircle(301, 13, 6, rgb565(212, 168, 138));
    canvas.fillRect(294, 9, 3, 8, rgb565(30, 26, 24));
    canvas.fillRect(306, 9, 3, 8, rgb565(30, 26, 24));
    canvas.fillRoundRect(295, 10, 5, 4, 1, TFT_BLACK);
    canvas.fillRoundRect(302, 10, 5, 4, 1, TFT_BLACK);
    canvas.drawFastHLine(300, 12, 2, TFT_BLACK);
    canvas.drawFastHLine(297, 16, 8, rgb565(62, 52, 47));          // beard
    canvas.fillRoundRect(296, 17, 10, 5, 2, rgb565(62, 52, 47));
  } else {
    canvas.fillRoundRect(293, 4, 16, 20, 7, hair);
    canvas.fillCircle(301, 13, 6, skin);
    canvas.fillRoundRect(295, 5, 12, 7, 5, hair);
    canvas.fillRect(294, 12, 3, 9, hair);
    canvas.fillRect(306, 12, 3, 9, hair);
    canvas.drawPixel(299, 12, TFT_BLACK);
    canvas.drawPixel(303, 12, TFT_BLACK);
    canvas.drawFastHLine(298, 15, 7, rgb565(52, 37, 33));
    canvas.drawFastHLine(299, 18, 5, TFT_WHITE);
  }
}

void drawFarmerHeadLarge(int cx, int cy, uint8_t character) {
  uint16_t hair = character == 3 ? rgb565(210, 176, 104) : rgb565(35, 27, 26);
  uint16_t skin = character == 2 ? rgb565(133, 84, 61)
                : character == 6 ? rgb565(172, 112, 78)
                : character == 7 ? rgb565(216, 156, 116)
                : rgb565(230, 184, 143);
  uint16_t beard = rgb565(52, 37, 33);
  uint16_t black = rgb565(16, 19, 22);

  if (character == 5) {
    // James: normal mid-length brown-gray hair with a gray goatee.
    uint16_t jHair = rgb565(124, 110, 97), jLight = rgb565(168, 156, 142), jDark = rgb565(88, 76, 66);
    canvas.fillRoundRect(cx - 14, cy - 14, 28, 34, 10, jHair);      // back + sides past the shoulders
    canvas.fillCircle(cx, cy, 10, skin);
    canvas.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, jHair);        // flat fringe
    canvas.fillRect(cx - 14, cy - 6, 5, 20, jHair);                 // mid-length sides to the jaw
    canvas.fillRect(cx + 9, cy - 6, 5, 20, jHair);
    canvas.drawLine(cx - 9, cy - 11, cx + 9, cy - 13, jLight);      // subtle gray highlights
    canvas.drawLine(cx - 10, cy - 3, cx - 10, cy + 9, jLight);
    canvas.drawLine(cx + 10, cy - 3, cx + 10, cy + 9, jDark);
    canvas.drawFastHLine(cx - 6, cy - 4, 4, jDark);                 // brows
    canvas.drawFastHLine(cx + 2, cy - 4, 4, jDark);
    canvas.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);   // dark grow glasses
    canvas.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    canvas.drawFastHLine(cx - 1, cy - 4, 2, black);          // bridge
    canvas.drawFastHLine(cx - 14, cy - 5, 4, black);         // arms into hair
    canvas.drawFastHLine(cx + 10, cy - 5, 4, black);
    canvas.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // glare
    canvas.drawFastHLine(cx - 4, cy + 5, 8, rgb565(150, 90, 80));
    // Grayish goatee: mustache, sides around the mouth, and chin patch.
    uint16_t jGoatee = rgb565(152, 146, 140);
    canvas.drawFastHLine(cx - 4, cy + 3, 8, jGoatee);
    canvas.drawFastHLine(cx - 3, cy + 2, 6, jGoatee);
    canvas.drawFastVLine(cx - 5, cy + 3, 4, jGoatee);
    canvas.drawFastVLine(cx + 4, cy + 3, 4, jGoatee);
    canvas.fillRoundRect(cx - 3, cy + 6, 7, 6, 2, jGoatee);
    return;
  }

  if (character == 1) {
    // Mathias: shoulder-length wavy brown hair, square shades, thick mustache.
    uint16_t mHair = rgb565(96, 64, 40), mHairLight = rgb565(134, 95, 60);
    canvas.fillRoundRect(cx - 14, cy - 14, 28, 31, 10, mHair);      // hair past the jaw
    canvas.fillCircle(cx, cy, 10, skin);
    canvas.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, mHair);        // swept fringe
    canvas.fillRect(cx - 14, cy - 6, 5, 23, mHair);                // long sides
    canvas.fillRect(cx + 9, cy - 6, 5, 23, mHair);
    canvas.drawFastVLine(cx - 12, cy - 2, 18, mHairLight);          // wavy strands
    canvas.drawFastVLine(cx - 11, cy + 2, 15, mHairLight);
    canvas.drawFastVLine(cx + 12, cy - 1, 16, mHairLight);
    canvas.drawFastVLine(cx + 11, cy + 4, 13, mHairLight);
    canvas.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);          // square shades
    canvas.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    canvas.drawFastHLine(cx - 1, cy - 4, 2, black);                 // bridge
    canvas.drawFastHLine(cx - 14, cy - 5, 4, black);                // arms into hair
    canvas.drawFastHLine(cx + 10, cy - 5, 4, black);
    canvas.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // lens glare
    canvas.fillRoundRect(cx - 7, cy + 4, 14, 5, 2, rgb565(110, 74, 48));   // thick mustache
    canvas.fillRect(cx - 1, cy + 4, 2, 2, skin);                    // philtrum gap
    return;
  }

  if (character == 6) {
    // Sergio: buzz cut, clear glasses, short goatee.
    uint16_t buzz = rgb565(45, 32, 26);
    canvas.fillCircle(cx, cy - 2, 11, buzz);                      // buzz: thin crescent
    canvas.fillCircle(cx, cy, 10, skin);                          // face carves it back
    for (int i = -8; i <= 8; i += 4)                              // stubble texture
      canvas.drawFastVLine(cx + i, cy - 12, 3, rgb565(74, 55, 42));
    canvas.fillRect(cx - 10, cy - 5, 2, 2, buzz);                 // sideburn hints
    canvas.fillRect(cx + 8, cy - 5, 2, 2, buzz);
    canvas.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, rgb565(206, 226, 232));  // clear lenses
    canvas.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, rgb565(206, 226, 232));
    canvas.drawRoundRect(cx - 10, cy - 6, 9, 8, 1, black);        // thin frames
    canvas.drawRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    canvas.drawFastHLine(cx - 1, cy - 4, 2, black);
    canvas.drawFastHLine(cx - 14, cy - 5, 4, black);
    canvas.drawFastHLine(cx + 10, cy - 5, 4, black);
    canvas.drawFastHLine(cx - 4, cy + 4, 8, beard);               // short goatee
    canvas.fillRect(cx - 2, cy + 7, 4, 3, beard);
    return;
  }

  if (character == 7) {
    // Vicente: jerry curl, medium length, dark brown.
    uint16_t vHair = rgb565(58, 38, 26), vCurl = rgb565(94, 64, 42);
    canvas.fillRoundRect(cx - 14, cy - 14, 28, 30, 10, vHair);
    canvas.fillCircle(cx, cy, 10, skin);
    canvas.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, vHair);
    canvas.fillRect(cx - 14, cy - 6, 5, 21, vHair);
    canvas.fillRect(cx + 9, cy - 6, 5, 21, vHair);
    for (int i = -12; i <= 12; i += 6)                            // curl texture
      canvas.fillCircle(cx + i, cy - 15 + abs(i) / 3, 2, vCurl);
    canvas.fillCircle(cx - 12, cy - 2, 2, vCurl);
    canvas.fillCircle(cx + 12, cy + 1, 2, vCurl);
    canvas.fillCircle(cx - 12, cy + 5, 2, vCurl);
    canvas.fillCircle(cx + 12, cy + 7, 2, vCurl);
    canvas.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);        // dark grow glasses
    canvas.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    canvas.drawFastHLine(cx - 1, cy - 4, 2, black);
    canvas.drawFastHLine(cx - 14, cy - 5, 4, black);
    canvas.drawFastHLine(cx + 10, cy - 5, 4, black);
    canvas.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));
    return;
  }

  canvas.fillRoundRect(cx - 14, cy - 14, 28, 36, 10, hair);
  if (character == 2) {
    for (int i = -2; i <= 2; ++i) {
      canvas.drawFastVLine(cx + i * 5, cy - 12, 36, rgb565(65, 44, 35));
      canvas.drawFastVLine(cx + i * 5 + 2, cy - 10, 33, hair);
    }
  }
  canvas.fillCircle(cx, cy, 10, skin);

  if (character == 0) {
    canvas.fillRoundRect(cx - 12, cy - 14, 24, 8, 6, hair);
    canvas.fillRect(cx - 13, cy - 6, 5, 28, hair);
    canvas.fillRect(cx + 8, cy - 6, 5, 28, hair);
    canvas.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);   // dark grow glasses
    canvas.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    canvas.drawFastHLine(cx - 1, cy - 4, 2, black);          // bridge
    canvas.drawFastHLine(cx - 14, cy - 5, 4, black);         // arms into hair
    canvas.drawFastHLine(cx + 10, cy - 5, 4, black);
    canvas.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // glare
    canvas.drawFastHLine(cx - 7, cy + 4, 14, beard);
    canvas.drawFastHLine(cx - 5, cy + 7, 10, TFT_WHITE);
    canvas.fillTriangle(cx - 6, cy + 9, cx + 6, cy + 9, cx, cy + 16, beard);
  } else if (character == 2) {
    canvas.fillRoundRect(cx - 10, cy - 5, 9, 7, 2, TFT_BLACK);
    canvas.fillRoundRect(cx + 1, cy - 5, 9, 7, 2, TFT_BLACK);
    canvas.drawLine(cx - 1, cy - 2, cx + 1, cy - 2, black);
    canvas.drawLine(cx - 10, cy - 5, cx - 1, cy - 5, black);
    canvas.drawLine(cx + 1, cy - 5, cx + 10, cy - 5, black);
    canvas.fillTriangle(cx - 8, cy + 4, cx + 8, cy + 4, cx, cy + 17, beard);
    canvas.drawFastHLine(cx - 4, cy + 6, 8, TFT_WHITE);
  } else if (character == 8) {
    // Pedro (photo reference for the head only): short black fade, dark
    // wayfarers, full beard. Same gardener outfit as the other growers.
    uint16_t pHair = rgb565(30, 26, 24);
    uint16_t pSkin = rgb565(212, 168, 138);
    uint16_t pBeard = rgb565(62, 52, 47);
    canvas.fillRoundRect(cx - 12, cy - 12, 24, 24, 8, pHair);   // short hair cap
    canvas.fillCircle(cx, cy, 10, pSkin);
    canvas.fillRoundRect(cx - 11, cy - 11, 22, 5, 4, pHair);    // flat fringe
    canvas.fillRect(cx - 11, cy - 6, 2, 6, pHair);              // tight fade sides
    canvas.fillRect(cx + 9, cy - 6, 2, 6, pHair);
    canvas.fillRoundRect(cx - 9, cy + 2, 18, 13, 6, pBeard);    // full beard
    canvas.fillRoundRect(cx - 2, cy + 3, 4, 2, 1, pSkin);       // mouth peeking
    canvas.drawFastHLine(cx - 8, cy + 4, 5, rgb565(122, 114, 106));  // gray flecks
    canvas.drawFastHLine(cx + 3, cy + 5, 5, rgb565(122, 114, 106));
    canvas.fillRoundRect(cx - 10, cy - 6, 9, 8, 2, black);      // dark wayfarers
    canvas.fillRoundRect(cx + 1, cy - 6, 9, 8, 2, black);
    canvas.drawFastHLine(cx - 1, cy - 4, 2, black);
    canvas.drawLine(cx - 8, cy - 4, cx - 5, cy - 2, rgb565(95, 95, 102));  // lens glare
  } else if (character == 4) {
    // Eric: long dark hair tucked behind both ears and thick black glasses.
    canvas.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, hair);
    canvas.fillRoundRect(cx - 15, cy - 4, 5, 22, 3, hair);
    canvas.fillRoundRect(cx + 10, cy - 4, 5, 22, 3, hair);
    canvas.fillRoundRect(cx - 10, cy - 5, 9, 7, 2, rgb565(10, 13, 16));
    canvas.fillRoundRect(cx + 1, cy - 5, 9, 7, 2, rgb565(10, 13, 16));
    canvas.drawFastHLine(cx - 1, cy - 2, 2, rgb565(10, 13, 16));
    canvas.drawFastHLine(cx - 9, cy - 4, 7, rgb565(96, 107, 111));
    canvas.drawFastHLine(cx + 2, cy - 4, 7, rgb565(96, 107, 111));
    canvas.drawFastHLine(cx - 4, cy + 6, 8, rgb565(112, 65, 52));
  } else {
    canvas.fillRoundRect(cx - 12, cy - 14, 24, 8, 6, hair);
    canvas.fillRect(cx - 13, cy - 6, 5, 29, hair);
    canvas.fillRect(cx + 8, cy - 6, 5, 29, hair);
    canvas.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);   // dark grow glasses
    canvas.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    canvas.drawFastHLine(cx - 1, cy - 4, 2, black);          // bridge
    canvas.drawFastHLine(cx - 14, cy - 5, 4, black);         // arms into hair
    canvas.drawFastHLine(cx + 10, cy - 5, 4, black);
    canvas.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // glare
    canvas.drawFastVLine(cx - 11, cy + 2, 15, rgb565(239, 216, 151));
    canvas.drawFastVLine(cx + 11, cy + 2, 15, rgb565(239, 216, 151));
    canvas.drawFastHLine(cx - 4, cy + 6, 8, rgb565(167, 83, 78));
    canvas.drawFastHLine(cx - 3, cy + 8, 6, TFT_WHITE);
  }
}

void drawCharacterMini(int cx, int top, uint8_t character) {
  uint16_t hair = character == 3 ? rgb565(210, 176, 104)
                : character == 5 ? rgb565(124, 110, 97)
                : character == 6 ? rgb565(45, 32, 26)
                : character == 7 ? rgb565(58, 38, 26)
                : character == 1 ? rgb565(96, 64, 40)
                : rgb565(35, 27, 26);
  uint16_t skin = character == 2 ? rgb565(133, 84, 61)
                : character == 6 ? rgb565(172, 112, 78)
                : character == 7 ? rgb565(216, 156, 116)
                : rgb565(230, 184, 143);
  const uint16_t black = rgb565(13, 16, 19);
  int faceY = top + 8;
  canvas.fillRoundRect(cx - 9, top, 18, character == 6 ? 10 : 15, 7, hair);
  canvas.fillCircle(cx, faceY, 6, skin);
  if (character == 6) {
    canvas.drawFastHLine(cx - 2, faceY + 3, 5, rgb565(52, 37, 33));   // short goatee
    canvas.fillRect(cx - 1, faceY + 4, 2, 2, rgb565(52, 37, 33));
  }
  if (character == 7) {
    canvas.drawFastVLine(cx - 8, top + 6, 8, hair);                  // medium sides
    canvas.drawFastVLine(cx + 7, top + 6, 8, hair);
    for (int i = -6; i <= 6; i += 4)                                 // curl texture
      canvas.drawPixel(cx + i, top + abs(i) / 3, rgb565(94, 64, 42));
  }
  if (character == 5) {
    canvas.drawFastVLine(cx - 8, top + 6, 7, hair);                 // mid-length sides
    canvas.drawFastVLine(cx + 7, top + 6, 7, hair);
    canvas.drawFastHLine(cx - 2, faceY + 3, 5, rgb565(152, 146, 140));  // goatee
    canvas.fillRect(cx - 1, faceY + 4, 3, 2, rgb565(152, 146, 140));
  }
  if (character == 1) {
    canvas.drawFastVLine(cx - 8, top + 6, 8, hair);                 // long sides
    canvas.drawFastVLine(cx + 7, top + 6, 8, hair);
    canvas.drawFastHLine(cx - 3, faceY + 2, 6, rgb565(110, 74, 48));  // mustache
  }
  canvas.fillRoundRect(cx - 6, faceY - 2, 5, 4, 1, black);   // dark grow glasses
  canvas.fillRoundRect(cx + 1, faceY - 2, 5, 4, 1, black);
  canvas.drawFastHLine(cx - 1, faceY, 2, black);
  if (character == 0) canvas.drawFastHLine(cx - 3, faceY + 3, 6, rgb565(52, 37, 33));
  if (character == 8) {
    canvas.drawFastHLine(cx - 4, faceY + 3, 9, rgb565(62, 52, 47));   // full beard
    canvas.fillRoundRect(cx - 5, faceY + 4, 10, 5, 2, rgb565(62, 52, 47));
  }
  if (character == 3) {
    canvas.drawFastVLine(cx - 8, top + 8, 10, rgb565(239, 216, 151));
    canvas.drawFastVLine(cx + 7, top + 8, 10, rgb565(239, 216, 151));
  } else if (character == 4 || character == 0) {
    canvas.drawFastVLine(cx - 8, top + 6, 11, hair);
    canvas.drawFastVLine(cx + 7, top + 6, 11, hair);
  }
  canvas.fillRoundRect(cx - 7, top + 15, 14, 8, 3, rgb565(39, 132, 71));
  canvas.fillRect(cx - 5, top + 23, 4, 4, black);
  canvas.fillRect(cx + 1, top + 23, 4, 4, black);
}

// Cog gear icon for the settings corner.
void drawGearIcon(int cx, int cy) {
  uint16_t metal = rgb565(178, 186, 194);
  uint16_t bg = 0x0861;
  for (int a = 0; a < 8; ++a) {
    float ang = a * PI / 4.0f;
    canvas.fillCircle(cx + (int)(cosf(ang) * 9.0f), cy + (int)(sinf(ang) * 9.0f), 3, metal);
  }
  canvas.fillCircle(cx, cy, 9, metal);
  canvas.fillCircle(cx, cy, 5, bg);
  canvas.fillCircle(cx, cy, 2, metal);
}

void drawHeader() {
  canvas.fillRect(0, 0, SCREEN_W, HEADER_H, 0x0861);
  canvas.drawFastHLine(0, HEADER_H - 1, SCREEN_W, 0x4D89);
  drawLeafLogo(7, 2, 1, TFT_WHITE);
  canvas.setTextColor(TEXT_MAIN, 0x0861);
  canvas.drawString("WINFLORIDA", 35, 4, 2);
  canvas.setTextColor(TEXT_DIM, 0x0861);
  canvas.drawString(growerName, 36, 18, 1);

  char money[14];
  snprintf(money, sizeof(money), "$%u", credits);
  canvas.setTextColor(GOLD, 0x0861);
  canvas.drawRightString(money, 275, 6, 2);

  // Stoned meter: 10 levels, from Sober to Stoned as FUCK.
  uint8_t sIdx = min(9, (int)(stonedLevel / 10));
  uint8_t lit = min(10, (int)((stonedLevel + 9) / 10));
  drawCharacterMini(118, 0, farmerCharacter);   // mini head: current grower
  canvas.setTextColor(TEXT_DIM, 0x0861);
  canvas.drawString(stonedLevelNames[sIdx], 131, 3, 1);
  canvas.fillRoundRect(130, 14, 90, 7, 3, 0x0630);
  for (uint8_t i = 0; i < lit; ++i) {
    uint16_t seg = i < 3 ? rgb565(70, 180, 90)
                 : i < 6 ? rgb565(230, 200, 60)
                 : i < 8 ? rgb565(240, 140, 40)
                 : rgb565(235, 60, 60);
    canvas.fillRect(124 + i * 9, 15, 8, 5, seg);
  }

  drawGearIcon(301, 14);
}

void drawNav() {
  const char *labels[] = {"GARDEN", "SUPPLY", "MARKET", "STASH", "NEARBY"};
  const Page pages[] = {GARDEN, SUPPLIES, MARKET, STASH, NEARBY};
  canvas.fillRect(0, NAV_Y, SCREEN_W, NAV_H, 0x0861);
  for (int i = 0; i < 5; ++i) {
    int x = i * 64;
    Page tabPage = pages[i];
    uint16_t fill = (page == tabPage) ? ACCENT_DARK : 0x18E3;
    canvas.fillRoundRect(x + 2, NAV_Y + 3, 60, 27, 5, fill);
    canvas.setTextColor((page == tabPage) ? TFT_WHITE : TEXT_DIM, fill);
    canvas.drawCentreString(labels[i], x + 32, NAV_Y + 11, 1);
  }
}

void drawStatBar(int x, int y, const char *label, int value, uint16_t color) {
  char valueText[8];
  snprintf(valueText, sizeof(valueText), "%d%%", value);
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawString(label, x, y, 1);
  canvas.setTextColor(TEXT_MAIN, BG);
  canvas.drawRightString(valueText, x + 124, y, 1);
  canvas.fillRoundRect(x, y + 12, 124, 9, 4, 0x0841);
  int fillWidth = (124 * value) / 100;
  if (fillWidth > 0) canvas.fillRoundRect(x, y + 12, fillWidth, 9, 4, color);
}

void drawLeaflet(int bx, int by, int tx, int ty, int width,
                 uint16_t darkColor, uint16_t lightColor, uint16_t veinColor) {
  float dx = (float)(tx - bx);
  float dy = (float)(ty - by);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) return;
  float px = -dy / len;
  float py = dx / len;
  int mx = (bx + tx) / 2;
  int my = (by + ty) / 2;
  int lx = mx + (int)(px * width * 0.5f);
  int ly = my + (int)(py * width * 0.5f);
  int rx = mx - (int)(px * width * 0.5f);
  int ry = my - (int)(py * width * 0.5f);

  canvas.fillTriangle(bx, by, lx, ly, tx, ty, darkColor);
  canvas.fillTriangle(bx, by, tx, ty, rx, ry, lightColor);
  canvas.drawLine(bx, by, tx, ty, veinColor);

  // Tiny edge notches suggest the serrated edge of a cannabis leaflet.
  for (int n = 1; n <= 2; ++n) {
    float t = 0.34f + n * 0.19f;
    int ex = bx + (int)(dx * t) + (int)(px * width * 0.34f);
    int ey = by + (int)(dy * t) + (int)(py * width * 0.34f);
    canvas.drawLine(ex, ey, ex + (int)(px * 2), ey + (int)(py * 2), veinColor);
    ex = bx + (int)(dx * t) - (int)(px * width * 0.34f);
    ey = by + (int)(dy * t) - (int)(py * width * 0.34f);
    canvas.drawLine(ex, ey, ex - (int)(px * 2), ey - (int)(py * 2), veinColor);
  }
}

void drawFanLeaf(int x, int y, float scale, int tiltDegrees) {
  const uint16_t leafDark = rgb565(20, 76, 35);
  const uint16_t leafMid = rgb565(38, 130, 58);
  const uint16_t leafLight = rgb565(78, 166, 75);
  const uint16_t vein = rgb565(148, 194, 111);
  const int offsets[] = {-66, -44, -22, 0, 22, 44, 66};
  const int lengths[] = {19, 27, 34, 43, 34, 27, 19};

  for (int i = 0; i < 7; ++i) {
    float angle = (tiltDegrees + offsets[i]) * 0.0174532925f;
    int length = (int)(lengths[i] * scale);
    int tx = x + (int)(sinf(angle) * length);
    int ty = y - (int)(cosf(angle) * length);
    int width = (int)((i == 3 ? 9 : (i == 2 || i == 4 ? 7 : 5)) * scale);
    if (width < 3) width = 3;
    drawLeaflet(x, y, tx, ty, width, leafDark, leafMid, vein);
    if (i == 3) {
      // Highlight the central leaflet for depth.
      canvas.drawLine(x, y, tx, ty, leafLight);
    }
  }
}

void drawPot() {
  uint16_t potDark = rgb565(91, 49, 33);
  uint16_t potMid = rgb565(157, 82, 49);
  uint16_t potLight = rgb565(205, 125, 76);
  canvas.fillRoundRect(62, 141, 54, 26, 5, potDark);
  canvas.fillRoundRect(65, 140, 48, 23, 4, potMid);
  canvas.fillRect(68, 142, 8, 18, potLight);
  canvas.drawFastHLine(65, 141, 48, rgb565(231, 159, 102));
  canvas.fillRoundRect(59, 137, 60, 8, 3, potDark);
  canvas.fillRoundRect(61, 137, 56, 5, 2, potMid);
  canvas.drawFastHLine(66, 138, 46, potLight);
  canvas.fillRect(65, 139, 48, 3, rgb565(74, 47, 29));
  canvas.fillCircle(73, 140, 2, rgb565(137, 94, 57));
  canvas.fillCircle(92, 139, 2, rgb565(137, 94, 57));
  canvas.fillCircle(106, 140, 2, rgb565(137, 94, 57));
  // Show the stem emerging from the soil after the pot is drawn in front.
  if (plantActive) {
    canvas.drawFastVLine(89, 135, 7, rgb565(35, 104, 48));
    canvas.drawPixel(90, 137, rgb565(111, 163, 78));
  }
}

void drawGardenBackdrop() {
  // A dim greenhouse gradient gives the foliage contrast on the small TFT.
  for (int y = HEADER_H; y < 172; ++y) {
    float blend = (float)(y - HEADER_H) / 144.0f;
    uint8_t r = 5 + (uint8_t)(blend * 8);
    uint8_t g = 20 + (uint8_t)(blend * 24);
    uint8_t b = 22 + (uint8_t)(blend * 15);
    canvas.drawFastHLine(0, y, 178, rgb565(r, g, b));
  }
  canvas.drawFastVLine(12, 31, 135, rgb565(29, 53, 51));
  canvas.drawFastVLine(165, 31, 135, rgb565(29, 53, 51));
  canvas.drawFastHLine(12, 57, 153, rgb565(23, 44, 42));
  canvas.drawFastHLine(12, 111, 153, rgb565(23, 44, 42));
  canvas.fillRect(0, 166, 178, 6, rgb565(32, 49, 39));
  canvas.drawFastHLine(0, 166, 178, rgb565(76, 91, 61));
}

void switchToSdBus() {
  touchscreenSPI.end();
  touchscreenSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
}

void switchToTouchBus() {
  touchscreenSPI.end();
  touchscreenSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
}

uint16_t readLE16(File &file) {
  uint8_t bytes[2];
  if (file.read(bytes, 2) != 2) return 0;
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

uint32_t readLE32(File &file) {
  uint8_t bytes[4];
  if (file.read(bytes, 4) != 4) return 0;
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool drawPlantBitmap() {
  if (!sdMounted || !plantActive) return false;

  char filename[16];
  snprintf(filename, sizeof(filename), "/PLANT%d.BMP", stageForGrowth());
  switchToSdBus();
  File file = SD.open(filename, FILE_READ);
  bool drawn = false;

  if (file) {
    file.seek(10);
    uint32_t pixelOffset = readLE32(file);
    file.seek(18);
    int32_t width = (int32_t)readLE32(file);
    int32_t height = (int32_t)readLE32(file);
    file.seek(28);
    uint16_t bitsPerPixel = readLE16(file);
    uint32_t compression = readLE32(file);

    if (width == ART_W && abs(height) == ART_H &&
        bitsPerPixel == 24 && compression == 0) {
      uint32_t rowBytes = ((uint32_t)width * 3 + 3) & ~3UL;
      bool oldSwap = canvas.getSwapBytes();
      canvas.setSwapBytes(true);
      file.seek(pixelOffset);

      drawn = true;
      for (int row = 0; row < ART_H; ++row) {
        if (file.read(bmpRow, rowBytes) != rowBytes) {
          drawn = false;
          break;
        }
        for (int col = 0; col < ART_W; ++col) {
          uint8_t b = bmpRow[col * 3 + 0];
          uint8_t g = bmpRow[col * 3 + 1];
          uint8_t r = bmpRow[col * 3 + 2];
          rgbRow[col] = rgb565(r, g, b);
        }
        int screenY = height > 0 ? (ART_H - 1 - row) : row;
        canvas.pushImage(0, HEADER_H + screenY, ART_W, 1, rgbRow);
      }
      canvas.setSwapBytes(oldSwap);
    }
    file.close();
  }

  switchToTouchBus();
  return drawn;
}

bool drawPlantBitmapFullScreenRegion(int xStart, int yStart, int xEnd, int yEnd) {
  if (!sdMounted || !plantActive) return false;

  char filename[16];
  snprintf(filename, sizeof(filename), "/PLANT%d.BMP", stageForGrowth());
  switchToSdBus();
  File file = SD.open(filename, FILE_READ);
  bool drawn = false;
  if (file) {
    file.seek(10);
    uint32_t pixelOffset = readLE32(file);
    file.seek(18);
    int32_t width = (int32_t)readLE32(file);
    int32_t height = (int32_t)readLE32(file);
    file.seek(28);
    uint16_t bitsPerPixel = readLE16(file);
    uint32_t compression = readLE32(file);

    if (width == ART_W && abs(height) == ART_H && bitsPerPixel == 24 && compression == 0) {
      uint32_t rowBytes = ((uint32_t)width * 3 + 3) & ~3UL;
      bool oldSwap = canvas.getSwapBytes();
      canvas.setSwapBytes(true);
      file.seek(pixelOffset);
      drawn = true;
      for (int row = 0; row < ART_H; ++row) {
        if (file.read(bmpRow, rowBytes) != rowBytes) {
          drawn = false;
          break;
        }
        for (int col = 0; col < ART_W; ++col) {
          uint8_t b = bmpRow[col * 3 + 0];
          uint8_t g = bmpRow[col * 3 + 1];
          uint8_t r = bmpRow[col * 3 + 2];
          rgbRow[col] = rgb565(r, g, b);
          int x0 = (col * SCREEN_W) / ART_W;
          int x1 = ((col + 1) * SCREEN_W) / ART_W;
          for (int x = x0; x < x1; ++x) scaledPlantRow[x] = rgbRow[col];
        }
        int screenY = height > 0 ? ART_H - 1 - row : row;
        int y0 = (screenY * SCREEN_H) / ART_H;
        int y1 = ((screenY + 1) * SCREEN_H) / ART_H;
        for (int y = y0; y < y1; ++y) {
          if (y >= yStart && y < yEnd) {
            canvas.pushImage(xStart, y, xEnd - xStart, 1, &scaledPlantRow[xStart]);
          }
        }
      }
      canvas.setSwapBytes(oldSwap);
    }
    file.close();
  }
  switchToTouchBus();
  return drawn;
}

bool drawPlantBitmapFullScreen() {
  return drawPlantBitmapFullScreenRegion(0, 0, SCREEN_W, SCREEN_H);
}

void drawMiniRoomPlant(int cx, int potBase, float scale) {
  uint16_t potDark = rgb565(92, 49, 34);
  uint16_t potMid = rgb565(164, 86, 55);
  uint16_t potLight = rgb565(203, 126, 78);
  int potW = (int)(25 * scale);
  int potH = (int)(15 * scale);
  canvas.fillRoundRect(cx - potW / 2, potBase - potH, potW, potH, 3, potDark);
  canvas.fillRoundRect(cx - potW / 2 + 2, potBase - potH, potW - 4, potH - 2, 2, potMid);
  canvas.drawFastVLine(cx - potW / 3, potBase - potH + 2, potH - 5, potLight);
  canvas.fillRoundRect(cx - potW / 2 - 2, potBase - potH - 3, potW + 4, 5, 2, potLight);
  if (!plantActive) return;

  int stage = stageForGrowth();
  int top = potBase - potH - (int)((20 + growth * 0.42f) * scale);
  canvas.drawLine(cx, potBase - potH, cx, top + 4, rgb565(35, 104, 48));
  if (stage >= 1) {
    int y = potBase - potH - (int)(15 * scale);
    drawFanLeaf(cx - (int)(8 * scale), y, scale * 0.58f, -35);
    drawFanLeaf(cx + (int)(8 * scale), y, scale * 0.58f, 35);
  }
  if (stage >= 2) {
    int y = potBase - potH - (int)(26 * scale);
    drawFanLeaf(cx - (int)(7 * scale), y, scale * 0.66f, -34);
    drawFanLeaf(cx + (int)(7 * scale), y, scale * 0.66f, 34);
  }
  drawFanLeaf(cx, top + 4, scale * 0.72f, 0);

  // Repaint the front of the pot after its foliage so the stalk visibly meets
  // the soil instead of sitting on top of the pot wall.
  canvas.fillRoundRect(cx - potW / 2, potBase - potH, potW, potH, 3, potDark);
  canvas.fillRoundRect(cx - potW / 2 + 2, potBase - potH, potW - 4, potH - 2, 2, potMid);
  canvas.drawFastVLine(cx - potW / 3, potBase - potH + 2, potH - 5, potLight);
  canvas.fillRoundRect(cx - potW / 2 - 2, potBase - potH - 3, potW + 4, 5, 2, potLight);
  canvas.fillRoundRect(cx - potW / 2 + 4, potBase - potH - 1, potW - 8, 3, 2,
                    rgb565(67, 44, 30));
  canvas.drawFastVLine(cx, potBase - potH - 3, 6, rgb565(35, 104, 48));
  canvas.drawPixel(cx + 1, potBase - potH - 1, rgb565(111, 163, 78));
}

void drawFullScreenGardenFallback() {
  for (int y = 0; y < SCREEN_H; ++y) {
    float blend = (float)y / SCREEN_H;
    uint8_t r = 5 + (uint8_t)(blend * 10);
    uint8_t g = 19 + (uint8_t)(blend * 30);
    uint8_t b = 21 + (uint8_t)(blend * 20);
    canvas.drawFastHLine(0, y, SCREEN_W, rgb565(r, g, b));
  }
  canvas.drawFastVLine(13, 0, SCREEN_H - 14, rgb565(29, 53, 51));
  canvas.drawFastVLine(306, 0, SCREEN_H - 14, rgb565(29, 53, 51));
  canvas.drawFastHLine(13, 53, 294, rgb565(23, 44, 42));
  canvas.drawFastHLine(13, 106, 294, rgb565(23, 44, 42));
  canvas.fillRect(0, 220, SCREEN_W, 8, rgb565(32, 49, 39));
  canvas.drawFastHLine(0, 220, SCREEN_W, rgb565(76, 91, 61));
  drawMiniRoomPlant(48, 220, 1.05f);
  drawMiniRoomPlant(122, 220, 1.2f);
  drawMiniRoomPlant(198, 220, 1.2f);
  drawMiniRoomPlant(272, 220, 1.05f);
}

void restoreFullScreenFallbackRegion() {
  const int left = 178;
  const int right = SCREEN_W;
  for (int y = 80; y < 210; ++y) {
    float blend = (float)y / SCREEN_H;
    uint8_t r = 5 + (uint8_t)(blend * 10);
    uint8_t g = 19 + (uint8_t)(blend * 30);
    uint8_t b = 21 + (uint8_t)(blend * 20);
    canvas.drawFastHLine(left, y, right - left, rgb565(r, g, b));
  }
  canvas.drawFastVLine(306, 80, 130, rgb565(29, 53, 51));
  canvas.drawFastHLine(left, 106, right - left, rgb565(23, 44, 42));
  drawMiniRoomPlant(198, 220, 1.2f);
  drawMiniRoomPlant(272, 220, 1.05f);
}

void drawPlantFullScreenPage() {
  if (!plantActive || !drawPlantBitmapFullScreen()) drawFullScreenGardenFallback();
  drawButton(264, 5, 48, 23, "CLOSE", ACCENT_DARK);
  canvas.fillRoundRect(87, 219, 146, 17, 4, 0x0861);
  canvas.setTextColor(TEXT_MAIN, 0x0861);
  canvas.drawCentreString("TAP ANYWHERE TO RETURN", 160, 223, 1);
  lastDrawnStage = stageForGrowth();
}

uint8_t plantedPotCount() {
  uint8_t count = 0;
  for (int i = 0; i < PLANTS_PER_ROOM; ++i) {
    if (plantedPotMask & (1 << i)) ++count;
  }
  return count;
}

void drawPlantingPage() {
  canvas.fillScreen(BG);
  canvas.setTextColor(TEXT_MAIN, BG);
  canvas.drawCentreString("START YOUR GARDEN", 160, 12, 2);
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawCentreString("Tap each seed; it plants in the next open pot", 160, 34, 1);
  // Strain picker: shown while the batch is untouched and you hold seeds of
  // more than one strain — the whole room grows the picked strain.
  if (plantedPotCount() == 0 && countSeededStrains() > 1) {
    canvas.fillRoundRect(58, 56, 204, 22, 5, PANEL);
    canvas.drawRoundRect(58, 56, 204, 22, 5, PANEL_LIGHT);
    canvas.setTextColor(ACCENT, PANEL);
    canvas.drawCentreString(strainNames[plantStrain], 160, 62, 1);
    drawButton(8, 57, 42, 20, "<", PANEL_LIGHT);
    drawButton(270, 57, 42, 20, ">", PANEL_LIGHT);
  }
  for (int i = 0; i < PLANTS_PER_ROOM; ++i) drawPlantingPot(i);
  drawPlantingTray();
}

void drawPlantingPot(int index) {
  const int potX[] = {48, 122, 198, 272};
  uint16_t potDark = rgb565(91, 49, 33);
  uint16_t potMid = rgb565(157, 82, 49);
  uint16_t potLight = rgb565(205, 125, 76);
  uint16_t soil = rgb565(67, 44, 30);
  int cx = potX[index];
  bool planted = plantedPotMask & (1 << index);
  canvas.fillRect(cx - 24, 115, 48, 56, BG);
  canvas.fillRoundRect(cx - 18, 125, 36, 39, 5, potDark);
  canvas.fillRoundRect(cx - 15, 127, 30, 34, 4, potMid);
  canvas.drawFastVLine(cx - 10, 131, 26, potLight);
  canvas.fillRoundRect(cx - 22, 120, 44, 9, 4, potDark);
  canvas.fillRoundRect(cx - 19, 121, 38, 5, 3, potMid);
  canvas.fillRoundRect(cx - 16, 122, 32, 3, 2, soil);
  if (planted) {
    canvas.drawFastVLine(cx, 116, 8, rgb565(35, 104, 48));
    drawFanLeaf(cx, 116, 0.28f, 0);
  } else {
    canvas.drawCircle(cx, 124, 2, rgb565(125, 87, 55));
  }
  char label[12];
  snprintf(label, sizeof(label), "POT %u", (unsigned)(index + 1));
  canvas.fillRect(cx - 25, 165, 50, 12, BG);
  canvas.setTextColor(planted ? ACCENT : TEXT_DIM, BG);
  canvas.drawCentreString(planted ? "PLANTED" : label, cx, 168, 1);
}

void drawPlantingTray() {
  canvas.fillRect(10, 184, 300, 52, BG);
  canvas.fillRoundRect(13, 187, 294, 39, 7, PANEL);
  canvas.drawRoundRect(13, 187, 294, 39, 7, PANEL_LIGHT);
  uint8_t remaining = min((int)(PLANTS_PER_ROOM - plantedPotCount()), (int)seedsByStrain[plantStrain]);
  const int seedX[] = {43, 121, 199, 277};
  for (int i = 0; i < remaining; ++i) {
    canvas.fillCircle(seedX[i], 205, 7, rgb565(152, 111, 67));
    canvas.drawPixel(seedX[i] - 2, 202, rgb565(222, 191, 126));
  }
  char progress[24];
  snprintf(progress, sizeof(progress), "%u OF 4 PLANTED", plantedPotCount());
  canvas.setTextColor(GOLD, BG);
  canvas.drawCentreString(progress, 160, 229, 1);
}

void plantSeedFromTap(int x, int y) {
  if (y < 188 || y > 228 || plantedPotCount() >= PLANTS_PER_ROOM || seedsByStrain[plantStrain] == 0) return;
  const int seedX[] = {43, 121, 199, 277};
  uint8_t remaining = min((int)(PLANTS_PER_ROOM - plantedPotCount()), (int)seedsByStrain[plantStrain]);
  bool tappedSeed = false;
  for (int i = 0; i < remaining; ++i) {
    if (abs(x - seedX[i]) <= 18) {
      tappedSeed = true;
      break;
    }
  }
  if (!tappedSeed) return;

  int plantedPot = -1;
  for (int pot = 0; pot < PLANTS_PER_ROOM; ++pot) {
    if ((plantedPotMask & (1 << pot)) == 0) {
      plantedPotMask |= (1 << pot);
      --seedsByStrain[plantStrain];
      plantedPot = pot;
      break;
    }
  }
  if (plantedPotMask == 0x0F) {
    firstPlantingDone = true;
    plantActive = true;
    activeStrain = plantStrain;
    growth = 0;
    hydration = 65;
    vigor = 80;
    pestType = 0;
    pestSeverity = 0;
    resetDefoliationForNewPlant();
    page = pageAfterPlanting;
  }
  saveGame();
  if (page == PLANTING) {
    if (plantedPot >= 0) drawPlantingPot(plantedPot);
    drawPlantingTray();
    presentFrame();
  } else render();
}

void drawPlant() {
  if (drawPlantBitmap()) return;
  drawGardenBackdrop();
  // Built-in fallback still shows a whole room when no microSD card is present.
  drawMiniRoomPlant(38, 166, 0.68f);
  drawMiniRoomPlant(89, 157, 0.46f);
  drawMiniRoomPlant(140, 166, 0.68f);
  if (!plantActive) {
    drawPot();
    canvas.setTextColor(TEXT_DIM, rgb565(12, 33, 35));
    canvas.drawCentreString("Plant a seed to begin", 89, 91, 1);
    return;
  }

  int stage = stageForGrowth();
  float size = 0.48f + (growth / 100.0f) * 0.55f;
  int stemTop = 133 - (int)(growth * 0.48f);
  int stemX = 89;
  uint16_t stemDark = rgb565(22, 86, 43);
  uint16_t stemLight = rgb565(111, 163, 78);

  // Main stem and branching nodes.
  canvas.drawLine(stemX, 139, stemX, stemTop, stemDark);
  canvas.drawLine(stemX + 1, 138, stemX + 1, stemTop + 1, stemLight);

  if (stage >= 1) {
    int y = 127 - (int)(growth * 0.13f);
    canvas.drawLine(stemX, y + 7, 74, y + 1, stemDark);
    drawFanLeaf(74, y + 1, size * 0.70f, -34);
    canvas.drawLine(stemX, y + 7, 104, y + 1, stemDark);
    drawFanLeaf(104, y + 1, size * 0.70f, 34);
  }
  if (stage >= 2) {
    int y = 106 - (int)(growth * 0.10f);
    canvas.drawLine(stemX, y + 8, 69, y + 2, stemDark);
    drawFanLeaf(69, y + 2, size * 0.82f, -37);
    canvas.drawLine(stemX, y + 8, 109, y + 2, stemDark);
    drawFanLeaf(109, y + 2, size * 0.82f, 37);
  }
  if (stage >= 3) {
    int y = 84 - (int)(growth * 0.06f);
    canvas.drawLine(stemX, y + 6, 77, y + 1, stemDark);
    drawFanLeaf(77, y + 1, size * 0.72f, -30);
    canvas.drawLine(stemX, y + 6, 101, y + 1, stemDark);
    drawFanLeaf(101, y + 1, size * 0.72f, 30);
  }

  // Top fan and small flower clusters appear as the plant matures.
  drawFanLeaf(stemX, stemTop + 5, size * 0.88f, 0);
  if (stage == 4) {
    uint16_t bud = rgb565(83, 135, 67);
    uint16_t budLight = rgb565(150, 173, 104);
    for (int i = 0; i < 7; ++i) {
      int bx = 83 + (i % 3) * 6;
      int by = stemTop + 15 + (i / 3) * 5;
      canvas.fillCircle(bx, by, 3, bud);
      canvas.fillCircle(bx - 1, by - 1, 1, budLight);
    }
  }
  // The large front pot is the last foreground layer, with a short visible
  // stalk crossing the soil line to make the root point clear.
  drawPot();
}

void drawGardenSidePanel() {
  canvas.fillRect(178, HEADER_H, 142, 144, BG);
  canvas.setTextColor(TEXT_MAIN, BG);
  canvas.drawString(stageName(), 188, 36, 2);
  char dayText[18];
  snprintf(dayText, sizeof(dayText), "DAY %02u", (unsigned)(1 + gameMinutes / 20));
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawString(dayText, 188, 54, 1);
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawRightString(currentStrainName(), 312, 60, 1);
  drawStatBar(188, 69, "GROWTH", plantActive ? growth : 0, ACCENT);
  drawStatBar(188, 101, "HYDRATION", plantActive ? hydration : 0, 0x3DDF);
  drawStatBar(188, 133, "VIGOR", plantActive ? vigor : 0, GOLD);

  canvas.fillRoundRect(186, 157, 132, 13, 4, pestType ? 0x6000 : 0x10A3);
  canvas.setTextColor(pestType ? TFT_YELLOW : 0x9EFB, pestType ? 0x6000 : 0x10A3);
  if (pestType) {
    const char *pestNames[] = {"", "MITES", "APHIDS", "GNATS"};
    char pestText[22];
    snprintf(pestText, sizeof(pestText), "%s  %u%%", pestNames[pestType], pestSeverity);
    canvas.drawCentreString(pestText, 252, 159, 1);
  } else {
    canvas.drawCentreString("NO PESTS", 252, 159, 1);
  }
}

void drawGardenZoomChip() {
  canvas.fillRoundRect(7, 31, 70, 13, 4, 0x0861);
  canvas.setTextColor(TEXT_DIM, 0x0861);
  canvas.drawCentreString("TAP TO ZOOM", 42, 34, 1);
}

void drawGardenPage() {
  drawPlant();
  drawGardenZoomChip();
  drawGardenSidePanel();
  drawGardenActions();
  lastDrawnStage = stageForGrowth();
}

void drawGardenActions() {
  canvas.fillRect(0, ACTION_Y, SCREEN_W, ACTION_H, BG);
  if (plantActive) {
    // Green = good to use, red = using it now can kill the plant,
    // gray = no supplies / not the right time.
    const uint16_t BTN_GREEN = 0x2C64;
    const uint16_t BTN_RED = 0xA000;
    // While growing, the three buttons stretch across the full row.
    // When HARVEST is available it takes the fourth slot again.
    bool harvestReady = growth >= 100;
    int waterX = 3,   waterW = harvestReady ? 76 : 103;
    int defolX = harvestReady ? 82 : 109,  defolW = harvestReady ? 76 : 102;
    int treatX = harvestReady ? 161 : 214, treatW = harvestReady ? 76 : 103;
    if (waterPacks == 0) drawButton(waterX, ACTION_Y + 1, waterW, 26, "WATER", PANEL_LIGHT, TEXT_DIM);
    else drawButton(waterX, ACTION_Y + 1, waterW, 26, "WATER", wateringWouldDrown() ? BTN_RED : BTN_GREEN, TFT_WHITE);
    uint8_t defState = defoliateButtonState();
    if (defState == 0) drawButton(defolX, ACTION_Y + 1, defolW, 26, "DEFOLIATE", PANEL_LIGHT, TEXT_DIM);
    else drawButton(defolX, ACTION_Y + 1, defolW, 26, "DEFOLIATE", defState == 2 ? BTN_RED : BTN_GREEN, TFT_WHITE);
    drawButton(treatX, ACTION_Y + 1, treatW, 26, "TREAT", pestType ? 0x6800 : PANEL);
    if (growth >= 100) {
      drawButton(240, ACTION_Y + 1, 77, 26, "HARVEST", ACCENT_DARK);
    }
  } else {
    drawButton(80, ACTION_Y + 1, 160, 26, totalSeeds() >= PLANTS_PER_ROOM ? "PLANT ROOM" : "BUY SEEDS", ACCENT_DARK);
  }
}

void drawPageTitle(const char *title, const char *subtitle) {
  canvas.setTextColor(TEXT_MAIN, BG);
  canvas.drawString(title, 12, 40, 2);
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawString(subtitle, 12, 60, 1);
}

void drawShopCard(int y, const char *label, const char *buttonText, bool enabled) {
  canvas.fillRoundRect(10, y, 300, 21, 4, PANEL);
  canvas.drawRoundRect(10, y, 300, 21, 4, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(label, 18, y + 6, 1);
  drawButton(257, y + 1, 45, 19, buttonText, enabled ? ACCENT_DARK : PANEL_LIGHT,
             enabled ? TFT_WHITE : TEXT_DIM);
}

// Big-card grid style shared with the seed catalog: the whole card is the
// tap target, an ACCENT border means "affordable right now".
void drawSupplyCard(int x, int y, const char *label, const char *value, bool affordable) {
  uint16_t fill = PANEL;
  canvas.fillRoundRect(x, y, 152, 40, 5, fill);
  canvas.drawRoundRect(x, y, 152, 40, 5, affordable ? ACCENT : PANEL_LIGHT);
  canvas.setTextColor(affordable ? TEXT_MAIN : TEXT_DIM, fill);
  canvas.drawCentreString(label, x + 76, y + 4, 2);
  canvas.setTextColor(affordable ? GOLD : TEXT_DIM, fill);
  canvas.drawCentreString(value, x + 76, y + 25, 1);
}

void drawSuppliesPage() {
  drawPageTitle("SUPPLY SHOP", "Tap a card to buy or open");
  char owned[24];
  snprintf(owned, sizeof(owned), "x%u owned - $5", waterPacks);
  drawSupplyCard(5, 72, "WATER +3", owned, credits >= 5);
  snprintf(owned, sizeof(owned), "x%u owned - $7", careKits);
  drawSupplyCard(163, 72, "CARE KITS +3", owned, credits >= 7);
  drawSupplyCard(5, 114, "CHOOSE A SEED", "STRAIN CATALOG", true);
  snprintf(owned, sizeof(owned), "x%u owned - $10", pestDefenseKits);
  drawSupplyCard(163, 114, "PEST DEF +2", owned, credits >= 10);
  snprintf(owned, sizeof(owned), "x%u owned - $12", remediationKits);
  drawSupplyCard(5, 156, "REMEDIATE +2", owned, credits >= 12);
  drawSupplyCard(163, 156, "HASH ROOM",
                 hashRoomUnlocked ? "OPEN THE ROOM" : "$150",
                 hashRoomUnlocked || credits >= HASH_ROOM_COST);
}

void drawSeedShopPage() {
  char subtitle[48];
  uint8_t harvestedCount = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    if (harvestedStrainMask & ((uint16_t)1 << i)) ++harvestedCount;
  }
  if (harvestedCount < 9) {
    snprintf(subtitle, sizeof(subtitle), "Harvested %u/9; next: %s", harvestedCount,
             strainNames[strainCatalogOrder[strainUnlockedThrough]]);
  } else if (harvestedCount == 9) {
    strlcpy(subtitle, "9/9 harvested; Toad Venom unlocked", sizeof(subtitle));
  } else {
    strlcpy(subtitle, "All 10 strains harvested", sizeof(subtitle));
  }
  drawPageTitle("SEED CATALOG", subtitle);
  for (uint8_t p = 0; p < STRAIN_COUNT; ++p) {
    // 5x2 grid of big strain cards; the whole card is the tap target.
    uint8_t i = strainCatalogOrder[p];   // display in easy -> hard order
    int x = (p % 2) ? 163 : 5;
    int y = 70 + (p / 2) * 27;
    bool unlocked = p <= strainUnlockedThrough;
    bool selected = seedsByStrain[i] > 0;
    const char *tag = "BUY";
    bool chipTag = false;
    if (!unlocked) tag = "LOCK";
    else if (selected) tag = "OWNED";
    else if (totalSeeds() > 95) tag = "FULL";
    else if (credits < strainPrices[i]) tag = "$$$";
    else chipTag = true;
    uint16_t fill = selected ? 0x1C64 : PANEL;
    canvas.fillRoundRect(x, y, 152, 25, 5, fill);
    canvas.drawRoundRect(x, y, 152, 25, 5, selected || chipTag ? ACCENT : PANEL_LIGHT);
    canvas.setTextColor(unlocked ? TEXT_MAIN : TEXT_DIM, fill);
    canvas.drawString(strainNames[i], x + 5, y + 3, 1);
    if (chipTag) {
      canvas.fillRect(x + 104, y + 2, 46, 11, ACCENT_DARK);
      canvas.setTextColor(TFT_WHITE, ACCENT_DARK);
      canvas.drawCentreString(tag, x + 127, y + 3, 1);
    } else {
      canvas.setTextColor(!unlocked || tag[0] == '$' || tag[0] == 'F' || tag[0] == 'H'
                          ? TEXT_DIM : tag[0] == 'O' ? GOLD : TEXT_DIM, fill);
      canvas.drawRightString(tag, x + 147, y + 3, 1);
    }
    if (unlocked) {
      canvas.setTextColor(i == 3 ? rgb565(153, 225, 129) : GOLD, fill);
      canvas.drawString(strainDifficulty[i], x + 5, y + 14, 1);
      char price[12];
      snprintf(price, sizeof(price), "$%u / 4", strainPrices[i]);
      canvas.setTextColor(TEXT_DIM, fill);
      canvas.drawRightString(price, x + 147, y + 14, 1);
    } else {
      canvas.setTextColor(TEXT_DIM, fill);
      canvas.drawRightString("Harvest to unlock", x + 147, y + 14, 1);
    }
  }
}

// Big-card grid like the seed catalog: the whole card is the tap target.
void drawMarketCard(int x, int y, const char *label, const char *value, bool enabled) {
  uint16_t fill = PANEL;
  canvas.fillRoundRect(x, y, 152, 38, 5, fill);
  canvas.drawRoundRect(x, y, 152, 38, 5, enabled ? ACCENT : PANEL_LIGHT);
  canvas.setTextColor(enabled ? TEXT_MAIN : TEXT_DIM, fill);
  canvas.drawCentreString(label, x + 76, y + 4, 2);
  canvas.setTextColor(enabled ? GOLD : TEXT_DIM, fill);
  canvas.drawCentreString(value, x + 76, y + 23, 1);
}

void drawMarketPage() {
  drawPageTitle("VIRTUAL MARKET", marketGearMode ? "Bong = flower/hash | Joint = flower + lighter + papers" : "Sell trimmed product from your stash");
  drawButton(12, 72, 144, 19, "SELL", marketGearMode ? PANEL_LIGHT : ACCENT_DARK);
  drawButton(164, 72, 144, 19, "GEAR", marketGearMode ? ACCENT_DARK : PANEL_LIGHT);
  char units[24];
  if (!marketGearMode) {
    char flowerPrice[24];
    stashPriceText(flowerPrice, sizeof(flowerPrice));
    snprintf(units, sizeof(units), "x%u - %s", stashTotal(), flowerPrice);
    drawMarketCard(5, 94, "SELL 1 FLOWER", units, stashTotal() > 0);
    snprintf(units, sizeof(units), "x%u - %s", stashTotal(), flowerPrice);
    drawMarketCard(163, 94, "SELL ALL FLOWER", units, stashTotal() > 0);
    char hashPrice[24];
    hashPriceText(hashPrice, sizeof(hashPrice));
    snprintf(units, sizeof(units), "x%u - %s", hashTotal(), hashPrice);
    drawMarketCard(5, 136, "SELL 1 HASH", units, hashRoomUnlocked && hashTotal() > 0);
    snprintf(units, sizeof(units), "x%u - %s", hashTotal(), hashPrice);
    drawMarketCard(163, 136, "SELL ALL HASH", units, hashRoomUnlocked && hashTotal() > 0);
    return;
  }

  drawMarketCard(5, 94, "BONG", bongs ? "x1 OWNED" : "$24", !bongs && credits >= 24);
  snprintf(units, sizeof(units), "x%u owned - $6", lighters);
  drawMarketCard(163, 94, "LIGHTERS +3", units, credits >= 6);
  snprintf(units, sizeof(units), "x%u owned - $5", rollingPapers);
  drawMarketCard(5, 136, "PAPERS +5", units, credits >= 5);
  snprintf(units, sizeof(units), "%u%%", stonedLevel);
  drawMarketCard(163, 136, "HIGH METER", units, false);

  bool gardenReady = true;  // smoke breaks only need gear + product
  drawButton(13, 178, 94, 25, "BONG+FLOWER", gardenReady && bongs && stashTotal() ? ACCENT_DARK : PANEL_LIGHT,
             gardenReady && bongs && stashTotal() ? TFT_WHITE : TEXT_DIM);
  drawButton(113, 178, 94, 25, "BONG+HASH", gardenReady && bongs && hashTotal() ? ACCENT_DARK : PANEL_LIGHT,
             gardenReady && bongs && hashTotal() ? TFT_WHITE : TEXT_DIM);
  drawButton(213, 178, 94, 25, "JOINT+FLOWER", gardenReady && stashTotal() && lighters && rollingPapers ? ACCENT_DARK : PANEL_LIGHT,
             gardenReady && stashTotal() && lighters && rollingPapers ? TFT_WHITE : TEXT_DIM);
}

void drawStashPage() {
  drawPageTitle("STASH & INVENTORY", stashGearMode ? "Lifestyle gear and temporary effects" : "Everything in your garden room");
  drawButton(20, 68, 136, 19, "GARDEN", stashGearMode ? PANEL_LIGHT : ACCENT_DARK);
  drawButton(164, 68, 136, 19, "GEAR", stashGearMode ? ACCENT_DARK : PANEL_LIGHT);
  if (stashGearMode) {
    const char *gearLabels[] = {"Reusable bong", "Lighters", "Rolling papers", "Growth boost ticks"};
    uint16_t gearValues[] = {bongs, lighters, rollingPapers, growthBoostTicks};
    for (int i = 0; i < 4; ++i) {
      int y = 99 + i * 20;
      canvas.setTextColor(TEXT_DIM, BG);
      canvas.drawString(gearLabels[i], 18, y, 1);
      char value[12];
      snprintf(value, sizeof(value), "%u", gearValues[i]);
      canvas.setTextColor(i == 3 && gearValues[i] ? ACCENT : TEXT_MAIN, BG);
      canvas.drawRightString(value, 302, y, 1);
      canvas.drawFastHLine(16, y + 11, 288, 0x3187);
    }
    return;
  }
  const char *labels[] = {"Water packs", "Care kits", "Pest defense", "Seeds", "Untrimmed flowers",
                          "Trimmed flowers", "Hash units", "Remediation kits", "Active plants"};
  uint16_t values[] = {waterPacks, careKits, pestDefenseKits, totalSeeds(), untrimmedUnits,
                       stashTotal(), hashTotal(), remediationKits, plantActive ? PLANTS_PER_ROOM : 0};
  for (int i = 0; i < 9; ++i) {
    int y = 91 + i * 10;
    canvas.setTextColor(TEXT_DIM, BG);
    canvas.drawString(labels[i], 18, y, 1);
    char value[12];
    snprintf(value, sizeof(value), "%u", values[i]);
    canvas.setTextColor((i == 4 || i == 5 || i == 6) ? GOLD : TEXT_MAIN, BG);
    canvas.drawRightString(value, 302, y, 1);
    canvas.drawFastHLine(16, y + 9, 288, 0x3187);
  }
  if (untrimmedUnits > 0) {
    drawButton(92, 187, 136, 17, "TRIM FLOWERS", ACCENT_DARK);
  }
}

void drawOptionsRow(int y, const char *label, const char *value) {
  canvas.fillRoundRect(12, y, 296, 20, 4, PANEL);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(label, 21, y + 6, 1);
  drawButton(213, y + 2, 84, 16, value, ACCENT_DARK);
}

// Big tappable settings rows for the full-screen pages.
void drawCompactNavRow(int y, const char *label) {
  canvas.fillRoundRect(12, y, 296, 24, 6, PANEL);
  canvas.drawRoundRect(12, y, 296, 24, 6, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(label, 20, y + 4, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawRightString(">", 296, y + 5, 2);
}

// Row with an inline value button (Music ON/OFF, inversion, ...).
void drawCompactValueRow(int y, const char *label, const char *value) {
  canvas.fillRoundRect(12, y, 296, 24, 6, PANEL);
  canvas.drawRoundRect(12, y, 296, 24, 6, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(label, 20, y + 4, 2);
  drawButton(213, y + 2, 84, 20, value, ACCENT_DARK);
}

void drawSettingsNavRow(int y, const char *label) {
  canvas.fillRoundRect(12, y, 296, 44, 8, PANEL);
  canvas.drawRoundRect(12, y, 296, 44, 8, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(label, 24, y + 14, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawRightString(">", 296, y + 14, 2);
}

// Row with an inline value button (Music ON/OFF, inversion, ...).
void drawSettingsRow(int y, const char *label, const char *value) {
  canvas.fillRoundRect(12, y, 296, 44, 8, PANEL);
  canvas.drawRoundRect(12, y, 296, 44, 8, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(label, 24, y + 14, 2);
  drawButton(212, y + 10, 84, 24, value, ACCENT_DARK);
}

// Fixed title strip above the scrollable settings content.
void drawSettingsTitle() {
  const char *title = "OPTIONS";
  const char *subtitle = "Gear icon exits";
  canvas.setTextColor(TEXT_MAIN, BG);
  canvas.drawString(title, 12, 40, 2);
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawString(subtitle, 12, 60, 1);
}

void drawGrowerIdCard() {
  // Grower ID tag: mini character card with name and all-time stats, sized
  // so the whole options hub fits on one screen without scrolling.
  canvas.fillRoundRect(10, 72, 300, 56, 8, PANEL);
  canvas.drawRoundRect(10, 72, 300, 56, 8, ACCENT_DARK);
  drawFarmerHeadLarge(34, 100, farmerCharacter);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(growerName, 54, 76, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawString(farmerCharacterName(farmerCharacter), 55, 94, 1);
  // All-time stats, right-aligned inside the card.
  uint8_t strainsFound = 0;
  for (uint16_t m = harvestedStrainMask; m; m >>= 1) strainsFound += m & 1;
  char statLine[28];
  canvas.setTextColor(GOLD, PANEL);
  snprintf(statLine, sizeof(statLine), "%u harvests", harvestCount);
  canvas.drawRightString(statLine, 302, 76, 1);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  snprintf(statLine, sizeof(statLine), "L%u %s", currentMasteryLevel(),
           masteryNicknameForLevel(currentMasteryLevel()));
  canvas.drawRightString(statLine, 302, 88, 1);
  canvas.setTextColor(TEXT_DIM, PANEL);
  snprintf(statLine, sizeof(statLine), "%u/%u strains", strainsFound, STRAIN_COUNT);
  canvas.drawRightString(statLine, 302, 100, 1);
}

// ============ WiFi login + OTA updates from GitHub (v15) ============
// WIFI LOGIN opens the on-screen keyboard to type the network name and
// password (saved to NVS with the game save). CHECK UPDATES connects,
// compares this build tag with version.txt from the repo's latest release,
// and INSTALL flashes the matching .bin for this board, then reboots.
#define OTA_GFX canvas
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

static const char *OTA_RELEASE_BASE =
    "https://github.com/VYBN3D/WinFloridaGardenSim/releases/latest/download/";
static const char *OTA_BIN_NAME = "WinFloridaGardenSim.ino.bin";

static bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

static void otaRestoreRadio() {
  // Hand the radio back the way the garden likes it (BLE/Nearby first).
  if (nearbyReady) {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  } else {
    WiFi.mode(WIFI_OFF);
  }
}

static void otaConnectToSaved(uint16_t waitMs) {
  if (wifiConnected() || !wifiSsid[0]) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  unsigned long start = millis();
  while (!wifiConnected() && millis() - start < waitMs) delay(100);
}

static void otaCheckUpdate() {
  snprintf(otaStatus, sizeof(otaStatus), "Connecting WiFi ...");
  render();
  otaConnectToSaved(9000);
  if (!wifiConnected()) {
    snprintf(otaStatus, sizeof(otaStatus), "No WiFi - check name/password");
    return;
  }
  snprintf(otaStatus, sizeof(otaStatus), "Checking for updates ...");
  render();
  HTTPClient http;
  WiFiClientSecure tls;
  tls.setInsecure();                   // repo is public; no pinning needed
  tls.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(12000);
  if (!http.begin(tls, String(OTA_RELEASE_BASE) + "version.txt")) {
    snprintf(otaStatus, sizeof(otaStatus), "Update check failed");
  } else {
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      body.trim();
      strlcpy(otaLatest, body.c_str(), sizeof(otaLatest));
      if (!strcmp(otaLatest, FW_VERSION))
        snprintf(otaStatus, sizeof(otaStatus), "Up to date (%s)", FW_VERSION);
      else
        snprintf(otaStatus, sizeof(otaStatus), "UPDATE READY: %s", otaLatest);
    } else {
      snprintf(otaStatus, sizeof(otaStatus), "Update check failed (HTTP %d)", code);
    }
    http.end();
  }
  otaRestoreRadio();                   // release the radio back to Nearby
}

static void otaInstallUpdate() {
  snprintf(otaStatus, sizeof(otaStatus), "Connecting WiFi ...");
  render();
  otaConnectToSaved(9000);
  if (!wifiConnected()) {
    snprintf(otaStatus, sizeof(otaStatus), "No WiFi - check name/password");
    return;
  }
  snprintf(otaStatus, sizeof(otaStatus), "Downloading update ... KEEP STILL");
  render();
  HTTPClient http;
  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  if (!http.begin(tls, String(OTA_RELEASE_BASE) + OTA_BIN_NAME)) {
    snprintf(otaStatus, sizeof(otaStatus), "Download failed");
    return;
  }
  int code = http.GET();
  if (code != 200) {
    snprintf(otaStatus, sizeof(otaStatus), "Download failed (HTTP %d)", code);
    http.end();
    return;
  }
  int size = http.getSize();
  if (size <= 0) {
    snprintf(otaStatus, sizeof(otaStatus), "Download failed (no size)");
    http.end();
    return;
  }
  if (!Update.begin(size)) {
    snprintf(otaStatus, sizeof(otaStatus), "Not enough OTA space");
    http.end();
    return;
  }
  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();
  if (written != (size_t)size) {
    snprintf(otaStatus, sizeof(otaStatus), "Transfer interrupted");
    Update.abort();
    return;
  }
  if (!Update.end(true)) {
    snprintf(otaStatus, sizeof(otaStatus), "Flash verify failed");
    return;
  }
  OTA_GFX.fillScreen(0x0861);
  OTA_GFX.setTextColor(TFT_WHITE, 0x0861);
  OTA_GFX.drawCentreString("UPDATE INSTALLED", 160, 100, 2);
  OTA_GFX.drawCentreString("REBOOTING", 160, 130, 2);
  presentFrame();
  delay(1200);
  ESP.restart();
}

static void drawOtaPage() {
  drawPageTitle("WIFI / UPDATES", "Gear icon exits");
  char line[48];
  OTA_GFX.fillRoundRect(10, 72, 300, 24, 6, PANEL);
  OTA_GFX.drawRoundRect(10, 72, 300, 24, 6, PANEL_LIGHT);
  snprintf(line, sizeof(line), "YOUR BUILD: %s - %s", FW_VERSION_FULL, FW_DATE);
  OTA_GFX.setTextColor(TEXT_MAIN, PANEL);
  OTA_GFX.drawCentreString(line, 160, 79, 1);
  OTA_GFX.fillRoundRect(10, 100, 300, 24, 6, PANEL);
  OTA_GFX.drawRoundRect(10, 100, 300, 24, 6, PANEL_LIGHT);
  snprintf(line, sizeof(line), "WIFI: %s", wifiConnected() ? WiFi.SSID().c_str()
           : wifiSsid[0] ? wifiSsid : "NOT SET UP");
  OTA_GFX.setTextColor(TEXT_MAIN, PANEL);
  OTA_GFX.drawCentreString(line, 160, 107, 1);
  drawButton(10, 130, 145, 26, "WIFI LOGIN", PANEL_LIGHT);
  drawButton(165, 130, 145, 26, "CHECK UPDATES", PANEL_LIGHT);
  OTA_GFX.setTextColor(otaLatest[0] && strcmp(otaLatest, FW_VERSION) ? GOLD : TEXT_DIM, BG);
  OTA_GFX.drawCentreString(otaStatus, 160, 166, 1);
  if (otaLatest[0] && strcmp(otaLatest, FW_VERSION)) {
    snprintf(line, sizeof(line), "INSTALL %s AND REBOOT", otaLatest);
    drawButton(10, 176, 300, 28, line, ACCENT_DARK, TFT_WHITE);
  }
}

static void drawWifiLoginPage() {
  drawPageTitle("WIFI LOGIN",
                wifiLoginStage == 0 ? "Type your network name" : "Type the WiFi password");
  OTA_GFX.fillRoundRect(14, 82, 292, 22, 4, PANEL);
  OTA_GFX.setTextColor(TEXT_MAIN, PANEL);
  if (wifiLoginStage == 0) {
    OTA_GFX.drawString(wifiSsidEdit, 21, 88, 1);
  } else {
    char dots[65];
    size_t length = strlen(wifiPassEdit);
    memset(dots, '*', length);
    dots[length] = '\0';
    OTA_GFX.drawString(dots, 21, 88, 1);
  }
  drawFullKeyboard(nameKbMode);
  drawButton(7, 177, 92, 25, "SPACE", PANEL_LIGHT);
  drawButton(111, 177, 98, 25, wifiLoginStage == 0 ? "NEXT" : "CONNECT", ACCENT_DARK);
  drawButton(221, 177, 92, 25, "CANCEL", PANEL_LIGHT);
}

static void touchWifiLogin(int x, int y) {
  char key = fullKeyboardKey(x, y, nameKbMode);
  if (key) {
    char *buf = wifiLoginStage == 0 ? wifiSsidEdit : wifiPassEdit;
    size_t cap = wifiLoginStage == 0 ? sizeof(wifiSsidEdit) : sizeof(wifiPassEdit);
    size_t length = strlen(buf);
    if (length < cap - 1) { buf[length] = key; buf[length + 1] = '\0'; }
    render();
    return;
  }
  if (y >= 151 && y < 170 && x >= 259) {
    char *buf = wifiLoginStage == 0 ? wifiSsidEdit : wifiPassEdit;
    size_t length = strlen(buf);
    if (length) buf[length - 1] = '\0';
  } else if (y >= 177 && y < 205) {
    if (x < 104) {
      char *buf = wifiLoginStage == 0 ? wifiSsidEdit : wifiPassEdit;
      size_t cap = wifiLoginStage == 0 ? sizeof(wifiSsidEdit) : sizeof(wifiPassEdit);
      size_t length = strlen(buf);
      if (length > 0 && length < cap - 1) { buf[length] = ' '; buf[length + 1] = '\0'; }
    } else if (x < 216) {
      if (wifiLoginStage == 0) {
        if (wifiSsidEdit[0]) { wifiLoginStage = 1; render(); }
      } else if (wifiSsidEdit[0]) {
        strlcpy(wifiSsid, wifiSsidEdit, sizeof(wifiSsid));
        strlcpy(wifiPass, wifiPassEdit, sizeof(wifiPass));
        saveGame();                    // creds ride along with the save
        snprintf(otaStatus, sizeof(otaStatus), "Connecting to %s ...", wifiSsid);
        page = OTA_PAGE;
        render();
        otaConnectToSaved(10000);
        snprintf(otaStatus, sizeof(otaStatus),
                 wifiConnected() ? "Connected to %s" : "Could not connect to %s", wifiSsid);
        otaRestoreRadio();
        render();
      }
    } else {
      page = OTA_PAGE;
    }
    render();
  }
}

static void touchOtaPage(int x, int y) {
  if (y >= 130 && y < 156 && x < 160) {
    wifiLoginStage = 0;
    strlcpy(wifiSsidEdit, wifiSsid, sizeof(wifiSsidEdit));  // prefill saved name
    wifiPassEdit[0] = '\0';
    page = WIFI_LOGIN;
    render();
  } else if (y >= 130 && y < 156) {
    otaCheckUpdate();
    render();
  } else if (y >= 176 && y < 204 && x >= 10 &&
             otaLatest[0] && strcmp(otaLatest, FW_VERSION)) {
    otaInstallUpdate();
    render();
  }
}

// ============ Cheat codes (v15) ============
static void applyCheatCode() {
  char code[16];
  strlcpy(code, cheatEdit, sizeof(code));
  for (char *c = code; *c; ++c) *c = (char)toupper((unsigned char)*c);
  if (!strcmp(code, "CASH") || !strcmp(code, "MONEY")) {
    credits = (uint16_t)min(65535, (int)credits + 500);
    strlcpy(cheatFeedback, "+$500 added", sizeof(cheatFeedback));
  } else if (!strcmp(code, "SEEDS")) {
    seedsByStrain[plantStrain] = min(255, (int)seedsByStrain[plantStrain] + 8);
    snprintf(cheatFeedback, sizeof(cheatFeedback), "+8 %s seeds", strainNames[plantStrain]);
  } else if (!strcmp(code, "STASH")) {
    stashQualityByStrain[plantStrain] = blendQuality(stashQualityByStrain[plantStrain],
                                                     stashByStrain[plantStrain], 90, 10);
    stashByStrain[plantStrain] = min(255, (int)stashByStrain[plantStrain] + 10);
    strlcpy(cheatFeedback, "+10 flower jars (A grade)", sizeof(cheatFeedback));
  } else if (!strcmp(code, "UNLOCK")) {
    strainUnlockedThrough = STRAIN_COUNT - 1;
    strlcpy(cheatFeedback, "all strains unlocked", sizeof(cheatFeedback));
  } else if (!strcmp(code, "HARVEST")) {
    if (plantActive) {
      growth = 100;
      strlcpy(cheatFeedback, "plant ready to harvest", sizeof(cheatFeedback));
    } else {
      strlcpy(cheatFeedback, "no plant in the garden", sizeof(cheatFeedback));
      return;
    }
  } else if (!strcmp(code, "TURBO")) {
    gameSpeed = 2;
    growthBoostTicks = 60;
    strlcpy(cheatFeedback, "TURBO: extreme speed + glow", sizeof(cheatFeedback));
  } else if (!strcmp(code, "HYDRO")) {
    hydration = 100;
    dryStreakTicks = 0;
    strlcpy(cheatFeedback, "soil soaked to 100%", sizeof(cheatFeedback));
  } else {
    strlcpy(cheatFeedback, "unknown code", sizeof(cheatFeedback));
    return;
  }
  saveGame();
}

static void drawCheatPage() {
  drawPageTitle("CHEATS", cheatFeedback[0] ? cheatFeedback : "Type a code, then tap ACTIVATE");
  canvas.fillRoundRect(14, 82, 292, 22, 4, PANEL);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(cheatEdit, 21, 88, 1);
  drawFullKeyboard(nameKbMode);
  drawButton(7, 177, 92, 25, "SPACE", PANEL_LIGHT);
  drawButton(111, 177, 98, 25, "ACTIVATE", ACCENT_DARK);
  drawButton(221, 177, 92, 25, "BACK", PANEL_LIGHT);
}

static void touchCheatPage(int x, int y) {
  char key = fullKeyboardKey(x, y, nameKbMode);
  if (key) {
    size_t length = strlen(cheatEdit);
    if (length < sizeof(cheatEdit) - 1) { cheatEdit[length] = key; cheatEdit[length + 1] = '\0'; }
    render();
    return;
  }
  if (y >= 151 && y < 170 && x >= 259) {
    size_t length = strlen(cheatEdit);
    if (length) cheatEdit[length - 1] = '\0';
  } else if (y >= 177 && y < 205) {
    if (x < 104) {
      size_t length = strlen(cheatEdit);
      if (length > 0 && length < sizeof(cheatEdit) - 1 &&
          cheatEdit[length - 1] != ' ') {
        cheatEdit[length] = ' ';
        cheatEdit[length + 1] = '\0';
      }
    } else if (x < 216) {
      if (cheatEdit[0]) {
        applyCheatCode();
        cheatEdit[0] = '\0';
      }
    } else {
      page = OPTIONS;
    }
    render();
  }
}

// Options as a big-card grid: the whole card is the tap target, so no more
// missed taps on tiny inline value buttons.
void drawOptionsCard(int x, int y, int w, int h, const char *label,
                     const char *value, uint16_t fill) {
  canvas.fillRoundRect(x, y, w, h, 8, fill);
  canvas.drawRoundRect(x, y, w, h, 8, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, fill);
  canvas.drawCentreString(label, x + w / 2, y + 4, 2);
  canvas.setTextColor(GOLD, fill);
  canvas.drawCentreString(value, x + w / 2, y + 27, 1);
}

void drawOptionsPage() {
  char volLabel[8];
  if (musicVolume) snprintf(volLabel, sizeof(volLabel), "%u%%", musicVolume);
  else strlcpy(volLabel, "OFF", sizeof(volLabel));
  // Grid sits below the settings title band (title + subtitle cover y 28..68).
  drawOptionsCard(5, 72, 152, 40, "GAME SPEED",
                  gameSpeed == 0 ? "RELAXED" : gameSpeed == 1 ? "QUICK" : "EXTREME", PANEL);
  drawOptionsCard(163, 72, 152, 40, "MUSIC", volLabel, PANEL);
  drawOptionsCard(5, 116, 152, 40, "FAST HARVEST", "TAP", ACCENT_DARK);
  drawOptionsCard(163, 116, 152, 40, "CHEATS", "TAP", PANEL_LIGHT);
  // Bottom rows: RESET GAME with TOUCH CAL beside it, and the WiFi/OTA
  // update button below. Must stay above the nav bar (NAV_Y = 207) or the
  // nav touch handler steals the taps (that was the STASH-jump bug).
  drawButton(5, 160, 152, 24, resetArmed ? "TAP AGAIN TO RESET" : "RESET GAME",
             resetArmed ? RED : PANEL_LIGHT);
  drawButton(163, 160, 152, 24, "TOUCH CAL", PANEL_LIGHT);
  drawButton(5, 188, 310, 18, "UPDATE - WIFI / OTA", PANEL_LIGHT);
}

// Grower menu, opened from the leaf: your grower as an ID card.
void drawGrowerMenuPage() {
  drawGrowerIdCard();
  // Full-width, big-font nav rows — the whole row is the tap target.
  // No BACK button: the leaf in the top-left closes this menu.
  const char *labels[2] = {"CHANGE GROWER", "GROWER NAME"};
  const int rowY[2] = {132, 172};
  for (uint8_t i = 0; i < 2; ++i) {
    canvas.fillRoundRect(10, rowY[i], 300, 34, 8, PANEL);
    canvas.drawRoundRect(10, rowY[i], 300, 34, 8, PANEL_LIGHT);
    canvas.setTextColor(TEXT_MAIN, PANEL);
    canvas.drawCentreString(labels[i], 160, rowY[i] + 4, 4);
    canvas.setTextColor(TEXT_DIM, PANEL);
    canvas.drawRightString(">", 296, rowY[i] + 8, 2);
  }
}

void drawGrowerNamePage() {
  drawPageTitle("SET GROWER NAME", "Tap ABC for caps, #+_ for symbols");
  canvas.fillRoundRect(14, 82, 292, 22, 4, PANEL);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString(growerNameEdit, 21, 88, 1);
  drawFullKeyboard(nameKbMode);
  drawButton(7, 177, 92, 25, "SPACE", PANEL_LIGHT);
  drawButton(111, 177, 98, 25, "SAVE", ACCENT_DARK);
  drawButton(221, 177, 92, 25, "CANCEL", PANEL_LIGHT);
}

void drawCharacterSelectPage() {
  // Giant character card cycled with < and > arrows; PICK confirms.
  canvas.fillScreen(BG);
  canvas.setTextColor(TEXT_MAIN, BG);
  canvas.drawCentreString("CHOOSE YOUR GROWER", 160, 8, 2);
  uint8_t c = characterSelectIndex % 9;
  canvas.fillRoundRect(58, 30, 204, 160, 10, PANEL);
  canvas.drawRoundRect(58, 30, 204, 160, 10, ACCENT_DARK);
  drawFarmerHeadLarge(160, 90, c);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString(farmerCharacterName(c), 160, 134, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawCentreString("Grower", 160, 154, 1);
  drawButton(10, 88, 44, 50, "<", ACCENT_DARK);
  drawButton(266, 88, 44, 50, ">", ACCENT_DARK);
  drawButton(58, 172, 204, 30, "PICK", ACCENT_DARK);  // above the nav strip
}

void drawHashRoomPage() {
  drawPageTitle("SECRET HASH ROOM", "Unlocked as a game upgrade");
  canvas.fillRoundRect(14, 82, 292, 70, 8, PANEL);
  canvas.drawRoundRect(14, 82, 292, 70, 8, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString("Hash mode is ready", 160, 97, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawCentreString("Choose HASH after a room is mature.", 160, 123, 1);
  drawButton(22, 165, 132, 30, "GARDEN", ACCENT_DARK);
  drawButton(166, 165, 132, 30, "MARKET", ACCENT_DARK);
}

void drawHarvestChoicePage() {
  drawPageTitle("HARVEST ROOM", "Choose what this batch becomes");
  canvas.fillRoundRect(12, 78, 296, 52, 7, PANEL);
  canvas.drawRoundRect(12, 78, 296, 52, 7, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString("FLOWER", 23, 86, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawString("4 untrimmed units; trim before selling", 23, 108, 1);
  drawButton(246, 89, 50, 28, "PICK", ACCENT_DARK);

  canvas.fillRoundRect(12, 139, 296, 52, 7, PANEL);
  canvas.drawRoundRect(12, 139, 296, 52, 7, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawString("HASH", 23, 147, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawString(hashRoomUnlocked ? "2 units; secret room conversion" : "Locked: buy the secret room", 23, 169, 1);
  drawButton(246, 150, 50, 28, hashRoomUnlocked ? "PICK" : "LOCK", hashRoomUnlocked ? ACCENT_DARK : PANEL_LIGHT,
             hashRoomUnlocked ? TFT_WHITE : TEXT_DIM);
}

void drawTrimPage() {
  drawPageTitle("TRIMMING ROOM", "Make the harvested flower sale-ready");
  canvas.fillRoundRect(14, 82, 292, 57, 8, PANEL);
  canvas.drawRoundRect(14, 82, 292, 57, 8, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString("Untrimmed flower units", 160, 91, 2);
  char amount[16];
  snprintf(amount, sizeof(amount), "%u", untrimmedUnits);
  canvas.setTextColor(GOLD, PANEL);
  canvas.drawCentreString(amount, 160, 116, 2);
  drawButton(18, 158, 136, 36, "TRIM FLOWERS", ACCENT_DARK);
  drawButton(166, 158, 136, 36, "LATER", PANEL_LIGHT);
}

bool selectedNearbyPeerValid() {
  return selectedNearbyPeer < MAX_NEARBY_PEERS &&
         nearbyPeers[selectedNearbyPeer].active &&
         millis() - nearbyPeers[selectedNearbyPeer].lastSeen < 20000UL;
}

void drawNearbyPeerTarget() {
  if (!selectedNearbyPeerValid()) {
    canvas.setTextColor(RED, BG);
    canvas.drawCentreString("Tap an online player first", 160, 69, 1);
    return;
  }
  const NearbyPeer &peer = nearbyPeers[selectedNearbyPeer];
  char target[38];
  snprintf(target, sizeof(target), "To %s %02X%02X", peer.grower,
           peer.mac[4], peer.mac[5]);
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawCentreString(target, 160, 68, 1);
}

void drawNearbyKeyboardRow(const char *keys, int count, int startX, int y) {
  for (int i = 0; i < count; ++i) {
    char key[2] = {keys[i], '\0'};
    drawButton(startX + i * 28, y, 26, 19, key, PANEL);
  }
}

// Full keyboard: three modes share the same geometry. Lowercase is the
// qwerty layout; uppercase is the same keys shifted; symbols mode swaps
// digits and punctuation in. Toggle keys sit in the gaps left of the
// shorter rows: "#+_" (left of home row) and the shift key (left of
// bottom row) — both show "abc" while symbols mode is open.
void keyboardRowStrings(uint8_t mode, const char *&r1, const char *&r2,
                        const char *&r3) {
  if (mode == 2) {
    r1 = "1234567890";
    r2 = "-_.,!?&+";
    r3 = "'():;/";
  } else if (mode == 1) {
    r1 = "QWERTYUIOP";
    r2 = "ASDFGHJKL";
    r3 = "ZXCVBNM";
  } else {
    r1 = "qwertyuiop";
    r2 = "asdfghjkl";
    r3 = "zxcvbnm";
  }
}

void drawFullKeyboard(uint8_t mode) {
  const char *r1, *r2, *r3;
  keyboardRowStrings(mode, r1, r2, r3);
  drawNearbyKeyboardRow(r1, 10, 21, 107);
  drawNearbyKeyboardRow(r2, 9, 35, 129);
  drawNearbyKeyboardRow(r3, 7, 63, 151);
  drawButton(259, 151, 45, 19, "DEL", PANEL_LIGHT);
  drawButton(7, 129, 26, 19, mode == 2 ? "abc" : "#+_", PANEL_LIGHT);
  drawButton(7, 151, 26, 19, mode == 0 ? "ABC" : "abc", PANEL_LIGHT);
}

// Shared touch resolution for the full keyboard. Returns the key to append
// (0 = none), and updates `mode` when a toggle key is pressed. DEL and the
// bottom action row stay with each page's own handler.
char fullKeyboardKey(int x, int y, uint8_t &mode) {
  const char *r1, *r2, *r3;
  keyboardRowStrings(mode, r1, r2, r3);
  if (y >= 107 && y < 126 && x >= 21 && x < 301) {
    int index = (x - 21) / 28;
    return index < 10 ? r1[index] : 0;
  }
  if (y >= 129 && y < 148) {
    if (x >= 7 && x < 33) {
      mode = mode == 2 ? 0 : 2;   // #+_ <-> letters
      return 0;
    }
    int index = (x - 35) / 28;
    return x < 287 && index < 9 ? r2[index] : 0;
  }
  if (y >= 151 && y < 170) {
    if (x >= 7 && x < 33) {
      mode = mode == 1 ? 0 : 1;   // ABC <-> abc (exits symbols to lowercase)
      return 0;
    }
    int index = (x - 63) / 28;
    return x < 257 && index < 7 ? r3[index] : 0;
  }
  return 0;
}

void drawNearbyPage() {
  if (nearbyMode == 1) {
    drawPageTitle("SEND A MESSAGE", "Tap ABC for caps, #+_ for symbols");
    drawNearbyPeerTarget();
    canvas.fillRoundRect(14, 82, 292, 22, 4, PANEL);
    canvas.setTextColor(TEXT_MAIN, PANEL);
    canvas.drawString(outgoingMessage, 21, 88, 1);
    drawFullKeyboard(msgKbMode);
    drawButton(7, 177, 92, 25, "SPACE", PANEL_LIGHT);
    drawButton(111, 177, 98, 25, "SEND", selectedNearbyPeerValid() ? ACCENT_DARK : PANEL_LIGHT);
    drawButton(221, 177, 92, 25, "BACK", PANEL_LIGHT);
    return;
  }

  if (nearbyMode == 2) {
    drawPageTitle("SEND SUPPLIES", "Tap a pack to send it to the selected player");
    drawNearbyPeerTarget();
    drawShopCard(84, "Seeds +4", seedsByStrain[sendSeedStrain] >= 4 ? "SEND" : "NEED",
                 seedsByStrain[sendSeedStrain] >= 4 && selectedNearbyPeerValid());
    // Strain picker: the gift carries THIS strain to the receiver.
    canvas.setTextColor(ACCENT, PANEL);
    canvas.drawRightString(strainNames[sendSeedStrain], 192, 90, 1);
    drawButton(196, 85, 21, 19, "<", PANEL_LIGHT);
    drawButton(220, 85, 21, 19, ">", PANEL_LIGHT);
    drawShopCard(109, "Pest defense +1", pestDefenseKits ? "SEND" : "NEED", pestDefenseKits > 0 && selectedNearbyPeerValid());
    drawShopCard(134, "Water packs +2", waterPacks >= 2 ? "SEND" : "NEED", waterPacks >= 2 && selectedNearbyPeerValid());
    drawShopCard(159, "Care kits +2", careKits >= 2 ? "SEND" : "NEED", careKits >= 2 && selectedNearbyPeerValid());
    drawButton(7, 184, 152, 19, "SEND MONEY", selectedNearbyPeerValid() ? ACCENT_DARK : PANEL_LIGHT);
    drawButton(165, 184, 148, 19, "BACK", PANEL_LIGHT);
    return;
  }

  if (nearbyMode == 4) {
    char balance[40];
    snprintf(balance, sizeof(balance), "You have $%u", credits);
    drawPageTitle("SEND MONEY", balance);
    drawNearbyPeerTarget();
    canvas.fillRoundRect(14, 82, 292, 22, 4, PANEL);
    canvas.setTextColor(TEXT_MAIN, PANEL);
    char amount[10];
    snprintf(amount, sizeof(amount), "$%s", outgoingCash[0] ? outgoingCash : "0");
    canvas.drawString(amount, 21, 88, 1);
    drawNearbyKeyboardRow("1234567890", 10, 21, 107);
    drawButton(35, 129, 45, 19, "CLR", PANEL_LIGHT);
    drawButton(105, 129, 70, 19, "000", PANEL_LIGHT);
    drawButton(259, 129, 45, 19, "DEL", PANEL_LIGHT);
    drawButton(7, 177, 92, 25, "ALL", PANEL_LIGHT);
    drawButton(111, 177, 98, 25, "SEND",
               selectedNearbyPeerValid() && outgoingCash[0] ? ACCENT_DARK : PANEL_LIGHT);
    drawButton(221, 177, 92, 25, "BACK", PANEL_LIGHT);
    return;
  }

  if (nearbyMode == 3) {
    drawPageTitle("MESSAGE INBOX", "Latest message, supply or cash delivery");
    canvas.fillRoundRect(14, 82, 292, 78, 8, PANEL);
    canvas.drawRoundRect(14, 82, 292, 78, 8, PANEL_LIGHT);
    canvas.setTextColor(TEXT_MAIN, PANEL);
    canvas.drawCentreString(lastNearbyMessage, 160, 105, 2);
    drawButton(108, 174, 104, 27, "BACK", PANEL_LIGHT);
    return;
  }

  char localRank[48];
  snprintf(localRank, sizeof(localRank), "%s - L%u %s", growerName,
           currentMasteryLevel(), masteryNicknameForLevel(currentMasteryLevel()));
  drawPageTitle("NEARBY GROWERS", localRank);
  drawNearbyPeerListOnly();
  drawButton(5, 179, 98, 24, "MESSAGE", selectedNearbyPeerValid() ? ACCENT_DARK : PANEL_LIGHT);
  drawButton(111, 179, 98, 24, "SUPPLIES", selectedNearbyPeerValid() ? ACCENT_DARK : PANEL_LIGHT);
  drawButton(217, 179, 98, 24, "INBOX", PANEL_LIGHT);
}

void sendNearbyGift(uint8_t item) {
  if (!selectedNearbyPeerValid()) return;
  const char *giftName = "supply";
  char strainTag[2] = "";
  switch (item) {
    case 1:
      if (seedsByStrain[sendSeedStrain] < 4) return;
      giftName = "4 seeds";
      strainTag[0] = '0' + sendSeedStrain;
      break;
    case 2: if (!pestDefenseKits) return; giftName = "a pest kit"; break;
    case 3: if (waterPacks < 2) return; giftName = "2 water packs"; break;
    case 4: if (careKits < 2) return; giftName = "2 care kits"; break;
    default: return;
  }
  if (sendNearbyPacket(nearbyPeers[selectedNearbyPeer].mac, PACKET_GIFT, item, strainTag)) {
    if (item == 1) {
      seedsByStrain[sendSeedStrain] -= 4;
      snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent 4 %s seeds", strainNames[sendSeedStrain]);
    }
    else if (item == 2) { --pestDefenseKits; snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent %s", giftName); }
    else if (item == 3) { waterPacks -= 2; snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent %s", giftName); }
    else if (item == 4) { careKits -= 2; snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent %s", giftName); }
    saveGame();
  } else {
    snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Send failed; supplies kept");
  }
  nearbyMode = 0;
}

void sendNearbyCash() {
  if (!selectedNearbyPeerValid()) return;
  uint32_t amount = (uint32_t)strtoul(outgoingCash, nullptr, 10);
  if (amount > credits) amount = credits;   // never send more than you have
  if (!amount) return;
  char text[8];
  snprintf(text, sizeof(text), "%lu", (unsigned long)amount);
  if (sendNearbyPacket(nearbyPeers[selectedNearbyPeer].mac, PACKET_CASH, 0, text)) {
    credits -= (uint16_t)amount;
    snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent $%lu", (unsigned long)amount);
    saveGame();
  } else {
    snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Send failed; money kept");
  }
  outgoingCash[0] = '\0';
  nearbyMode = 0;
}

void presentFrame() {
  // Direct-to-TFT rendering is retained to avoid a full-screen heap buffer.
}

void render(bool presentNow) {
  static Page lastSeenPage = GARDEN;
  if (page != lastSeenPage) {
    if (page == CHARACTER_SELECT) characterSelectIndex = farmerCharacter;
    lastSeenPage = page;
  }
  if (page == PLANTING) {
    // Land on a strain you actually hold before drawing the picker/tray.
    if (plantedPotCount() == 0 && !seedsByStrain[plantStrain]) {
      plantStrain = nextSeededStrain(plantStrain, 1);
    }
    canvas.fillScreen(BG);
    drawPlantingPage();
  } else if (page == PLANT_VIEW) {
    canvas.fillScreen(BG);
    drawPlantFullScreenPage();
  } else if (page == CHARACTER_SELECT) {
    canvas.fillScreen(BG);
    drawCharacterSelectPage();
  } else {
    // Settings pages run full screen (no nav bar); the header gear icon
    // toggles back to the garden from any of them.
    bool settingsPage = page == OPTIONS || page == GROWER_MENU;
    if (settingsPage) {
      // Full screen, scrollable: draw the content first (it may bleed under
      // the title strip), then repaint header and fixed title on top.
      static Page lastSettingsPage = GARDEN;
      if (lastSettingsPage != page) { settingsScroll = 0; lastSettingsPage = page; }
      int16_t scrollMax = 0;   // hub fits on one screen now; keep scroll at 0
      if (settingsScroll < 0) settingsScroll = 0;
      if (settingsScroll > scrollMax) settingsScroll = scrollMax;
      canvas.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, BG);
      switch (page) {
        case OPTIONS: drawOptionsPage(); break;
        case GROWER_MENU: drawGrowerMenuPage(); break;
        default: break;
      }
      drawHeader();
      canvas.fillRect(0, HEADER_H, SCREEN_W, 40, BG);
      drawSettingsTitle();
    } else {
      // Keep the persistent header and navigation from being cleared on every
      // button press. Page changes redraw only the central content area.
      canvas.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
      drawHeader();
      switch (page) {
        case GARDEN: drawGardenPage(); break;
        case SUPPLIES: drawSuppliesPage(); break;
        case MARKET: drawMarketPage(); break;
        case STASH: drawStashPage(); break;
        case HASH_ROOM: drawHashRoomPage(); break;
        case HARVEST_SELECT: drawHarvestChoicePage(); break;
        case TRIM_ROOM: drawTrimPage(); break;
        case NEARBY: drawNearbyPage(); break;
        case NAME_EDIT: drawGrowerNamePage(); break;
        case OTA_PAGE: drawOtaPage(); break;
        case WIFI_LOGIN: drawWifiLoginPage(); break;
        case CHEAT_CODE: drawCheatPage(); break;
        case SEED_SHOP: drawSeedShopPage(); break;
        case CHARACTER_SELECT: break;
        case PLANT_VIEW: break;
        case PLANTING: break;
        default: break;
      }
      drawNav();
    }
  }
  if (presentNow) presentFrame();
}

uint8_t waterToleranceNow() {
  uint8_t strain = min((int)activeStrain, (int)STRAIN_COUNT - 1);
  return strainWaterTolerance[strain] - stageWaterPenalty[stageForGrowth()];
}

bool wateringWouldDrown() {
  return plantActive && hydration + WATER_AMOUNT > waterToleranceNow();
}

// Current defoliation window from growth, or 255 before the first one / at harvest.
uint8_t currentDefoliateWindow() {
  if (!plantActive || growth >= 100) return 255;
  uint8_t w = 255;
  for (uint8_t i = 0; i < DEFOLIATE_WINDOWS; ++i) if (growth >= defoliateWindowStart[i]) w = i;
  return w;
}

void resetDefoliationForNewPlant() {
  defoliateWindow = 255;
  defoliateClicks = 0;
  defoliateLimit = 2;
  defoliatePromptPending = 255;
  defoliateReadyAt = 0;
  dryStreakTicks = 0;  // fresh start: soil is watered on planting
}

// Opens a new defoliation window when growth crosses its threshold.
void updateDefoliateWindow() {
  uint8_t w = currentDefoliateWindow();
  if (w == 255 || w == defoliateWindow) return;
  uint8_t strain = min((int)activeStrain, (int)STRAIN_COUNT - 1);
  defoliateWindow = w;
  defoliateClicks = 0;
  defoliateLimit = random(strainDefoliateMin[strain], strainDefoliateMax[strain] + 1);
  defoliatePromptPending = w;
  // The new window stays gray (and its prompt waits) until the cooldown passes.
  defoliateReadyAt = gameMinutes + DEFOLIATE_COOLDOWN_MINUTES;
  saveGame();
}

// 0 = gray (unavailable), 1 = green (safe), 2 = red (next click may kill)
uint8_t defoliateButtonState() {
  uint8_t w = currentDefoliateWindow();
  if (w == 255 || w != defoliateWindow) return 0;
  uint8_t strain = min((int)activeStrain, (int)STRAIN_COUNT - 1);
  return defoliateClicks >= strainDefoliateMin[strain] ? 2 : 1;
}

void killPlant(uint8_t reason) {
  deathReason = reason;
  plantDeathAnimationRequested = true;
  deathLedUntil = millis() + 5000;  // red back-LED flag for the kill event
  startSong(&deathJingle);  // sad descending chiptune over the cutscene
  // Light red immediately: the death cutscene blocks the loop (animation
  // frames plus a tap-to-continue wait), so updateHarvestLed() cannot
  // refresh the LED until long after this 5 s window would have expired.
  harvestLedRed();
}

void waterPlant() {
  if (!plantActive || waterPacks == 0) return;
  bool drown = wateringWouldDrown();
  --waterPacks;
  hydration = min(100, hydration + WATER_AMOUNT);
  vigor = min(100, vigor + 4);
  if (drown) {
    killPlant(1);
    saveGame();
    return;
  }
  waterAnimationRequested = true;
  saveGame();
}

void defoliateRoom() {
  if (!plantActive || growth >= 100) return;
  if (defoliateButtonState() == 0) return;  // grayed out: not time to defoliate
  ++defoliateClicks;
  if (defoliateClicks > defoliateLimit) {
    killPlant(2);
    saveGame();
    return;
  }
  // Thinning the canopy is maintenance: it lifts the growth pause and helps
  // plant health and pests, but grants no growth percentage.
  vigor = min(100, vigor + 10);
  if (pestType > 0) {
    if (pestSeverity > 20) pestSeverity -= 20;
    else { pestSeverity = 0; pestType = 0; }
  }
  saveGame();
}

void treatPests() {
  if (!plantActive || pestType == 0 || pestDefenseKits == 0) return;
  --pestDefenseKits;
  pestType = 0;
  pestSeverity = 0;
  vigor = min(100, vigor + 12);
  saveGame();
}

void finishHarvest(bool makeHash) {
  if (!plantActive || growth < 100) return;
  if (makeHash && !hashRoomUnlocked) return;

  // A mold event appears on about one harvest in five. The affected crop is
  // discarded immediately; the rest of the batch remains usable.
  moldLossUnits = random(100) < 20 ? (makeHash ? 1 : random(1, 3)) : 0;
  lastHarvestUnits = (makeHash ? 2 : PLANTS_PER_ROOM) - moldLossUnits;
  // Remediation kits buy the toxic portion back: one kit per unit returns
  // it to the batch so it can still be trimmed and sold.
  moldRescuedUnits = 0;
  if (moldLossUnits && remediationKits) {
    moldRescuedUnits = min(moldLossUnits, remediationKits);
    remediationKits -= moldRescuedUnits;
    moldLossUnits -= moldRescuedUnits;
    lastHarvestUnits += moldRescuedUnits;
  }
  if (makeHash) {
    // Vigor at harvest grades the batch: it sets what the jar sells for.
    hashQualityByStrain[activeStrain] = blendQuality(hashQualityByStrain[activeStrain],
                                                     hashByStrain[activeStrain], vigor, lastHarvestUnits);
    hashByStrain[activeStrain] += lastHarvestUnits;
  } else {
    untrimmedQuality = blendQuality(untrimmedQuality, untrimmedUnits, vigor, lastHarvestUnits);
    untrimmedUnits += lastHarvestUnits;
    untrimmedStrain = activeStrain;
  }
  moldAnimationRequested = moldLossUnits > 0;
  startSong(&harvestFanfare);  // rising chiptune fanfare
  if (harvestCount < 65535) ++harvestCount;
  harvestedStrainMask |= (uint16_t)1 << activeStrain;
  while (strainUnlockedThrough < STRAIN_COUNT - 1) {
    // Unlock the next strain in easy -> hard catalog order once it is grown.
    if (harvestedStrainMask & ((uint16_t)1 << strainCatalogOrder[strainUnlockedThrough])) {
      ++strainUnlockedThrough;
    } else {
      break;
    }
  }

  pestType = 0;
  pestSeverity = 0;
  growth = 0;
  hydration = 72;
  vigor = 82;
  plantActive = false;
  plantedPotMask = 0;
  if (totalSeeds() >= PLANTS_PER_ROOM) {
    firstPlantingDone = false;
    pageAfterPlanting = makeHash ? HASH_ROOM : TRIM_ROOM;
    page = PLANTING;
  } else {
    page = makeHash ? HASH_ROOM : TRIM_ROOM;
  }
  saveGame();
  if (makeHash) {
    hashAnimationRequested = true;
    lastFarmerVisitAt = millis();
  } else {
    flowerHarvestAnimationRequested = true;
    lastFarmerVisitAt = millis();
  }
}

void trimAllFlowers() {
  if (untrimmedUnits == 0) return;
  // The trimmed batch carries its harvest vigor into the stash jar (blended
  // with whatever is already in that jar).
  stashQualityByStrain[untrimmedStrain] = blendQuality(stashQualityByStrain[untrimmedStrain],
                                                       stashByStrain[untrimmedStrain], untrimmedQuality, untrimmedUnits);
  stashByStrain[untrimmedStrain] += untrimmedUnits;
  untrimmedUnits = 0;
  saveGame();
  page = MARKET;
  flowerTrimAnimationRequested = true;
  lastFarmerVisitAt = millis();
}

void buyWater() {
  if (credits < 5) return;
  credits -= 5;
  waterPacks = min(99, waterPacks + 3);
  playCoin();
  saveGame();
}

void buyCareKits() {
  if (credits < 7) return;
  credits -= 7;
  careKits = min(99, careKits + 3);
  playCoin();
  saveGame();
}

void openSeedShop(Page returnTo) {
  pageAfterSeedShop = returnTo;
  page = SEED_SHOP;
}

void buySeeds(uint8_t strain) {
  if (strain >= STRAIN_COUNT || strainCatalogPos[strain] > strainUnlockedThrough ||
      credits < strainPrices[strain] || totalSeeds() > 95) return;
  credits -= strainPrices[strain];
  seedsByStrain[strain] += PLANTS_PER_ROOM;
  seedStrain = strain;
  plantStrain = strain;   // the pack you just bought is what you'll plant
  if (!plantActive && totalSeeds() >= PLANTS_PER_ROOM) {
    pageAfterPlanting = pageAfterSeedShop;
    firstPlantingDone = false;
    plantedPotMask = 0;
    page = PLANTING;
  } else {
    page = pageAfterSeedShop;
  }
  playCoin();
  saveGame();
}

void buyPestDefense() {
  if (credits < 10) return;
  credits -= 10;
  pestDefenseKits = min(99, pestDefenseKits + 2);
  playCoin();
  saveGame();
}

void buyRemediation() {
  if (credits < 12) return;
  credits -= 12;
  remediationKits = min(99, remediationKits + 2);
  playCoin();
  saveGame();
}

void buyBong() {
  if (credits < 24 || bongs > 0) return;
  credits -= 24;
  bongs = 1;
  playCoin();
  saveGame();
}

void buyLighters() {
  if (credits < 6) return;
  credits -= 6;
  lighters = min(99, lighters + 3);
  playCoin();
  saveGame();
}

void buyRollingPapers() {
  if (credits < 5) return;
  credits -= 5;
  rollingPapers = min(99, rollingPapers + 5);
  playCoin();
  saveGame();
}

void takeSmokeBreak(uint8_t product, bool makeJoint) {
  // Smoking only needs the gear and product; it no longer depends on the plant.
  if (makeJoint) {
    int16_t strain = cheapestStashStrain();
    if (product != 1 || strain < 0 || lighters == 0 || rollingPapers == 0) return;
    --stashByStrain[strain];
    --lighters;
    --rollingPapers;
    smokeAccessoryUsed = 4; // Hand-rolled joint; flower, paper, and lighter used.
    smokeProductUsed = 1;
  } else {
    if (bongs == 0) return;
    int16_t strain = -1;
    if (product == 1) strain = cheapestStashStrain();
    else if (product == 2) strain = cheapestHashStrain();
    if (strain < 0) return;
    if (product == 1) --stashByStrain[strain];
    else --hashByStrain[strain];
    smokeAccessoryUsed = 1; // Reusable bong.
    smokeProductUsed = product;
  }
  growthBoostTicks = min(99, growthBoostTicks + 10);
  stonedLevel = min(100, stonedLevel + 25);
  smokeAnimationRequested = true;
  lastFarmerVisitAt = millis();
  saveGame();
}

void buyHashRoom() {
  if (hashRoomUnlocked) {
    page = HASH_ROOM;
    return;
  }
  if (credits < HASH_ROOM_COST) return;
  credits -= HASH_ROOM_COST;
  hashRoomUnlocked = true;
  playCoin();
  saveGame();
  page = HASH_ROOM;
}

const char *gradeName(uint8_t grade) {
  switch (grade) {
    case 120: return "A+";
    case 100: return "A";
    case 80:  return "B";
    case 60:  return "C";
    default:  return "D";
  }
}

// Vigor at harvest -> percent of the strain's base price paid at the market:
// 90+ premium (A+), 70-89 full price (A), 50-69 80% (B), 30-49 60% (C), else 40% (D).
uint8_t sellGradeForQuality(uint8_t q) {
  if (q >= 90) return 120;
  if (q >= 70) return 100;
  if (q >= 50) return 80;
  if (q >= 30) return 60;
  return 40;
}

uint16_t flowerUnitPrice(uint8_t strain) {
  return (uint16_t)((uint32_t)strainFlowerValue[strain] *
                    sellGradeForQuality(stashQualityByStrain[strain]) / 100);
}

uint16_t hashUnitPrice(uint8_t strain) {
  return (uint16_t)((uint32_t)strainHashValue[strain] *
                    sellGradeForQuality(hashQualityByStrain[strain]) / 100);
}

// Weighted-blend a jar's existing quality with new units of another quality.
uint8_t blendQuality(uint8_t oldQ, uint16_t oldUnits, uint8_t newQ, uint16_t newUnits) {
  uint32_t total = (uint32_t)oldUnits + newUnits;
  if (!total) return newQ;
  return (uint8_t)(((uint32_t)oldQ * oldUnits + (uint32_t)newQ * newUnits) / total);
}

void sellOneFlower() {
  // SELL 1 moves the top-shelf jar first.
  int16_t strain = priciestStashStrain();
  if (strain < 0) return;
  --stashByStrain[strain];
  credits += flowerUnitPrice(strain);
  playCoin();
  saveGame();
}

void sellAllFlowers() {
  uint32_t total = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    total += (uint32_t)stashByStrain[i] * flowerUnitPrice(i);
    stashByStrain[i] = 0;
  }
  if (total == 0) return;
  uint32_t newCredits = (uint32_t)credits + total;
  credits = newCredits > 65535 ? 65535 : (uint16_t)newCredits;
  playCoin();
  saveGame();
}

void sellOneHash() {
  // SELL 1 moves the top-shelf jar first.
  int16_t strain = priciestHashStrain();
  if (!hashRoomUnlocked || strain < 0) return;
  --hashByStrain[strain];
  credits += hashUnitPrice(strain);
  playCoin();
  saveGame();
}

void sellAllHash() {
  if (!hashRoomUnlocked) return;
  uint32_t total = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    total += (uint32_t)hashByStrain[i] * hashUnitPrice(i);
    hashByStrain[i] = 0;
  }
  if (total == 0) return;
  uint32_t newCredits = (uint32_t)credits + total;
  credits = newCredits > 65535 ? 65535 : (uint16_t)newCredits;
  playCoin();
  saveGame();
}

void prepareDemoHarvest() {
  plantActive = true;
  growth = 100;
  hydration = 100;
  vigor = 100;
  pestType = 0;
  pestSeverity = 0;
  page = GARDEN;
  saveGame();
}

void resetGame() {
  preferences.clear();
  newGame();
  resetArmed = false;
}

void drawWateringFrame(int frame) {
  // The selected farmer crosses the room in the shared branded outfit.
  uint16_t skin = farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143);
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);
  uint16_t can = rgb565(202, 169, 73);
  uint16_t water = rgb565(76, 184, 230);
  int x = 136;

  canvas.fillRect(x + 11, 145, 5, 17, pants);
  canvas.fillRect(x + 22, 145, 5, 17, pants);
  canvas.fillRoundRect(x + 6, 124, 27, 25, 6, shirt);
  canvas.fillRoundRect(x + 2, 126, 8, 20, 3, shirt);
  canvas.fillRoundRect(x + 29, 126, 8, 20, 3, shirt);
  canvas.drawFastHLine(x + 2, 144, 8, rgb565(28, 87, 48));
  canvas.drawFastHLine(x + 29, 144, 8, rgb565(28, 87, 48));
  canvas.drawLine(x + 8, 130, x - 5, 126, shirt);
  canvas.drawLine(x - 5, 126, x - 11, 124, skin);
  canvas.drawLine(x + 33, 135, x + 37, 136, shirt);
  canvas.drawLine(x + 37, 136, x + 40, 137, skin);
  drawLeafLogo(x + 8, 125, 1, TFT_WHITE);
  drawFarmerHeadLarge(x + 20, 112, farmerCharacter);

  int canX = x - 20;
  canvas.fillRoundRect(canX, 120, 18, 12, 3, can);
  canvas.drawRoundRect(canX, 120, 18, 12, 3, rgb565(111, 92, 49));
  canvas.drawLine(canX + 3, 120, canX + 7, 116, can);
  canvas.drawLine(canX + 7, 116, canX + 12, 120, can);
  canvas.drawLine(canX, 124, canX - 8, 121, can);

  int targetX = 22 + frame * 18;
  canvas.drawLine(canX - 8, 121, targetX, 146, water);
  for (int i = 0; i < 4; ++i) {
    int dx = ((frame + i) % 4) * 3 - 4;
    int dy = ((frame * 2 + i) % 5) * 3;
    canvas.fillCircle(targetX + dx, 143 + dy, 1, water);
  }
}

void animateWatering() {
  canvas.fillScreen(BG);
  drawHeader();
  drawPlant();
  drawGardenSidePanel();
  // Keep the bottom of the page alive through the animation: the watering
  // frames only paint the middle band, so the action row and nav bar can
  // stay visible the whole time.
  drawGardenActions();
  drawNav();
  canvas.setTextColor(GOLD, BG);
  canvas.drawString("WATERING THE ROOM", 12, 37, 1);
  for (int frame = 0; frame < 8; ++frame) {
    drawWateringFrame(frame);
    presentFrame();
    pumpDelay(420);
  }
  // No tap-to-continue: finish with one clean full garden redraw so the
  // frame trails vanish and the updated stats are on screen — then the
  // game simply carries on.
  render();
}

void drawFarmerInspector(int pose) {
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);
  uint16_t skin = farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143);
  // Fullscreen: standing on the right. Garden view: standing just left of the
  // stats panel, as if he walked out from behind it.
  const int cx = page == PLANT_VIEW ? 274 : 152;
  const int footY = page == PLANT_VIEW ? 207 : 168;
  int torsoW = farmerCharacter == 4 ? 46 : 36;
  int torsoH = farmerCharacter == 4 ? 36 : 41;
  canvas.fillRect(cx - 10, footY - 18, 7, 18, pants);
  canvas.fillRect(cx + 3, footY - 18, 7, 18, pants);
  canvas.fillRoundRect(cx - torsoW / 2, footY - 18 - torsoH, torsoW, torsoH, 6, shirt);
  canvas.fillRoundRect(cx - torsoW / 2 - 5, footY - 18 - torsoH + 5, 8, torsoH - 8, 3, shirt);
  canvas.fillRoundRect(cx + torsoW / 2 - 3, footY - 18 - torsoH + 5, 8, torsoH - 8, 3, shirt);
  drawLeafLogo(cx - 12, footY - 18 - torsoH + 3, 1, TFT_WHITE);
  canvas.drawLine(cx - torsoW / 2 - 2, footY - 31, cx - torsoW / 2 + 1, footY - 24, skin);
  canvas.drawLine(cx + torsoW / 2 + 2, footY - 31, cx + torsoW / 2 - 1, footY - 24, skin);
  drawFarmerHeadLarge(cx, footY - 59, farmerCharacter);
}

void drawFarmerSpeechBubble(const FarmerSaying &saying) {
  bool zoomed = page == PLANT_VIEW;
  int bubbleX = zoomed ? 181 : 59;
  int bubbleY = zoomed ? 80 : 37;
  int tailY = bubbleY + 43;
  int apexX = bubbleX + 79;   // tail points at the farmer's head
  canvas.fillTriangle(apexX - 10, tailY - 2, apexX + 6, tailY - 2, apexX, tailY + 13, TFT_WHITE);
  canvas.drawTriangle(apexX - 10, tailY - 2, apexX + 6, tailY - 2, apexX, tailY + 13, PANEL_LIGHT);
  canvas.fillRoundRect(bubbleX, bubbleY, 132, 43, 7, TFT_WHITE);
  canvas.drawRoundRect(bubbleX, bubbleY, 132, 43, 7, PANEL_LIGHT);
  canvas.setTextColor(0x0000, TFT_WHITE);
  canvas.drawCentreString(saying.line1, bubbleX + 66, bubbleY + (saying.line2[0] ? 6 : 15), 1);
  if (saying.line2[0]) canvas.drawCentreString(saying.line2, bubbleX + 66, bubbleY + 22, 1);
}

void animateFarmerVisit() {
  const FarmerSaying &saying = farmerSayings[random(20)];
  drawFarmerInspector(2);
  drawFarmerSpeechBubble(saying);
  presentFrame();
  pumpDelay(2300);
  // Restore by redrawing the exact content that was covered. The display's
  // GRAM read-back returns corrupted colors, so pixel snapshots are not
  // usable; redrawing identical pixels is visually flicker-free. Only the
  // covered regions are touched — stats, buttons, and header are not redrawn
  // unless the overlay actually covered them.
  if (page == GARDEN) {
    drawPlant();              // plant scene incl. bubble/farmer area (x 0-177)
    drawGardenZoomChip();     // bubble's left edge overlapped the chip
    drawGardenSidePanel();    // bubble/farmer overlapped the panel's left edge
  } else if (page == PLANT_VIEW) {
    // One region spanning the bubble and the farmer, art restored 1:1.
    if (!drawPlantBitmapFullScreenRegion(180, 79, 314, 209)) {
      restoreFullScreenFallbackRegion();
    }
  }
  presentFrame();
}

void beginCutsceneScene() {
  // Cutscenes own the display until they finish; no tabs or prior menu are
  // drawn underneath. Frame updates are confined to the scene body.
  canvas.fillScreen(BG);
  drawHeader();
  presentFrame();
}

void waitForCutsceneContinue() {
  canvas.fillRect(78, 216, 164, 18, BG);
  canvas.setTextColor(GOLD, BG);
  canvas.drawCentreString("TAP TO CONTINUE", 160, 220, 1);
  presentFrame();
  while (touchscreen.tirqTouched() && touchscreen.touched()) pumpDelay(12);
  while (!(touchscreen.tirqTouched() && touchscreen.touched())) pumpDelay(12);
  while (touchscreen.tirqTouched() && touchscreen.touched()) pumpDelay(12);
}

void drawSmokeCutsceneFrame(int frame) {
  canvas.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  canvas.fillRoundRect(13, 38, 294, 158, 10, PANEL);
  canvas.drawRoundRect(13, 38, 294, 158, 10, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString("SMOKE BREAK", 160, 47, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawCentreString(smokeAccessoryUsed == 4 ? "JOINT + FLOWER" :
                          (smokeProductUsed == 2 ? "BONG + HASH" : "BONG + FLOWER"), 160, 70, 1);

  uint16_t glass = rgb565(82, 191, 173);
  uint16_t glassDark = rgb565(34, 112, 105);
  uint16_t item = rgb565(222, 213, 174);
  if (smokeAccessoryUsed == 1) {
    canvas.fillRoundRect(65, 122, 34, 39, 10, glassDark);
    canvas.fillRoundRect(69, 127, 26, 30, 8, glass);
    canvas.fillRect(78, 91, 8, 35, glassDark);
    canvas.fillRect(80, 87, 4, 14, glass);
    canvas.drawLine(83, 112, 96, 104, glassDark);
    canvas.fillRoundRect(91, 100, 12, 7, 2, rgb565(105, 76, 47));
    canvas.drawFastHLine(72, 145, 21, rgb565(140, 231, 218));
  } else if (smokeAccessoryUsed == 4) {
    // Hand-rolled joint: a slim paper cone with a glowing tip.
    canvas.drawLine(72, 149, 125, 131, item);
    canvas.drawLine(73, 152, 126, 134, rgb565(169, 144, 104));
    canvas.drawLine(76, 151, 77, 146, rgb565(105, 76, 47));
    canvas.fillCircle(126, 132, 3, GOLD);
    canvas.fillCircle(127, 131, 1, TFT_YELLOW);
  } else {
    canvas.fillRoundRect(74, 128, 53, 34, 4, item);
    canvas.drawRoundRect(74, 128, 53, 34, 4, GOLD);
    canvas.drawFastHLine(80, 138, 40, rgb565(169, 144, 104));
    canvas.drawFastHLine(80, 145, 34, rgb565(169, 144, 104));
    canvas.drawFastHLine(80, 152, 38, rgb565(169, 144, 104));
  }

  uint16_t skin = farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143);
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);
  int cx = 226;
  int footY = 164;
  canvas.fillRect(cx - 12, footY - 22, 8, 22, pants);
  canvas.fillRect(cx + 4, footY - 22, 8, 22, pants);
  canvas.fillRoundRect(cx - 23, footY - 62, 46, 42, 8, shirt);
  canvas.fillRoundRect(cx - 29, footY - 55, 11, 32, 4, shirt);
  canvas.fillRoundRect(cx + 18, footY - 55, 11, 32, 4, shirt);
  drawLeafLogo(cx - 12, footY - 58, 1, TFT_WHITE);
  canvas.drawLine(cx - 19, footY - 53, 160, 117, shirt);
  canvas.drawLine(160, 117, 156, 119, skin);
  canvas.drawLine(cx + 23, footY - 48, cx + 25, footY - 24, shirt);
  canvas.drawLine(cx + 25, footY - 24, cx + 27, footY - 22, skin);
  int headY = footY - 78;
  drawFarmerHeadLarge(cx, headY, farmerCharacter);

  for (int puff = 0; puff < frame; ++puff) {
    int px = 108 + puff * 12;
    int py = 105 - (puff % 2) * 8;
    canvas.drawCircle(px, py, 3 + (puff % 3), TEXT_DIM);
    canvas.drawCircle(px + 4, py - 3, 2 + (puff % 2), 0xBDF7);
  }
  canvas.setTextColor(GOLD, PANEL);
  canvas.drawCentreString(frame >= 3 ? "GARDEN GLOW ACTIVATED" : "A LITTLE ROOM BOOST", 160, 176, 1);
}

// Optional SD art for the gown-up intro. Two naming schemes, tried in order:
//   /GOWN<c><f>.BMP — per-character (e.g. GOWN14.BMP = Mathias, frame 4)
//   /GOWN<f>.BMP    — one generic set for any character
// All files: 320x240, 24-bit BMP. Falls back to the built-in vector scene.
// Memory-friendly: rows stream through a small heap buffer, no framebuffer.
bool tryDrawGownBmp(const char *filename) {
  switchToSdBus();
  File file = SD.open(filename, FILE_READ);
  bool drawn = false;
  if (file) {
    file.seek(10);
    uint32_t pixelOffset = readLE32(file);
    file.seek(18);
    int32_t width = (int32_t)readLE32(file);
    int32_t height = (int32_t)readLE32(file);
    file.seek(28);
    uint16_t bitsPerPixel = readLE16(file);
    uint32_t compression = readLE32(file);
    if (width == SCREEN_W && abs(height) == SCREEN_H &&
        bitsPerPixel == 24 && compression == 0) {
      uint8_t *row = (uint8_t *)malloc((SCREEN_W * 3 + 3) & ~3UL);
      if (row) {
        uint32_t rowBytes = (SCREEN_W * 3 + 3) & ~3UL;
        bool oldSwap = canvas.getSwapBytes();
        canvas.setSwapBytes(true);
        file.seek(pixelOffset);
        drawn = true;
        for (int r = 0; r < SCREEN_H && drawn; ++r) {
          if (file.read(row, rowBytes) != rowBytes) {
            drawn = false;
            break;
          }
          for (int x = 0; x < SCREEN_W; ++x) {
            rgbRow[x] = rgb565(row[x * 3 + 2], row[x * 3 + 1], row[x * 3 + 0]);
          }
          int screenY = height > 0 ? (SCREEN_H - 1 - r) : r;
          canvas.pushImage(0, screenY, SCREEN_W, 1, rgbRow);
        }
        canvas.setSwapBytes(oldSwap);
        free(row);
      }
    }
    file.close();
  }
  switchToTouchBus();
  return drawn;
}

bool drawGownUpBmpFrame(int frame) {
  if (!sdMounted) return false;
  char filename[16];
  snprintf(filename, sizeof(filename), "/GOWN%d%d.BMP", farmerCharacter, frame);
  if (tryDrawGownBmp(filename)) return true;
  snprintf(filename, sizeof(filename), "/GOWN%d.BMP", frame);
  return tryDrawGownBmp(filename);
}

// Scaled copy of drawFarmerHeadLarge for the intro close-up (scale 1 = normal).
void drawFarmerHeadScaled(int cx, int cy, uint8_t character, int s) {
#define SP(v) ((v) * (s))
  uint16_t hair = character == 3 ? rgb565(210, 176, 104) : rgb565(35, 27, 26);
  uint16_t skin = character == 2 ? rgb565(133, 84, 61)
                : character == 6 ? rgb565(172, 112, 78)
                : character == 7 ? rgb565(216, 156, 116)
                : rgb565(230, 184, 143);
  uint16_t beard = rgb565(52, 37, 33);
  uint16_t black = rgb565(16, 19, 22);

  if (character == 5) {
    uint16_t jHair = rgb565(124, 110, 97), jLight = rgb565(168, 156, 142), jDark = rgb565(88, 76, 66);
    canvas.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(34), min(SP(10), SP(16)), jHair);
    canvas.fillCircle(cx, cy, SP(10), skin);
    canvas.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), jHair);
    canvas.fillRect(cx + SP(-14), cy + SP(-6), SP(5), SP(20), jHair);
    canvas.fillRect(cx + SP(9), cy + SP(-6), SP(5), SP(20), jHair);
    canvas.drawLine(cx + SP(-9), cy + SP(-11), cx + SP(9), cy + SP(-13), jLight);
    canvas.drawLine(cx + SP(-10), cy + SP(-3), cx + SP(-10), cy + SP(9), jLight);
    canvas.drawLine(cx + SP(10), cy + SP(-3), cx + SP(10), cy + SP(9), jDark);
    canvas.drawFastHLine(cx + SP(-6), cy + SP(-4), SP(4), jDark);
    canvas.drawFastHLine(cx + 2, cy + SP(-4), SP(4), jDark);
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    canvas.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    canvas.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    canvas.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    canvas.drawFastHLine(cx + SP(-4), cy + SP(5), SP(8), rgb565(150, 90, 80));
    uint16_t jGoatee = rgb565(152, 146, 140);
    canvas.drawFastHLine(cx + SP(-4), cy + SP(3), SP(8), jGoatee);
    canvas.drawFastHLine(cx + SP(-3), cy + SP(2), SP(6), jGoatee);
    canvas.drawFastVLine(cx + SP(-5), cy + SP(3), SP(4), jGoatee);
    canvas.drawFastVLine(cx + SP(4), cy + SP(3), SP(4), jGoatee);
    canvas.fillRoundRect(cx + SP(-3), cy + SP(6), SP(7), SP(6), min(SP(2), SP(3)), jGoatee);
    return;
  }

  if (character == 1) {
    uint16_t mHair = rgb565(96, 64, 40), mHairLight = rgb565(134, 95, 60);
    canvas.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(31), min(SP(10), SP(15)), mHair);
    canvas.fillCircle(cx, cy, SP(10), skin);
    canvas.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), mHair);
    canvas.fillRect(cx + SP(-14), cy + SP(-6), SP(5), SP(23), mHair);
    canvas.fillRect(cx + SP(9), cy + SP(-6), SP(5), SP(23), mHair);
    canvas.drawFastVLine(cx + SP(-12), cy + SP(-2), SP(18), mHairLight);
    canvas.drawFastVLine(cx + SP(-11), cy + SP(2), SP(15), mHairLight);
    canvas.drawFastVLine(cx + SP(12), cy + SP(-1), SP(16), mHairLight);
    canvas.drawFastVLine(cx + SP(11), cy + SP(4), SP(13), mHairLight);
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    canvas.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    canvas.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    canvas.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    canvas.fillRoundRect(cx + SP(-7), cy + SP(4), SP(14), SP(5), min(SP(2), SP(2)), rgb565(110, 74, 48));
    canvas.fillRect(cx + SP(-1), cy + SP(4), SP(2), SP(2), skin);
    return;
  }

  if (character == 6) {
    uint16_t buzz = rgb565(45, 32, 26);
    canvas.fillCircle(cx, cy + SP(-2), SP(11), buzz);             // buzz crescent
    canvas.fillCircle(cx, cy, SP(10), skin);                      // face carves it back
    for (int i = -8; i <= 8; i += 4)
      canvas.drawFastVLine(cx + SP(i), cy + SP(-12), SP(3), rgb565(74, 55, 42));
    canvas.fillRect(cx + SP(-10), cy + SP(-5), SP(2), SP(2), buzz);
    canvas.fillRect(cx + SP(8), cy + SP(-5), SP(2), SP(2), buzz);
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), rgb565(206, 226, 232));
    canvas.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), rgb565(206, 226, 232));
    canvas.drawRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.drawRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    canvas.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    canvas.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    canvas.drawFastHLine(cx + SP(-4), cy + SP(4), SP(8), beard);
    canvas.fillRect(cx + SP(-2), cy + SP(7), SP(4), SP(3), beard);
    return;
  }

  if (character == 7) {
    uint16_t vHair = rgb565(58, 38, 26), vCurl = rgb565(94, 64, 42);
    canvas.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(30), min(SP(10), SP(14)), vHair);
    canvas.fillCircle(cx, cy, SP(10), skin);
    canvas.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), vHair);
    canvas.fillRect(cx + SP(-14), cy + SP(-6), SP(5), SP(21), vHair);
    canvas.fillRect(cx + SP(9), cy + SP(-6), SP(5), SP(21), vHair);
    for (int i = -12; i <= 12; i += 6)
      canvas.fillCircle(cx + SP(i), cy + SP(-15) + abs(i) / 3 * s / 3, s + s / 2, vCurl);
    canvas.fillCircle(cx + SP(-12), cy + SP(-2), s + s / 2, vCurl);
    canvas.fillCircle(cx + SP(12), cy + SP(1), s + s / 2, vCurl);
    canvas.fillCircle(cx + SP(-12), cy + SP(5), s + s / 2, vCurl);
    canvas.fillCircle(cx + SP(12), cy + SP(7), s + s / 2, vCurl);
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    canvas.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    canvas.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    canvas.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    return;
  }

  canvas.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(36), min(SP(10), SP(17)), hair);
  if (character == 2) {
    for (int i = -2; i <= 2; ++i) {
      canvas.drawFastVLine(cx + SP(i * 5), cy + SP(-12), SP(36), rgb565(65, 44, 35));
      canvas.drawFastVLine(cx + SP(i * 5 + 2), cy + SP(-10), SP(33), hair);
    }
  }
  canvas.fillCircle(cx, cy, SP(10), skin);

  if (character == 0) {
    canvas.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(8), min(SP(6), SP(11)), hair);
    canvas.fillRect(cx + SP(-13), cy + SP(-6), SP(5), SP(28), hair);
    canvas.fillRect(cx + SP(8), cy + SP(-6), SP(5), SP(28), hair);
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    canvas.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    canvas.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    canvas.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    canvas.drawFastHLine(cx + SP(-7), cy + SP(4), SP(14), beard);
    canvas.drawFastHLine(cx + SP(-5), cy + SP(7), SP(10), TFT_WHITE);
    canvas.fillTriangle(cx + SP(-6), cy + SP(9), cx + SP(6), cy + SP(9), cx, cy + SP(16), beard);
  } else if (character == 2) {
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-5), SP(9), SP(7), min(SP(2), SP(3)), TFT_BLACK);
    canvas.fillRoundRect(cx + SP(1), cy + SP(-5), SP(9), SP(7), min(SP(2), SP(3)), TFT_BLACK);
    canvas.drawLine(cx + SP(-1), cy + SP(-2), cx + SP(1), cy + SP(-2), black);
    canvas.drawLine(cx + SP(-10), cy + SP(-5), cx + SP(-1), cy + SP(-5), black);
    canvas.drawLine(cx + SP(1), cy + SP(-5), cx + SP(10), cy + SP(-5), black);
    canvas.fillTriangle(cx + SP(-8), cy + SP(4), cx + SP(8), cy + SP(4), cx, cy + SP(17), beard);
    canvas.drawFastHLine(cx + SP(-4), cy + SP(6), SP(8), TFT_WHITE);
  } else if (character == 8) {
    // Pedro: same head as drawFarmerHeadLarge's Pedro branch, scaled.
    uint16_t pHair = rgb565(30, 26, 24), pSkin = rgb565(212, 168, 138);
    uint16_t pBeard = rgb565(62, 52, 47);
    canvas.fillRoundRect(cx + SP(-12), cy + SP(-12), SP(24), SP(24), min(SP(8), SP(12)), pHair);
    canvas.fillCircle(cx, cy, SP(10), pSkin);
    canvas.fillRoundRect(cx + SP(-11), cy + SP(-11), SP(22), SP(5), min(SP(4), SP(7)), pHair);
    canvas.fillRect(cx + SP(-11), cy + SP(-6), SP(2), SP(6), pHair);
    canvas.fillRect(cx + SP(9), cy + SP(-6), SP(2), SP(6), pHair);
    canvas.fillRoundRect(cx + SP(-9), cy + SP(2), SP(18), SP(13), min(SP(6), SP(10)), pBeard);
    canvas.drawFastHLine(cx + SP(-8), cy + SP(4), SP(5), rgb565(122, 114, 106));
    canvas.drawFastHLine(cx + SP(3), cy + SP(5), SP(5), rgb565(122, 114, 106));
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(2), SP(5)), black);
    canvas.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(2), SP(5)), black);
    canvas.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    canvas.drawLine(cx + SP(-8), cy + SP(-4), cx + SP(-5), cy + SP(-2), rgb565(95, 95, 102));
  } else if (character == 4) {
    canvas.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), hair);
    canvas.fillRoundRect(cx + SP(-15), cy + SP(-4), SP(5), SP(22), min(SP(3), 2 * s), hair);
    canvas.fillRoundRect(cx + SP(10), cy + SP(-4), SP(5), SP(22), min(SP(3), 2 * s), hair);
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-5), SP(9), SP(7), min(SP(2), SP(3)), rgb565(10, 13, 16));
    canvas.fillRoundRect(cx + SP(1), cy + SP(-5), SP(9), SP(7), min(SP(2), SP(3)), rgb565(10, 13, 16));
    canvas.drawFastHLine(cx + SP(-1), cy + SP(-2), SP(2), rgb565(10, 13, 16));
    canvas.drawFastHLine(cx + SP(-9), cy + SP(-4), SP(7), rgb565(96, 107, 111));
    canvas.drawFastHLine(cx + SP(2), cy + SP(-4), SP(7), rgb565(96, 107, 111));
    canvas.drawFastHLine(cx + SP(-4), cy + SP(6), SP(8), rgb565(112, 65, 52));
  } else {
    canvas.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(8), min(SP(6), SP(11)), hair);
    canvas.fillRect(cx + SP(-13), cy + SP(-6), SP(5), SP(29), hair);
    canvas.fillRect(cx + SP(8), cy + SP(-6), SP(5), SP(29), hair);
    canvas.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    canvas.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    canvas.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    canvas.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    canvas.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    canvas.drawFastVLine(cx + SP(-11), cy + SP(2), SP(15), rgb565(239, 216, 151));
    canvas.drawFastHLine(cx + SP(-4), cy + SP(6), SP(8), rgb565(167, 83, 78));
    canvas.drawFastHLine(cx + SP(-3), cy + SP(8), SP(6), TFT_WHITE);
  }
#undef SP
}

void drawGownUpCutsceneFrame(int frame) {
  if (drawGownUpBmpFrame(frame)) return;  // SD art overrides the vector scene
  bool bearded = farmerCharacter == 0 || farmerCharacter == 2 || farmerCharacter == 5 ||
                  farmerCharacter == 6;
  // Only the opening frame gets text; the rest of the tour speaks for itself.
  const char *captions[] = {
    "New hire!", "", "", "", "", "", "", ""
  };
  canvas.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  canvas.fillRect(0, NAV_Y, SCREEN_W, SCREEN_H - NAV_Y, BG);  // clear caption strip
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);

  int cx, footY, cy;
  if (frame <= 3) {
    // ---- Extreme close-up: slow push-in on the grower's face ----
    static const int zoomS[] = {3, 4, 5, 5};
    static const int zoomCy[] = {142, 136, 128, 126};
    for (int y = HEADER_H; y < NAV_Y; ++y) {
      float blend = (float)(y - HEADER_H) / (NAV_Y - HEADER_H);
      uint8_t v = 34 + (uint8_t)(blend * 22);
      canvas.drawFastHLine(0, y, SCREEN_W, rgb565(v - 4, v + 8, v + 4));
    }
    cx = 160;
    cy = zoomCy[frame];
    drawFarmerHeadScaled(cx, cy, farmerCharacter, zoomS[frame]);
    canvas.setTextColor(TEXT_DIM, BG);
    canvas.drawCentreString(captions[min(frame, 9)], 160, 212, 1);
    return;
  } else if (frame <= 5) {
    // ---- Locker room: hairnet, then beard net ----
    for (int y = HEADER_H; y < 172; ++y) {
      float blend = (float)(y - HEADER_H) / 144.0f;
      uint8_t v = 30 + (uint8_t)(blend * 26);
      canvas.drawFastHLine(0, y, SCREEN_W, rgb565(v - 4, v + 8, v + 4));
    }
    canvas.fillRect(0, 172, SCREEN_W, NAV_Y - 172, rgb565(38, 46, 42));
    canvas.drawFastHLine(0, 172, SCREEN_W, rgb565(58, 70, 60));
    for (int i = 0; i < 6; ++i) {  // lockers along the back wall
      int lx = 14 + i * 49;
      canvas.fillRoundRect(lx, 46, 42, 110, 4, rgb565(52, 74, 78));
      canvas.drawRoundRect(lx, 46, 42, 110, 4, rgb565(70, 96, 100));
      for (int v = 0; v < 3; ++v)
        canvas.drawFastHLine(lx + 10, 62 + v * 5, 22, rgb565(40, 58, 62));
      canvas.fillRect(lx + 30, 96, 4, 12, rgb565(140, 150, 140));
    }
    cx = 160;
    footY = 174;
    canvas.fillRoundRect(cx - 14, footY - 2, 30, 3, 1, rgb565(30, 38, 34));
    canvas.fillRect(cx - 12, footY - 22, 8, 22, pants);
    canvas.fillRect(cx + 4, footY - 22, 8, 22, pants);
    cy = footY - 78;
  } else if (frame == 6) {
    // ---- Walking down the hallway toward the grow-room door ----
    for (int y = HEADER_H; y < 172; ++y) {
      float blend = (float)(y - HEADER_H) / 144.0f;
      uint8_t v = 30 + (uint8_t)(blend * 26);
      canvas.drawFastHLine(0, y, SCREEN_W, rgb565(v - 4, v + 8, v + 4));
    }
    canvas.fillRect(0, 172, SCREEN_W, NAV_Y - 172, rgb565(38, 46, 42));
    canvas.drawFastHLine(0, 172, SCREEN_W, rgb565(58, 70, 60));
    canvas.fillRect(252, 44, 52, 128, rgb565(12, 40, 22));  // open doorway
    canvas.fillRect(256, 48, 44, 120, rgb565(24, 88, 44));
    for (int i = 0; i < 3; ++i) {
      int px = 262 + i * 14;
      canvas.fillRect(px, 128, 2, 30, rgb565(14, 50, 26));
      canvas.fillCircle(px + 1, 124, 5, rgb565(20, 70, 36));
    }
    canvas.drawRoundRect(250, 42, 56, 132, 4, rgb565(90, 110, 96));
    canvas.fillTriangle(256, 172, 300, 172, 236, NAV_Y - 1, rgb565(26, 74, 40));

    cx = 196;
    footY = 173;  // mid-stride bob
    canvas.fillRoundRect(cx - 14, footY - 2, 30, 3, 1, rgb565(30, 38, 34));
    canvas.fillRect(cx - 12, footY - 20, 7, 18, pants);  // stride legs
    canvas.fillRect(cx + 5, footY - 22, 8, 22, pants);
    for (int i = 0; i < 3; ++i)  // motion dashes behind the farmer
      canvas.drawFastHLine(cx - 26 - i * 6, footY - 14 - i * 6, 6,
                           rgb565(120, 140, 130));
    cy = footY - 78;
  } else {
    // ---- Grow room interior: rows of plants under hanging lights ----
    for (int y = HEADER_H; y < 204; ++y) {
      float blend = (float)(y - HEADER_H) / 176.0f;
      uint8_t r = 5 + (uint8_t)(blend * 10);
      uint8_t g = 19 + (uint8_t)(blend * 30);
      uint8_t b = 21 + (uint8_t)(blend * 20);
      canvas.drawFastHLine(0, y, SCREEN_W, rgb565(r, g, b));
    }
    canvas.drawFastHLine(0, 70, SCREEN_W, rgb565(23, 44, 42));
    canvas.drawFastHLine(0, 118, SCREEN_W, rgb565(23, 44, 42));
    canvas.fillRect(0, 204, SCREEN_W, NAV_Y - 204, rgb565(32, 49, 39));
    canvas.drawFastHLine(0, 204, SCREEN_W, rgb565(76, 91, 61));
    for (int i = 0; i < 3; ++i) {  // hanging grow lights
      int lx = 34 + i * 104;
      canvas.fillRect(lx, 34, 44, 5, rgb565(70, 82, 88));
      canvas.drawFastHLine(lx + 2, 40, 40, GOLD);
      canvas.drawFastHLine(lx + 8, 42, 28, rgb565(180, 170, 90));
    }
    bool savedActive = plantActive;  // borrowed by the live plant drawing
    uint8_t savedGrowth = growth;
    plantActive = true;
    growth = 55;  drawMiniRoomPlant(50, 150, 0.55f);
    growth = 75;  drawMiniRoomPlant(105, 150, 0.60f);
    growth = 40;  drawMiniRoomPlant(215, 150, 0.55f);
    growth = 90;  drawMiniRoomPlant(270, 150, 0.60f);
    growth = 100; drawMiniRoomPlant(45, 196, 0.95f);
    growth = 100; drawMiniRoomPlant(120, 198, 1.05f);
    growth = 100; drawMiniRoomPlant(200, 198, 1.00f);
    growth = 100; drawMiniRoomPlant(275, 196, 0.95f);
    plantActive = savedActive;
    growth = savedGrowth;

    cx = 160;
    footY = 202;
    canvas.fillRoundRect(cx - 14, footY - 2, 30, 3, 1, rgb565(26, 40, 32));
    canvas.fillRect(cx - 12, footY - 22, 8, 22, pants);
    canvas.fillRect(cx + 4, footY - 22, 8, 22, pants);
    cy = footY - 78;
  }

  canvas.fillRoundRect(cx - 18, footY - 62, 36, 42, 8, shirt);
  canvas.fillRoundRect(cx - 23, footY - 55, 8, 32, 4, shirt);
  canvas.fillRoundRect(cx + 15, footY - 55, 8, 32, 4, shirt);
  drawLeafLogo(cx - 12, footY - 58, 1, TFT_WHITE);
  drawFarmerHeadLarge(cx, cy, farmerCharacter);

  if (frame >= 4) {  // hairnet goes on in the locker room and stays on
    uint16_t net = rgb565(238, 238, 238), mesh = rgb565(185, 190, 195);
    canvas.fillRoundRect(cx - 13, cy - 17, 26, 14, 7, net);
    for (int i = -9; i <= 9; i += 6) canvas.drawFastVLine(cx + i, cy - 16, 11, mesh);
    canvas.drawFastHLine(cx - 12, cy - 13, 24, mesh);
    canvas.drawFastHLine(cx - 13, cy - 9, 26, mesh);
    canvas.drawFastHLine(cx - 13, cy - 4, 26, mesh);
  }
  if (frame >= 5 && bearded) {  // beard net after the hairnet
    uint16_t net = rgb565(238, 238, 238), mesh = rgb565(185, 190, 195);
    canvas.fillRoundRect(cx - 8, cy + 2, 16, 15, 5, net);
    canvas.drawFastVLine(cx - 3, cy + 3, 13, mesh);
    canvas.drawFastVLine(cx + 3, cy + 3, 13, mesh);
    canvas.drawLine(cx - 8, cy + 4, cx - 13, cy - 1, mesh);
    canvas.drawLine(cx + 8, cy + 4, cx + 13, cy - 1, mesh);
  }
  if (captions[min(frame, 7)][0]) {
    canvas.setTextColor(TEXT_DIM, BG);
    canvas.drawCentreString(captions[min(frame, 7)], 160, 212, 1);
  }
}

void animateGownUpCutscene() {
  beginCutsceneScene();
  for (int frame = 0; frame < 8; ++frame) {
    drawGownUpCutsceneFrame(frame);
    presentFrame();
    pumpDelay(1200);  // brisk but readable
  }
  waitForCutsceneContinue();
}

void animateSmokeBreak() {
  beginCutsceneScene();
  for (int frame = 0; frame < 5; ++frame) {
    drawSmokeCutsceneFrame(frame);
    presentFrame();
    pumpDelay(420);
  }
  waitForCutsceneContinue();
}

void drawHashWashFrame(int frame) {
  canvas.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  canvas.fillRoundRect(13, 38, 294, 158, 10, PANEL);
  canvas.drawRoundRect(13, 38, 294, 158, 10, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString("HASH WASH ROOM", 160, 46, 2);

  uint16_t woodDark = rgb565(74, 42, 30);
  uint16_t wood = rgb565(143, 83, 49);
  uint16_t woodLight = rgb565(190, 126, 77);
  uint16_t water = rgb565(56, 137, 170);
  canvas.fillRoundRect(78, 107, 163, 73, 15, woodDark);
  canvas.fillRoundRect(85, 111, 149, 64, 12, wood);
  for (int x = 103; x <= 216; x += 22) {
    canvas.drawFastVLine(x, 116, 53, woodDark);
    canvas.drawFastVLine(x + 2, 118, 47, woodLight);
  }
  canvas.fillRoundRect(74, 98, 171, 21, 9, woodDark);
  canvas.fillRoundRect(80, 99, 159, 15, 7, woodLight);
  canvas.fillRoundRect(86, 103, 147, 10, 5, rgb565(55, 45, 35));
  canvas.fillRoundRect(91, 106, 137, 7, 3, water);
  canvas.drawFastHLine(96, 107, 127, rgb565(115, 204, 216));

  // Leafy material drops into the barrel, then the paddle sweeps through it.
  if (frame == 0) {
    drawFanLeaf(154, 84, 0.28f, -12);
    drawFanLeaf(166, 82, 0.24f, 18);
  } else if (frame == 1) {
    drawFanLeaf(151, 100, 0.26f, -12);
    drawFanLeaf(166, 102, 0.23f, 18);
  } else {
    drawFanLeaf(143, 111, 0.20f, -20);
    drawFanLeaf(165, 109, 0.19f, 15);
  }

  const int paddleX[] = {183, 171, 148, 132, 148, 171, 183};
  int sweep = paddleX[frame];
  canvas.drawLine(231, 62, sweep, 145, woodLight);
  canvas.drawLine(230, 62, sweep - 2, 145, woodDark);
  canvas.fillTriangle(sweep - 9, 141, sweep + 9, 141, sweep, 159, woodDark);
  canvas.fillTriangle(sweep - 6, 142, sweep + 6, 142, sweep, 155, woodLight);
  canvas.drawLine(235, 57, 231, 69, rgb565(230, 184, 143));
  canvas.fillCircle(231, 69, 3, rgb565(230, 184, 143));

  if (frame >= 2 && frame <= 5) {
    for (int ripple = 0; ripple < 3; ++ripple) {
      int rippleX = 112 + ((ripple * 37 + frame * 11) % 100);
      int rippleY = 108 + (ripple % 2) * 3;
      canvas.drawFastHLine(rippleX, rippleY, 12 + (frame % 3) * 4, rgb565(137, 218, 222));
    }
    for (int bubble = 0; bubble < frame - 1; ++bubble) {
      int bx = 116 + ((bubble * 27 + frame * 9) % 95);
      canvas.drawCircle(bx, 102 - (bubble % 2) * 5, 2, rgb565(164, 225, 224));
    }
  }
  if (frame == 6) {
    canvas.fillRoundRect(250, 136, 33, 39, 5, rgb565(135, 166, 132));
    canvas.drawRoundRect(250, 136, 33, 39, 5, rgb565(198, 219, 171));
    canvas.fillRoundRect(258, 129, 17, 9, 2, woodLight);
    canvas.fillCircle(266, 151, 7, rgb565(177, 190, 90));
    canvas.fillCircle(264, 149, 2, rgb565(221, 226, 141));
    canvas.fillCircle(270, 153, 2, rgb565(221, 226, 141));
  }
  canvas.setTextColor(GOLD, PANEL);
  if (frame == 6) {
    char batchLabel[30];
    snprintf(batchLabel, sizeof(batchLabel), "%u HASH UNITS READY", lastHarvestUnits);
    canvas.drawCentreString(batchLabel, 160, 180, 1);
  } else {
    canvas.drawCentreString("LEAVES IN • PADDLE WASH", 160, 180, 1);
  }
}

void animateHashWash() {
  beginCutsceneScene();
  for (int frame = 0; frame < 7; ++frame) {
    drawHashWashFrame(frame);
    presentFrame();
    pumpDelay(700);
  }
  waitForCutsceneContinue();
}

void drawFlowerHarvestFrame(int frame) {
  canvas.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  canvas.fillRoundRect(13, 38, 294, 158, 10, PANEL);
  canvas.drawRoundRect(13, 38, 294, 158, 10, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString("FLOWER HARVEST", 160, 46, 2);

  const int plantX[] = {53, 99, 145, 191};
  for (int i = frame; i < 4; ++i) {
    int x = plantX[i];
    int top = 88 + (i % 2) * 8;
    canvas.drawFastVLine(x, top + 14, 48, rgb565(35, 104, 48));
    canvas.drawLine(x, top + 31, x - 11, top + 23, rgb565(35, 104, 48));
    canvas.drawLine(x, top + 37, x + 12, top + 29, rgb565(35, 104, 48));
    drawFanLeaf(x - 11, top + 23, 0.22f, -28);
    drawFanLeaf(x + 12, top + 29, 0.22f, 28);
    drawFanLeaf(x, top + 13, 0.28f, 0);
    canvas.fillRoundRect(x - 12, 158, 24, 9, 3, rgb565(91, 49, 33));
  }

  // A small pair of shears crosses the next stem as the harvest basket fills.
  int cutX = plantX[min(frame, 3)];
  canvas.drawCircle(cutX - 5, 145, 5, rgb565(185, 195, 196));
  canvas.drawCircle(cutX + 5, 145, 5, rgb565(185, 195, 196));
  canvas.drawLine(cutX - 2, 142, cutX + 14, 132, rgb565(208, 218, 218));
  canvas.drawLine(cutX + 2, 142, cutX - 13, 132, rgb565(208, 218, 218));

  canvas.fillRoundRect(232, 130, 47, 39, 6, rgb565(117, 70, 43));
  canvas.drawRoundRect(232, 130, 47, 39, 6, rgb565(193, 139, 83));
  canvas.drawFastHLine(238, 137, 35, rgb565(219, 164, 103));
  for (int i = 0; i < frame; ++i) {
    int bx = 240 + (i % 3) * 11;
    int by = 129 + (i / 3) * 8;
    canvas.fillCircle(bx, by, 4, rgb565(122, 151, 82));
    canvas.fillCircle(bx + 2, by - 2, 2, rgb565(178, 193, 118));
  }
  canvas.setTextColor(GOLD, PANEL);
  if (frame >= 5) {
    char batchLabel[32];
    snprintf(batchLabel, sizeof(batchLabel), "%u FLOWER UNITS GATHERED", lastHarvestUnits);
    canvas.drawCentreString(batchLabel, 160, 180, 1);
  } else {
    canvas.drawCentreString("GATHERING THE ROOM", 160, 180, 1);
  }
}

void animateFlowerHarvest() {
  beginCutsceneScene();
  for (int frame = 0; frame < 6; ++frame) {
    drawFlowerHarvestFrame(frame);
    presentFrame();
    pumpDelay(520);
  }
  waitForCutsceneContinue();
}

void drawToxicMoldLogo(int cx, int cy, int scale) {
  const uint16_t toxic = rgb565(216, 244, 68);
  const uint16_t dark = rgb565(39, 55, 35);
  const uint16_t ink = rgb565(28, 34, 29);

  // Bright hazard badge with a large skull-and-crossbones toxic mark.
  canvas.fillCircle(cx, cy, 18 * scale, toxic);
  canvas.fillCircle(cx, cy, 15 * scale, dark);
  for (int offset = -1; offset <= 1; ++offset) {
    canvas.drawLine(cx - 10 * scale + offset, cy - 8 * scale,
                    cx + 10 * scale + offset, cy + 9 * scale, toxic);
    canvas.drawLine(cx - 10 * scale + offset, cy + 9 * scale,
                    cx + 10 * scale + offset, cy - 8 * scale, toxic);
  }
  for (int side = -1; side <= 1; side += 2) {
    canvas.fillCircle(cx + side * 10 * scale, cy - 8 * scale, 2 * scale, toxic);
    canvas.fillCircle(cx + side * 10 * scale, cy + 9 * scale, 2 * scale, toxic);
    canvas.fillCircle(cx + side * 10 * scale, cy - 8 * scale, scale, dark);
    canvas.fillCircle(cx + side * 10 * scale, cy + 9 * scale, scale, dark);
  }

  canvas.fillCircle(cx, cy - 2 * scale, 7 * scale, toxic);
  canvas.fillRoundRect(cx - 6 * scale, cy + 2 * scale, 12 * scale,
                       6 * scale, 2 * scale, toxic);
  canvas.fillCircle(cx - 3 * scale, cy - 2 * scale, 2 * scale, ink);
  canvas.fillCircle(cx + 3 * scale, cy - 2 * scale, 2 * scale, ink);
  canvas.fillTriangle(cx - scale, cy + scale, cx + scale, cy + scale,
                      cx, cy + 3 * scale, ink);
  for (int tooth = -2; tooth <= 2; ++tooth) {
    canvas.drawFastVLine(cx + tooth * 2 * scale, cy + 4 * scale,
                         3 * scale, ink);
  }
}

void drawMoldWarningFrame(int scale) {
  canvas.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  canvas.fillRoundRect(8, 36, 304, 160, 10, PANEL);
  canvas.drawRoundRect(8, 36, 304, 160, 10, RED);
  drawToxicMoldLogo(83, 119, scale);

  canvas.setTextColor(RED, PANEL);
  canvas.drawCentreString("MOLD FOUND", 222, 59, 2);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  char discarded[28];
  char saved[28];
  snprintf(discarded, sizeof(discarded), "%u UNIT%s DISCARDED",
           moldLossUnits, moldLossUnits == 1 ? "" : "S");
  snprintf(saved, sizeof(saved), "%u UNIT%s SAVED",
           lastHarvestUnits, lastHarvestUnits == 1 ? "" : "S");
  canvas.drawCentreString("TOXIC CROP", 222, 89, 2);
  canvas.setTextColor(GOLD, PANEL);
  canvas.drawCentreString(discarded, 222, 120, 1);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawCentreString(saved, 222, 140, 1);
  canvas.drawCentreString("Contaminated portion removed", 222, 164, 1);
  if (moldRescuedUnits) {
    char rescued[28];
    snprintf(rescued, sizeof(rescued), "REMEDIATION SAVED %u UNIT%s",
             moldRescuedUnits, moldRescuedUnits == 1 ? "" : "S");
    canvas.setTextColor(0x2C64, PANEL);
    canvas.drawCentreString(rescued, 222, 152, 1);
  }
}

void animateMoldWarning() {
  for (int scale = 1; scale <= 4; ++scale) {
    if (scale == 1) beginCutsceneScene();
    drawMoldWarningFrame(scale);
    presentFrame();
    pumpDelay(300);
  }
  waitForCutsceneContinue();
}

void drawFlowerTrimFrame(int frame) {
  canvas.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  canvas.fillRoundRect(13, 38, 294, 158, 10, PANEL);
  canvas.drawRoundRect(13, 38, 294, 158, 10, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString("TRIMMING ROOM", 160, 46, 2);
  canvas.fillRoundRect(35, 147, 250, 20, 5, rgb565(113, 73, 48));
  canvas.drawFastHLine(42, 149, 236, rgb565(188, 139, 88));

  // Flower clusters gather on the work surface as the shears move across.
  int clusterCount = 1 + frame;
  for (int i = 0; i < clusterCount; ++i) {
    int x = 74 + (i % 5) * 35;
    int y = 138 - (i / 5) * 12 - (i % 2) * 3;
    canvas.fillCircle(x, y, 7, rgb565(105, 143, 75));
    canvas.fillCircle(x - 3, y - 2, 4, rgb565(133, 163, 88));
    canvas.fillCircle(x + 3, y + 1, 4, rgb565(146, 171, 94));
    canvas.drawPixel(x, y - 3, rgb565(192, 199, 131));
  }

  int shearX = 90 + frame * 20;
  canvas.drawCircle(shearX - 5, 116, 5, rgb565(185, 195, 196));
  canvas.drawCircle(shearX + 5, 116, 5, rgb565(185, 195, 196));
  canvas.drawLine(shearX - 2, 113, shearX + 15, 99, rgb565(208, 218, 218));
  canvas.drawLine(shearX + 2, 113, shearX - 14, 99, rgb565(208, 218, 218));
  canvas.drawLine(285, 96, shearX + 8, 113, rgb565(230, 184, 143));
  canvas.fillCircle(shearX + 8, 113, 3, rgb565(230, 184, 143));

  for (int i = 0; i < frame; ++i) {
    int leafX = 55 + ((i * 31 + frame * 9) % 206);
    int leafY = 124 + (i % 3) * 5;
    drawFanLeaf(leafX, leafY, 0.10f, i * 14);
  }
  canvas.setTextColor(GOLD, PANEL);
  canvas.drawCentreString(frame >= 5 ? "FLOWER READY FOR MARKET" : "TRIMMING THE HARVEST", 160, 180, 1);
}

void animateFlowerTrim() {
  beginCutsceneScene();
  for (int frame = 0; frame < 6; ++frame) {
    drawFlowerTrimFrame(frame);
    presentFrame();
    pumpDelay(520);
  }
  waitForCutsceneContinue();
}

void touchGarden(int x, int y) {
  if (x < ART_W && y >= HEADER_H && y < ACTION_Y) {
    page = PLANT_VIEW;
    return;
  }
  if (y < ACTION_Y) return;
  if (!plantActive) {
    if (x >= 80 && x <= 240) {
      if (totalSeeds() >= PLANTS_PER_ROOM) {
        pageAfterPlanting = GARDEN;
        firstPlantingDone = false;
        plantedPotMask = 0;
        page = PLANTING;
        saveGame();
      } else {
        openSeedShop(GARDEN);
      }
    }
    return;
  }
  if (growth >= 100) {
    if (x < 80) waterPlant();
    else if (x < 160) defoliateRoom();
    else if (x < 240) treatPests();
    else page = HARVEST_SELECT;
  } else {
    // Wide three-button layout while the plant is growing.
    if (x < 106) waterPlant();
    else if (x < 211) defoliateRoom();
    else treatPests();
  }
}

void handleNearbyTouch(int x, int y) {
  if (nearbyMode == 0) {
    if (y >= 80 && y < 172) {
      int index = (y - 80) / 23;
      if (index < MAX_NEARBY_PEERS && nearbyPeers[index].active &&
          millis() - nearbyPeers[index].lastSeen < 20000UL) {
        selectedNearbyPeer = index;
      }
    } else if (y >= 179 && y < 205) {
      if (x < 108 && selectedNearbyPeerValid()) {
        nearbyMode = 1;
        outgoingMessage[0] = '\0';
        msgKbMode = 0;
      } else if (x < 214 && selectedNearbyPeerValid()) {
        nearbyMode = 2;
        sendSeedStrain = seedStrain;
        if (!seedsByStrain[sendSeedStrain]) sendSeedStrain = nextSeededStrain(sendSeedStrain, 1);
      } else if (x >= 214) {
        nearbyMode = 3;
      }
    }
    return;
  }

  if (nearbyMode == 1) {
    char key = fullKeyboardKey(x, y, msgKbMode);
    if (key) {
      int length = strlen(outgoingMessage);
      if (length < (int)sizeof(outgoingMessage) - 1) {
        outgoingMessage[length] = key;
        outgoingMessage[length + 1] = '\0';
      }
      return;
    }
    int length = strlen(outgoingMessage);
    if (y >= 151 && y < 170 && x >= 259) {
      if (length) outgoingMessage[length - 1] = '\0';
    } else if (y >= 177 && y < 205) {
      if (x < 104) {
        if (length > 0 && length < (int)sizeof(outgoingMessage) - 1 &&
            outgoingMessage[length - 1] != ' ') {
          outgoingMessage[length] = ' ';
          outgoingMessage[length + 1] = '\0';
        }
      } else if (x < 216) {
        if (selectedNearbyPeerValid() && length > 0 &&
            sendNearbyPacket(nearbyPeers[selectedNearbyPeer].mac,
                             PACKET_MESSAGE, 0, outgoingMessage)) {
          snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Message sent");
        }
        nearbyMode = 0;
      } else {
        nearbyMode = 0;
      }
    }
    return;
  }

  if (nearbyMode == 2) {
    // Strain arrows on the seeds card (tap < or > to pick what you send).
    if (y >= 84 && y < 105 && x >= 196 && x < 245) {
      sendSeedStrain = nextSeededStrain(sendSeedStrain, x < 220 ? -1 : 1);
      render();
      return;
    }
    if (x >= 245 && y >= 84 && y < 105) sendNearbyGift(1);
    else if (x >= 245 && y >= 109 && y < 130) sendNearbyGift(2);
    else if (x >= 245 && y >= 134 && y < 155) sendNearbyGift(3);
    else if (x >= 245 && y >= 159 && y < 180) sendNearbyGift(4);
    else if (y >= 184 && y < 205) {
      if (x < 162) {
        if (selectedNearbyPeerValid()) {
          nearbyMode = 4;
          outgoingCash[0] = '\0';
        }
      } else {
        nearbyMode = 0;
      }
    }
    render();
    return;
  }

  if (nearbyMode == 4) {
    int length = strlen(outgoingCash);
    if (y >= 107 && y < 126 && x >= 21 && x < 301) {
      const char *keys = "1234567890";
      int index = (x - 21) / 28;
      if (index < 10 && length < 4) {
        outgoingCash[length] = keys[index];
        outgoingCash[length + 1] = '\0';
      }
    } else if (y >= 129 && y < 148) {
      if (x >= 35 && x < 80) {
        outgoingCash[0] = '\0';
      } else if (x >= 105 && x < 175) {
        if (length && length < 4) strncat(outgoingCash, "000", 4 - length);
      } else if (x >= 259) {
        if (length) outgoingCash[length - 1] = '\0';
      }
    } else if (y >= 177 && y < 205) {
      if (x < 104) {
        snprintf(outgoingCash, sizeof(outgoingCash), "%u", credits);
      } else if (x < 216) {
        sendNearbyCash();
      } else {
        nearbyMode = 0;
      }
    }
    return;
  }

  if (nearbyMode == 3 && y >= 174 && y < 205) nearbyMode = 0;
}

void appendGrowerNameLetter(char letter) {
  size_t length = strlen(growerNameEdit);
  if (length >= sizeof(growerNameEdit) - 1) return;
  if (length == 0 && isalpha((unsigned char)letter)) letter = (char)toupper((unsigned char)letter);
  growerNameEdit[length] = letter;
  growerNameEdit[length + 1] = '\0';
}

void handleGrowerNameTouch(int x, int y) {
  char key = fullKeyboardKey(x, y, nameKbMode);
  if (key) {
    appendGrowerNameLetter(key);
    return;
  }
  if (y >= 151 && y < 170 && x >= 259) {
    int length = strlen(growerNameEdit);
    if (length) growerNameEdit[length - 1] = '\0';
  } else if (y >= 177 && y < 205) {
    int length = strlen(growerNameEdit);
    if (x < 104) {
      if (length > 0 && length < sizeof(growerNameEdit) - 1 &&
          growerNameEdit[length - 1] != ' ') appendGrowerNameLetter(' ');
    } else if (x < 216) {
      if (growerNameEdit[0]) {
        strlcpy(growerName, growerNameEdit, sizeof(growerName));
        saveGame();
      }
      page = growerNameReturn;
    } else {
      page = growerNameReturn;
    }
  }
}

void chooseFarmerCharacter(uint8_t character) {
  bool firstTimeSetup = !farmerCharacterChosen && !selectingFarmerFromOptions;
  const char *oldCharacterName = farmerCharacterName(farmerCharacter);
  bool growerNameIsDefault = strcmp(growerName, oldCharacterName) == 0;
  farmerCharacter = min((int)character, 8);
  farmerCharacterChosen = true;
  if (growerNameIsDefault || !growerName[0]) {
    strlcpy(growerName, farmerCharacterName(farmerCharacter), sizeof(growerName));
  }
  if (firstTimeSetup) {
    // First launch only: the new grower sanitizes up, then picks a strain
    // from the catalog before any seeds can be planted. After planting the
    // first room they land in the garden like everyone else.
    animateGownUpCutscene();
    pageAfterSeedShop = GARDEN;
    page = SEED_SHOP;
  } else {
    page = selectingFarmerFromOptions ? (growerFromMenu ? GROWER_MENU : OPTIONS) : PLANTING;
  }
  growerFromMenu = false;
  selectingFarmerFromOptions = false;
  saveGame();
}

void handleTouch(int x, int y) {
  if (page == CHARACTER_SELECT) {
    if (y >= 88 && y < 138 && x < 54) {
      characterSelectIndex = (characterSelectIndex + 8) % 9;  // wrap: 9 growers
      render();
      return;
    }
    if (y >= 88 && y < 138 && x >= 266) {
      characterSelectIndex = (characterSelectIndex + 1) % 9;
      render();
      return;
    }
    if (y >= 172 && y < 202) {
      chooseFarmerCharacter(characterSelectIndex);
      render();
      return;
    }
    render();
    return;
  }

  if (page == PLANT_VIEW) {
    page = GARDEN;
    render();
    return;
  }
  if (y < HEADER_H && x > 282) {
    // Gear icon: enter the options hub, or tap again to go back to the garden.
    if (page == OTA_PAGE || page == WIFI_LOGIN) {
      otaRestoreRadio();                 // release the radio back to Nearby
      page = GARDEN;
    } else {
      page = page == OPTIONS ? GARDEN : OPTIONS;
    }
    resetArmed = false;
    render();
    return;
  }
  if (y < HEADER_H && x < 105) {
    // Leaf: open the grower ID menu; tap again to go back.
    if (page == GROWER_MENU) {
      page = growerMenuReturn;
    } else {
      growerMenuReturn = page;
      page = GROWER_MENU;
    }
    render();
    return;
  }

  if (y >= NAV_Y) {
    const Page pages[] = {GARDEN, SUPPLIES, MARKET, STASH, NEARBY};
    int tab = constrain(x / 64, 0, 4);
    if (page == OTA_PAGE || page == WIFI_LOGIN) otaRestoreRadio();
    page = pages[tab];
    render();
    return;
  }

  bool redrawGardenCare = false;
  switch (page) {
    case GARDEN:
      touchGarden(x, y);
      redrawGardenCare = page == GARDEN && plantActive &&
                         y >= ACTION_Y && y < NAV_Y &&
                         (growth >= 100 ? (x >= 80 && x < 240) : x >= 106) &&
                         !waterAnimationRequested;
      break;
    case SUPPLIES:
      // Big-card grid like the seed catalog: each card is its own tap target.
      if (y >= 72 && y < 112) {
        if (x < 160) buyWater();
        else buyCareKits();
      } else if (y >= 114 && y < 154) {
        if (x < 160) openSeedShop(SUPPLIES);
        else buyPestDefense();
      } else if (y >= 156 && y < 196) {
        if (x < 160) buyRemediation();
        else buyHashRoom();
      }
      break;
    case MARKET:
      if (y >= 72 && y < 93) {
        marketGearMode = x >= 160;
      } else if (marketGearMode) {
        // Gear grid: bong / lighters / papers cards; the stoned card is info.
        if (y >= 94 && y < 132) {
          if (x < 160) buyBong();
          else buyLighters();
        } else if (y >= 136 && y < 174) {
          if (x < 160) buyRollingPapers();
        } else if (y >= 177 && y < 205) {
          if (x < 107) takeSmokeBreak(1, false);
          else if (x < 213) takeSmokeBreak(2, false);
          else takeSmokeBreak(1, true);
        }
      } else {
        // Sell grid: sell-one cards on the left, sell-all cards on the right.
        if (y >= 94 && y < 132) {
          if (x < 160) sellOneFlower();
          else sellAllFlowers();
        } else if (y >= 136 && y < 174) {
          if (x < 160) sellOneHash();
          else sellAllHash();
        }
      }
      break;
    case STASH:
      if (y >= 68 && y < 89) {
        stashGearMode = x >= 160;
      } else if (!stashGearMode && untrimmedUnits > 0 && y >= 187 && y < 205) {
        page = TRIM_ROOM;
      }
      break;
    case OPTIONS:
      // Big-card grid: each card is its own tap target, anywhere on the card.
      if (y >= 72 && y < 116 && x < 160) {
        gameSpeed = (gameSpeed + 1) % 3;   // RELAXED -> QUICK -> EXTREME
        lastGameTick = millis();
        saveGame();
      } else if (y >= 72 && y < 116) {
        // Volume steps: OFF -> 10 -> 20 -> ... -> 100 -> OFF
        musicVolume = musicVolume == 0 ? 10 : musicVolume >= 100 ? 0 : musicVolume + 10;
        if (musicVolume) startSong(backgroundSong);
        else { chipToneOff(); drumToneOff(); activeSong = nullptr; drumPlaying = false; }
        saveGame();
      } else if (y >= 116 && y < 156 && x < 160) {
        prepareDemoHarvest();
      } else if (y >= 116 && y < 156) {
        page = CHEAT_CODE;               // cheat console
        cheatEdit[0] = '\0';
        cheatFeedback[0] = '\0';
        render();
        return;
      } else if (y >= 160 && y < 184 && x < 163) {
        if (resetArmed) resetGame();
        else {
          resetArmed = true;
          resetArmedAt = millis();
        }
      } else if (y >= 160 && y < 184) {
        runTouchCalibration();
        render();
        return;
      } else if (y >= 188 && y < 206) {
        page = OTA_PAGE;                 // WiFi login + OTA updates screen
        render();
        return;
      }
      break;
    case OTA_PAGE:
      touchOtaPage(x, y);
      break;
    case WIFI_LOGIN:
      touchWifiLogin(x, y);
      break;
    case CHEAT_CODE:
      touchCheatPage(x, y);
      break;
    case GROWER_MENU:
      if (y >= 72 && y < 128) {
        growerFromMenu = true;             // the card itself: change grower
        selectingFarmerFromOptions = true;
        page = CHARACTER_SELECT;
      } else if (y >= 132 && y < 166) {
        growerFromMenu = true;
        selectingFarmerFromOptions = true;
        page = CHARACTER_SELECT;
      } else if (y >= 170 && y < 204) {
        strlcpy(growerNameEdit, growerName, sizeof(growerNameEdit));
        growerNameReturn = GROWER_MENU;
        nameKbMode = 0;
        page = NAME_EDIT;
      }
      break;
    case HASH_ROOM:
      if (y >= 165 && y < 199) page = (x < 160) ? GARDEN : MARKET;
      break;
    case HARVEST_SELECT:
      if (y >= 78 && y < 130 && x >= 240) finishHarvest(false);
      else if (y >= 139 && y < 192 && x >= 240 && hashRoomUnlocked) finishHarvest(true);
      break;
    case TRIM_ROOM:
      if (y >= 158 && y < 198) {
        if (x < 160) trimAllFlowers();
        else page = GARDEN;
      }
      break;
    case NEARBY:
      handleNearbyTouch(x, y);
      break;
    case NAME_EDIT:
      handleGrowerNameTouch(x, y);
      break;
    case SEED_SHOP:
      // Big-card grid: rows of 27px, two columns; the whole card buys.
      if (y >= 70 && y < 203 && x >= 5) {
        uint8_t row = (uint8_t)((y - 70) / 27);
        uint8_t col = x >= 160 ? 1 : 0;
        if (row < 5) buySeeds(strainCatalogOrder[row * 2 + col]);
      }
      break;
    case PLANT_VIEW:
      page = GARDEN;
      break;
    case PLANTING:
      break;
  }
  bool cutscenePending = waterAnimationRequested || smokeAnimationRequested ||
                         hashAnimationRequested || flowerHarvestAnimationRequested ||
                         moldAnimationRequested || flowerTrimAnimationRequested ||
                         plantDeathAnimationRequested;
  if (plantDeathAnimationRequested) animatePlantDeath();
  if (waterAnimationRequested) {
    waterAnimationRequested = false;
    animateWatering();
    // The watering animation ends on a fully drawn, up-to-date garden scene;
    // skip the follow-up renders so nothing else gets redrawn on top.
    return;
  }
  if (smokeAnimationRequested) {
    smokeAnimationRequested = false;
    animateSmokeBreak();
  }
  if (hashAnimationRequested) {
    hashAnimationRequested = false;
    animateHashWash();
  }
  if (flowerHarvestAnimationRequested) {
    flowerHarvestAnimationRequested = false;
    animateFlowerHarvest();
  }
  if (moldAnimationRequested) {
    moldAnimationRequested = false;
    animateMoldWarning();
  }
  if (flowerTrimAnimationRequested) {
    flowerTrimAnimationRequested = false;
    animateFlowerTrim();
  }
  if (cutscenePending) render();
  else if (redrawGardenCare) {
    drawGardenSidePanel();
    drawGardenActions();
    presentFrame();
  } else render();
}

void drawPlantDeathFrame(int frame) {
  canvas.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  canvas.fillRoundRect(8, 36, 304, 160, 10, PANEL);
  canvas.drawRoundRect(8, 36, 304, 160, 10, RED);

  // Wilting plant: the stem bends and the leaves brown frame by frame.
  int baseX = 80, baseY = 168;
  uint8_t g = 150 - frame * 18;
  uint16_t leaf = rgb565(90 + frame * 14, g, 50);
  uint16_t stem = rgb565(80 + frame * 10, 120 - frame * 12, 50);
  int droop = frame * 6;
  canvas.drawLine(baseX, baseY, baseX + droop / 2, baseY - 60 + droop, stem);
  canvas.drawLine(baseX + 1, baseY, baseX + 1 + droop / 2, baseY - 60 + droop, stem);
  for (int i = 0; i < 4; ++i) {
    int ly = baseY - 18 - i * 12 + droop * (i + 1) / 5;
    int lx = baseX + droop * i / 8;
    canvas.fillTriangle(lx, ly, lx - 18, ly + 4 + frame * 2, lx - 8, ly + 8, leaf);
    canvas.fillTriangle(lx, ly, lx + 18, ly + 4 + frame * 2, lx + 8, ly + 8, leaf);
  }
  if (deathReason == 1) {
    for (int i = 0; i < frame * 3; ++i)
      canvas.fillCircle(40 + (i * 23) % 80, 176 - (i % 3) * 3, 2, 0x3DDF);
  } else {
    for (int i = 0; i < frame * 2; ++i)
      canvas.fillTriangle(40 + (i * 29) % 80, 178, 46 + (i * 29) % 80, 174, 50 + (i * 29) % 80, 180, leaf);
  }
  canvas.fillRoundRect(baseX - 22, baseY, 44, 18, 4, rgb565(150, 86, 52));

  canvas.setTextColor(RED, PANEL);
  canvas.drawCentreString("PLANT LOST", 222, 59, 2);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString(deathReason == 1 ? "OVERWATERED"
                        : deathReason == 3 ? "DEHYDRATED" : "OVER-DEFOLIATED", 222, 89, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawCentreString(deathReason == 1 ? "The roots drowned"
                        : deathReason == 3 ? "The soil went bone dry"
                        : "Too many leaves removed", 222, 120, 1);
  canvas.drawCentreString(currentStrainName(), 222, 140, 1);
  canvas.setTextColor(GOLD, PANEL);
  canvas.drawCentreString("The room has been cleared", 222, 164, 1);
}

void animatePlantDeath() {
  plantDeathAnimationRequested = false;
  for (int frame = 0; frame <= 5; ++frame) {
    if (frame == 0) beginCutsceneScene();
    drawPlantDeathFrame(frame);
    presentFrame();
    pumpDelay(260);
  }
  waitForCutsceneContinue();

  // The cutscene (frames + tap-to-continue) can outlast the window started in
  // killPlant(); restart the 5 s red-strobe window now that the loop is free
  // to run updateHarvestLed() again.
  deathLedUntil = millis() + 5000;

  plantActive = false;
  plantedPotMask = 0;
  growth = 0;
  hydration = 72;
  vigor = 82;
  pestType = 0;
  pestSeverity = 0;
  growthBoostTicks = 0;
  resetDefoliationForNewPlant();
  page = GARDEN;
  lastFarmerVisitAt = millis();
  saveGame();
}

void showDefoliatePrompt() {
  uint8_t w = defoliatePromptPending;
  defoliatePromptPending = 255;
  if (w >= DEFOLIATE_WINDOWS || !plantActive) return;
  beginCutsceneScene();
  canvas.fillRoundRect(8, 36, 304, 160, 10, PANEL);
  canvas.drawRoundRect(8, 36, 304, 160, 10, ACCENT);
  canvas.setTextColor(ACCENT, PANEL);
  canvas.drawCentreString(defoliateWindowNames[w], 160, 62, 2);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString("Your plants need care", 160, 96, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawCentreString("Tap DEFOLIATE while it is green.", 160, 126, 1);
  canvas.drawCentreString("Red means one more cut could kill it.", 160, 142, 1);
  canvas.setTextColor(GOLD, PANEL);
  canvas.drawCentreString("Each strain tolerates a different amount", 160, 166, 1);
  presentFrame();
  waitForCutsceneContinue();
  render();
}

// ---------------------------------------------------------------------------
// Pest invasion cutscene: bugs crawl all over the whole screen.
// ---------------------------------------------------------------------------
const int PEST_BUG_MAX = 46;
const int PEST_PANEL_X = 34, PEST_PANEL_Y = 78, PEST_PANEL_W = 252, PEST_PANEL_H = 84;

struct PestBug {
  float x, y;
  float heading;
  float speed;
};
PestBug pestBugs[PEST_BUG_MAX];

void drawPestBug(int x, int y, float heading, int frame, uint8_t type) {
  float dx = cosf(heading), dy = sinf(heading);
  float px = -dy, py = dx;  // perpendicular, for legs
  int wiggle = (frame & 1) ? 1 : -1;
  if (type == 1) {
    // Spider mite: small red body, 8 legs.
    uint16_t body = rgb565(205, 52, 38), leg = rgb565(120, 30, 24);
    for (int i = -1; i <= 2; ++i) {
      float ox = dx * (i - 0.5f) * 1.3f, oy = dy * (i - 0.5f) * 1.3f;
      int w = (i & 1) ? wiggle : -wiggle;
      canvas.drawLine(x + ox, y + oy, x + ox + px * 5 + dx * w, y + oy + py * 5 + dy * w, leg);
      canvas.drawLine(x + ox, y + oy, x + ox - px * 5 - dx * w, y + oy - py * 5 - dy * w, leg);
    }
    canvas.fillCircle(x, y, 3, body);
    canvas.drawPixel(x + dx * 2, y + dy * 2, TFT_BLACK);
  } else if (type == 2) {
    // Aphid: green pear-shaped body, 6 legs, antennae.
    uint16_t body = rgb565(126, 200, 70), dark = rgb565(60, 110, 40);
    for (int i = -1; i <= 1; ++i) {
      float ox = dx * i * 2.0f, oy = dy * i * 2.0f;
      int w = (i & 1) ? wiggle : -wiggle;
      canvas.drawLine(x + ox, y + oy, x + ox + px * 5 + dx * w, y + oy + py * 5 + dy * w, dark);
      canvas.drawLine(x + ox, y + oy, x + ox - px * 5 - dx * w, y + oy - py * 5 - dy * w, dark);
    }
    canvas.fillCircle(x - dx * 1.5f, y - dy * 1.5f, 3, body);
    canvas.fillCircle(x + dx * 2.5f, y + dy * 2.5f, 2, body);
    canvas.drawLine(x + dx * 4, y + dy * 4, x + dx * 7 + px * 2, y + dy * 7 + py * 2, dark);
    canvas.drawLine(x + dx * 4, y + dy * 4, x + dx * 7 - px * 2, y + dy * 7 - py * 2, dark);
  } else {
    // Fungus gnat: dark body with flapping gray wings.
    uint16_t body = rgb565(30, 30, 34), wing = rgb565(170, 180, 190);
    int span = (frame & 1) ? 4 : 2;
    canvas.fillCircle(x + px * span, y + py * span, 2, wing);
    canvas.fillCircle(x - px * span, y - py * span, 2, wing);
    canvas.fillCircle(x, y, 2, body);
    canvas.drawLine(x, y, x - dx * 4, y - dy * 4, body);
  }
}

void erasePestBug(int x, int y) {
  const int R = 8;
  int ex = x - R, ey = y - R, ew = R * 2 + 1, eh = R * 2 + 1;
  canvas.fillRect(ex, ey, ew, eh, BG);
  // Restore the part of the message panel the bug was covering.
  int ix0 = max(ex, PEST_PANEL_X + 4), iy0 = max(ey, PEST_PANEL_Y + 4);
  int ix1 = min(ex + ew, PEST_PANEL_X + PEST_PANEL_W - 4);
  int iy1 = min(ey + eh, PEST_PANEL_Y + PEST_PANEL_H - 4);
  if (ix1 > ix0 && iy1 > iy0) canvas.fillRect(ix0, iy0, ix1 - ix0, iy1 - iy0, PANEL);
}

void drawPestPanelText(uint8_t type, bool showContinue) {
  const char *title = type == 1 ? "YOU HAVE MITES!" : (type == 2 ? "YOU HAVE APHIDS!" : "YOU HAVE GNATS!");
  const char *line = type == 1 ? "Spider mites are draining the leaves"
                   : (type == 2 ? "Aphids are sucking the sap" : "Fungus gnats are in the soil");
  canvas.setTextColor(RED, PANEL);
  canvas.drawCentreString(title, 160, PEST_PANEL_Y + 12, 4);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString(line, 160, PEST_PANEL_Y + 44, 1);
  canvas.setTextColor(showContinue ? GOLD : TEXT_DIM, PANEL);
  canvas.drawCentreString(showContinue ? "TAP TO CONTINUE - then TREAT" : "Use TREAT with a pest kit",
                          160, PEST_PANEL_Y + 62, 1);
}

void animatePestInvasion() {
  pestAnimationRequested = false;
  uint8_t type = pestType ? pestType : 1;
  startSong(&pestAlarm);  // two-note siren while the bugs crawl in

  canvas.fillScreen(BG);
  canvas.fillRoundRect(PEST_PANEL_X, PEST_PANEL_Y, PEST_PANEL_W, PEST_PANEL_H, 10, PANEL);
  canvas.drawRoundRect(PEST_PANEL_X, PEST_PANEL_Y, PEST_PANEL_W, PEST_PANEL_H, 10, RED);
  drawPestPanelText(type, false);

  // Bugs swarm in from every edge of the screen.
  for (int i = 0; i < PEST_BUG_MAX; ++i) {
    int edge = random(4);
    PestBug &b = pestBugs[i];
    if (edge == 0)      { b.x = random(SCREEN_W); b.y = -6; }
    else if (edge == 1) { b.x = SCREEN_W + 6; b.y = random(SCREEN_H); }
    else if (edge == 2) { b.x = random(SCREEN_W); b.y = SCREEN_H + 6; }
    else                { b.x = -6; b.y = random(SCREEN_H); }
    b.heading = atan2f(SCREEN_H / 2 - b.y + random(-80, 80), SCREEN_W / 2 - b.x + random(-120, 120));
    b.speed = (type == 3 ? 3.0f : 1.4f) + random(0, 10) / 10.0f;
  }

  unsigned long start = millis();
  bool continueShown = false;
  int active = 6;
  for (int frame = 0; ; ++frame) {
    if (active < PEST_BUG_MAX && (frame % 3) == 0) ++active;

    for (int i = 0; i < active; ++i) erasePestBug((int)pestBugs[i].x, (int)pestBugs[i].y);
    canvas.drawRoundRect(PEST_PANEL_X, PEST_PANEL_Y, PEST_PANEL_W, PEST_PANEL_H, 10, RED);

    bool canContinue = millis() - start > 2500;
    drawPestPanelText(type, canContinue);
    continueShown = canContinue;

    for (int i = 0; i < active; ++i) {
      PestBug &b = pestBugs[i];
      b.heading += (random(-30, 31) / 100.0f) * (type == 3 ? 2.0f : 1.0f);
      b.x += cosf(b.heading) * b.speed;
      b.y += sinf(b.heading) * b.speed;
      // Turn back inward instead of leaving the screen.
      if (b.x < 4 || b.x > SCREEN_W - 4 || b.y < 4 || b.y > SCREEN_H - 4) {
        if (frame > 40) b.heading = atan2f(SCREEN_H / 2 - b.y, SCREEN_W / 2 - b.x) + random(-60, 61) / 100.0f;
      }
      drawPestBug((int)b.x, (int)b.y, b.heading, frame + i, type);
    }
    presentFrame();
    pumpDelay(type == 3 ? 25 : 40);

    if (continueShown && touchscreen.tirqTouched() && touchscreen.touched()) {
      while (touchscreen.tirqTouched() && touchscreen.touched()) delay(12);
      break;
    }
  }
  previousTouch = false;
  page = GARDEN;
  canvas.fillScreen(BG);
  render();
}

void gameTick() {
  bool boostWasActive = growthBoostTicks > 0;

  if (plantActive) {
    if (!ticksDuringSleep) {
      // Water used per tick. Raise these to make plants need water more often.
      uint8_t waterUse = WATER_DRAIN_PER_TICK;
      if (activeStrain == 0 || activeStrain == 2 || activeStrain == 7 ||
          activeStrain == 8 || activeStrain == 9) waterUse += THIRSTY_STRAIN_EXTRA_DRAIN;
      hydration = hydration > waterUse ? hydration - waterUse : 0;
      if (vigor > 0) --vigor;

      // Wilt timer: count how long the plant sits below 20% hydration and kill
      // it if the drought outlasts the limit (reason 3 = DEHYDRATED).
      if (hydration < 20) {
        if (dryStreakTicks < DRY_TICKS_LIMIT) ++dryStreakTicks;
        if (dryStreakTicks >= DRY_TICKS_LIMIT) {
          killPlant(3);
          saveGame();
          return;
        }
      } else {
        dryStreakTicks = 0;
      }

      if (pestType == 0 && growth > 12 && random(100) < 3) {
        pestType = random(1, 4);
        pestAnimationRequested = true;
        pestSeverity = 5;
      }
      if (pestType > 0) {
        pestSeverity = min(100, pestSeverity + 7);
        if (pestSeverity >= 35 && vigor > 3) vigor -= 3;
      }
    }

    uint8_t strainIndex = min((int)activeStrain, (int)STRAIN_COUNT - 1);
    if (!ticksDuringSleep && hydration == 0) {
      // Bone-dry soil: the plant withers fast and will not reach harvest —
      // vigor drains hard even through pest pressure, and at zero it dies.
      vigor = vigor > 6 ? vigor - 6 : 0;
      if (vigor == 0) {
        killPlant(3);
        saveGame();
        return;
      }
    } else if (hydration >= strainHydrationNeed[strainIndex] && vigor >= 18 &&
               growth < 100 && pestSeverity < 85 &&
               // Defoliation gate: once a window is ready (cooldown passed)
               // and still untouched, growth pauses until it is done —
               // defoliating is required care, not a bonus.
               !(defoliateWindow != 255 && gameMinutes >= defoliateReadyAt &&
                 defoliateWindow == currentDefoliateWindow() && defoliateClicks == 0)) {
      // With the sleep-proof wall clock, normal speed targets a ~30-minute
      // grow: base 1 + bonuses lands around 2 per 30 s tick (~50 ticks).
      int pace = gameSpeed == 0 ? 1 : (gameSpeed == 1 ? 3 : 6);
      pace += strainPaceModifier[strainIndex];
      pace = max(1, pace);
      if (boostWasActive) pace += 3;
      if (pestType > 0 && pestSeverity >= 35) pace = max(1, pace / 2);
      // Well-watered soil grows a touch faster: keep 20+ points above the
      // strain's need and the plant gains a bit of pace.
      if (hydration >= strainHydrationNeed[strainIndex] + 20) pace += 1;
      growth = min(100, growth + pace);
    } else if (!ticksDuringSleep && vigor > 2 && pestSeverity < 35) {
      vigor -= 2;
    }
  }
  if (boostWasActive) {
    --growthBoostTicks;
    saveGame();
  }
  updateDefoliateWindow();
  if (stonedLevel > 0) --stonedLevel;
  ++gameMinutes;
  // Save timed growth every five minutes; touchscreen actions save immediately.
  if (gameMinutes % 10 == 0) saveGame();
}

// --- Harvest-ready LED ---------------------------------------------------
// On this CYD the back RGB LED is common-anode (active-LOW): a GPIO pin
// sinks current, so LOW lights a color and HIGH turns it off.

void harvestLedOff() {
  if (LED_SINGLE_PIN != 255) {
    digitalWrite(LED_SINGLE_PIN, LOW);
    return;
  }
  digitalWrite(LED_R_PIN, HIGH);
  digitalWrite(LED_G_PIN, HIGH);
  digitalWrite(LED_B_PIN, HIGH);
}

void harvestLedOn() {
  if (LED_SINGLE_PIN != 255) {
    digitalWrite(LED_SINGLE_PIN, HIGH);
    return;
  }
  digitalWrite(LED_R_PIN, HIGH);
  digitalWrite(LED_B_PIN, HIGH);
  digitalWrite(LED_G_PIN, LOW);
}

void harvestLedRed() {
  if (LED_SINGLE_PIN != 255) {
    digitalWrite(LED_SINGLE_PIN, HIGH);
    return;
  }
  digitalWrite(LED_G_PIN, HIGH);
  digitalWrite(LED_B_PIN, HIGH);
  digitalWrite(LED_R_PIN, LOW);
}

// Smoke-break rainbow: cycles the RGB LED through the full spectrum
// (red -> yellow -> green -> cyan -> blue -> magenta, ~1.1s per lap).
void harvestLedRainbow() {
  const uint8_t seq[6][3] = {
    {LOW, HIGH, HIGH},   // red
    {LOW, LOW, HIGH},    // yellow
    {HIGH, LOW, HIGH},   // green
    {HIGH, LOW, LOW},    // cyan
    {HIGH, HIGH, LOW},   // blue
    {LOW, HIGH, LOW},    // magenta
  };
  uint8_t step = (millis() / 180) % 6;
  digitalWrite(LED_R_PIN, seq[step][0]);
  digitalWrite(LED_G_PIN, seq[step][1]);
  digitalWrite(LED_B_PIN, seq[step][2]);
}

// Back LED states, in priority order: flashing green when the plant is ready
// to harvest; flashing red while a plant death animation is playing (the
// plant was killed); a very fast red strobe while a pest infestation is
// active; dark the rest of the time.
void updateHarvestLed() {
  static unsigned long lastToggle = 0;
  static bool lit = false;
  bool ready = plantActive && growth >= 100;
  bool dying = plantDeathAnimationRequested || (long)(deathLedUntil - millis()) > 0;
  bool pest = plantActive && growth < 100 && pestType > 0;
  if (!ready && !dying && !pest) {
    if (stonedLevel > 0 || growthBoostTicks > 0) {
      // Smoke-break rainbow: stays up while the high or Garden Glow (the
      // growth boost) is running. Both decay one point per game tick, so
      // the rainbow winds down with the effects and then goes dark.
      harvestLedRainbow();
      lit = true;
      return;
    }
    if (lit) {
      harvestLedOff();
      lit = false;
    }
    return;
  }
  unsigned long interval = (pest && !ready && !dying) ? 60 : 250;
  if (millis() - lastToggle >= interval) {
    lastToggle = millis();
    lit = !lit;
    if (lit) {
      if (ready) harvestLedOn();
      else harvestLedRed();
    } else {
      harvestLedOff();
    }
  }
}

void initSdAndTouch() {
  // Mount the card on the CYD's standard SD SPI pins first. Some cards need
  // a couple of tries and a slower clock before they cooperate.
  touchscreenSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  sdMounted = SD.begin(SD_CS, touchscreenSPI, 12000000);
  for (int attempt = 0; !sdMounted && attempt < 4; ++attempt) {
    delay(250);
    touchscreenSPI.end();
    touchscreenSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    uint32_t clock = attempt < 2 ? 12000000 : 4000000;
    sdMounted = SD.begin(SD_CS, touchscreenSPI, clock);
    Serial.printf("SD retry %d @ %u Hz: %s\n", attempt + 1, (unsigned)clock,
                  sdMounted ? "mounted" : "failed");
  }
  if (sdMounted) Serial.println("microSD mounted; plant art enabled");
  else Serial.println("microSD not found; using built-in plant drawing");

  // Touch has separate board pins, on the same VSPI peripheral.
  switchToTouchBus();
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(1);
}

// Deep sleep: near-zero drain (radios off, RAM lost). Wakes as a full
// reboot on the next BOOT press; the game state was saved to flash.
bool sleepPromptActive = false;

void drawSleepPrompt(bool deepHighlight) {
  canvas.fillScreen(BG);
  canvas.fillRoundRect(30, 40, 260, 140, 8, PANEL);
  canvas.drawRoundRect(30, 40, 260, 140, 8, PANEL_LIGHT);
  canvas.setTextColor(TEXT_MAIN, PANEL);
  canvas.drawCentreString("POWER", 160, 52, 2);
  canvas.setTextColor(TEXT_DIM, PANEL);
  canvas.drawCentreString("QUICK PRESS = SLEEP", 160, 88, 1);
  if (deepHighlight) {
    canvas.setTextColor(RED, PANEL);
    canvas.drawCentreString("HOLD = DEEP SLEEP...", 160, 104, 1);
  } else {
    canvas.drawCentreString("HOLD 1.5s = DEEP SLEEP", 160, 104, 1);
  }
  drawButton(70, 130, 180, 26, "TAP SCREEN TO CANCEL", ACCENT_DARK);
  presentFrame();
}

void enterDeepSleep() {
  saveGame();
  chipToneOff();
  drumToneOff();
  activeSong = nullptr;
  drumPlaying = false;
  otaRestoreRadio();                 // close any WiFi session before sleep
  if (nearbyReady) {
    esp_now_deinit();
    nearbyReady = false;
    WiFi.mode(WIFI_OFF);
  }
  canvas.fillScreen(0x0861);
  canvas.setTextColor(TFT_WHITE, 0x0861);
  canvas.drawCentreString("DEEP SLEEP", 160, 108, 2);
  canvas.drawCentreString("PRESS BOOT TO POWER ON", 160, 139, 1);
  presentFrame();
  delay(400);
  canvas.writecommand(0x28);  // display off
  digitalWrite(TFT_BL, LOW);

  // Release the button before arming wake, or GPIO0 LOW would wake at once.
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();  // does not return; wake = full reboot
}

void sleepUntilBootWake() {
  saveGame();
  chipToneOff();      // silence both voices through light sleep
  drumToneOff();
  activeSong = nullptr;
  drumPlaying = false;
  otaRestoreRadio();                 // close any WiFi session before sleep
  if (nearbyReady) {
    esp_now_deinit();
    nearbyReady = false;
    WiFi.mode(WIFI_OFF);
  }
  canvas.fillScreen(0x0861);
  canvas.setTextColor(TFT_WHITE, 0x0861);
  canvas.drawCentreString("SLEEP MODE", 160, 108, 2);
  canvas.drawCentreString("PRESS BOOT TO WAKE", 160, 139, 1);
  presentFrame();
  delay(250);
  canvas.writecommand(0x28);  // ILI9341 display off
  digitalWrite(TFT_BL, LOW);

  // Release the button before arming low-level wake, or GPIO0 would wake at once.
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);
  esp_err_t wakeReady = esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  if (wakeReady == ESP_OK) esp_light_sleep_start();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
  justWokeFromSleep = true;  // loop runs the missed ticks dormant (no drying)
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);

  canvas.init();
  canvas.setRotation(1);
  canvas.invertDisplay(displayInverted);
  initSdAndTouch();
  startNearbyRadio();
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  previousTouch = false;
  lastGameTick = millis();
  lastFarmerVisitAt = millis();
  render();
}

void showBootSplash() {
  // Solid black backdrop, then the leaf breathes via a small 72x72 sprite
  // blitted atomically each step - no erase-then-redraw flicker.
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawCentreString("WINFLORIDA", 160, 134, 4);
  canvas.setTextColor(0x9EFB, TFT_BLACK);
  canvas.drawCentreString("GARDEN SIM", 160, 176, 2);
  // Firmware tagline: tiny dim line under the title, handy after OTA swaps.
  canvas.setTextColor(0x630C, TFT_BLACK);
  canvas.drawCentreString(FW_VERSION_FULL " - " FW_DATE, 160, 196, 1);
  presentFrame();

  TFT_eSprite spr(&canvas);
  spr.createSprite(72, 72);
  const int frames = 20;
  for (int i = 0; i < frames; ++i) {
    float t = (float)i / (float)frames;
    float pulse = 0.5f - 0.5f * cosf(t * 2.0f * PI);  // 0 -> 1 -> 0, slow breath
    int v = 140 + (int)(pulse * 115.0f);  // logo brightness 140..255
    spr.fillSprite(TFT_BLACK);
    for (int row = 0; row < 24; ++row) {
      for (int col = 0; col < 24; ++col) {
        int bit = row * 24 + col;
        if (winLeafLogoMask[bit >> 3] & (0x80 >> (bit & 7)))
          spr.fillRect(col * 3, row * 3, 3, 3, rgb565(v - 18, v, v - 10));
      }
    }
    spr.pushSprite(124, 50);
    delay(80);
  }
  spr.deleteSprite();
}

// ---------------------------------------------------------------------------
// Touch calibration
// Raw XPT2046 readings that line up with screen edges x=0/319 and y=0/239.
// Defaults are the common CYD values. Recalibrate from the options hub
// (TOUCH CAL button), then tap the 4 crosshairs.
// The result is saved to flash and survives RESET GAME.
// ---------------------------------------------------------------------------
int touchRawXMin = 200;
int touchRawXMax = 3700;
int touchRawYMin = 240;
int touchRawYMax = 3800;

void loadTouchCalibration() {
  touchRawXMin = preferences.getShort("tcXMin", 200);
  touchRawXMax = preferences.getShort("tcXMax", 3700);
  touchRawYMin = preferences.getShort("tcYMin", 240);
  touchRawYMax = preferences.getShort("tcYMax", 3800);
  if (abs(touchRawXMax - touchRawXMin) < 1000 || abs(touchRawYMax - touchRawYMin) < 1000) {
    touchRawXMin = 200; touchRawXMax = 3700;
    touchRawYMin = 240; touchRawYMax = 3800;
  }
}

void saveTouchCalibration() {
  preferences.putShort("tcXMin", touchRawXMin);
  preferences.putShort("tcXMax", touchRawXMax);
  preferences.putShort("tcYMin", touchRawYMin);
  preferences.putShort("tcYMax", touchRawYMax);
}

// Converts the current touch to screen coordinates using the calibration.
void readTouch(int &x, int &y) {
  TS_Point raw = touchscreen.getPoint();
  x = constrain(map(raw.x, touchRawXMin, touchRawXMax, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
  y = constrain(map(raw.y, touchRawYMin, touchRawYMax, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
}

bool calibrationTouchDown() {
  return touchscreen.tirqTouched() && touchscreen.touched();
}

// Waits for one tap and returns the averaged raw reading.
TS_Point sampleCalibrationTap() {
  while (calibrationTouchDown()) delay(12);
  delay(150);
  long sx = 0, sy = 0;
  int n = 0;
  while (n < 16) {
    if (calibrationTouchDown()) {
      TS_Point p = touchscreen.getPoint();
      if (p.z > 300) { sx += p.x; sy += p.y; ++n; }
    }
    delay(10);
  }
  while (calibrationTouchDown()) delay(12);
  TS_Point avg;
  avg.x = sx / n;
  avg.y = sy / n;
  avg.z = 0;
  return avg;
}

void drawCalibrationTarget(int x, int y, int step) {
  canvas.fillScreen(BG);
  canvas.setTextColor(TEXT_MAIN, BG);
  canvas.drawCentreString("TOUCH CALIBRATION", 160, 90, 2);
  char msg[24];
  snprintf(msg, sizeof(msg), "Tap the cross  %d/4", step);
  canvas.setTextColor(TEXT_DIM, BG);
  canvas.drawCentreString(msg, 160, 116, 1);
  canvas.drawFastHLine(x - 10, y, 21, GOLD);
  canvas.drawFastVLine(x, y - 10, 21, GOLD);
  canvas.drawCircle(x, y, 6, GOLD);
  presentFrame();
}

void runTouchCalibration() {
  const int M = 20;  // crosshair inset from the edges
  const int px[4] = {M, SCREEN_W - 1 - M, SCREEN_W - 1 - M, M};
  const int py[4] = {M, M, SCREEN_H - 1 - M, SCREEN_H - 1 - M};
  TS_Point r[4];
  for (int i = 0; i < 4; ++i) {
    drawCalibrationTarget(px[i], py[i], i + 1);
    r[i] = sampleCalibrationTap();
    Serial.printf("cal %d: raw x=%d y=%d\n", i + 1, r[i].x, r[i].y);
  }

  // Average the left/right and top/bottom pairs, then extend to the screen edges.
  float rxL = (r[0].x + r[3].x) / 2.0f, rxR = (r[1].x + r[2].x) / 2.0f;
  float ryT = (r[0].y + r[1].y) / 2.0f, ryB = (r[2].y + r[3].y) / 2.0f;
  float perPxX = (rxR - rxL) / (float)(SCREEN_W - 1 - 2 * M);
  float perPxY = (ryB - ryT) / (float)(SCREEN_H - 1 - 2 * M);
  int xMin = (int)(rxL - perPxX * M), xMax = (int)(rxR + perPxX * M);
  int yMin = (int)(ryT - perPxY * M), yMax = (int)(ryB + perPxY * M);

  canvas.fillScreen(BG);
  if (abs(xMax - xMin) < 1000 || abs(yMax - yMin) < 1000) {
    canvas.setTextColor(RED, BG);
    canvas.drawCentreString("Calibration failed - kept old values", 160, 112, 1);
  } else {
    touchRawXMin = xMin; touchRawXMax = xMax;
    touchRawYMin = yMin; touchRawYMax = yMax;
    saveTouchCalibration();
    canvas.setTextColor(ACCENT, BG);
    canvas.drawCentreString("Touch calibrated", 160, 112, 2);
  }
  Serial.printf("touch cal: X %d..%d  Y %d..%d\n", touchRawXMin, touchRawXMax, touchRawYMin, touchRawYMax);
  presentFrame();
  delay(900);
}

void setup() {
  pinMode(TFT_BL, OUTPUT);
  // Backlight stays off until a clean frame is on the panel, so the power-on
  // white / rainbow garbage on the display is never visible.

  Serial.begin(115200);
  Serial.println();
  Serial.printf("[BUILD] %s: per-strain seeds + OTA updates + cheats + remediation\n", FW_VERSION_FULL);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // Chiptune voices on LEDC (8-bit resolution for duty/volume control)
  if (SPEAKER_PIN != 255) ledcAttach(SPEAKER_PIN, 1000, 8);
  if (DRUM_PIN != 255) ledcAttach(DRUM_PIN, 1000, 8);
  // Harvest LED pins
  if (LED_SINGLE_PIN != 255) {
    pinMode(LED_SINGLE_PIN, OUTPUT);
  } else {
    pinMode(LED_R_PIN, OUTPUT);
    pinMode(LED_G_PIN, OUTPUT);
    pinMode(LED_B_PIN, OUTPUT);
  }
  harvestLedOff();
  canvas.init();
  canvas.setRotation(1);  // Landscape 320x240

  loadGame();
  catchUpGrowTime();   // grow continues while the screen slept / board reset
  Serial.printf("[CLOCK] t=%lus, grow epoch=%lu (sleep catch-up done)\n",
                (unsigned long)time(nullptr), (unsigned long)growEpoch);
  canvas.invertDisplay(displayInverted);
  canvas.fillScreen(BG);
  presentFrame();
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);  // panel is clean, light it up

  initSdAndTouch();
  startNearbyRadio();
  randomSeed((uint32_t)micros() ^ (uint32_t)analogRead(34));
  loadTouchCalibration();
  showBootSplash();
  lastGameTick = millis();
  lastFarmerVisitAt = millis();
  render();
}

void pollInstallerRequest() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      installerBuf[installerLen] = 0;
      if (installerLen && strstr(installerBuf, "SDINSTALL")) {
        sdInstallerMode = true;
        installerLen = 0;
        Serial.println("READY");
        // Park the game visuals and silence the soundtrack while installing.
        chipToneOff();
        drumToneOff();
        activeSong = nullptr;
        drumPlaying = false;
        canvas.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, BG);
        drawHeader();
        canvas.setTextColor(GOLD, BG);
        canvas.drawCentreString("SD INSTALL MODE", 160, 108, 2);
        canvas.setTextColor(TEXT_DIM, BG);
        canvas.drawCentreString("Send files over USB; QUIT exits", 160, 140, 1);
        presentFrame();
        return;
      }
      installerLen = 0;
      continue;
    }
    if (installerLen < (int)sizeof(installerBuf) - 1) installerBuf[installerLen++] = c;
  }
}

void handleInstallerSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  switchToSdBus();
  if (cmd.startsWith("FILE ")) {
    int sp = cmd.indexOf(' ', 5);
    String name = cmd.substring(5, sp);
    uint32_t size = cmd.substring(sp + 1).toInt();
    Serial.println("OK");
    SD.remove(name);
    File f = SD.open(name, FILE_WRITE);
    if (!f) { switchToTouchBus(); Serial.println("OPENFAIL"); return; }
    uint32_t got = 0;
    uint8_t buf[512];
    while (got < size) {
      int avail = Serial.available();
      if (avail <= 0) continue;
      int n = avail > 512 ? 512 : avail;
      n = Serial.readBytes(buf, n);
      if (n <= 0) continue;
      if (f.write(buf, n) != (size_t)n) break;
      got += n;
    }
    f.close();
    Serial.println(got == size ? "DONE" : "SHORT");
  } else if (cmd == "LIST") {
    File root = SD.open("/");
    File entry = root.openNextFile();
    while (entry) {
      Serial.print(entry.name());
      Serial.print(' ');
      Serial.println(entry.size());
      entry = root.openNextFile();
    }
    Serial.println("ENDLIST");
  } else if (cmd == "QUIT") {
    sdInstallerMode = false;
    Serial.println("BYE");
  }
  switchToTouchBus();
}

void loop() {
  if (sleepPromptActive) {
    // Power popup is up: the next BOOT press's duration chooses the mode,
    // any screen tap cancels back to the game.
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      delay(35);
      if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        unsigned long pressStart = millis();
        bool deepArmed = false;
        while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
          if (!deepArmed && millis() - pressStart >= 1500) {
            deepArmed = true;
            drawSleepPrompt(true);
          }
          delay(10);
        }
        sleepPromptActive = false;
        if (deepArmed) enterDeepSleep();
        else sleepUntilBootWake();
        return;
      }
    }
    if (touchscreen.tirqTouched() && touchscreen.touched()) {
      while (touchscreen.tirqTouched() && touchscreen.touched()) delay(10);
      sleepPromptActive = false;
      render();
    }
    delay(8);
    return;
  }
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    delay(35);
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      sleepPromptActive = true;
      drawSleepPrompt(false);
      return;
    }
  }

  // New-message popup: tap it to jump to the inbox, tap elsewhere to dismiss,
  // or let it time out (the underlying page is redrawn once, on expiry).
  if (nearbyPopupActive) {
    if (page == NEARBY) {
      nearbyPopupActive = false;   // inbox is already on screen; drop the card
    } else if ((long)(nearbyPopupUntil - millis()) <= 0) {
      nearbyPopupActive = false;
      render();
    } else if (touchscreen.tirqTouched() && touchscreen.touched()) {
      int x, y;
      readTouch(x, y);
      bool onPopup = x >= 160 && y >= 190;
      while (touchscreen.tirqTouched() && touchscreen.touched()) delay(10);
      if (onPopup) {
        nearbyPopupActive = false;
        page = NEARBY;
        nearbyMode = 3;
        render();
      }
      return;  // consume the tap either way so the page under doesn't react
    }
  }

  // SD installer tool: watch for the serial trigger, then service files.
  if (sdInstallerMode) {
    handleInstallerSerial();
    delay(2);
    return;
  }
  pollInstallerRequest();

  serviceNearby();
  updateHarvestLed();
  updateMusic();  // advance the chiptune sequencer

  // Grow clock ticks by WALL time (the RTC keeps counting through sleep), so
  // a wake from light sleep or deep sleep catches up on the missed ticks.
  uint32_t nowE = (uint32_t)time(nullptr);
  unsigned long now = millis();
  const uint8_t secPerTick = gameSpeed == 2 ? 4 : 30;
  if (nowE > growEpoch) {
    uint32_t ticks = (nowE - growEpoch) / secPerTick;
    if (ticks > 0) {
      if (ticks > 2880) { ticks = 2880; growEpoch = nowE; }  // bound a huge jump
      else growEpoch += ticks * secPerTick;
      // A burst right after wake is sleep time: run it dormant (no drying).
      ticksDuringSleep = justWokeFromSleep;
      while (ticks--) gameTick();
      ticksDuringSleep = false;
      // Light refresh: while the growth stage (plant art) is unchanged, the
      // garden page only needs its stat numbers updated, and the fullscreen
      // view (no stats) needs no redraw at all. No more periodic flicker.
      if (page == GARDEN && plantActive && growth < 100 &&
          stageForGrowth() == lastDrawnStage) {
        drawGardenSidePanel();
      } else if (page == PLANT_VIEW && plantActive &&
                 stageForGrowth() == lastDrawnStage) {
        // Fullscreen art unchanged; nothing on screen needs updating.
      } else {
        render();
      }
    }
  }

  if (plantDeathAnimationRequested) {
    animatePlantDeath();
    render();
  }
  if (pestAnimationRequested && plantActive && page != PLANTING && page != CHARACTER_SELECT) {
    animatePestInvasion();
  }
  if (defoliatePromptPending != 255 && gameMinutes >= defoliateReadyAt &&
      (page == GARDEN || page == PLANT_VIEW)) {
    showDefoliatePrompt();
  }

  if (resetArmed && millis() - resetArmedAt > 10000UL) {
    resetArmed = false;
    render();
  }

  if ((page == GARDEN || page == PLANT_VIEW) && plantActive && growth < 100 &&
      now - lastFarmerVisitAt >= 10000UL) {
    lastFarmerVisitAt = now;
    animateFarmerVisit();
    now = millis();
  }

  if (page == PLANTING) {
    bool seedTouched = touchscreen.tirqTouched() && touchscreen.touched();
    if (!seedTouched) {
      previousTouch = false;
      refreshNearbyPopup();
      delay(8);
      return;
    }
    int x, y;
    readTouch(x, y);
    if (!previousTouch) {
      previousTouch = true;
      // Strain picker arrows (batch must still be untouched).
      if (plantedPotCount() == 0 && countSeededStrains() > 1 && y >= 56 && y < 78) {
        if (x < 54) plantStrain = nextSeededStrain(plantStrain, -1);
        else if (x > 266) plantStrain = nextSeededStrain(plantStrain, 1);
        render();
        refreshNearbyPopup();
        delay(8);
        return;
      }
      plantSeedFromTap(x, y);
    }
    refreshNearbyPopup();
    delay(8);
    return;
  }

  // (Settings pages use the normal tap handling again; the drag-scroll
  // gesture system was removed when the hub stopped scrolling.)
  bool isTouched = touchscreen.tirqTouched() && touchscreen.touched();
  if (!isTouched) {
    previousTouch = false;
    refreshNearbyPopup();
    delay(8);
    return;
  }

  if (!previousTouch) {
    previousTouch = true;
    int x, y;
    readTouch(x, y);
    handleTouch(x, y);
  }
  refreshNearbyPopup();
  delay(4);
}
