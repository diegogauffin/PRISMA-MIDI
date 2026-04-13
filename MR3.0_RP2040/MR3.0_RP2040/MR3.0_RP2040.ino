#include <Adafruit_NeoPixel.h>
#include <Control_Surface.h>
#include <EEPROM.h>

// ============================================================================
// CONFIGURACIÓN DE HARDWARE (PRISMA MR3.0 RP2040 DUAL MIDI)
// ============================================================================
#include <SerialPIO.h>

const uint8_t MODEL_FAMILY = 0; // MIDIROOTs
const uint8_t NUM_MAIN_BUTTONS = 4;
const uint8_t NUM_AUX_JACKS = 4;
const uint8_t N_ANALOGS = 2;
const uint8_t NUM_BANKS = 5;

const uint8_t TOTAL_BUTTONS = NUM_MAIN_BUTTONS + NUM_AUX_JACKS;

// PINES
constexpr uint8_t BUTTON_PINS[TOTAL_BUTTONS] = {2, 3, 4, 5, 6, 7, 8, 9};
constexpr uint8_t ANALOG_PINS[N_ANALOGS] = {A0, A1};

#define PIN_BTN_LEDS 10
#define PIN_BANK_LEDS 11
#define PIN_STATUS_LED 16

Adafruit_NeoPixel stripBtns(NUM_MAIN_BUTTONS, PIN_BTN_LEDS, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripBanks(NUM_BANKS, PIN_BANK_LEDS, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel statusLed(1, PIN_STATUS_LED, NEO_GRB + NEO_KHZ800);

// --- INTERFACES DUALES ---
// Instanciamos un Serial por PIO en el Pin 12 en lugar del viejo UART
SerialPIO pioSerial(12, NOPIN); // TX en Pin 12, Sin RX
USBMIDI_Interface usbmidi;
HardwareSerialMIDI_Interface serialmidi {pioSerial, MIDI_BAUD};


// --- SYSEX CONSTANTS ---
const uint8_t SYSEX_MAN_ID = 0x7D;
const uint8_t SYSEX_MODEL_ID = 0x01;
const uint8_t CMD_GET_CONFIG      = 0x01;
const uint8_t CMD_SET_CONFIG      = 0x02;
const uint8_t CMD_CONFIG_RESPONSE  = 0x03;
const uint8_t CMD_GET_INFO        = 0x04;
const uint8_t CMD_INFO_RESPONSE    = 0x05;
const uint8_t CMD_SET_ANALOG      = 0x06;
const uint8_t CMD_SET_GLOBAL      = 0x07;
const uint8_t CMD_GET_GLOBAL      = 0x08;
const uint8_t CMD_SET_BANK_BULK   = 0x09;
const uint8_t CMD_BTN_PRESS       = 0x0A;
const uint8_t CMD_POT_LIVE        = 0x0B;
const uint8_t CMD_CALIBRATE_POT   = 0x0C;
const uint8_t CMD_SET_BANK_LEDS   = 0x0D;

// ============================================================================
// CONFIGURACIÓN Y MEMORIA
// ============================================================================

enum class ButtonMode : uint8_t { CC = 0, Latched = 1, Note = 2, BankInc = 3, BankDec = 4 };

struct ButtonConfig {
    ButtonMode mode; uint8_t number; uint8_t channel;
    uint8_t r, g, b; uint8_t brightness; 
    uint8_t flags; // Bit 0: latchedState, Bit 1: ledAlwaysOn
};

struct AnalogConfig { uint16_t minValue; uint16_t maxValue; uint8_t number; uint8_t channel; };

struct Configuration {
  char magic[2] = {'P', 'X'}; 
  uint8_t deviceId = 0x01;
  uint8_t bankIncBtn = 255; uint8_t bankDecBtn = 255; uint8_t globalBr = 150; 
  uint8_t bankR = 255; uint8_t bankG = 0; uint8_t bankB = 255; uint8_t bankBr = 130;
  ButtonConfig buttons[NUM_BANKS][TOTAL_BUTTONS];
  AnalogConfig analogs[N_ANALOGS];
};

Configuration gConfig;

void initDefaultConfig() {
    gConfig.magic[0] = 'P'; gConfig.magic[1] = 'X'; gConfig.deviceId = 0x01;
    gConfig.bankIncBtn = 255; gConfig.bankDecBtn = 255; gConfig.globalBr = 150;
    for (uint8_t b = 0; b < NUM_BANKS; b++) {
        for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            gConfig.buttons[b][i] = {ButtonMode::CC, (uint8_t)(20+i), 7, 255, 255, 255, 255, 0};
        }
    }
    gConfig.analogs[0] = {0, 1023, 10, 7}; gConfig.analogs[1] = {0, 1023, 20, 7};
}

void loadConfigs() {
    EEPROM.get(0, gConfig);
    if (gConfig.magic[0] != 'P' || gConfig.magic[1] != 'X') {
        initDefaultConfig();
        EEPROM.put(0, gConfig); 
    }
    
    // Auto-fix para teclados que quedaron con CCs solapados por versiones anteriores
    if (gConfig.analogs[0].number == 10 && gConfig.analogs[1].number == 11) {
        gConfig.analogs[0].number = 10;
        gConfig.analogs[1].number = 20; // Separamos 10 CCs para evitar solapamiento entre bancos
        EEPROM.put(0, gConfig); 
    }
    
    gConfig.deviceId = 0x01;
}

void saveConfigs() { 
    static unsigned long lastSave = 0;
    if (millis() - lastSave > 1000) {
        EEPROM.put(0, gConfig);
        lastSave = millis();
    }
}

void refreshBankLEDs(bool force = false);
void updateButtonLeds();

// --- INTERFACES DUALES ---
// Los objetos usbmidi y serialmidi ahora estan inicializados al tope del script con PIO

Bank<NUM_BANKS> bank(NUM_BANKS);

// ============================================================================
// CLASE SMART POT (SOFT TAKEOVER)
// ============================================================================

template <uint8_t NumBanks>
class CCSmartPotentiometer {
public:
  CCSmartPotentiometer(BankConfig<NumBanks> bankConfig, pin_t pin, uint8_t index)
      : bank(bankConfig.bank), analog(pin), bankType(bankConfig.type), potIndex(index) {
    for (uint8_t i = 0; i < NumBanks; i++) {
        lastValue[i] = 0;
        isLocked[i] = false; // Inicia desbloqueado
        hasBeenTouched[i] = false;
    }
  }

  void begin() { }

  void update() {
    uint8_t currentBank = bank.getSelection();
    
    // Configuración inicial en el primer ciclo
    if (isFirstUpdate) {
        previousBank = currentBank;
        isFirstUpdate = false;
    } 
    // Al cambiar de banco, se bloquea SOLO si ya habíamos usado este banco antes
    else if (currentBank != previousBank) {
      if (hasBeenTouched[currentBank]) {
          isLocked[currentBank] = true;
      }
      previousBank = currentBank;
    }

    if (analog.update()) {
      uint16_t raw10 = analog.getValue(); // Valor estable de 10 bits (0-1023)
      
      uint16_t minV = gConfig.analogs[potIndex].minValue;
      uint16_t maxV = gConfig.analogs[potIndex].maxValue;
      
      // Auto-fix retro compatible
      if (maxV > 0 && maxV <= 128) { minV *= 8; maxV *= 8; }
      
      uint16_t constrained = constrain(raw10, minV, maxV);
      uint8_t currentMidiValue = map(constrained, minV, maxV, 0, 127);
      
      if (isLocked[currentBank]) {
        // Se desbloquea si cruza el último valor guardado (+/- 3 de tolerancia)
        if (abs((int)currentMidiValue - (int)lastValue[currentBank]) <= 3) {
          isLocked[currentBank] = false;
        }
      }

      if (!isLocked[currentBank]) {
        if (currentMidiValue != lastValue[currentBank] || !hasBeenTouched[currentBank]) {
          uint8_t baseCC = gConfig.analogs[potIndex].number;
          uint8_t cc = baseCC + (currentBank * 10);
          uint8_t ccChan = constrain((int)gConfig.analogs[potIndex].channel - 1, 0, 15);
          
          if (bankType == BankType::ChangeAddress) {
             // Eliminamos ChangeAddress para que el mapeo sea TOTALMENTE independiente por banco
             // ccBase = constrain(ccBase + currentBank, 0, 127); 
          }
          
          MIDIAddress addr = {cc, Channel(ccChan)};
          // Envio explícito DUAL (USB y Serial HW)
          usbmidi.sendControlChange(addr, currentMidiValue);
          serialmidi.sendControlChange(addr, currentMidiValue);
          
          lastValue[currentBank] = currentMidiValue;
          hasBeenTouched[currentBank] = true;
        }
      }
    }
  }

  uint16_t getFilteredValue() { return analog.getValue(); }

private:
  Bank<NumBanks> &bank;
  AH::FilteredAnalog<10, 3, uint16_t> analog; 
  BankType bankType;
  uint8_t potIndex;
  uint8_t lastValue[NumBanks];
  bool isLocked[NumBanks];
  bool hasBeenTouched[NumBanks];
  uint8_t previousBank = 255;
  bool isFirstUpdate = true;
};

using CCSmartPot = CCSmartPotentiometer<NUM_BANKS>;

CCSmartPot potentiometer1{ {bank, BankType::ChangeAddress}, ANALOG_PINS[0], 0 };
CCSmartPot potentiometer2{ {bank, BankType::ChangeAddress}, ANALOG_PINS[1], 1 };

// ============================================================================
// COMMUNICATION UTILITIES
// ============================================================================

static uint8_t sysexBuffer[256];

void sendSysExResponse(uint8_t cmd, const uint8_t* payload, size_t len) {
    if (len > 250) len = 250;
    sysexBuffer[0] = 0xF0;
    sysexBuffer[1] = SYSEX_MAN_ID;
    sysexBuffer[2] = gConfig.deviceId;
    sysexBuffer[3] = cmd;
    if (payload && len > 0) memcpy(&sysexBuffer[4], payload, len);
    sysexBuffer[4 + len] = 0xF7;
    
    // El frontend Hangar de configuración funciona única y exclusivamente por USB. 
    // Protegemos el puerto Serial hardware evitando mandar data inútil/gráfica de UI 
    // que ahogaría en latencia y ruido a los delays, sintes y pedaleras viejas.
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
      uint8_t feedbackData[] = { index_, (uint8_t)(state == AH::Button::Falling ? 1 : 0) };
      sendSysExResponse(CMD_BTN_PRESS, feedbackData, 2);
    }
    if (index_ == gConfig.bankIncBtn && state == AH::Button::Falling) { bank.select((b + 1) % NUM_BANKS); return; }
    if (index_ == gConfig.bankDecBtn && state == AH::Button::Falling) { bank.select(b == 0 ? NUM_BANKS - 1 : b - 1); return; }

    auto &cfg = gConfig.buttons[b][index_];
    if (state == AH::Button::Falling) {
      if (cfg.mode == ButtonMode::Latched) { 
          cfg.flags ^= 0x01; // Toggle latched bit
          sendValue((cfg.flags & 0x01) ? 127 : 0, b);
          // REMOVED: saveConfigs() to prevent EEPROM wear
      }
      else if (cfg.mode == ButtonMode::BankInc) bank.select((b + 1) % NUM_BANKS);
      else if (cfg.mode == ButtonMode::BankDec) bank.select(b == 0 ? NUM_BANKS - 1 : b - 1);
      else sendValue(127, b);
    } 
    else if (state == AH::Button::Rising) { 
        if (cfg.mode != ButtonMode::Latched && (uint8_t)cfg.mode < 3) sendValue(0, b);
    }
  }
  bool isPressed() { return button_.getState() == AH::Button::Pressed; }
