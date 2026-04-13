export { };

declare global {
    interface Navigator {
        requestMIDIAccess(options?: WebMidi.MIDIOptions): Promise<WebMidi.MIDIAccess>;
    }
}

declare namespace WebMidi {
    interface MIDIOptions {
        sysex?: boolean;
        software?: boolean;
    }

    interface MIDIAccess extends EventTarget {
        inputs: MIDIInputMap;
        outputs: MIDIOutputMap;
        onstatechange: ((event: MIDIConnectionEvent) => void) | null;
        sysexEnabled: boolean;
    }

    interface MIDIInputMap extends Map<string, MIDIInput> { }
    interface MIDIOutputMap extends Map<string, MIDIOutput> { }

    interface MIDIPort extends EventTarget {
        id: string;
        manufacturer?: string;
        name?: string;
        type: "input" | "output";
        version?: string;
        state: "connected" | "disconnected";
        connection: "open" | "closed" | "pending";
        onstatechange: ((event: MIDIConnectionEvent) => void) | null;
        open(): Promise<MIDIPort>;
        close(): Promise<MIDIPort>;
    }

    interface MIDIInput extends MIDIPort {
        onmidimessage: ((event: MIDIMessageEvent) => void) | null;
    }

    interface MIDIOutput extends MIDIPort {
        send(data: number[] | Uint8Array, timestamp?: number): void;
        clear(): void;
    }

    interface MIDIMessageEvent extends Event {
        data: Uint8Array;
    }

    interface MIDIConnectionEvent extends Event {
        port: MIDIPort;
    }
}
