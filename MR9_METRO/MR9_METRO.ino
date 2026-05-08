#include <Adafruit_NeoPixel.h>
#include <Control_Surface.h>
#include <EEPROM.h>
#include <SerialPIO.h>

// --- HARDWARE CONFIGURATION (MR9) ---
const uint8_t MODEL_FAMILY = 0; 
const uint8_t NUM_MAIN_BUTTONS = 4; 
const uint8_t NUM_AUX_JACKS = 4;    
const uint8_t N_ANALOG_INPUTS = 2; 
const uint8_t NUM_BANKS = 5;
const uint8_t BANK_INDICATOR_TYPE = 1; // LEDs Discretos
const uint8_t HAS_SERIAL_OUT = 1;

#define PIN 16           // Status LED
#define NUMPIXELS 1
#define PIN_STRIP 7      // Button LEDs
#define NUM_LEDS 4

const pin_t bankLedPins[] = {9, 10, 11, 12, 13};

Adafruit_NeoPixel statusLed(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripBtns(NUM_LEDS, PIN_STRIP, NEO_GRB + NEO_KHZ800);

// --- MIDI INTERFACES ---
#define PIN_SERIAL_TX 8
SerialPIO pioSerial(PIN_SERIAL_TX, NOPIN);
USBMIDI_Interface usbmidi;
HardwareSerialMIDI_Interface serialmidi{pioSerial, MIDI_BAUD};

// --- SYSEX CONSTANTS ---
const uint8_t SYSEX_MAN_ID = 0x7D;
const uint8_t SYSEX_MODEL_ID = 0x01;

const uint8_t CMD_GET_CONFIG      = 0x01;
const uint8_t CMD_SET_CONFIG      = 0x02;
const uint8_t CMD_CONFIG_RESPONSE = 0x03;
const uint8_t CMD_GET_INFO        = 0x04;
const uint8_t CMD_INFO_RESPONSE   = 0x05;
const uint8_t CMD_SET_ANALOG      = 0x06;
const uint8_t CMD_SET_GLOBAL      = 0x07;
const uint8_t CMD_GET_GLOBAL      = 0x08;
const uint8_t CMD_SET_BANK_BULK   = 0x09;
const uint8_t CMD_BTN_PRESS       = 0x0A;
const uint8_t CMD_POT_LIVE        = 0x0B;
const uint8_t CMD_CALIBRATE_POT   = 0x0C;
const uint8_t CMD_SET_BANK_LEDS   = 0x0D;
const uint8_t CMD_SET_METRO       = 0x10;
const uint8_t CMD_GET_METRO       = 0x11;

// --- CONFIGURATION DATA ---
const int TOTAL_BUTTONS = NUM_MAIN_BUTTONS + NUM_AUX_JACKS;

enum class ButtonMode : uint8_t {
  CC      = 0,
  Latched = 1,
  Note    = 2,
  BankInc = 3,
  BankDec = 4,
  TapTempo = 5
};

struct __attribute__((packed)) ButtonConfig {
    ButtonMode mode;
    uint8_t number;
    uint8_t channel;
    uint8_t r, g, b;
    uint8_t brightness;
    uint8_t velocity; 
    uint8_t flags; // Bit 0: latchedState, Bit 1: ledAlwaysOn
};

struct __attribute__((packed)) AnalogConfig {
  uint16_t minValue;
  uint16_t maxValue;
  uint8_t number;
  uint8_t channel;
};

struct __attribute__((packed)) Configuration {
  char magic[2] = {'P', 'X'}; 
  uint8_t deviceId = 0x01;
  uint8_t bankIncBtn = 255; uint8_t bankDecBtn = 255; uint8_t globalBr = 150; 
  
  ButtonConfig buttons[NUM_BANKS][TOTAL_BUTTONS];
  AnalogConfig analogs[N_ANALOG_INPUTS];

  // New fields at the END to preserve offsets
  uint8_t bankR = 255; uint8_t bankG = 255; uint8_t bankB = 255; uint8_t bankBr = 130;
  uint8_t tapTempoBtn = 255;
  uint8_t metroR = 211; uint8_t metroG = 0; uint8_t metroB = 189; uint8_t metroBr = 200;
  bool metroEnabled = false;
};

Configuration gConfig;

// Forward declarations to fix scope
void refreshBankLEDs(bool force = false);
void updateButtonLeds(bool force = false);

void loadConfigs() {
    EEPROM.begin(1024);
    EEPROM.get(0, gConfig);
    if (gConfig.magic[0] != 'P' || gConfig.magic[1] != 'X') {
        gConfig.magic[0] = 'P'; gConfig.magic[1] = 'X'; 
        gConfig.deviceId = 0x01;
        gConfig.bankIncBtn = 255; gConfig.bankDecBtn = 255; gConfig.globalBr = 150;
        gConfig.bankR = 255; gConfig.bankG = 255; gConfig.bankB = 255; gConfig.bankBr = 130;
        gConfig.metroEnabled = false; gConfig.tapTempoBtn = 255;
        gConfig.metroR = 211; gConfig.metroG = 0; gConfig.metroB = 189; gConfig.metroBr = 200;
        
        for (uint8_t b = 0; b < NUM_BANKS; b++) {
            for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
                gConfig.buttons[b][i] = {(ButtonMode)0, (uint8_t)(20 + i), 7, 255, 255, 255, 127, 127, 0};
            }
        }
        gConfig.analogs[0] = {0, 1023, 10, 11};
        gConfig.analogs[1] = {0, 1023, 11, 11};
        EEPROM.put(0, gConfig);
        EEPROM.commit();
    }
}

