#include <Adafruit_NeoPixel.h>
#include <Control_Surface.h>
#include <EEPROM.h>

// --- HARDWARE CONFIGURATION ---
const uint8_t MODEL_FAMILY = 0; // MIDIROOTs
const uint8_t NUM_MAIN_BUTTONS = 4; // 5, 4, 3, 2
const uint8_t NUM_AUX_JACKS = 4;    // (2 Stereo Jacks: 4 pins)
const uint8_t N_ANALOG_INPUTS = 2; // Expression Pedals
const uint8_t NUM_BANKS = 5;
const uint8_t BANK_INDICATOR_TYPE = 1; // LEDs Discretos
const uint8_t HAS_SERIAL_OUT = 1;

#define PIN 16
#define NUMPIXELS 1
#define PIN_STRIP 8
#define NUM_LEDS 4

const pin_t bankLedPins[] = {9, 10, 11, 12, 13};
#define PIN_BTN_SELECTOR 255 

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip(NUM_LEDS, PIN_STRIP, NEO_GRB + NEO_KHZ800);

// --- SYSEX CONSTANTS ---
const uint8_t SYSEX_MAN_ID = 0x7D;
const uint8_t SYSEX_MODEL_ID = 0x01;

const uint8_t CMD_GET_CONFIG = 0x01;
const uint8_t CMD_SET_CONFIG = 0x02;
const uint8_t CMD_CONFIG_RESPONSE = 0x03;
const uint8_t CMD_GET_INFO = 0x04;
const uint8_t CMD_INFO_RESPONSE = 0x05;
const uint8_t CMD_SET_ANALOG = 0x06;
const uint8_t CMD_SET_GLOBAL = 0x07;
const uint8_t CMD_GET_GLOBAL = 0x08;
const uint8_t CMD_SET_BANK_BULK = 0x09;
const uint8_t CMD_BTN_PRESS = 0x0A;
const uint8_t CMD_POT_LIVE = 0x0B;

// --- CONFIGURATION DATA ---
const int TOTAL_BUTTONS = NUM_MAIN_BUTTONS + NUM_AUX_JACKS;

enum class ButtonMode : uint8_t {
  CC      = 0,
  Latched = 1,
  Note    = 2,
  BankInc = 3,
  BankDec = 4
};

struct ButtonConfig {
    ButtonMode mode;
    uint8_t number;
    uint8_t channel;
    uint8_t r, g, b;
    uint8_t brightness;
    bool latchedState;
};

struct AnalogConfig {
  uint16_t minValue;
  uint16_t maxValue;
  uint8_t number;
  uint8_t channel;
};

struct Configuration {
  char magic[2] = {'P', 'R'};
  uint8_t deviceId = 0x01;
  uint8_t bankIncBtn = 255; 
  uint8_t bankDecBtn = 255; 
  uint8_t globalBr = 150; 
  ButtonConfig buttons[NUM_BANKS][TOTAL_BUTTONS];
  AnalogConfig analogs[N_ANALOG_INPUTS];
};

Configuration gConfig;

void loadConfigs() {
    EEPROM.begin(512);
    EEPROM.get(0, gConfig);
    if (gConfig.magic[0] != 'P' || gConfig.magic[1] != 'R') {
        gConfig.magic[0] = 'P'; gConfig.magic[1] = 'R'; gConfig.deviceId = 0x01;
        gConfig.bankIncBtn = 255;
        gConfig.bankDecBtn = 255;
        gConfig.globalBr = 150;
        for (uint8_t b = 0; b < NUM_BANKS; b++) {
            for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
                gConfig.buttons[b][i] = {(ButtonMode)0, (uint8_t)(20 + i), 7, 255, 255, 255, 255, false};
            }
        }
        gConfig.analogs[0] = {0, 1023, 10, 11};
        gConfig.analogs[1] = {0, 1023, 11, 11};
        EEPROM.put(0, gConfig);
        EEPROM.commit();
    }
}

void saveConfigs() {
    EEPROM.put(0, gConfig);
    EEPROM.commit();
}

USBMIDI_Interface midi;
Bank<NUM_BANKS> bank(NUM_BANKS);

