// BLE MIDI Central Bridge — SuperMini nRF52840 
//
// https://github.com/sneak-thief/BLE-MIDI-Host-nrf52840/
//
// Scans for and connects to a BLE MIDI peripheral, pairs with it, then forwards
// all received MIDI to:
//   - USB MIDI (class-compliant, native nRF52840 USB)
//   - Serial MIDI out on D1 (TX) -> optocoupler -> DIN-5 or TRS midi
//   - Serial MIDI in on D0 (RX) -> 220 Ohm resistor -> DIN-5 or TRS midi
//
// See README.md for board setup and how to target other nRF52840 boards.

#include <bluefruit.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>

// ---- Configuration ---------------------------------------------------------
#define SERIAL_MIDI_PORT        Serial1     // SuperMini: D0 = TX, D1 = RX
#define SERIAL_MIDI_BAUD        31250

// Use the board variant's own LED constant so the right pin is always used
// regardless of the core's pin renumbering. On the SuperMini nRF52840 the USR
// LED (P0.15) is LED_BUILTIN = Arduino pin 22, and it is ACTIVE-HIGH
// (the variant defines LED_STATE_ON = 1).
//
// Ignore the blue blinking LED on the SuperMini/ProMicro - it's just the battery
// charging status
//
#define LED_STATUS              LED_BUILTIN
#define LED_ACTIVE_LOW          0           // SuperMini USR LED: HIGH = on
// ----------------------------------------------------------------------------

// Standard BLE MIDI UUIDs, stored little-endian for Bluefruit (reversed bytes).
// Service:        03b80e5a-ede8-4b33-a751-6ce34ec4c700
// Characteristic: 7772e5db-3868-4112-a1a9-f2669d106bf3
static const uint8_t BLEMIDI_SERVICE_UUID[16] = {
    0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7,
    0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03
};
static const uint8_t BLEMIDI_CHAR_UUID[16] = {
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1,
    0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77
};

BLEClientService        midiService(BLEMIDI_SERVICE_UUID);
BLEClientCharacteristic midiChar(BLEMIDI_CHAR_UUID);

Adafruit_USBD_MIDI usb_midi;

static inline void ledOn()  { digitalWrite(LED_STATUS, LED_ACTIVE_LOW ? LOW  : HIGH); }
static inline void ledOff() { digitalWrite(LED_STATUS, LED_ACTIVE_LOW ? HIGH : LOW ); }

static inline uint8_t msgLen(uint8_t status) {
    if (status < 0x80) return 0;
    if (status < 0xC0) return 3;
    if (status < 0xE0) return 2;
    if (status < 0xF0) return 3;
    switch (status) {
        case 0xF1: case 0xF3: return 2;
        case 0xF2:            return 3;
        case 0xF6: case 0xF8: case 0xFA: case 0xFB:
        case 0xFC: case 0xFE: case 0xFF: return 1;
        default:              return 0;
    }
}

static void sendOut(uint8_t status, uint8_t d1, uint8_t d2, uint8_t len) {
    uint8_t cin = status >> 4;
    if (status >= 0xF8) cin = 0x0F;
    uint8_t packet[4] = { (uint8_t)cin, status, d1, (uint8_t)(len >= 3 ? d2 : 0) };
    usb_midi.writePacket(packet);

    SERIAL_MIDI_PORT.write(status);
    if (len >= 2) SERIAL_MIDI_PORT.write(d1);
    if (len >= 3) SERIAL_MIDI_PORT.write(d2);
}

// Parse a BLE MIDI packet (header + timestamped MIDI, with running status).
static void parseBleMidi(const uint8_t* data, uint16_t length) {
    if (length < 3 || !(data[0] & 0x80)) return;

    uint16_t i = 1;
    uint8_t  runningStatus = 0;

    while (i < length) {
        if (data[i] & 0x80) {            // timestamp byte
            i++;
            if (i >= length) break;
        }
        uint8_t b = data[i];

        if (b == 0xF0) {                 // SysEx — skip to F7
            while (i < length && data[i] != 0xF7) i++;
            i++;
            runningStatus = 0;
            continue;
        }

        if (b & 0x80) { runningStatus = b; i++; }
        if (runningStatus == 0) { i++; continue; }

        uint8_t len = msgLen(runningStatus);
        if (len == 0) { i++; continue; }

        uint8_t d1 = 0, d2 = 0;
        if (len >= 2 && i < length) d1 = data[i++];
        if (len >= 3 && i < length) d2 = data[i++];

        sendOut(runningStatus, d1, d2, len);
    }
}

