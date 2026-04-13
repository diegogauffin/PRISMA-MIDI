#include <Adafruit_NeoPixel.h>
#include <Control_Surface.h>
#include <EEPROM.h> // Para guardar la configuración

// --- CONFIGURACIÓN DE HARDWARE ---
#define PIN 16
#define NUMPIXELS 1
#define PIN_STRIP 8
#define NUM_LEDS 4

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip(NUM_LEDS, PIN_STRIP, NEO_GRB + NEO_KHZ800);

// --- VARIABLES GLOBALES Y MEMORIA ---
uint8_t ledBrightness = 130;
const int EEPROM_SIZE = 2; // [0]: Brillo, [1]: Banco
const int ADDR_BRIGHTNESS = 0;
const int ADDR_BANK = 1;

uint32_t bankColors[5] = {strip.Color(211, 0, 189), strip.Color(117, 0, 209),
                          strip.Color(41, 183, 183), strip.Color(255, 255, 0),
                          strip.Color(255, 0, 255)};

// --- COMPONENTES MIDI ---
USBMIDI_Interface midi;
Bank<5> bank(5);

// --- LOGICA SYSEX Y CALLBACKS ---
class MyMIDIInput : public MIDI_Callbacks {
public:
  void onSysExMessage(MIDI_Interface &midi_if, SysExMessage msg) override {
    Serial.print(msg.data[1]);
    if (msg.length == 5 && msg.data[1] == 0x7D) {
      uint8_t paramID = msg.data[2];
      uint8_t value = msg.data[3];

      switch (paramID) {
      case 0x01: // Brillo
        ledBrightness = map(value, 0, 127, 0, 255);
        break;
      case 0x02: // Banco
        if (value < 5)
          bank.select(value);
        break;
      case 0x0A: // COMANDO GUARDAR EN FLASH
        EEPROM.write(ADDR_BRIGHTNESS, ledBrightness);
        EEPROM.write(ADDR_BANK, bank.getSelection());
        EEPROM.commit(); // Crucial en la Pico
        break;
      }
    }
  }
};

MyMIDIInput sysExCallbacks;

// --- SELECTORES Y BOTONES ---
IncrementSelectorLEDs<5> selector{bank, {2}, {9, 10, 11, 12, 13}};
Bankable::CCButton pulsador1{
    {bank, BankType::ChangeAddress}, 5, {0x51, Channel_11}};
Bankable::CCButtonLatched<5> pulsador2{
    {bank, BankType::ChangeAddress}, 4, {0x52, Channel_11}};
Bankable::CCButtonLatched<5> pulsador3{
    {bank, BankType::ChangeAddress}, 3, {0x53, Channel_11}};
Bankable::CCButtonLatched<5> pulsador4{
    {bank, BankType::ChangeAddress}, 7, {0x54, Channel_11}};

using CCSmartPot = Bankable::CCSmartPotentiometer<5>;
CCSmartPot potentiometer{
    {bank, BankType::ChangeAddress}, A0, {0x01, Channel_11}};
CCSmartPot potentiometer2{
    {bank, BankType::ChangeAddress}, A1, {0x02, Channel_11}};

// --- FUNCIONES DE APOYO ---
uint32_t applyBrightness(uint32_t color, uint8_t brightness) {
  uint8_t r = (uint8_t)((color >> 16) & 0xFF);
  uint8_t g = (uint8_t)((color >> 8) & 0xFF);
  uint8_t b = (uint8_t)(color & 0xFF);
  return strip.Color((r * brightness) / 255, (g * brightness) / 255,
                     (b * brightness) / 255);
}

void updateBankLeds() {
  uint8_t activeBank = bank.getSelection();
  uint32_t color = bankColors[activeBank];
  strip.setPixelColor(0, applyBrightness(color, ledBrightness));
  strip.setPixelColor(
      1, pulsador2.getState() ? applyBrightness(color, ledBrightness) : 0);
  strip.setPixelColor(
      2, pulsador3.getState() ? applyBrightness(color, ledBrightness) : 0);
  strip.setPixelColor(3, strip.Color(200, 200, 200));
  strip.show();
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  ledBrightness = EEPROM.read(ADDR_BRIGHTNESS);
  if (ledBrightness == 255)
    ledBrightness = 130; // Valor por defecto si está vacía
  bank.select(EEPROM.read(ADDR_BANK) % 5);

  pixels.begin();
  strip.begin();

  midi.setCallbacks(sysExCallbacks);
  Control_Surface.begin();
}

void loop() {
  Control_Surface.loop();
  pixels.setPixelColor(0, pixels.Color(255, 255, 255));
  pixels.show();
  updateBankLeds();
}