// ---------------------------------------------------------------------------
// ChickenMicro - test hybride tricolore + partiel rapide
// ESP32-S3 + dalle 2.13" GDEY0213Z98 (SSD1680, 122x250, noir/blanc/rouge)
//
// HYPOTHESE TESTEE ICI
// --------------------
// Le SSD1680 accepte une waveform ecrite par le MCU (datasheet p.4 "Waveform
// can stored in On-chip OTP or written by MCU", et le programme de reference
// 14.2 le fait : "Command 0x32 Write 224bytes LUT").
//
// C'est exactement ce que fait le driver monochrome de GxEPD2 :
//
//     void GxEPD2_213_BN::_Init_Part() {
//       _writeCommand(0x32);
//       _writeDataPGM(lut_partial, sizeof(lut_partial));   // 154 octets
//     }
//
// Il n'utilise donc PAS la waveform tricolore de l'OTP, d'ou les ~300 ms au
// lieu des 14 s specifies. Le prix a payer : sa LUT ne pilote que les
// transitions noir<->blanc, elle ne deplace jamais les particules rouges.
//
// Sur SSD1680 les deux RAM changent de sens selon la waveform chargee :
//
//     waveform tricolore (OTP)  : 0x24 = plan noir   0x26 = plan ROUGE
//     waveform mono (LUT 0x32)  : 0x24 = image neuve 0x26 = image PRECEDENTE
//
// CE QUI S'EST PASSE AU PREMIER ESSAI
// -----------------------------------
// Le rouge s'effacait un peu plus a chaque partiel. La fenetre 0x44/0x45 ne
// protege rien : sur SSD1680 elle borne les ecritures RAM, pas les gates
// pilotes. Le partiel n'ignore pas les pixels hors fenetre, il leur applique
// la transition "inchange" de la LUT -- et celle de GxEPD2 n'est pas nulle :
//
//     LUT0 (0->0, blanc->blanc) : 0x00, 0x40, 0, 0, ...   0x40 = VSH1
//     LUT3 (1->1, noir->noir)   : 0x00, 0x80, 0, 0, ...   0x80 = VSL
//
// (5 rangees de 12 octets, une par transition ; chaque octet = 4 phases sur
//  2 bits ; 00=VSS 01=VSH1 10=VSL 11=VSH2. Le groupe 1 dure TPA=1 frame.)
//
// Une frame d'impulsion sur chaque pixel inchange, 50 fois de suite : c'est
// ca qui deplace les particules rouges.
//
// CE QU'ON NE PEUT PAS FAIRE
// --------------------------
// Repeindre le rouge pendant le partiel mono est impossible. La LUT mono n'a
// aucune phase capable de deplacer le pigment rouge (c'est precisement ce qui
// rend la waveform tricolore longue de 14 s), et la RAM 0x26 est deja prise
// comme "image precedente" : il ne reste aucun canal pour exprimer "rouge
// ici".
//
// CE QU'ON TESTE A LA PLACE
// -------------------------
// Supprimer la cause : une LUT ou les transitions inchangees sont a zero.
// Un pixel qui ne change pas ne recoit alors aucune tension, donc le rouge
// hors zone rapide ne devrait plus bouger du tout.
//
// Le test alterne un cycle sur deux pour que la comparaison soit directe :
//     cycle pair   -> LUT d'origine GxEPD2   (temoin)
//     cycle impair -> LUT adoucie            (LUT0 et LUT3 mises a zero)
//
//     y 0   .. 32   bandeau rouge     <- temoin haut
//     y 32  .. 104  zone rapide N/B   <- partiel mono
//     y 104 .. 122  bandeau rouge     <- temoin bas
//
// Cablage : voir include/epd_config.h
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "epd_config.h"

// Saturation du pigment au demarrage (~45 s). Mets a 0 pour iterer vite.
#define CONDITION_ON_BOOT 1