// --- POTENTIOMETERS ---
using CCSmartPot = Bankable::CCSmartPotentiometer<NUM_BANKS>;
CCSmartPot potentiometer1{ {bank, BankType::ChangeAddress}, A0, {10, Channel_11} };
CCSmartPot potentiometer2{ {bank, BankType::ChangeAddress}, A1, {11, Channel_11} };

constexpr analog_t maxRawValue = CCSmartPot::getMaxRawValue();

analog_t mapPot0(analog_t raw) {
  uint16_t minV = gConfig.analogs[0].minValue; uint16_t maxV = gConfig.analogs[0].maxValue;
  if(maxV <= minV) maxV = minV + 1;
  return map(constrain(raw, minV, maxV), minV, maxV, 0, maxRawValue);
}
analog_t mapPot1(analog_t raw) {
  uint16_t minV = gConfig.analogs[1].minValue; uint16_t maxV = gConfig.analogs[1].maxValue;
  if(maxV <= minV) maxV = minV + 1;
  return map(constrain(raw, minV, maxV), minV, maxV, 0, maxRawValue);
}

// --- BOTONES DINÁMICOS ---
class DynamicButton : public Updatable<MIDIOutputElement> {
public:
  DynamicButton(uint8_t index, uint8_t pin) : index_(index), button_(pin) {}
  void begin() { button_.begin(); }
  void update() override {
    uint8_t b = bank.getSelection();
    AH::Button::State state = button_.update();

    if (state != AH::Button::Released && state != AH::Button::Pressed) {
      // Send Feedback to Frontend for identification
      uint8_t fbData[] = { 0xF0, SYSEX_MAN_ID, gConfig.deviceId, CMD_BTN_PRESS, index_, (uint8_t)(state == AH::Button::Falling ? 1 : 0), 0xF7 };
      midi.sendSysEx(fbData, sizeof(fbData));
    }

    if (index_ == gConfig.bankIncBtn) {
      if (state == AH::Button::Falling) {
        uint8_t next = (b + 1) >= NUM_BANKS ? 0 : b + 1;
        bank.select(next);
      }
      return;
    }
    if (index_ == gConfig.bankDecBtn) {
      if (state == AH::Button::Falling) {
        int8_t prev = (int8_t)b - 1;
        if (prev < 0) prev = NUM_BANKS - 1;
        bank.select((uint8_t)prev);
      }
      return;
    }

    auto &cfg = gConfig.buttons[b][index_];
    if (state == AH::Button::Falling) {
      if (cfg.mode == ButtonMode::Latched) { 
        cfg.latchedState = !cfg.latchedState; 
        sendValue(cfg.latchedState ? 127 : 0, b); 
      }
      else if (cfg.mode == ButtonMode::BankInc) { 
        uint8_t next = (b + 1) >= NUM_BANKS ? 0 : b + 1;
        bank.select(next); 
      }
      else if (cfg.mode == ButtonMode::BankDec) { 
        int8_t prev = (int8_t)b - 1;
        if (prev < 0) prev = NUM_BANKS - 1;
        bank.select((uint8_t)prev); 
      }
      else { 
        sendValue(127, b); 
      }
    } else if (state == AH::Button::Rising) {
      if ((uint8_t)cfg.mode < 3 && cfg.mode != ButtonMode::Latched) {
          sendValue(0, b);
      }
    }
  }
  bool isPressed() { return button_.getState() == AH::Button::Pressed; }
private:
  void sendValue(uint8_t value, uint8_t currentBank) {
    auto &cfg = gConfig.buttons[currentBank][index_];
    const MIDIAddress addr = {cfg.number, Channel(constrain((int)cfg.channel - 1, 0, 15))};
    if (cfg.mode == ButtonMode::Note) {
      if (value > 0) midi.sendNoteOn(addr, 127); else midi.sendNoteOff(addr, 0);
    } else {
      midi.sendControlChange(addr, value);
    }
  }
  uint8_t index_; AH::Button button_;
};

// Pins: 5,4,3,2 (Internal), 6,7 (AuxL), 14,15 (AuxR)
DynamicButton dynButtons[TOTAL_BUTTONS] = { 
  {0, 5}, {1, 4}, {2, 3}, {3, 2}, 
  {4, 6}, {5, 7}, {6, 14}, {7, 15} 
};