bool configDirty = false;
void saveConfigs() { configDirty = true; }

void processPendingSave() {
    static unsigned long lastSave = 0;
    if (configDirty && (millis() - lastSave > 1500)) {
        EEPROM.put(0, gConfig);
        EEPROM.commit();
        lastSave = millis();
        configDirty = false;
    }
}

uint8_t activeBank = 0;

// --- GLOBAL STATE ---
uint8_t potFeedbackColor = 0;   // 0: None, 1: Red/Up, 2: Green/Down
unsigned long potFeedbackTime = 0;
unsigned long beatOffTime = 0;
bool isBeat = false;
uint8_t clockCounter = 0;

// --- ADVANCED SMART POTENTIOMETER CLASS ---

template <uint8_t NumBanks> class CCSmartPotentiometer {
public:
  CCSmartPotentiometer(pin_t pin, uint8_t index)
      : analog(pin), potIndex(index) {
    for (uint8_t i = 0; i < NumBanks; i++) {
      lastValue[i] = 0;
      isLocked[i] = false;
      hasBeenTouched[i] = false;
    }
  }

  void update() {
    if (activeBank != previousBank) {
      if (hasBeenTouched[activeBank]) isLocked[activeBank] = true;
      previousBank = activeBank;
    }

    if (analog.update()) {
      uint16_t raw10 = analog.getValue();
      uint16_t minV = gConfig.analogs[potIndex].minValue;
      uint16_t maxV = gConfig.analogs[potIndex].maxValue;
      if (maxV <= minV) maxV = minV + 1;

      uint16_t constrained = constrain(raw10, minV, maxV);
      uint8_t currentMidiValue = map(constrained, minV, maxV, 0, 127);

      if (isLocked[activeBank]) {
        if (abs((int)currentMidiValue - (int)lastValue[activeBank]) <= 3) {
            isLocked[activeBank] = false;
            potFeedbackColor = 0;
        } else {
            // Visualize direction needed
            if (currentMidiValue < lastValue[activeBank]) potFeedbackColor = 1; // Need to go UP (Physical < MIDI)
            else potFeedbackColor = 2; // Need to go DOWN (Physical > MIDI)
            potFeedbackTime = millis() + 1500;
        }
      }

      if (!isLocked[activeBank]) {
        if (currentMidiValue != lastValue[activeBank] || !hasBeenTouched[activeBank]) {
          uint8_t cc = gConfig.analogs[potIndex].number;
          uint8_t ch = gConfig.analogs[potIndex].channel;
          const MIDIAddress addr = {cc, Channel(constrain((int)ch - 1, 0, 15))};
          usbmidi.sendControlChange(addr, currentMidiValue);
          serialmidi.sendControlChange(addr, currentMidiValue);
          
          uint8_t pb[] = { 0xF0, SYSEX_MAN_ID, gConfig.deviceId, CMD_POT_LIVE, (uint8_t)potIndex, 
                           (uint8_t)((raw10>>7)&0x7F), (uint8_t)(raw10&0x7F), 0xF7 };
          usbmidi.sendSysEx(pb, sizeof(pb));
          
          lastValue[activeBank] = currentMidiValue;
          hasBeenTouched[activeBank] = true;
        }
      }
    }
    
    if (potFeedbackColor != 0 && millis() > potFeedbackTime) {
        potFeedbackColor = 0;
    }
  }

private:
  FilteredAnalog<10, 7, uint32_t> analog;
  uint8_t potIndex;
  uint8_t previousBank = 255;
  uint8_t lastValue[NumBanks];
  bool isLocked[NumBanks];
  bool hasBeenTouched[NumBanks];
};