void midi_notify_callback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    parseBleMidi(data, len);
}

void scan_callback(ble_gap_evt_adv_report_t* report) {
    Bluefruit.Central.connect(report);
}

void connect_callback(uint16_t conn_handle) {
    if (!midiService.discover(conn_handle)) {
        Bluefruit.disconnect(conn_handle);
        return;
    }
    if (!midiChar.discover()) {
        Bluefruit.disconnect(conn_handle);
        return;
    }

    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    conn->requestConnectionParameter(12);   // 12 * 1.25ms = 15ms
    conn->requestDataLengthUpdate();
    conn->requestMtuExchange(247);

    // BLE MIDI peripherals require an authenticated (paired) link before they
    // will accept the notification subscription. Pair now; notifications are
    // enabled in connection_secured_callback() once the link is encrypted.
    conn->requestPairing();
}

// Runs once the link is encrypted/authenticated; now enable notifications.
void connection_secured_callback(uint16_t conn_handle) {
    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    if (!conn->secured()) {
        conn->requestPairing();
        return;
    }
    midiChar.enableNotify();
    ledOn();
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
    (void) conn_handle;
    (void) reason;
    ledOff();
}

void setup() {
    pinMode(LED_STATUS, OUTPUT);
    ledOff();

    SERIAL_MIDI_PORT.begin(SERIAL_MIDI_BAUD);

    usb_midi.begin();

    // Disable Bluefruit's automatic status-LED blinking. By default the library
    // blinks an onboard LED to show advertising/connection activity, and on the
    // SuperMini/Pro Micro nRF52840 that lands on the same blue LED (P0.15) we use
    // for status — causing it to blink instead of staying solid when connected.
    // Turn it off BEFORE Bluefruit.begin() so it never takes over the pin.
    Bluefruit.autoConnLed(false);

    Bluefruit.begin(0, 1);                  // 0 peripheral, 1 central
    Bluefruit.setName("XIAO MIDI Bridge");
    Bluefruit.setTxPower(8);
    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);

    // "Just works" pairing (no PIN), matching how BLE MIDI devices normally pair.
    Bluefruit.Security.setIOCaps(false, false, false);
    Bluefruit.Security.setSecuredCallback(connection_secured_callback);

    midiService.begin();
    midiChar.setNotifyCallback(midi_notify_callback);
    midiChar.begin();

    Bluefruit.Central.setConnectCallback(connect_callback);
    Bluefruit.Central.setDisconnectCallback(disconnect_callback);

    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Scanner.restartOnDisconnect(true);
    Bluefruit.Scanner.setInterval(160, 80); // 0.625ms units: 100ms / 50ms
    Bluefruit.Scanner.useActiveScan(true);
    Bluefruit.Scanner.filterUuid(BLEUuid(BLEMIDI_SERVICE_UUID));
    Bluefruit.Scanner.start(0);             // scan forever until connected
}

void loop() {
    // Also bridge USB MIDI input out to serial MIDI.
    while (usb_midi.available()) {
        uint8_t packet[4];
        if (usb_midi.readPacket(packet)) {
            uint8_t cin = packet[0] & 0x0F;
            if (cin >= 0x02) {
                uint8_t status = packet[1];
                uint8_t len = msgLen(status);
                if (len) {
                    SERIAL_MIDI_PORT.write(status);
                    if (len >= 2) SERIAL_MIDI_PORT.write(packet[2]);
                    if (len >= 3) SERIAL_MIDI_PORT.write(packet[3]);
                }
            }
        }
    }
    delay(1);
}