private:
  void sendValue(uint8_t value, uint8_t currentBank) {
    auto &cfg = gConfig.buttons[currentBank][index_];
    const MIDIAddress addr = {cfg.number, Channel(constrain((int)cfg.channel-1,0,15))};
    if (cfg.mode == ButtonMode::Note) { 
        if (value > 0) {
            usbmidi.sendNoteOn(addr, 127);
            serialmidi.sendNoteOn(addr, 127);
        } else {
            usbmidi.sendNoteOff(addr, 0);
            serialmidi.sendNoteOff(addr, 0);
        }
    } else { 
        // Envio explícito DUAL
        usbmidi.sendControlChange(addr, value); 
        serialmidi.sendControlChange(addr, value);
    }
  }
  uint8_t index_; AH::Button button_;
};

DynamicButton dynButtons[TOTAL_BUTTONS] = { {0,2},{1,3},{2,4},{3,5},{4,6},{5,7},{6,8},{7,9} };

class MyMIDIInput : public MIDI_Callbacks {
public:
  void onSysExMessage(MIDI_Interface &midi_if, SysExMessage msg) override {
    if (msg.length >= 5 && msg.data[1] == SYSEX_MAN_ID && msg.data[2] == gConfig.deviceId) {
      uint8_t cmd = msg.data[3];
      if (cmd == CMD_GET_INFO) {
        const char* name = "PRISMA MR3.0 RP2040 DUAL MIDI"; uint8_t nLen = strlen(name);
        uint8_t d[40]; d[0] = 3; d[1] = 6; d[2] = 0; d[3] = MODEL_FAMILY; d[4] = NUM_MAIN_BUTTONS; d[5] = N_ANALOGS; d[6] = NUM_AUX_JACKS; d[7] = NUM_BANKS; d[8] = 1; d[9] = 1; d[10] = nLen;
        memcpy(&d[11], name, nLen); sendSysExResponse(CMD_INFO_RESPONSE, d, 11 + nLen);
      } else if (cmd == CMD_GET_CONFIG) {
        uint8_t requestedBank = (msg.length >= 6) ? msg.data[4] : 255;
        for (uint8_t b = 0; b < NUM_BANKS; b++) { 
          if (requestedBank != 255 && b != requestedBank) continue;
          for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            auto &cfg = gConfig.buttons[b][i];
            uint8_t d[] = { b, i, (uint8_t)cfg.mode, cfg.number, cfg.channel, (uint8_t)((cfg.r>>7)&1), (uint8_t)(cfg.r&127), (uint8_t)((cfg.g>>7)&1), (uint8_t)(cfg.g&127), (uint8_t)((cfg.b>>7)&1), (uint8_t)(cfg.b&127), (uint8_t)((cfg.brightness>>7)&1), (uint8_t)(cfg.brightness&127), cfg.flags };
            sendSysExResponse(CMD_CONFIG_RESPONSE, d, 14);
          }
        }
      } else if (cmd == CMD_SET_CONFIG && msg.length >= 18) {
        uint8_t b = msg.data[4]; uint8_t idx = msg.data[5];
        if (b < NUM_BANKS && idx < TOTAL_BUTTONS) {
            auto &cfg = gConfig.buttons[b][idx];
            cfg.mode = (ButtonMode)msg.data[6]; cfg.number = msg.data[7]; cfg.channel = constrain(msg.data[8], 1, 16);
            cfg.r = (msg.data[9]<<7)|msg.data[10]; cfg.g = (msg.data[11]<<7)|msg.data[12];
            cfg.b = (msg.data[13]<<7)|msg.data[14]; cfg.brightness = (msg.data[15]<<7)|msg.data[16];
            if (msg.length >= 19) cfg.flags = msg.data[17];
            saveConfigs(); updateButtonLeds();
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
        gConfig.bankIncBtn = msg.data[4]; gConfig.bankDecBtn = msg.data[5]; gConfig.globalBr = msg.data[6]; saveConfigs();
        updateButtonLeds();
      } else if (cmd == CMD_GET_GLOBAL) {
        uint16_t m0 = gConfig.analogs[0].minValue; uint16_t M0 = gConfig.analogs[0].maxValue;
        uint16_t m1 = gConfig.analogs[1].minValue; uint16_t M1 = gConfig.analogs[1].maxValue;
        
        uint8_t d[] = { 
            gConfig.bankIncBtn, gConfig.bankDecBtn, gConfig.globalBr, 
            (uint8_t)((gConfig.bankR>>7)&1), (uint8_t)(gConfig.bankR&127), 
            (uint8_t)((gConfig.bankG>>7)&1), (uint8_t)(gConfig.bankG&127), 
            (uint8_t)((gConfig.bankB>>7)&1), (uint8_t)(gConfig.bankB&127), 
            (uint8_t)((gConfig.bankBr>>7)&1), (uint8_t)(gConfig.bankBr&127),
            // Analog 0
            (uint8_t)((m0>>7)&127), (uint8_t)(m0&127), (uint8_t)((M0>>7)&127), (uint8_t)(M0&127), gConfig.analogs[0].number, gConfig.analogs[0].channel,
            // Analog 1
            (uint8_t)((m1>>7)&127), (uint8_t)(m1&127), (uint8_t)((M1>>7)&127), (uint8_t)(M1&127), gConfig.analogs[1].number, gConfig.analogs[1].channel
        };
        sendSysExResponse(CMD_GET_GLOBAL, d, sizeof(d));
      } else if (cmd == CMD_CALIBRATE_POT && msg.length >= 6) {
        uint8_t idx = msg.data[4]; uint8_t type = msg.data[5];
        if (idx < N_ANALOGS) {
          uint16_t cur = (idx == 0) ? potentiometer1.getFilteredValue() : potentiometer2.getFilteredValue();
          uint16_t UI = cur; // Raw ADC (0-1023)
          if (type == 0) gConfig.analogs[idx].minValue = (UI > 1020) ? 1023 : UI + 3; 
          else gConfig.analogs[idx].maxValue = (UI < 3) ? 0 : UI - 3;
          saveConfigs(); 
          uint16_t adjusted = (type == 0) ? gConfig.analogs[idx].minValue : gConfig.analogs[idx].maxValue;
          uint8_t d[] = { idx, type, (uint8_t)((adjusted>>7)&127), (uint8_t)(adjusted&127) };
          sendSysExResponse(CMD_CALIBRATE_POT, d, 4);
        }
      } else if (cmd == CMD_SET_BANK_LEDS && msg.length >= 12) {
        gConfig.bankR = (msg.data[4]<<7)|msg.data[5]; gConfig.bankG = (msg.data[6]<<7)|msg.data[7];
        gConfig.bankB = (msg.data[8]<<7)|msg.data[9]; gConfig.bankBr = (msg.data[10]<<7)|msg.data[11];
        saveConfigs(); refreshBankLEDs(true);
      } else if (cmd == CMD_SET_BANK_BULK) {
        uint8_t b = msg.data[4]; 
        uint8_t bytesPerBtn = (msg.length >= (5 + (TOTAL_BUTTONS * 12))) ? 12 : 11;
        if (b < NUM_BANKS) {
          for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
            uint16_t base = 5 + (i * bytesPerBtn); auto &cfg = gConfig.buttons[b][i];
            cfg.mode = (ButtonMode)msg.data[base]; cfg.number = msg.data[base+1]; cfg.channel = constrain(msg.data[base+2], 1, 16);
            cfg.r = (msg.data[base+3]<<7)|msg.data[base+4]; cfg.g = (msg.data[base+5]<<7)|msg.data[base+6];
            cfg.b = (msg.data[base+7]<<7)|msg.data[base+8]; cfg.brightness = (msg.data[base+9]<<7)|msg.data[base+10]; 
            if (bytesPerBtn == 12) cfg.flags = msg.data[base+11];
          }
          saveConfigs(); updateButtonLeds();
        }
      }
    }
  }
};

 MyMIDIInput sysExCallbacks;

