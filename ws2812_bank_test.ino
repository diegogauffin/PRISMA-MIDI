#include <Adafruit_NeoPixel.h>
#include <Control_Surface.h>
#include <EEPROM.h>

// --- CONFIGURACIÓN DE HARDWARE ---
#define PIN_WS2812_BANK 10  // Pin para la tira LED WS2812 de los bancos
#define NUM_BANK_LEDS 6     // 6 LEDs en la tira WS2812
#define PIN_BTN_SELECTOR 6  // Pin del pulsador para cambiar el banco

// Pines y configuración de la tira NeoPixel anterior (si aún se usa para los pulsadores)
#define NUMPIXELS 1
#define PIN 16
#define PIN_STRIP 8
#define NUM_LEDS 4

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip(NUM_LEDS, PIN_STRIP, NEO_GRB + NEO_KHZ800);

// Nueva tira WS2812 exclusiva para visualizar el Banco Activo
Adafruit_NeoPixel bankStrip(NUM_BANK_LEDS, PIN_WS2812_BANK, NEO_GRB + NEO_KHZ800);

// --- VARIABLES GLOBALES Y MEMORIA ---
uint8_t ledBrightness = 130;
const int EEPROM_SIZE = 2; // [0]: Brillo, [1]: Banco
const int ADDR_BRIGHTNESS = 0;
const int ADDR_BANK = 1;

uint32_t bankColors[5] = {
  strip.Color(211, 0, 189), 
  strip.Color(117, 0, 209),
  strip.Color(41, 183, 183), 
  strip.Color(255, 255, 0),
  strip.Color(255, 0, 255)
};

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
        EEPROM.update(ADDR_BRIGHTNESS, ledBrightness);
        EEPROM.update(ADDR_BANK, bank.getSelection());
        // EEPROM.commit(); // <-- Eliminado, no es necesario ni compatible con AVR (Pro Micro)
        break;
      }
    }
  }
};

MyMIDIInput sysExCallbacks;

// --- SELECTORES Y BOTONES ---
// Uso de un solo selector (sin LEDs integrados) para manejar el banco
IncrementSelector<5> selector{bank, PIN_BTN_SELECTOR};

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
  
  // Actualizar la tira vieja (botones de pulsadores)
  strip.setPixelColor(0, applyBrightness(color, ledBrightness));
  strip.setPixelColor(
      1, pulsador2.getState() ? applyBrightness(color, ledBrightness) : 0);
  strip.setPixelColor(
      2, pulsador3.getState() ? applyBrightness(color, ledBrightness) : 0);
  strip.setPixelColor(3, strip.Color(200, 200, 200));
  strip.show();

  // Actualizar la nueva tira WS2812 que indica el Banco Activo
  bankStrip.clear();
  
  // Encendemos el LED correspondiente al banco actual (del 0 al 4)
  // Utilizamos el color del banco y el nivel de brillo general
  bankStrip.setPixelColor(activeBank, applyBrightness(color, ledBrightness));
  bankStrip.show();
}

void setup() {
  Serial.begin(115200);
  // EEPROM.begin(EEPROM_SIZE); // <-- Eliminado, no es necesario en arquitectura AVR (Pro Micro)
  ledBrightness = EEPROM.read(ADDR_BRIGHTNESS);
  if (ledBrightness == 255)
    ledBrightness = 130; 
  bank.select(EEPROM.read(ADDR_BANK) % 5);

  pixels.begin();
  strip.begin();
  bankStrip.begin(); // Iniciamos tira LED de bancos

  midi.setCallbacks(sysExCallbacks);
  Control_Surface.begin();
}

void loop() {
  Control_Surface.loop();
  pixels.setPixelColor(0, pixels.Color(255, 255, 255));
  pixels.show();
  
  updateBankLeds();
}
