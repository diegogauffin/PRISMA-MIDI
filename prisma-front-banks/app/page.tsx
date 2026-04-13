"use client";

import { useEffect, useState, useCallback } from "react";

// SysEx Constants
const SYSEX_START = 0xF0;
const SYSEX_END = 0xF7;
const MAN_ID = 0x7D;
const MODEL_ID = 0x01;
const CMD_GET_CONFIG = 0x01;
const CMD_SET_CONFIG = 0x02;
const CMD_CONFIG_RESPONSE = 0x03;
const CMD_GET_INFO = 0x04;
const CMD_INFO_RESPONSE = 0x05;
const CMD_POT_CONFIG_RESPONSE = 0x06;
const CMD_SET_POT_CONFIG = 0x07;

interface ButtonConfig {
  id: number; // 0, 1, 2
  mode: number; // 0: CC, 1: Latched, 2: Note
  value: number;
  channel: number;
  r: number;
  g: number;
  b: number;
  brightness: number;
}

interface PotConfig {
  id: number;
  value: number;
  channel: number;
  minRaw: number;
  maxRaw: number;
}

interface DeviceInfo {
  name: string;
  version: string;
  buttonCount: number;
  potCount: number;
}

export default function Home() {
  const [midiAccess, setMidiAccess] = useState<WebMidi.MIDIAccess | null>(null);
  const [input, setInput] = useState<WebMidi.MIDIInput | null>(null);
  const [output, setOutput] = useState<WebMidi.MIDIOutput | null>(null);
  const [status, setStatus] = useState("Initializing...");
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo | null>(null);
  const [configs, setConfigs] = useState<ButtonConfig[]>([
    { id: 0, mode: 0, value: 0, channel: 11, r: 0, g: 0, b: 255, brightness: 255 },
    { id: 1, mode: 1, value: 0, channel: 11, r: 0, g: 255, b: 0, brightness: 255 },
    { id: 2, mode: 1, value: 0, channel: 11, r: 255, g: 0, b: 0, brightness: 255 },
  ]);
  const [potConfigs, setPotConfigs] = useState<PotConfig[]>([
    { id: 0, value: 16, channel: 11, minRaw: 6000, maxRaw: 10000 },
    { id: 1, value: 17, channel: 11, minRaw: 6000, maxRaw: 10000 },
  ]);

  // Palette State
  const [palette, setPalette] = useState([
    { r: 255, g: 0, b: 0, name: "Red" },
    { r: 0, g: 255, b: 0, name: "Green" },
    { r: 0, g: 0, b: 255, name: "Blue" },
    { r: 255, g: 255, b: 0, name: "Yellow" },
    { r: 0, g: 255, b: 255, name: "Cyan" },
    { r: 255, g: 0, b: 255, name: "Magenta" },
    { r: 255, g: 128, b: 0, name: "Orange" },
    { r: 128, g: 0, b: 255, name: "Purple" },
    { r: 255, g: 255, b: 255, name: "White" },
    { r: 0, g: 0, b: 0, name: "Off" },
  ]);
  const [isEditingPalette, setIsEditingPalette] = useState(false);

  // Helpers
  const rgbToHex = (r: number, g: number, b: number) => {
    return "#" + ((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1);
  };

  const hexToRgb = (hex: string) => {
    const result = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
    return result ? {
      r: parseInt(result[1], 16),
      g: parseInt(result[2], 16),
      b: parseInt(result[3], 16)
    } : { r: 0, g: 0, b: 0 };
  };

  const updatePalette = (index: number, hex: string) => {
    const rgb = hexToRgb(hex);
    setPalette(prev => {
      const newPalette = [...prev];
      newPalette[index] = { ...newPalette[index], ...rgb };
      return newPalette;
    });
  };

  // Note Conversion Helpers
  const NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

  const midiToNote = (midi: number) => {
    if (midi < 0 || midi > 127) return "Invalid";
    const note = NOTES[midi % 12];
    const octave = Math.floor(midi / 12) - 1;
    return `${note}${octave}`;
  };

  const noteToMidi = (noteStr: string) => {
    const match = noteStr.toUpperCase().match(/^([A-G][#B]?)(-?[0-9]+)$/);
    if (!match) return -1;

    let note = match[1];
    const octave = parseInt(match[2]);

    // Handle flats
    if (note.endsWith("B")) {
      const base = note.charAt(0);
      const baseIdx = NOTES.indexOf(base);
      // Simple flat handling: Cb -> B, Db -> C#, etc.
      // For now let's just assume standard sharps from the array or explicit flats if we want to be fancy.
      // But simpler: just map flats to the previous semitone.
      // Actually, let's stick to the requested format (C4, E#6). 
      // If user types Eb, we can handle it or just stick to sharps as canonical.
      // Let's support basic flats:
      const flatMap: { [key: string]: string } = { "DB": "C#", "EB": "D#", "GB": "F#", "AB": "G#", "BB": "A#" };
      if (flatMap[note]) note = flatMap[note];
    }

    const noteIdx = NOTES.indexOf(note);
    if (noteIdx === -1) return -1;

    const midi = (octave + 1) * 12 + noteIdx;
    return (midi >= 0 && midi <= 127) ? midi : -1;
  };

  const handleMidiMessage = useCallback((event: WebMidi.MIDIMessageEvent) => {
    const data = event.data;
    if (data[0] === SYSEX_START && data[data.length - 1] === SYSEX_END) {
      // Check ID
      if (data[1] === MAN_ID && data[2] === MODEL_ID) {
        if (data[3] === CMD_CONFIG_RESPONSE) {
          // Parse Config
          // F0 7D 01 03 [Idx] [Mode] [Val] [Ch] [R_H] [R_L] [G_H] [G_L] [B_H] [B_L] F7
          const idx = data[4];
          const mode = data[5];
          const value = data[6];
          const channel = data[7];

          // Decode 7-bit split RGB
          const r = (data[8] << 7) | (data[9] & 0x7F);
          const g = (data[10] << 7) | (data[11] & 0x7F);
          const b = (data[12] << 7) | (data[13] & 0x7F);

          // Decode 7-bit split Brightness
          let brightness = 255;
          if (data.length > 15) {
            brightness = (data[14] << 7) | (data[15] & 0x7F);
          }

          setConfigs((prev) => {
            const newConfigs = [...prev];
            // Ensure array is large enough
            while (newConfigs.length <= idx) {
              newConfigs.push({ id: newConfigs.length, mode: 0, value: 0, channel: 1, r: 0, g: 0, b: 0, brightness: 255 });
            }
            newConfigs[idx] = { id: idx, mode, value, channel, r, g, b, brightness };
            return newConfigs;
          });
        } else if (data[3] === CMD_POT_CONFIG_RESPONSE) {
          // F0 7D 01 06 [Idx] [Val] [Ch] [Min_H] [Min_L] [Max_H] [Max_L] F7
          const idx = data[4];
          const value = data[5];
          const channel = data[6];
          const minRaw = (data[7] << 7) | (data[8] & 0x7F);
          const maxRaw = (data[9] << 7) | (data[10] & 0x7F);

          setPotConfigs((prev) => {
            const newPots = [...prev];
            while (newPots.length <= idx) {
              newPots.push({ id: newPots.length, value: 0, channel: 1, minRaw: 0, maxRaw: 16383 });
            }
            newPots[idx] = { id: idx, value, channel, minRaw, maxRaw };
            return newPots;
          });
        } else if (data[3] === CMD_INFO_RESPONSE) {
          // F0 7D 01 05 [Major] [Minor] [Patch] [ButtonCount] [PotCount] [NameLen] [NameBytes...] F7
          const major = data[4];
          const minor = data[5];
          const patch = data[6];
          const buttonCount = data[7];
          const potCount = data[8];
          const nameLen = data[9];

          let name = "";
          for (let i = 0; i < nameLen; i++) {
            name += String.fromCharCode(data[10 + i]);
          }

          setDeviceInfo({
            name: name,
            version: `${major}.${minor}.${patch}`,
            buttonCount,
            potCount
          });
        }
      }
    }
  }, []);

  useEffect(() => {
    if (navigator.requestMIDIAccess) {
      navigator.requestMIDIAccess({ sysex: true }).then(
        (access) => {
          setMidiAccess(access);
          setStatus("MIDI Access Granted");

          // Auto-connect to first available input/output if possible
          const inputs = Array.from(access.inputs.values());
          const outputs = Array.from(access.outputs.values());

          console.log("Inputs:", inputs);
          console.log("Outputs:", outputs);

          if (inputs.length > 0 && outputs.length > 0) {

            // Simple heuristic: connect to the first one
            // In a real app, we'd let user choose
            const firstInput = inputs[0];
            const firstOutput = outputs[0];

            setInput(firstInput);
            setOutput(firstOutput);
            setStatus(`Connected to ${firstInput.name}`);

            firstInput.onmidimessage = handleMidiMessage;

            // Auto-fetch config and info
            // Send GET_CONFIG: F0 7D 01 01 F7
            firstOutput.send([SYSEX_START, MAN_ID, MODEL_ID, CMD_GET_CONFIG, SYSEX_END]);
            // Send GET_INFO: F0 7D 01 04 F7
            firstOutput.send([SYSEX_START, MAN_ID, MODEL_ID, CMD_GET_INFO, SYSEX_END]);
          }
        },
        () => setStatus("MIDI Access Denied")
      );
    } else {
      setStatus("Web MIDI not supported");
    }
  }, [handleMidiMessage]);

  const refreshConfig = () => {

    console.log("Refreshing config...");
    if (output) {
      // Send GET_CONFIG: F0 7D 01 01 F7
      output.send([SYSEX_START, MAN_ID, MODEL_ID, CMD_GET_CONFIG, SYSEX_END]);
    }
  };

  const saveConfig = (config: ButtonConfig) => {

    console.log("Saving config...", config);
    if (output) {
      // Send SET_CONFIG: F0 7D 01 02 [Idx] [Mode] [Val] [Ch] [R_H] [R_L] [G_H] [G_L] [B_H] [B_L] F7
      output.send([
        SYSEX_START,
        MAN_ID,
        MODEL_ID,
        CMD_SET_CONFIG,
        config.id,
        config.mode,
        config.value,
        config.channel,
        (config.r >> 7) & 0x01, config.r & 0x7F, // R split
        (config.g >> 7) & 0x01, config.g & 0x7F, // G split
        (config.b >> 7) & 0x01, config.b & 0x7F, // B split
        (config.brightness >> 7) & 0x01, config.brightness & 0x7F, // Brightness split
        SYSEX_END
      ]);
    }
  };

  const updateConfig = (index: number, field: keyof ButtonConfig, value: number) => {
    setConfigs((prev) => {
      const newConfigs = [...prev];
      newConfigs[index] = { ...newConfigs[index], [field]: value };
      return newConfigs;
    });
  };

  const savePotConfig = (config: PotConfig) => {
    console.log("Saving pot config...", config);
    if (output) {
      // F0 7D 01 07 [Idx] [Val] [Ch] [Min_H] [Min_L] [Max_H] [Max_L] F7
      output.send([
        SYSEX_START, MAN_ID, MODEL_ID, CMD_SET_POT_CONFIG,
        config.id, config.value, config.channel,
        (config.minRaw >> 7) & 0x7F, config.minRaw & 0x7F,
        (config.maxRaw >> 7) & 0x7F, config.maxRaw & 0x7F,
        SYSEX_END
      ]);
    }
  };

  const updatePotConfig = (index: number, field: keyof PotConfig, value: number) => {
    setPotConfigs((prev) => {
      const newPots = [...prev];
      newPots[index] = { ...newPots[index], [field]: value };
      return newPots;
    });
  };

  return (
    <div className="min-h-screen bg-gray-900 text-white p-8 font-sans">
      <h1 className="text-3xl font-bold mb-2 text-center text-transparent bg-clip-text bg-gradient-to-r from-purple-400 to-pink-600">
        {deviceInfo ? `${deviceInfo.name} Configurator` : "Prisma Pedal Configurator"}
      </h1>
      {deviceInfo && (
        <p className="text-center text-gray-400 text-sm mb-6">
          Firmware v{deviceInfo.version}
        </p>
      )}

      <div className="max-w-4xl mx-auto">
        <div className="mb-6 flex justify-between items-center bg-gray-800 p-4 rounded-lg">
          <span className="text-gray-300">Status: <span className="font-semibold text-white">{status}</span></span>
          <button
            onClick={refreshConfig}
            className="px-4 py-2 bg-blue-600 hover:bg-blue-500 rounded text-sm font-medium transition-colors"
            disabled={!output}
          >
            Refresh Config
          </button>
        </div>

        <div className="grid grid-cols-1 md:grid-cols-3 gap-6">
          {[...configs].reverse().map((config) => (
            <div key={config.id} className="bg-gray-800 rounded-xl p-6 border border-gray-700 shadow-lg">
              <h2 className="text-xl font-semibold mb-4 text-gray-200">Button {config.id === 0 ? "5" : config.id === 1 ? "4" : "3"}</h2>

              <div className="space-y-4">
                <div>
                  <label className="block text-xs text-gray-400 mb-1">Mode</label>
                  <select
                    value={config.mode}
                    onChange={(e) => updateConfig(config.id, "mode", parseInt(e.target.value))}
                    className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-purple-500"
                  >
                    <option value={0}>CC Momentary</option>
                    <option value={1}>CC Latched</option>
                    <option value={2}>Note</option>
                  </select>
                </div>

                <div>
                  <label className="block text-xs text-gray-400 mb-1">
                    {config.mode === 2 ? "Note (e.g. C4)" : "Value (CC)"}
                  </label>
                  {config.mode === 2 ? (
                    <div className="flex gap-2">
                      <select
                        value={config.value % 12}
                        onChange={(e) => {
                          const newNoteIdx = parseInt(e.target.value);
                          const currentOctave = Math.floor(config.value / 12) - 2; // Adjusted offset
                          let newMidi = (currentOctave + 2) * 12 + newNoteIdx; // Adjusted offset
                          // Clamp to valid MIDI range
                          if (newMidi < 0) newMidi = 0;
                          if (newMidi > 127) newMidi = 127;
                          updateConfig(config.id, "value", newMidi);
                        }}
                        className="w-1/2 bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-purple-500"
                      >
                        {NOTES.map((note, idx) => (
                          <option key={note} value={idx}>{note}</option>
                        ))}
                      </select>
                      <select
                        value={Math.floor(config.value / 12) - 2} // Adjusted offset
                        onChange={(e) => {
                          const newOctave = parseInt(e.target.value);
                          const currentNoteIdx = config.value % 12;
                          let newMidi = (newOctave + 2) * 12 + currentNoteIdx; // Adjusted offset
                          // Clamp to valid MIDI range
                          if (newMidi < 0) newMidi = 0;
                          if (newMidi > 127) newMidi = 127;
                          updateConfig(config.id, "value", newMidi);
                        }}
                        className="w-1/2 bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-purple-500"
                      >
                        {Array.from({ length: 11 }, (_, k) => k - 1).map((oct) => (
                          <option key={oct} value={oct}>Oct {oct}</option>
                        ))}
                      </select>
                    </div>
                  ) : (
                    <input
                      type="number"
                      value={config.value}
                      onChange={(e) => updateConfig(config.id, "value", parseInt(e.target.value))}
                      className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-purple-500"
                    />
                  )}
                </div>

                <div>
                  <label className="block text-xs text-gray-400 mb-1">Channel</label>
                  <input
                    type="number"
                    value={config.channel}
                    onChange={(e) => updateConfig(config.id, "channel", parseInt(e.target.value))}
                    className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-purple-500"
                  />
                </div>

                <div>
                  <div className="flex justify-between items-center mb-1">
                    <label className="block text-xs text-gray-400">Color</label>
                    <button
                      onClick={() => setIsEditingPalette(!isEditingPalette)}
                      className="text-xs text-blue-400 hover:text-blue-300"
                    >
                      {isEditingPalette ? "Done" : "Edit Palette"}
                    </button>
                  </div>

                  <div className="grid grid-cols-5 gap-2 mb-2">
                    {palette.map((color, idx) => (
                      isEditingPalette ? (
                        <input
                          key={idx}
                          type="color"
                          value={rgbToHex(color.r, color.g, color.b)}
                          onChange={(e) => updatePalette(idx, e.target.value)}
                          className="w-8 h-8 p-0 border-0 rounded-full overflow-hidden cursor-pointer"
                        />
                      ) : (
                        <button
                          key={idx}
                          onClick={() => {
                            updateConfig(config.id, "r", color.r);
                            updateConfig(config.id, "g", color.g);
                            updateConfig(config.id, "b", color.b);
                          }}
                          className={`w-8 h-8 rounded-full border-2 transition-transform hover:scale-110 ${config.r === color.r && config.g === color.g && config.b === color.b
                            ? "border-white scale-110 ring-2 ring-purple-500"
                            : "border-transparent"
                            }`}
                          style={{ backgroundColor: `rgb(${color.r}, ${color.g}, ${color.b})` }}
                          title={color.name}
                        />
                      )
                    ))}
                  </div>
                  <div className="text-xs text-gray-500 text-center">
                    Current: RGB({config.r}, {config.g}, {config.b})
                  </div>
                </div>

                <div>
                  <label className="block text-xs text-gray-400 mb-1">Brightness: {config.brightness}</label>
                  <input
                    type="range"
                    min="0"
                    max="255"
                    value={config.brightness}
                    onChange={(e) => updateConfig(config.id, "brightness", parseInt(e.target.value))}
                    className="w-full h-2 bg-gray-700 rounded-lg appearance-none cursor-pointer"
                  />
                </div>

                <button
                  onClick={() => saveConfig(config)}
                  className="w-full mt-4 py-2 bg-purple-600 hover:bg-purple-500 rounded text-sm font-medium transition-colors"
                >
                  Save to Pedal
                </button>
              </div>
            </div>
          ))}
        </div>

        {potConfigs.length > 0 && (
          <div className="mt-12">
            <h2 className="text-2xl font-bold mb-6 text-gray-100 flex items-center">
              <span className="bg-blue-600 w-2 h-8 mr-3 rounded-full"></span>
              Potentiometers
            </h2>
            <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
              {potConfigs.map((pot) => (
                <div key={pot.id} className="bg-gray-800 rounded-xl p-6 border border-gray-700 shadow-lg relative overflow-hidden">
                  <div className="absolute top-0 right-0 p-4 opacity-10">
                    <svg width="40" height="40" viewBox="0 0 24 24" fill="currentColor"><path d="M12,2A10,10 0 0,0 2,12A10,10 0 0,0 12,22A10,10 0 0,0 22,12A10,10 0 0,0 12,2M12,4A8,8 0 0,1 20,12A8,8 0 0,1 12,20A8,8 0 0,1 4,12A8,8 0 0,1 12,4M12,6A6,6 0 0,0 6,12A6,6 0 0,0 12,18A6,6 0 0,0 18,12A6,6 0 0,0 12,6M12,8A4,4 0 0,1 16,12A4,4 0 0,1 12,16A4,4 0 0,1 8,12A4,4 0 0,1 12,8Z" /></svg>
                  </div>
                  <h3 className="text-xl font-semibold mb-4 text-gray-200">Potentiometer {pot.id + 1}</h3>
                  <div className="space-y-4">
                    <div className="grid grid-cols-2 gap-4">
                      <div>
                        <label className="block text-xs text-gray-400 mb-1">CC Value</label>
                        <input
                          type="number"
                          value={pot.value}
                          min="0"
                          max="127"
                          onChange={(e) => updatePotConfig(pot.id, "value", parseInt(e.target.value))}
                          className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-blue-500"
                        />
                      </div>
                      <div>
                        <label className="block text-xs text-gray-400 mb-1">Channel</label>
                        <input
                          type="number"
                          value={pot.channel}
                          min="1"
                          max="16"
                          onChange={(e) => updatePotConfig(pot.id, "channel", parseInt(e.target.value))}
                          className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-blue-500"
                        />
                      </div>
                    </div>

                    <div className="pt-2 border-t border-gray-700">
                      <span className="text-xs font-semibold text-gray-500 uppercase tracking-wider">Mapping Parameters</span>
                      <div className="grid grid-cols-2 gap-4 mt-2">
                        <div>
                          <label className="block text-xs text-gray-400 mb-1">Min Raw</label>
                          <input
                            type="number"
                            value={pot.minRaw}
                            min="0"
                            max="16383"
                            onChange={(e) => updatePotConfig(pot.id, "minRaw", parseInt(e.target.value))}
                            className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-blue-500"
                          />
                        </div>
                        <div>
                          <label className="block text-xs text-gray-400 mb-1">Max Raw</label>
                          <input
                            type="number"
                            value={pot.maxRaw}
                            min="0"
                            max="16383"
                            onChange={(e) => updatePotConfig(pot.id, "maxRaw", parseInt(e.target.value))}
                            className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm focus:outline-none focus:border-blue-500"
                          />
                        </div>
                      </div>
                    </div>

                    <button
                      onClick={() => savePotConfig(pot)}
                      className="w-full mt-4 py-2 bg-blue-600 hover:bg-blue-500 rounded text-sm font-medium transition-colors"
                    >
                      Save Pot Settings
                    </button>
                  </div>
                </div>
              ))}
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