CCSmartPotentiometer<NUM_BANKS> analogPots[] = {
  {A0, 0}, {A1, 1}
};

void processMetronome() {
    if (millis() > beatOffTime && isBeat) {
        isBeat = false;
    }
    
    // Status LED logic: always show metronome color if enabled
    if (gConfig.metroEnabled || potFeedbackColor != 0) {
        if (potFeedbackColor != 0) {
            // Pot feedback priority
            bool blink = (millis() / 150) % 2;
            if (blink) {
                if (potFeedbackColor == 1) statusLed.setPixelColor(0, statusLed.Color(230, 0, 0)); // RED
                else if (potFeedbackColor == 2) statusLed.setPixelColor(0, statusLed.Color(0, 230, 0)); // GREEN
                else if (potFeedbackColor == 3) statusLed.setPixelColor(0, statusLed.Color(0, 0, 230)); // BLUE
            } else {
                statusLed.setPixelColor(0, 0);
            }
        } else {
            // Determine color and pulse status
            uint8_t r = gConfig.metroR;
            uint8_t g = gConfig.metroG;
            uint8_t b = gConfig.metroB;
            
            if (isBeat) {
                // Full brightness pulse
                statusLed.setPixelColor(0, statusLed.Color(r, g, b));
            } else {
                // Dim state (15% brightness) to avoid color distortion
                statusLed.setPixelColor(0, statusLed.Color((r*40)/255, (g*40)/255, (b*40)/255));
            }
        }
        
        static uint32_t lastShow = 0;
        if (millis() - lastShow > 10) {
            statusLed.show();
            lastShow = millis();
        }
    } else {
        statusLed.setPixelColor(0, 0);
        statusLed.show();
    }
}

// --- SYSEX & MIDI CALLBACKS ---
class MyMIDIInput : public MIDI_Callbacks {
public:
  void onRealTimeMessage(MIDI_Interface &, RealTimeMessage rt) override {
    // DIAGNOSTIC: Flash status LED briefly on MIDI Start/Continue
    if (rt.message == 0xFA || rt.message == 0xFB) {
        clockCounter = 0;
        isBeat = true;
        beatOffTime = millis() + 40; // Super sharp flash on start
        return;
    }

    if (gConfig.metroEnabled && rt.message == 0xF8) { 
        clockCounter++;
        if (clockCounter >= 24) { 
            clockCounter = 0;
            isBeat = true;
            beatOffTime = millis() + 100; // Increased duration for better visibility
        }
    }
  }

