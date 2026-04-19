#include <Adafruit_NeoPixel.h>
#include <Control_Surface.h>
#include <EEPROM.h>
#include <SerialPIO.h>

/**
 * PRISMA MR1-3.0 FIRMWARE
 * Board: RP2040 (Raspberry Pi Pico)
 * Pin 13 no funciona
 * Hardware Layout:
 * - 8 Buttons: Pins 2, 3, 4, 5, 6, 7, 14, 15
 * - 5 Bank LEDs (Discrete): Pins 8, 9, 10, 11, 12
 * - 1 Neopixel: Pin 1 (Feedback for Pin 3 button)
 * - 2 Expression Pedals: A0, A2
 * Pin A1 no funciona 
 */

// --- HARDWARE CONFIGURATION ---
const uint8_t MODEL_FAMILY = 0;     // MIDIROOTs
const uint8_t NUM_MAIN_BUTTONS = 8; // All buttons on main board
const uint8_t NUM_AUX_JACKS = 0;    // No external aux jacks in this version
const uint8_t N_ANALOGS = 2;        // Expression Pedals
const uint8_t NUM_BANKS = 5;
const uint8_t BANK_INDICATOR_TYPE = 1; // 1 = Discrete LEDs
const uint8_t HAS_SERIAL_OUT = 1;

const uint8_t TOTAL_BUTTONS = NUM_MAIN_BUTTONS + NUM_AUX_JACKS;
const uint8_t NEOPIXEL_BTN_INDEX = 1; // Default index for hardware specific Neopixel pin

// PINES
constexpr uint8_t BUTTON_PINS[TOTAL_BUTTONS] = {2, 3, 4, 5, 6, 7, 14, 15};
constexpr uint8_t BANK_LED_PINS[NUM_BANKS] = {8, 9, 10, 11, 12};
constexpr uint8_t ANALOG_PINS[N_ANALOGS] = {A0, A1};
#define PIN_NEOPIXEL 1
#define PIN_SERIAL_TX 0

// NEOPX (Only 1 pixel for button on Pin 3, which is index 1 in BUTTON_PINS)
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// --- MIDI INTERFACES ---
SerialPIO pioSerial(PIN_SERIAL_TX, NOPIN); // Use PIO to avoid conflict on Pin 1
USBMIDI_Interface usbmidi;
HardwareSerialMIDI_Interface serialmidi{pioSerial, MIDI_BAUD};

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
const uint8_t CMD_CALIBRATE_POT = 0x0C;
const uint8_t CMD_SET_BANK_LEDS = 0x0D;

// --- VERSION ---
uint8_t V_MAJOR = 3;
uint8_t V_MINOR = 6;

// --- CONFIGURATION DATA ---
enum class ButtonMode : uint8_t {
  CC = 0,
  Latched = 1,
  Note = 2,
  BankInc = 3,
  BankDec = 4
};

struct ButtonConfig {
  ButtonMode mode;
  uint8_t number;
  uint8_t channel;
  uint8_t r, g, b;
  uint8_t brightness;
  uint8_t flags; // Bit 0: latchedState, Bit 1: ledAlwaysOn
  uint8_t velocity;
};

struct AnalogConfig {
  uint16_t minValue;
  uint16_t maxValue;
  uint8_t number;
  uint8_t channel;
};

struct Configuration {
  char magic[2] = {'M', 'R'};
  uint8_t deviceId = 0x01;
  uint8_t bankIncBtn = 255;
  uint8_t bankDecBtn = 255;
  uint8_t globalBr = 150;
  ButtonConfig buttons[NUM_BANKS][TOTAL_BUTTONS];
  AnalogConfig analogs[N_ANALOGS]; 
};

Configuration gConfig;