// --- SYSEX ---
class MyMIDIInput : public MIDI_Callbacks {
public:
  void onSysExMessage(MIDI_Interface &midi_if, SysExMessage msg) override {
    if (msg.length >= 5 && msg.data[1] == SYSEX_MAN_ID && msg.data[2] == gConfig.deviceId) {
      uint8_t cmd = msg.data[3];
      if (cmd == CMD_GET_INFO) {
        const char* name = "PRISMA MIDIROOTs"; uint8_t nLen = strlen(name);
        uint8_t total = 16 + nLen; uint8_t *data = new uint8_t[total];
        data[0] = 0xF0; data[1] = SYSEX_MAN_ID; data[2] = gConfig.deviceId; data[3] = CMD_INFO_RESPONSE;
        data[4] = 1; data[5] = 2; data[6] = 0; data[7] = MODEL_FAMILY; data[8] = NUM_MAIN_BUTTONS;
        data[9] = N_ANALOG_INPUTS; data[10] = NUM_AUX_JACKS; data[11] = NUM_BANKS; data[12] = BANK_INDICATOR_TYPE;
        data[13] = HAS_SERIAL_OUT; data[14] = nLen; memcpy(&data[15], name, nLen); data[total - 1] = 0xF7;
        midi_if.sendSysEx(data, total); delete[] data;
      } else if (cmd == CMD_GET_CONFIG) {
        uint8_t bankToFetch = (msg.length >= 6) ? msg.data[4] : 255; 
        for (uint8_t b = 0; b < NUM_BANKS; b++) {
          if (bankToFetch != 255 && b != bankToFetch) continue;
          for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            auto &cfg = gConfig.buttons[b][i];
            uint8_t data[] = { 
              0xF0, SYSEX_MAN_ID, gConfig.deviceId, CMD_CONFIG_RESPONSE, b, i, 
              (uint8_t)cfg.mode, cfg.number, cfg.channel, 
              (uint8_t)((cfg.r >> 7) & 0x01), (uint8_t)(cfg.r & 0x7F), 
              (uint8_t)((cfg.g >> 7) & 0x01), (uint8_t)(cfg.g & 0x7F), 
              (uint8_t)((cfg.b >> 7) & 0x01), (uint8_t)(cfg.b & 0x7F), 
              (uint8_t)((cfg.brightness >> 7) & 0x01), (uint8_t)(cfg.brightness & 0x7F), 0xF7 
            };
            midi_if.sendSysEx(data, sizeof(data));
          }
        }
      } else if (cmd == CMD_SET_CONFIG && msg.length >= 18) {
        uint8_t b = msg.data[4]; uint8_t idx = msg.data[5];
        if (b < NUM_BANKS && idx < TOTAL_BUTTONS) {
            auto &cfg = gConfig.buttons[b][idx];
            cfg.mode = (ButtonMode)msg.data[6]; cfg.number = msg.data[7]; cfg.channel = constrain(msg.data[8], 1, 16);
            cfg.r = (msg.data[9] << 7) | msg.data[10]; cfg.g = (msg.data[11] << 7) | msg.data[12];
            cfg.b = (msg.data[13] << 7) | msg.data[14]; cfg.brightness = (msg.data[15] << 7) | msg.data[16];
            saveConfigs();
        }
      } else if (cmd == CMD_SET_ANALOG && msg.length >= 11) {
        uint8_t idx = msg.data[4];
        if(idx < N_ANALOG_INPUTS) {
           gConfig.analogs[idx].minValue = (msg.data[5] << 7) | msg.data[6]; 
           gConfig.analogs[idx].maxValue = (msg.data[7] << 7) | msg.data[8];
           gConfig.analogs[idx].number = msg.data[9]; 
           gConfig.analogs[idx].channel = constrain(msg.data[10], 1, 16);
           saveConfigs();
        }
      } else if (cmd == CMD_SET_GLOBAL && msg.length >= 7) { 
        gConfig.bankIncBtn = msg.data[4];
        gConfig.bankDecBtn = msg.data[5];
        gConfig.globalBr = msg.data[6];
        saveConfigs();
      } else if (cmd == CMD_GET_GLOBAL) { 
        uint8_t data[] = { 0xF0, SYSEX_MAN_ID, gConfig.deviceId, 0x08, gConfig.bankIncBtn, gConfig.bankDecBtn, gConfig.globalBr, 0xF7 };
        midi_if.sendSysEx(data, sizeof(data));
      } else if (cmd == CMD_SET_BANK_BULK && msg.length >= (5 + (TOTAL_BUTTONS * 11))) {
        uint8_t b = msg.data[4];
        if (b < NUM_BANKS) {
          for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            uint16_t base = 5 + (i * 11);
            auto &cfg = gConfig.buttons[b][i];
            cfg.mode = (ButtonMode)msg.data[base];
            cfg.number = msg.data[base + 1];
            cfg.channel = constrain(msg.data[base + 2], 1, 16);
            cfg.r = (msg.data[base + 3] << 7) | msg.data[base + 4];
            cfg.g = (msg.data[base + 5] << 7) | msg.data[base + 6];
            cfg.b = (msg.data[base + 7] << 7) | msg.data[base + 8];
            cfg.brightness = (msg.data[base + 9] << 7) | msg.data[base + 10];
          }
          saveConfigs();
        }
      }
    }
  }
};