void refreshBankLEDs(bool force) {
  uint8_t sel = bank.getSelection(); static uint8_t lastSel = 255;
  if(!force && sel == lastSel) return;
  for(uint8_t i = 0; i < NUM_BANKS; i++) {
    if (i == sel) { stripBanks.setPixelColor(i, stripBanks.Color((gConfig.bankR*gConfig.bankBr)/255, (gConfig.bankG*gConfig.bankBr)/255, (gConfig.bankB*gConfig.bankBr)/255)); }
    else { stripBanks.setPixelColor(i, 0); }
  }
  stripBanks.show(); lastSel = sel;
}

void updateButtonLeds() {
    uint8_t b = bank.getSelection();
    for(uint8_t i = 0; i < NUM_MAIN_BUTTONS; i++) {
        auto &cfg = gConfig.buttons[b][i];
        uint32_t color = 0;
        bool isOn = false;
        bool isGlobal = (i == gConfig.bankIncBtn || i == gConfig.bankDecBtn) && (i < TOTAL_BUTTONS);

        if (isGlobal) {
            isOn = true;
            uint8_t br = (gConfig.globalBr > 210) ? 210 : gConfig.globalBr; 
            if (dynButtons[i].isPressed()) color = stripBtns.Color(255, 255, 255);
            else color = stripBtns.Color(br, br, br);
        } else {
            bool alwaysOn = (cfg.flags >> 1) & 0x01;
            if (cfg.mode == ButtonMode::Latched) {
                isOn = (cfg.flags & 0x01);
            } else if (cfg.mode == ButtonMode::BankInc || cfg.mode == ButtonMode::BankDec) {
                isOn = true;
                if (dynButtons[i].isPressed()) color = stripBtns.Color(255, 255, 255);
                else color = stripBtns.Color((cfg.r*cfg.brightness)/255, (cfg.g*cfg.brightness)/255, (cfg.b*cfg.brightness)/255);
            } else {
                isOn = dynButtons[i].isPressed() || alwaysOn;
            }
        }

        if (isOn) {
            if (color == 0) { // Fallback to base configuration color
                color = stripBtns.Color((cfg.r*cfg.brightness)/255, (cfg.g*cfg.brightness)/255, (cfg.b*cfg.brightness)/255);
            }
            stripBtns.setPixelColor(i, color);
        } else {
            stripBtns.setPixelColor(i, 0); // Completely off
        }
    }
    stripBtns.show();
}