  void onSysExMessage(MIDI_Interface &midi_if, SysExMessage msg) override {
    if (msg.length >= 5 && msg.data[1] == SYSEX_MAN_ID && msg.data[2] == gConfig.deviceId) {
      // FEEDBACK: Blink BLUE on any valid SysEx from MIDI Hangar
      potFeedbackColor = 3; 
      potFeedbackTime = millis() + 150; 

      uint8_t cmd = msg.data[3];
      if (cmd == CMD_GET_INFO) {
        const char* name = "PRISMA MR9 METRO"; uint8_t nLen = strlen(name);
        uint8_t total = 16 + nLen; uint8_t *data = new uint8_t[total];
        data[0] = 0xF0; data[1] = SYSEX_MAN_ID; data[2] = gConfig.deviceId; data[3] = CMD_INFO_RESPONSE;
        data[4] = 3; data[5] = 8; data[6] = 0; // v3.8.0
        data[7] = MODEL_FAMILY; data[8] = NUM_MAIN_BUTTONS;
        data[9] = N_ANALOG_INPUTS; data[10] = NUM_AUX_JACKS; data[11] = NUM_BANKS; data[12] = BANK_INDICATOR_TYPE;
        data[13] = HAS_SERIAL_OUT; data[14] = nLen; memcpy(&data[15], name, nLen); data[total - 1] = 0xF7;
        usbmidi.sendSysEx(data, total); delete[] data;
      } else if (cmd == CMD_GET_CONFIG) {
        uint8_t b_target = (msg.length >= 6) ? msg.data[4] : 255;
        for (uint8_t b = 0; b < NUM_BANKS; b++) {
          if (b_target != 255 && b != b_target) continue;
          for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            auto &cfg = gConfig.buttons[b][i];
            uint8_t d[] = { 
              0xF0, SYSEX_MAN_ID, gConfig.deviceId, CMD_CONFIG_RESPONSE, b, i, 
              (uint8_t)cfg.mode, cfg.number, cfg.channel, 
              (uint8_t)((cfg.r>>7)&1), (uint8_t)(cfg.r&127), (uint8_t)((cfg.g>>7)&1), (uint8_t)(cfg.g&127),
              (uint8_t)((cfg.b>>7)&1), (uint8_t)(cfg.b&127), (uint8_t)((cfg.brightness>>7)&1), (uint8_t)(cfg.brightness&127),
              cfg.flags, cfg.velocity, 0xF7 
            };
            usbmidi.sendSysEx(d, sizeof(d));
          }
        }
      } else if (cmd == CMD_SET_METRO && msg.length >= 10) {
        gConfig.tapTempoBtn = msg.data[4];
        gConfig.metroR = (msg.data[5]<<7)|msg.data[6];
        gConfig.metroG = (msg.data[7]<<7)|msg.data[8];
        gConfig.metroB = (msg.data[9]<<7)|msg.data[10];
        gConfig.metroBr = msg.data[11];
        gConfig.metroEnabled = msg.data[12];
        saveConfigs();
      } else if (cmd == CMD_SET_GLOBAL && msg.length >= 7) {
        gConfig.bankIncBtn = msg.data[4];
        gConfig.bankDecBtn = msg.data[5];
        gConfig.globalBr = msg.data[6];
        if (msg.length >= 14) { // Extended global (colors)
          gConfig.bankR = (msg.data[7] << 7) | msg.data[8];
          gConfig.bankG = (msg.data[9] << 7) | msg.data[10];
          gConfig.bankB = (msg.data[11] << 7) | msg.data[12];
          gConfig.bankBr = msg.data[13];
        }
        saveConfigs();
      } else if (cmd == CMD_SET_BANK_LEDS && msg.length >= 13) {
        gConfig.bankR = (msg.data[4] << 7) | msg.data[5];
        gConfig.bankG = (msg.data[6] << 7) | msg.data[7];
        gConfig.bankB = (msg.data[8] << 7) | msg.data[9];
        gConfig.bankBr = (msg.data[10] << 7) | msg.data[11];
        saveConfigs();
        refreshBankLEDs(true);
      } else if (cmd == CMD_GET_GLOBAL) {
        uint8_t d[] = { 
          0xF0, SYSEX_MAN_ID, gConfig.deviceId, CMD_GET_GLOBAL,
          gConfig.bankIncBtn, gConfig.bankDecBtn, gConfig.globalBr,
          (uint8_t)((gConfig.bankR >> 7) & 1), (uint8_t)(gConfig.bankR & 127),
          (uint8_t)((gConfig.bankG >> 7) & 1), (uint8_t)(gConfig.bankG & 127),
          (uint8_t)((gConfig.bankB >> 7) & 1), (uint8_t)(gConfig.bankB & 127),
          gConfig.bankBr, 0xF7 
        };
        usbmidi.sendSysEx(d, sizeof(d));
      } else if (cmd == CMD_GET_METRO) {
        uint8_t d[] = { 0xF0, SYSEX_MAN_ID, gConfig.deviceId, CMD_GET_METRO,
            gConfig.tapTempoBtn, (uint8_t)((gConfig.metroR>>7)&1), (uint8_t)(gConfig.metroR&127),
            (uint8_t)((gConfig.metroG>>7)&1), (uint8_t)(gConfig.metroG&127),
            (uint8_t)((gConfig.metroB>>7)&1), (uint8_t)(gConfig.metroB&127),
            gConfig.metroBr, (uint8_t)gConfig.metroEnabled, 0xF7
        };
        usbmidi.sendSysEx(d, sizeof(d));
      } else if (cmd == CMD_SET_BANK_BULK) {
        uint8_t b = msg.data[4]; 
        if (b < NUM_BANKS) {
          for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            uint16_t base = 5 + (i * 13); auto &cfg = gConfig.buttons[b][i];
            cfg.mode = (ButtonMode)msg.data[base]; cfg.number = msg.data[base+1]; cfg.channel = constrain(msg.data[base+2], 1, 16);
            cfg.r = (msg.data[base+3]<<7)|msg.data[base+4]; cfg.g = (msg.data[base+5]<<7)|msg.data[base+6];
            cfg.b = (msg.data[base+7]<<7)|msg.data[base+8]; cfg.brightness = (msg.data[base+9]<<7)|msg.data[base+10]; 
            cfg.flags = msg.data[base+11]; cfg.velocity = msg.data[base+12];
          }
          saveConfigs();
        }
      } else if (cmd == CMD_SET_ANALOG && msg.length >= 12) {
        uint8_t idx = msg.data[5];
        if (idx < N_ANALOG_INPUTS) {
          auto &cfg = gConfig.analogs[idx];
          cfg.number = msg.data[6];
          cfg.channel = constrain(msg.data[7], 1, 16);
          cfg.minValue = (msg.data[8] << 7) | msg.data[9];
          cfg.maxValue = (msg.data[10] << 7) | msg.data[11];
          saveConfigs();
        }
      } else if (cmd == CMD_CALIBRATE_POT && msg.length >= 8) {
        uint8_t idx = msg.data[4]; uint8_t type = msg.data[5];
        uint16_t val = (msg.data[6] << 7) | msg.data[7];
        if (idx < N_ANALOG_INPUTS) {
            if (type == 0) gConfig.analogs[idx].minValue = val;
            else gConfig.analogs[idx].maxValue = val;
            saveConfigs();
        }
      }
    }
  }
};