void initDefaultConfig() {
  gConfig.magic[0] = 'M';
  gConfig.magic[1] = 'R';
  gConfig.deviceId = 0x01;
  gConfig.bankIncBtn = 255;
  gConfig.bankDecBtn = 255;
  gConfig.globalBr = 150;
  for (uint8_t b = 0; b < NUM_BANKS; b++) {
    for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
      gConfig.buttons[b][i] = {
          ButtonMode::CC, (uint8_t)(20 + i), 7, 255, 255, 255, 255, 0, 127};
    }
  }
  gConfig.analogs[0] = {0, 1023, 10, 7};
  gConfig.analogs[1] = {0, 1023, 11, 7};
}

void loadConfigs() {
  EEPROM.begin(512);
  EEPROM.get(0, gConfig);
  if (gConfig.magic[0] != 'M' || gConfig.magic[1] != 'R') {
    initDefaultConfig();
    EEPROM.put(0, gConfig);
    EEPROM.commit();
  }
}

bool configDirty = false;

void saveConfigs(bool immediate = false) {
  if (immediate) {
    EEPROM.put(0, gConfig);
    EEPROM.commit();
    configDirty = false;
    return;
  }
  configDirty = true;
}

void processPendingSave() {
  static unsigned long lastSave = 0;
  if (configDirty && (millis() - lastSave > 1500)) {
    EEPROM.put(0, gConfig);
    EEPROM.commit();
    lastSave = millis();
    configDirty = false;
    Serial.println("EEPROM: Saved changes");
  }
}

// --- GLOBALS ---
Bank<NUM_BANKS> bank(NUM_BANKS);

// ============================================================================
// CLASE SMART POT (SOFT TAKEOVER)
// ============================================================================
template <uint8_t NumBanks> class CCSmartPotentiometer {
public:
  CCSmartPotentiometer(BankConfig<NumBanks> bankConfig, pin_t pin,
                       uint8_t index)
      : bank(bankConfig.bank), analog(pin), potIndex(index) {
    for (uint8_t i = 0; i < NumBanks; i++) {
      lastValue[i] = 0;
      isLocked[i] = false;
      hasBeenTouched[i] = false;
    }
  }

  void update() {
    uint8_t currentBank = bank.getSelection();
    if (currentBank != previousBank) {
      if (hasBeenTouched[currentBank])
        isLocked[currentBank] = true;
      previousBank = currentBank;
    }

    if (analog.update()) {
      uint16_t raw10 = analog.getValue();
      uint16_t minV = gConfig.analogs[potIndex].minValue;
      uint16_t maxV = gConfig.analogs[potIndex].maxValue;
      if (maxV <= minV)
        maxV = minV + 1;

      uint16_t constrained = constrain(raw10, minV, maxV);
      uint8_t currentMidiValue = map(constrained, minV, maxV, 0, 127);

      if (isLocked[currentBank]) {
        if (abs((int)currentMidiValue - (int)lastValue[currentBank]) <= 3)
          isLocked[currentBank] = false;
      }

      if (!isLocked[currentBank]) {
        if (currentMidiValue != lastValue[currentBank] ||
            !hasBeenTouched[currentBank]) {
          uint8_t baseCC = gConfig.analogs[potIndex].number;
          uint8_t cc = baseCC + (currentBank * 10);
          uint8_t ccChan =
              constrain((int)gConfig.analogs[potIndex].channel - 1, 0, 15);
          MIDIAddress addr = {cc, Channel(ccChan)};
          usbmidi.sendControlChange(addr, currentMidiValue);
          serialmidi.sendControlChange(addr, currentMidiValue);
          lastValue[currentBank] = currentMidiValue;
          hasBeenTouched[currentBank] = true;
          uint8_t liveData[] = {potIndex, (uint8_t)((raw10 >> 7) & 127),
                                (uint8_t)(raw10 & 127)};
          sendSysExResponse(CMD_POT_LIVE, liveData, 3);
        }
      }
    }
  }

  uint16_t getFilteredValue() { return analog.getValue(); }

private:
  Bank<NumBanks> &bank;
  AH::FilteredAnalog<10, 3, uint16_t> analog;
  uint8_t potIndex;
  uint8_t lastValue[NumBanks];
  bool isLocked[NumBanks];
  bool hasBeenTouched[NumBanks];
  uint8_t previousBank = 255;
};