uint32_t colorWheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) return statusLed.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  if(WheelPos < 170) { WheelPos -= 85; return statusLed.Color(0, WheelPos * 3, 255 - WheelPos * 3); }
  WheelPos -= 170; return statusLed.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

void animateRainbow() {
  const int duration = 3000; const int steps = 256; const int delayTime = duration / steps;
  for (int i = 0; i < steps; ++i) { statusLed.setPixelColor(0, colorWheel(i)); statusLed.show(); delay(delayTime); }
}

void setup() {
    loadConfigs(); AH::Button::setDebounceTime(25);
    stripBtns.begin(); stripBanks.begin(); statusLed.begin(); statusLed.setBrightness(127); animateRainbow(); 
    statusLed.setPixelColor(0, statusLed.Color(5, 5, 5)); statusLed.show();
    
    // RP2040 UART HARD-BINDING
    // Inicializar pines de botones con Pull-Up para evitar estados flotantes
    for (uint8_t i = 0; i < TOTAL_BUTTONS; i++) {
        pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    }

    Serial1.begin(31250); // Mantenemos solo la apertur directa de la capa base
    
    // Conectar Callbacks a ambas interfaces
    usbmidi.setCallbacks(sysExCallbacks);
    serialmidi.setCallbacks(sysExCallbacks);
    
    Control_Surface.begin();
    for (auto &b : dynButtons) b.begin();
}

void loop() {
    Control_Surface.loop();
    for (auto &b : dynButtons) b.update();
    potentiometer1.update();
    potentiometer2.update();
    updateButtonLeds(); refreshBankLEDs();

    // Pot Live Stream
    static uint16_t lastP[2] = {0,0};
    static unsigned long lastPotTime = 0;
    if (millis() - lastPotTime > 30) {
        uint16_t v0 = potentiometer1.getFilteredValue(); uint16_t v1 = potentiometer2.getFilteredValue();
        if (abs((int)v0-(int)lastP[0]) > 2 || abs((int)v1-(int)lastP[1]) > 2) {
            uint8_t d[] = { (uint8_t)((v0>>7)&127), (uint8_t)(v0&127), (uint8_t)((v1>>7)&127), (uint8_t)(v1&127) };
             sendSysExResponse(CMD_POT_LIVE, d, 4); lastP[0] = v0; lastP[1] = v1;
            lastPotTime = millis();
        }
    }
}