MyMIDIInput sysExCallbacks;

// --- LED LOGIC ---
uint32_t applyBrightness(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  return strip.Color((r * brightness) / 255, (g * brightness) / 255, (b * brightness) / 255);
}

void refreshBankLEDs() {
  uint8_t sel = bank.getSelection();
  for(uint8_t i=0; i<5; i++) {
    pinMode(bankLedPins[i], OUTPUT);
    digitalWrite(bankLedPins[i], (i == sel) ? HIGH : LOW);
  }
}

void updateBankLeds() {
    uint8_t b = bank.getSelection();
    for(uint8_t i = 0; i < NUM_MAIN_BUTTONS; i++) {
        bool isOn = false;
        uint32_t color = 0;
        
        if (i == gConfig.bankIncBtn || i == gConfig.bankDecBtn) {
            isOn = true;
            uint8_t br = (gConfig.globalBr > 200) ? 200 : gConfig.globalBr;
            if (dynButtons[i].isPressed()) color = applyBrightness(255, 255, 255, 255);
            else color = applyBrightness(255, 255, 255, br);
        } else {
            auto &cfg = gConfig.buttons[b][i];
            if (cfg.mode == ButtonMode::Latched) {
                isOn = cfg.latchedState;
            } else if (cfg.mode == ButtonMode::BankInc || cfg.mode == ButtonMode::BankDec) {
                isOn = true;
                if (dynButtons[i].isPressed()) color = applyBrightness(255, 255, 255, 255);
                else color = applyBrightness(cfg.r, cfg.g, cfg.b, cfg.brightness);
            } else {
                isOn = dynButtons[i].isPressed();
            }
            
            if (isOn && color == 0) color = applyBrightness(cfg.r, cfg.g, cfg.b, cfg.brightness);
        }
        
        strip.setPixelColor(i, isOn ? color : 0);
    }
    strip.show();
}

void setup() {
    Serial.begin(115200);
    loadConfigs(); pixels.begin(); strip.begin();
    pixels.setPixelColor(0, pixels.Color(255,255,255)); pixels.show();
    midi.setCallbacks(sysExCallbacks);
    Control_Surface.begin();
    for (auto &b : dynButtons) b.begin();
    potentiometer1.map(mapPot0); potentiometer2.map(mapPot1);
}

uint16_t lastPotVal[2] = {0,0};

void loop() {
    Control_Surface.loop();
    for (auto &b : dynButtons) b.update();
    
    // Live View for Potentometers
    uint16_t val0 = potentiometer1.getRawValue();
    uint16_t val1 = potentiometer2.getRawValue();
    if (abs((int)val0 - (int)lastPotVal[0]) > 4 || abs((int)val1 - (int)lastPotVal[1]) > 4) {
      uint8_t liveData[] = { 
        0xF0, SYSEX_MAN_ID, gConfig.deviceId, CMD_POT_LIVE, 
        (uint8_t)((val0>>7)&0x7F), (uint8_t)(val0&0x7F), 
        (uint8_t)((val1>>7)&0x7F), (uint8_t)(val1&0x7F), 
        0xF7 
      };
      midi.sendSysEx(liveData, sizeof(liveData));
      lastPotVal[0] = val0; lastPotVal[1] = val1;
    }

    updateBankLeds(); refreshBankLEDs();
}