using CCSmartPot = CCSmartPotentiometer<NUM_BANKS>;
CCSmartPot potentiometer1{{bank, BankType::ChangeAddress}, ANALOG_PINS[0], 0};
CCSmartPot potentiometer2{{bank, BankType::ChangeAddress}, ANALOG_PINS[1], 1};

// ============================================================================
// COMMUNICATION UTILITIES
// ============================================================================
static uint8_t sysexBuffer[256];

void sendSysExResponse(uint8_t cmd, const uint8_t *payload, size_t len) {
  if (len > 250)
    len = 250;
  sysexBuffer[0] = 0xF0;
  sysexBuffer[1] = SYSEX_MAN_ID;
  sysexBuffer[2] = gConfig.deviceId;
  sysexBuffer[3] = cmd;
  if (payload && len > 0)
    memcpy(&sysexBuffer[4], payload, len);
  sysexBuffer[4 + len] = 0xF7;
  usbmidi.sendSysEx(sysexBuffer, 5 + len);
}

class DynamicButton : public Updatable<> {
public:
  DynamicButton(uint8_t index, uint8_t pin) : index_(index), button_(pin) {}
  void begin() { button_.begin(); }
  void update() override {
    uint8_t b = bank.getSelection();
    AH::Button::State state = button_.update();

    if (state == AH::Button::Falling || state == AH::Button::Rising) {
      uint8_t feedbackData[] = {
          index_, (uint8_t)(state == AH::Button::Falling ? 1 : 0)};
      sendSysExResponse(CMD_BTN_PRESS, feedbackData, 2);
    }

    if (index_ == gConfig.bankIncBtn && state == AH::Button::Falling) {
      bank.select((b + 1) % NUM_BANKS);
      return;
    }
    if (index_ == gConfig.bankDecBtn && state == AH::Button::Falling) {
      bank.select(b == 0 ? NUM_BANKS - 1 : b - 1);
      return;
    }

    auto &cfg = gConfig.buttons[b][index_];
    if (state == AH::Button::Falling) {
      if (cfg.mode == ButtonMode::Latched) {
        cfg.flags ^= 0x01; // Toggle latched bit
        sendValue((cfg.flags & 0x01) ? 127 : 0, b);
      } else if (cfg.mode == ButtonMode::BankInc)
        bank.select((b + 1) % NUM_BANKS);
      else if (cfg.mode == ButtonMode::BankDec)
        bank.select(b == 0 ? NUM_BANKS - 1 : b - 1);
      else
        sendValue(127, b);
    } else if (state == AH::Button::Rising) {
      if (cfg.mode != ButtonMode::Latched && (uint8_t)cfg.mode < 3)
        sendValue(0, b);
    }
  }
  bool isPressed() { return button_.getState() == AH::Button::Pressed; }

private:
  void sendValue(uint8_t value, uint8_t currentBank) {
    auto &cfg = gConfig.buttons[currentBank][index_];
    const MIDIAddress addr = {cfg.number,
                              Channel(constrain((int)cfg.channel - 1, 0, 15))};
    if (cfg.mode == ButtonMode::Note) {
      if (value > 0) {
        usbmidi.sendNoteOn(addr, cfg.velocity);
        serialmidi.sendNoteOn(addr, cfg.velocity);
      } else {
        usbmidi.sendNoteOff(addr, 0);
        serialmidi.sendNoteOff(addr, 0);
      }
    } else {
      usbmidi.sendControlChange(addr, value);
      serialmidi.sendControlChange(addr, value);
    }
  }
  uint8_t index_;
  AH::Button button_;
};

DynamicButton dynButtons[TOTAL_BUTTONS] = {{0, 2}, {1, 3}, {2, 4},  {3, 5},
                                           {4, 6}, {5, 7}, {6, 14}, {7, 15}};