// ---------------------------------------------------------------------------
// Deux drivers sur les memes pins.
// SPIClass::begin() sort immediatement s'il est deja initialise, donc les
// pins custom passes au SPI.begin() du setup survivent aux deux init().
// Cout memoire : 3C = 2 x 4000 o, BW = 4000 o, soit 12 Ko.
// ---------------------------------------------------------------------------
GxEPD2_3C<GxEPD2_213_Z98c, GxEPD2_213_Z98c::HEIGHT> tri(
    GxEPD2_213_Z98c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

GxEPD2_BW<GxEPD2_213_BN, GxEPD2_213_BN::HEIGHT> fast(
    GxEPD2_213_BN(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ---------------------------------------------------------------------------
// Geometrie (rotation 1 : 250 x 122)
// ---------------------------------------------------------------------------
static const int16_t SCREEN_W = 250;
static const int16_t SCREEN_H = 122;

static const int16_t HEADER_H = 32;                 // bandeau rouge haut
static const int16_t FAST_Y   = HEADER_H;           // 32
static const int16_t FAST_H   = 72;                 // 32 -> 104
static const int16_t FOOTER_Y = FAST_Y + FAST_H;    // 104
static const int16_t FOOTER_H = SCREEN_H - FOOTER_Y;// 18

static const uint16_t PARTIALS_PER_CYCLE = 50;
static const uint32_t PARTIAL_PERIOD_MS  = 1000;

static uint16_t cycle        = 0;
static uint16_t partialIndex = 0;
static uint32_t partialTotal = 0;  // somme des durees, pour la moyenne
static bool     gentleCycle  = false;
static bool     lutInstalled = false;

// ---------------------------------------------------------------------------
// Helpers de dessin (generiques : les deux drivers derivent d'Adafruit_GFX)
// ---------------------------------------------------------------------------
static void centerText(Adafruit_GFX &g, const char *s, int16_t x, int16_t w,
                       int16_t baselineY) {
  int16_t bx, by;
  uint16_t bw, bh;
  g.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  g.setCursor(x + (w - (int16_t)bw) / 2 - bx, baselineY);
  g.print(s);
}

// ---------------------------------------------------------------------------
// Bascule entre les deux drivers
// ---------------------------------------------------------------------------
// serial_diag_bitrate = 0 : sinon init() rappelle Serial.begin() a chaque
// bascule, ce qui coupe le CDC USB.
static void switchToTri() {
  fast.hibernate();
  tri.init(0, true, 50, false);
  tri.setRotation(1);
}

static void switchToFast() {
  tri.hibernate();

  // initial = false -> pas de full refresh initial force. Indispensable :
  // un full mono (0x22/0xF4) chargerait la waveform tricolore de l'OTP et
  // enverrait la RAM 0x26 (qui contient l'image precedente, pas du rouge)
  // dans le plan rouge. Bouillie garantie.
  fast.init(0, false, 50, false);
  fast.setRotation(1);

  // La RAM 0x26 contient encore le plan rouge ecrit par la passe tricolore.
  // Sous la waveform mono elle est relue comme "image precedente" : on la
  // remet a blanc, ce qui correspond a l'etat physique de la zone rapide.
  fast.epd2.writeScreenBufferAgain(0xFF);

  // GxEPD2 rechargera sa propre LUT au premier partiel (_using_partial_mode
  // est remis a false par init()), donc la notre doit etre reinstallee apres.
  lutInstalled = false;
}

// ---------------------------------------------------------------------------
// LUT adoucie
// ---------------------------------------------------------------------------
// Copie de GxEPD2_213_BN::lut_partial (153 octets) avec LUT0 et LUT3 -- les
// deux transitions "le pixel ne change pas" -- mises a zero. Un pixel
// inchange ne recoit donc plus aucune tension.
//
// Structure SSD1680 : 5 rangees de 12 octets (LUT0 = 0->0, LUT1 = 0->1,
// LUT2 = 1->0, LUT3 = 1->1, LUT4 = VCOM), puis 12 groupes de 7 octets
// (TPA, TPB, SRAB, TPC, TPD, SRCD, RP), puis 9 octets de config.
// 60 + 84 + 9 = 153.
static const uint8_t lut_gentle[] = {
  0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT0 0->0  ZERO (etait 0x00,0x40)
  0x80, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT1 0->1  inchange
  0x40, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT2 1->0  inchange
  0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT3 1->1  ZERO (etait 0x00,0x80)
  0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT4 VCOM  inchange

  0x0A, 0, 0, 0, 0, 0, 0x02,                 // groupe 0 : TPA=10 frames, RP=2
  0x01, 0, 0, 0, 0, 0, 0x00,                 // groupe 1 : TPA=1 frame
  0x01, 0, 0, 0, 0, 0, 0x00,                 // groupe 2 : TPA=1 frame
  0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,

  0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

// Ecriture directe : GxEPD2 n'expose pas _writeCommand, mais on a les pins.
// Sequence SSD1680 : commande avec D/C bas, donnees avec D/C haut.
static void installGentleLut() {
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

  digitalWrite(EPD_DC, LOW);
  digitalWrite(EPD_CS, LOW);
  SPI.transfer(0x32);
  digitalWrite(EPD_CS, HIGH);

  digitalWrite(EPD_DC, HIGH);
  digitalWrite(EPD_CS, LOW);
  for (size_t i = 0; i < sizeof(lut_gentle); i++) SPI.transfer(lut_gentle[i]);
  digitalWrite(EPD_CS, HIGH);

  SPI.endTransaction();
  Serial.println("[lut]  LUT adoucie installee (LUT0 et LUT3 a zero)");
}

// ---------------------------------------------------------------------------
// Passe tricolore : peint les deux bandeaux rouges (14 s)
// ---------------------------------------------------------------------------
static void paintTricolorFrame() {
  const uint32_t t0 = millis();

  tri.setFullWindow();
  tri.firstPage();
  do {
    tri.fillScreen(GxEPD_WHITE);

    tri.fillRect(0, 0, SCREEN_W, HEADER_H, GxEPD_RED);
    tri.setTextColor(GxEPD_WHITE);
    tri.setFont(&FreeSansBold9pt7b);
    tri.setCursor(8, 21);
    tri.print("CHICKENMICRO");

    char buf[16];
    snprintf(buf, sizeof(buf), "cycle %u", cycle);
    int16_t bx, by;
    uint16_t bw, bh;
    tri.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
    tri.setCursor(SCREEN_W - 8 - bw - bx, 21);
    tri.print(buf);

    tri.fillRect(0, FOOTER_Y, SCREEN_W, FOOTER_H, GxEPD_RED);
    centerText(tri, "TEMOIN ROUGE BAS", 0, SCREEN_W, FOOTER_Y + 13);

    tri.setTextColor(GxEPD_BLACK);
  } while (tri.nextPage());

  Serial.printf("[tri]  repeint en %lu ms\n", (unsigned long)(millis() - t0));
}

// ---------------------------------------------------------------------------
// Partiel mono dans la fenetre rapide (~300 ms attendus)
// ---------------------------------------------------------------------------
static void fastTick() {
  const uint32_t t0 = millis();

  char big[16], sub[24];
  snprintf(big, sizeof(big), "%u", partialIndex);
  snprintf(sub, sizeof(sub), "%u / %u", partialIndex, PARTIALS_PER_CYCLE);

  fast.setPartialWindow(0, FAST_Y, SCREEN_W, FAST_H);
  fast.firstPage();
  do {
    fast.fillScreen(GxEPD_WHITE);
    fast.setTextColor(GxEPD_BLACK);

    fast.setFont(&FreeSansBold9pt7b);
    centerText(fast, "PARTIEL MONO", 0, SCREEN_W, FAST_Y + 18);

    fast.setFont(&FreeSansBold18pt7b);
    centerText(fast, big, 0, SCREEN_W, FAST_Y + 50);

    fast.setFont(&FreeSansBold9pt7b);
    centerText(fast, sub, 0, SCREEN_W, FAST_Y + 68);
  } while (fast.nextPage());

  const uint32_t dt = millis() - t0;
  partialTotal += dt;
  Serial.printf("[fast] partiel %u/%u en %lu ms\n", partialIndex,
                PARTIALS_PER_CYCLE, (unsigned long)dt);

  // Le premier partiel force GxEPD2 a charger sa LUT (_Init_Part). On ecrase
  // juste apres, et _using_partial_mode reste vrai : la notre tient jusqu'au
  // prochain init().
  if (gentleCycle && !lutInstalled) {
    installGentleLut();
    lutInstalled = true;
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("[chickenMicro] test hybride tricolore + partiel mono");

  pinMode(BTN_BOOT, INPUT_PULLUP);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);

  tri.init(0, true, 50, false);
  tri.setRotation(1);
  Serial.printf("[chickenMicro] ecran %dx%d\n", tri.width(), tri.height());

#if CONDITION_ON_BOOT
  Serial.println("[tri]  conditionnement du pigment (~45 s)...");
  tri.epd2.clearScreen(0x00, 0xFF);  // tout noir
  tri.epd2.clearScreen(0xFF, 0x00);  // tout rouge
  tri.epd2.clearScreen(0xFF, 0xFF);  // tout blanc
#endif

  paintTricolorFrame();
  switchToFast();
  gentleCycle = (cycle % 2 == 1);
  Serial.printf("[test] cycle %u : LUT %s\n", cycle,
                gentleCycle ? "ADOUCIE" : "origine GxEPD2");
}

void loop() {
  static uint32_t last = 0;

  if (millis() - last < PARTIAL_PERIOD_MS) {
    delay(10);
    return;
  }
  last = millis();

  partialIndex++;
  fastTick();

  if (partialIndex >= PARTIALS_PER_CYCLE) {
    Serial.printf("[test] cycle %u : %u partiels, moyenne %lu ms\n", cycle,
                  PARTIALS_PER_CYCLE,
                  (unsigned long)(partialTotal / PARTIALS_PER_CYCLE));
    Serial.println("[test] regarde les bandeaux rouges MAINTENANT (10 s)");
    delay(10000);

    cycle++;
    partialIndex = 0;
    partialTotal = 0;
    gentleCycle  = (cycle % 2 == 1);

    switchToTri();
    paintTricolorFrame();
    switchToFast();
    Serial.printf("[test] cycle %u : LUT %s\n", cycle,
                  gentleCycle ? "ADOUCIE" : "origine GxEPD2");
    last = millis();
  }
}