MyMIDIInput sysExCallbacks;


// --- DYNAMIC BUTTONS (MR9 PINS) ---
class DynamicButton : public Updatable<MIDIOutputElement> {
public:
  DynamicButton(uint8_t index, uint8_t pin) : index_(index), button_(pin) {}
  void begin() { button_.begin(); }
  void update() override {
    uint8_t b = activeBank;
    AH::Button::State state = button_.update();
    if (state != AH::Button::Released && state != AH::Button::Pressed) {
      uint8_t fb[] = { 0xF0, SYSEX_MAN_ID, gConfig.deviceId, CMD_BTN_PRESS, index_, (uint8_t)(state == AH::Button::Falling ? 1 : 0), 0xF7 };
      usbmidi.sendSysEx(fb, sizeof(fb));
    }
    if (index_ == gConfig.bankIncBtn && state == AH::Button::Falling) { 
      activeBank = (activeBank + 1) % NUM_BANKS; 
      refreshBankLEDs();
      return; 
    }
    if (index_ == gConfig.bankDecBtn && state == AH::Button::Falling) { 
      activeBank = (activeBank == 0) ? NUM_BANKS - 1 : activeBank - 1; 
      refreshBankLEDs();
      return; 
    }

    auto &cfg = gConfig.buttons[b][index_];
    if (state == AH::Button::Falling) {
      if (cfg.mode == ButtonMode::Latched) { cfg.flags ^= 0x01; sendValue((cfg.flags&1)?127:0, b); }
      else if (cfg.mode == ButtonMode::BankInc) { activeBank = (activeBank + 1) % NUM_BANKS; refreshBankLEDs(); }
      else if (cfg.mode == ButtonMode::BankDec) { activeBank = (activeBank == 0) ? NUM_BANKS - 1 : activeBank - 1; refreshBankLEDs(); }
      else { sendValue(127, b); }
    } else if (state == AH::Button::Rising) {
      if ((uint8_t)cfg.mode < 3 && cfg.mode != ButtonMode::Latched) sendValue(0, b);
    }
  }
  bool isPressed() { return button_.getState() == AH::Button::Pressed; }
private:
  void sendValue(uint8_t value, uint8_t currentBank) {
    auto &cfg = gConfig.buttons[currentBank][index_];
    const MIDIAddress addr = {cfg.number, Channel(constrain((int)cfg.channel - 1, 0, 15))};
    if (cfg.mode == ButtonMode::Note) {
      if (value > 0) { usbmidi.sendNoteOn(addr, cfg.velocity); serialmidi.sendNoteOn(addr, cfg.velocity); }
      else { usbmidi.sendNoteOff(addr, 0); serialmidi.sendNoteOff(addr, 0); }
    } else {
      usbmidi.sendControlChange(addr, value); serialmidi.sendControlChange(addr, value);
    }
  }
  uint8_t index_; AH::Button button_;
};