// ============================================================================
// SYSEX HANDLER
// ============================================================================
class MyMIDIInput : public MIDI_Callbacks {
public:
  void onSysExMessage(MIDI_Interface &midi_if, SysExMessage msg) override {
    if (msg.length >= 5 && msg.data[1] == SYSEX_MAN_ID &&
        msg.data[2] == gConfig.deviceId) {
      uint8_t cmd = msg.data[3];
      if (cmd == CMD_GET_INFO) {
        const char *name = "PRISMA MR1-3.0";
        uint8_t nLen = strlen(name);
        uint8_t d[40];
        d[0] = V_MAJOR; d[1] = V_MINOR;
        d[2] = 0;
        d[3] = MODEL_FAMILY;
        d[4] = NUM_MAIN_BUTTONS;
        d[5] = N_ANALOGS;
        d[6] = NUM_AUX_JACKS;
        d[7] = NUM_BANKS;
        d[8] = BANK_INDICATOR_TYPE;
        d[9] = HAS_SERIAL_OUT;
        d[10] = nLen;
        memcpy(&d[11], name, nLen);
        sendSysExResponse(CMD_INFO_RESPONSE, d, 11 + nLen);
      } else if (cmd == CMD_GET_CONFIG) {
        uint8_t requestedBank = (msg.length >= 6) ? msg.data[4] : 255;
        for (uint8_t b = 0; b < NUM_BANKS; b++) {
          if (requestedBank != 255 && b != requestedBank)
            continue;
          for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            auto &cfg = gConfig.buttons[b][i];
            uint8_t d[] = {b,
                           i,
                           (uint8_t)cfg.mode,
                           cfg.number,
                           cfg.channel,
                           (uint8_t)((cfg.r >> 7) & 1),
                           (uint8_t)(cfg.r & 127),
                           (uint8_t)((cfg.g >> 7) & 1),
                           (uint8_t)(cfg.g & 127),
                           (uint8_t)((cfg.b >> 7) & 1),
                           (uint8_t)(cfg.b & 127),
                           (uint8_t)((cfg.brightness >> 7) & 1),
                           (uint8_t)(cfg.brightness & 127),
                           cfg.flags,
                           cfg.velocity};
            sendSysExResponse(CMD_CONFIG_RESPONSE, d, 15);
          }
        }
      } else if (cmd == CMD_SET_CONFIG && msg.length >= 18) {
        uint8_t b = msg.data[4];
        uint8_t idx = msg.data[5];
        if (b < NUM_BANKS && idx < TOTAL_BUTTONS) {
          auto &cfg = gConfig.buttons[b][idx];
          cfg.mode = (ButtonMode)msg.data[6];
          cfg.number = msg.data[7];
          cfg.channel = constrain(msg.data[8], 1, 16);
          cfg.r = (msg.data[9] << 7) | msg.data[10];
          cfg.g = (msg.data[11] << 7) | msg.data[12];
          cfg.b = (msg.data[13] << 7) | msg.data[14];
          cfg.brightness = (msg.data[15] << 7) | msg.data[16];
          if (msg.length >= 19)
            cfg.flags = msg.data[17];
          if (msg.length >= 20)
            cfg.velocity = msg.data[18];
          saveConfigs();
        }
      } else if (cmd == CMD_SET_ANALOG && msg.length >= 12) {
        uint8_t idx = msg.data[5];
        if (idx < N_ANALOGS) {
          gConfig.analogs[idx].number = msg.data[6];
          gConfig.analogs[idx].channel = constrain(msg.data[7], 1, 16);
          gConfig.analogs[idx].minValue = (msg.data[8] << 7) | msg.data[9];
          gConfig.analogs[idx].maxValue = (msg.data[10] << 7) | msg.data[11];
          saveConfigs();
        }
      } else if (cmd == CMD_SET_GLOBAL) {
        gConfig.bankIncBtn = msg.data[4];
        gConfig.bankDecBtn = msg.data[5];
        gConfig.globalBr = msg.data[6];
        saveConfigs();
      } else if (cmd == CMD_GET_GLOBAL) {
        uint16_t m0 = gConfig.analogs[0].minValue;
        uint16_t M0 = gConfig.analogs[0].maxValue;
        uint16_t m1 = gConfig.analogs[1].minValue;
        uint16_t M1 = gConfig.analogs[1].maxValue;

        uint8_t d[23]; // 27 total bytes minus header
        d[0] = gConfig.bankIncBtn;
        d[1] = gConfig.bankDecBtn;
        d[2] = gConfig.globalBr;
        // Padding for RGB/BR not used in MR1 discrete LEDs
        d[3] = 0; d[4] = 0; d[5] = 0; d[6] = 0; d[7] = 0; d[8] = 0; d[9] = 0; d[10] = 0;

        // Pedal 0 (index 11..16 in payload, starting after header 4 bytes)
        d[11] = (uint8_t)((m0 >> 7) & 127);
        d[12] = (uint8_t)(m0 & 127);
        d[13] = (uint8_t)((M0 >> 7) & 127);
        d[14] = (uint8_t)(M0 & 127);
        d[15] = gConfig.analogs[0].number;
        d[16] = gConfig.analogs[0].channel;

        // Pedal 1 (index 17..22)
        d[17] = (uint8_t)((m1 >> 7) & 127);
        d[18] = (uint8_t)(m1 & 127);
        d[19] = (uint8_t)((M1 >> 7) & 127);
        d[20] = (uint8_t)(M1 & 127);
        d[21] = gConfig.analogs[1].number;
        d[22] = gConfig.analogs[1].channel;

        sendSysExResponse(CMD_GET_GLOBAL, d, 23);
      } else if (cmd == CMD_CALIBRATE_POT && msg.length >= 6) {
        uint8_t idx = msg.data[4];
        uint8_t type = msg.data[5];
        if (idx < N_ANALOGS) {
          uint16_t cur = (idx == 0) ? potentiometer1.getFilteredValue()
                                    : potentiometer2.getFilteredValue();
          if (type == 0) // MIN
            gConfig.analogs[idx].minValue = (cur > 1020) ? 1023 : cur + 3;
          else // MAX
            gConfig.analogs[idx].maxValue = (cur < 3) ? 0 : cur - 3;
          saveConfigs();
          uint16_t adjusted = (type == 0) ? gConfig.analogs[idx].minValue : gConfig.analogs[idx].maxValue;
          uint8_t d[] = {idx, type, (uint8_t)((adjusted >> 7) & 127),
                         (uint8_t)(adjusted & 127)};
          sendSysExResponse(CMD_CALIBRATE_POT, d, 4);
        }
      } else if (cmd == CMD_SET_BANK_BULK) {
        uint8_t b = msg.data[4];
        uint8_t bytesPerBtn = (msg.length >= (5 + (TOTAL_BUTTONS * 13)))
                                  ? 13
                                  : ((msg.length >= (5 + (TOTAL_BUTTONS * 12)))
                                         ? 12
                                         : 11);
        if (b < NUM_BANKS) {
          for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            uint16_t base = 5 + (i * bytesPerBtn);
            auto &cfg = gConfig.buttons[b][i];
            cfg.mode = (ButtonMode)msg.data[base];
            cfg.number = msg.data[base + 1];
            cfg.channel = constrain(msg.data[base + 2], 1, 16);
            cfg.r = (msg.data[base + 3] << 7) | msg.data[base + 4];
            cfg.g = (msg.data[base + 5] << 7) | msg.data[base + 6];
            cfg.b = (msg.data[base + 7] << 7) | msg.data[base + 8];
            cfg.brightness = (msg.data[base + 9] << 7) | msg.data[base + 10];
            if (bytesPerBtn >= 12)
              cfg.flags = msg.data[base + 11];
            if (bytesPerBtn >= 13)
              cfg.velocity = msg.data[base + 12];
          }
          saveConfigs();
        }
      }
    }
  }
};

