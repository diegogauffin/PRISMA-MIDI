#include <Adafruit_NeoPixel.h>
#include <Control_Surface.h>
#include <EEPROM.h>

// --- HARDWARE CONFIGURATION ---
#define PIN 16
#define NUMPIXELS 1
#define PIN_STRIP 8
#define NUM_LEDS 4

#define PIN_WS2812_BANK 10  // Pin for WS2812 bank indicators
#define NUM_BANK_LEDS 6     

#define PIN_BTN_SELECTOR 2

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip(NUM_LEDS, PIN_STRIP, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel bankStrip(NUM_BANK_LEDS, PIN_WS2812_BANK, NEO_GRB + NEO_KHZ800);

// --- SYSEX CONSTANTS (MIDI HANGAR) ---
const uint8_t SYSEX_MAN_ID = 0x7D;
const uint8_t SYSEX_MODEL_ID = 0x01; // MR9 Model
const uint8_t CMD_GET_CONFIG = 0x01;
const uint8_t CMD_SET_CONFIG = 0x02;
const uint8_t CMD_CONFIG_RESPONSE = 0x03;
const uint8_t CMD_GET_INFO = 0x04;
const uint8_t CMD_INFO_RESPONSE = 0x05;

// --- CONFIGURATION DATA ---
const int NUM_CONFIG_BUTTONS = 3;
struct ButtonConfig {
    uint8_t mode; // 0: CC, 1: Latched, 2: Note
    uint8_t value; // Note/CC value
    uint8_t channel;
    uint8_t r, g, b;
    uint8_t brightness;
};

ButtonConfig buttonConfigs[NUM_CONFIG_BUTTONS];
bool configsChanged = false;

// Configs in EEPROM start here
const int EEPROM_START_ADDR = 10; 

void loadConfigs() {
    // AVR/Pro Micro does not need EEPROM.begin
    for (int i = 0; i < NUM_CONFIG_BUTTONS; i++) {
        int addr = EEPROM_START_ADDR + (i * sizeof(ButtonConfig));
        EEPROM.get(addr, buttonConfigs[i]);
        
        // Si el canal no tiene sentido (memoria sucia), seteamos defaults con color visible
        if (buttonConfigs[i].channel > 16 || buttonConfigs[i].channel == 0) {
            // mode, value, channel, r, g, b, brightness
            if(i == 0) buttonConfigs[i] = {0, 0, 1, 255, 0, 0, 130}; // Red
            if(i == 1) buttonConfigs[i] = {1, 0, 1, 0, 255, 0, 130}; // Green
            if(i == 2) buttonConfigs[i] = {1, 0, 1, 0, 0, 255, 130}; // Blue
        }
    }
}

void saveConfigs() {
    for (int i = 0; i < NUM_CONFIG_BUTTONS; i++) {
        int addr = EEPROM_START_ADDR + (i * sizeof(ButtonConfig));
        EEPROM.put(addr, buttonConfigs[i]);
    }
    // No EEPROM.commit() needed for AVR/Pro Micro
}


USBMIDI_Interface midi;
Bank<5> bank(5);

class MyMIDIInput : public MIDI_Callbacks {
public:
  void onSysExMessage(MIDI_Interface &midi_if, SysExMessage msg) override {
    if (msg.length >= 5 && msg.data[1] == SYSEX_MAN_ID && msg.data[2] == SYSEX_MODEL_ID) {
      uint8_t command = msg.data[3];

      if (command == CMD_GET_INFO) {
        // Send INFO RESPONSE
        const char* deviceName = "MIDIROOTS // MR9";
        uint8_t nameLen = strlen(deviceName);
        uint8_t responseLen = 9 + nameLen;
        uint8_t sysexData[responseLen];

        sysexData[0] = SYSEX_MAN_ID;
        sysexData[1] = SYSEX_MODEL_ID;
        sysexData[2] = CMD_INFO_RESPONSE;
        sysexData[3] = 1; // Major version
        sysexData[4] = 0; // Minor version
        sysexData[5] = 0; // Patch version
        sysexData[6] = NUM_CONFIG_BUTTONS; // Buttons count
        sysexData[7] = nameLen;
        for (int i = 0; i < nameLen; i++) {
            sysexData[8 + i] = deviceName[i];
        }

        midi_if.sendSysEx(sysexData, responseLen);

      } else if (command == CMD_GET_CONFIG) {
        // Enviar config response para cada botón
        for (int i = 0; i < NUM_CONFIG_BUTTONS; i++) {
            uint8_t r1 = (buttonConfigs[i].r >> 7) & 0x01;
            uint8_t r2 = buttonConfigs[i].r & 0x7F;
            uint8_t g1 = (buttonConfigs[i].g >> 7) & 0x01;
            uint8_t g2 = buttonConfigs[i].g & 0x7F;
            uint8_t b1 = (buttonConfigs[i].b >> 7) & 0x01;
            uint8_t b2 = buttonConfigs[i].b & 0x7F;
            uint8_t br1 = (buttonConfigs[i].brightness >> 7) & 0x01;
            uint8_t br2 = buttonConfigs[i].brightness & 0x7F;

            uint8_t sysexData[] = {
                SYSEX_MAN_ID, SYSEX_MODEL_ID, CMD_CONFIG_RESPONSE,
                (uint8_t)i,
                buttonConfigs[i].mode,
                buttonConfigs[i].value,
                buttonConfigs[i].channel,
                r1, r2,
                g1, g2,
                b1, b2,
                br1, br2
            };
            midi_if.sendSysEx(sysexData, sizeof(sysexData));
        }

      } else if (command == CMD_SET_CONFIG && msg.length >= 16) {
        uint8_t idx = msg.data[4];
        if (idx < NUM_CONFIG_BUTTONS) {
            buttonConfigs[idx].mode = msg.data[5];
            buttonConfigs[idx].value = msg.data[6];
            buttonConfigs[idx].channel = msg.data[7];
            
            buttonConfigs[idx].r = (msg.data[8] << 7) | msg.data[9];
            buttonConfigs[idx].g = (msg.data[10] << 7) | msg.data[11];
            buttonConfigs[idx].b = (msg.data[12] << 7) | msg.data[13];
            
            buttonConfigs[idx].brightness = (msg.data[14] << 7) | msg.data[15];

            configsChanged = true;
            saveConfigs();
        }
      }
    }
  }
};

MyMIDIInput sysExCallbacks;

// --- BANK INDICATOR BUTTON ---
IncrementSelector<5> selector{bank, PIN_BTN_SELECTOR};

// Para simplificar la base que recibe SysEx, las configuraciones dinámicas 
// requieren clases custom o actualizarlas en loop. Como Base, los definimos estáticos:
Bankable::CCButton pulsador1{ {bank, BankType::ChangeAddress}, 5, {0x51, Channel_11} };
Bankable::CCButtonLatched<5> pulsador2{ {bank, BankType::ChangeAddress}, 4, {0x52, Channel_11} };
Bankable::CCButtonLatched<5> pulsador3{ {bank, BankType::ChangeAddress}, 3, {0x53, Channel_11} };
Bankable::CCButtonLatched<5> pulsador4{ {bank, BankType::ChangeAddress}, 7, {0x54, Channel_11} };

// --- FUNCIONES DE APOYO ---
uint32_t applyBrightness(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  uint32_t finalR = (r * brightness) / 255;
  uint32_t finalG = (g * brightness) / 255;
  uint32_t finalB = (b * brightness) / 255;
  return strip.Color(finalR, finalG, finalB);
}

void updateBankLeds() {
    uint8_t activeBank = bank.getSelection();

    uint32_t colors[3] = {
      applyBrightness(buttonConfigs[0].r, buttonConfigs[0].g, buttonConfigs[0].b, buttonConfigs[0].brightness),
      applyBrightness(buttonConfigs[1].r, buttonConfigs[1].g, buttonConfigs[1].b, buttonConfigs[1].brightness),
      applyBrightness(buttonConfigs[2].r, buttonConfigs[2].g, buttonConfigs[2].b, buttonConfigs[2].brightness)
    };

    int pins[3] = {5, 4, 3};
    bool latchedStates[3] = {
      false, // pulsador 1 is NOT Latched, so it doesn't have a toggle state
      pulsador2.getState(), 
      pulsador3.getState()
    };

    for (int i = 0; i < 3; i++) {
        bool ledOn = false;
        
        if (buttonConfigs[i].mode == 2) {
            // NOTE -> Siempre encendido
            ledOn = true;
        } else if (buttonConfigs[i].mode == 1) {
            // LATCHED -> Enciende opaco al pulsar (según estado latched)
            // IMPORTANTE: Como el pulsador 1 es estáticamente CCButton, no guarda latencia.
            // Para fines demostrativos usamos latchedStates. 
            ledOn = latchedStates[i];
        } else {
            // MOMENTARY (Mode 0: CC) -> Encendido sólo mientras se mantiene presionado
            ledOn = (digitalRead(pins[i]) == LOW);
        }
        
        strip.setPixelColor(i, ledOn ? colors[i] : 0);
    }

    // Pulsador 4 - Blanco (Fijo)
    strip.setPixelColor(3, strip.Color(100, 100, 100)); 
    strip.show();

    // Leds WS2812 exclusivos para Bancos
    bankStrip.clear();
    bankStrip.setPixelColor(activeBank, bankStrip.Color(200, 200, 0));
    bankStrip.show();
}

void setup() {
    Serial.begin(115200);
    loadConfigs();

    pixels.begin();
    strip.begin();
    bankStrip.begin();
    
    // Iniciar con LEDs de inicio
    pixels.setPixelColor(0, pixels.Color(255, 255, 255));
    pixels.show();

    midi.setCallbacks(sysExCallbacks);
    Control_Surface.begin();
}

void loop() {
    Control_Surface.loop();
    
    // Mantener LED de estado principal
    pixels.setPixelColor(0, pixels.Color(255, 255, 255));
    pixels.show();

    updateBankLeds();
}