DynamicButton dynButtons[TOTAL_BUTTONS] = { 
  {0, 5}, {1, 4}, {2, 3}, {3, 2},    // Main buttons
  {4, 0}, {5, 1}, {6, 14}, {7, 15}   // Aux jacks → pins 0, 1, 14, 15
};

// --- LED REFRESH (MR9 DISCRETE) ---
void refreshBankLEDs(bool force) {
  static uint8_t lastSel = 255;
  if(!force && activeBank == lastSel) return;
  for(uint8_t i=0; i<NUM_BANKS; i++) {
    digitalWrite(bankLedPins[i], (i == activeBank) ? HIGH : LOW);
  }
  lastSel = activeBank;
}

void updateButtonLeds(bool force) {
    uint8_t b = activeBank;
    static uint32_t lastColors[NUM_MAIN_BUTTONS];
    bool changed = force;
    
    for(uint8_t i = 0; i < NUM_MAIN_BUTTONS; i++) {
        auto &cfg = gConfig.buttons[b][i];
        uint32_t color = 0;
        bool isOn = false;
        bool isGlobal = (i == gConfig.bankIncBtn || i == gConfig.bankDecBtn);
        bool isTapTempo = (cfg.mode == ButtonMode::TapTempo) && gConfig.metroEnabled;

        if (isTapTempo && isBeat) {
            isOn = true;
            color = stripBtns.Color(gConfig.metroR, gConfig.metroG, gConfig.metroB);
        } else if (isGlobal) {
            isOn = true;
            uint8_t br = (gConfig.globalBr > 210) ? 210 : gConfig.globalBr; 
            if (dynButtons[i].isPressed()) color = stripBtns.Color(255, 255, 255);
            else color = stripBtns.Color(br, br, br);
        } else {
            bool alwaysOn = (cfg.flags >> 1) & 0x01;
            if (cfg.mode == ButtonMode::Latched) isOn = (cfg.flags & 0x01);
            else if (cfg.mode == ButtonMode::BankInc || cfg.mode == ButtonMode::BankDec) {
                isOn = true;
                if (dynButtons[i].isPressed()) color = stripBtns.Color(255, 255, 255);
                else color = stripBtns.Color((cfg.r*cfg.brightness)/255, (cfg.g*cfg.brightness)/255, (cfg.b*cfg.brightness)/255);
            } else if (cfg.mode == ButtonMode::TapTempo) {
                isOn = true; // Always show color on TapTempo buttons, pulse is handled by isBeat check above
                color = stripBtns.Color((cfg.r*30)/255, (cfg.g*30)/255, (cfg.b*30)/255); // Dim when no beat
            } else isOn = dynButtons[i].isPressed() || alwaysOn;
        }

        uint32_t finalColor = isOn ? color : 0;
        if (finalColor == 0 && isOn && cfg.mode != ButtonMode::TapTempo) { 
             finalColor = stripBtns.Color((cfg.r*cfg.brightness)/255, (cfg.g*cfg.brightness)/255, (cfg.b*cfg.brightness)/255);
        }
        
        if (finalColor != lastColors[i]) {
            stripBtns.setPixelColor(i, finalColor);
            lastColors[i] = finalColor;
            changed = true;
        }
    }
    if (changed) stripBtns.show();
}

void setup() {
    loadConfigs();
    statusLed.begin(); stripBtns.begin();
    // pioSerial is initialized by Control_Surface.begin() via serialmidi
    usbmidi.setCallbacks(sysExCallbacks);
    serialmidi.setCallbacks(sysExCallbacks);
    for (uint8_t i = 0; i < 5; i++) pinMode(bankLedPins[i], OUTPUT);
    for (auto &b : dynButtons) b.begin();
    Control_Surface.begin(); // Start interface LAST
}

void loop() {
    Control_Surface.loop();
    for (auto &b : dynButtons) b.update();
    for (auto &p : analogPots) p.update();
    refreshBankLEDs();
    updateButtonLeds();
    processPendingSave();
    processMetronome();
}