MyMIDIInput sysExCallbacks;

// ============================================================================
// LED LOGIC
// ============================================================================
void refreshBankLEDs() {
  uint8_t sel = bank.getSelection();
  for (uint8_t i = 0; i < NUM_BANKS; i++) {
    digitalWrite(BANK_LED_PINS[i], (i == sel) ? HIGH : LOW);
  }
}

void updateButtonLeds() {
  uint8_t b = bank.getSelection();
  // Neopixel linked to specific button index
  auto &cfg = gConfig.buttons[b][NEOPIXEL_BTN_INDEX];
  uint32_t color = 0;
  bool isOn = false;

  // 1. Check if it's a Global Bank Button
  if (NEOPIXEL_BTN_INDEX == gConfig.bankIncBtn || NEOPIXEL_BTN_INDEX == gConfig.bankDecBtn) {
    isOn = true;
    uint8_t br = (gConfig.globalBr > 210) ? 210 : gConfig.globalBr;
    if (dynButtons[NEOPIXEL_BTN_INDEX].isPressed())
      color = pixel.Color(255, 255, 255);
    else
      color = pixel.Color(br, br, br);
  }
  // 2. Otherwise use the per-bank configuration
  else {
    bool alwaysOn = (cfg.flags >> 1) & 0x01;
    if (cfg.mode == ButtonMode::Latched) {
      isOn = (cfg.flags & 0x01);
    } else if (cfg.mode == ButtonMode::BankInc ||
               cfg.mode == ButtonMode::BankDec) {
      isOn = true;
      if (dynButtons[NEOPIXEL_BTN_INDEX].isPressed())
        color = pixel.Color(255, 255, 255);
      else
        color = pixel.Color((cfg.r * cfg.brightness) / 255,
                            (cfg.g * cfg.brightness) / 255,
                            (cfg.b * cfg.brightness) / 255);
    } else {
      isOn = dynButtons[NEOPIXEL_BTN_INDEX].isPressed() || alwaysOn;
    }
  }

  // 3. Finalize Color and State
  if (isOn) {
    if (color == 0) { // Fallback to base configuration color
      color = pixel.Color((cfg.r * cfg.brightness) / 255,
                          (cfg.g * cfg.brightness) / 255,
                          (cfg.b * cfg.brightness) / 255);
    }
    pixel.setPixelColor(0, color);
  } else {
    pixel.setPixelColor(0, 0); // Completely off
  }
  pixel.show();
}

// ============================================================================
// SETUP & LOOP
// ============================================================================
void setup() {
  loadConfigs();
  
  // Initialize pins: Buttons as Pull-Up to prevent floating states
  const uint8_t pins[] = {2, 3, 4, 5, 6, 7, 14, 15};
  for (uint8_t i = 0; i < 8; i++) {
    pinMode(pins[i], INPUT_PULLUP);
  }

  for (uint8_t i = 0; i < NUM_BANKS; i++)
    pinMode(BANK_LED_PINS[i], OUTPUT);
  pixel.begin();
  pixel.setPixelColor(0, pixel.Color(10, 10, 10));
  pixel.show();

  pioSerial.begin(31250); // HW MIDI on Pin 0 (TX)
  usbmidi.setCallbacks(sysExCallbacks);
  Control_Surface.begin();
  for (auto &b : dynButtons)
    b.begin();
}

void loop() {
  Control_Surface.loop();
  for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
    dynButtons[i].update();
  }
  refreshBankLEDs();
  updateButtonLeds();
  processPendingSave();

  // Pot Live Stream (Hangar View)
  static uint16_t lastP[2] = {0, 0};
  static unsigned long lastPotTime = 0;
  if (millis() - lastPotTime > 40) {
    uint16_t v0 = potentiometer1.getFilteredValue();
    uint16_t v1 = potentiometer2.getFilteredValue();
    if (abs((int)v0 - (int)lastP[0]) > 2 || abs((int)v1 - (int)lastP[1]) > 2) {
      uint8_t d[] = {(uint8_t)((v0 >> 7) & 127), (uint8_t)(v0 & 127),
                     (uint8_t)((v1 >> 7) & 127), (uint8_t)(v1 & 127)};
      sendSysExResponse(CMD_POT_LIVE, d, 4);
      lastP[0] = v0;
      lastP[1] = v1;
      lastPotTime = millis();
    }
  }
}
