// Native mode RF69 driver.
// see https://jeelabs.org/ref/RFM69.pdf
//
// JeeLabs packet format:
//  0: packet length N = 2..65
//  1: group parity (b7..6), dest id (b5..0)
//  2: flags (b7..6), src id (b5..0)
//  3..(N-1): payload data (max 63 bytes)
//  N..(N+1): 16-bit crc

#ifndef Yield
#define Yield()
#endif

template< typename SPI >
struct RF69 {
    int16_t afc;
    uint8_t rssi;
    uint8_t lna;
    uint8_t myId;
    uint8_t parity;

    void init (uint8_t id, uint8_t group, int freq);
    void encrypt (const char* key);
    void txPower (uint8_t level);

    void listen ();
    void rssiCapture ();
    int receive (void* ptr, int len);
    void send (uint8_t header, const void* ptr, int len);
    void sleep () { setMode(MODE_SLEEP); }

    static uint8_t readReg (uint8_t addr) { return rwReg(addr, 0); }
    static void writeReg (uint8_t addr, uint8_t val) { rwReg(addr|0x80, val); }
    static void setMode (uint8_t newMode) { writeReg(REG_OPMODE, newMode); }

    // TODO somewhat redundant to have readReg, writeReg, and rwReg ...
    static uint8_t rwReg (uint8_t cmd, uint8_t val) {
        SPI::enable();
        SPI::transfer(cmd);
        uint8_t r = SPI::transfer(val);
        SPI::disable();
        return r;
    }

    enum {
        REG_FIFO          = 0x00,
        REG_OPMODE        = 0x01,
        REG_FRFMSB        = 0x07,
        REG_PALEVEL       = 0x11,
        REG_LNAVALUE      = 0x18,
        REG_AFCMSB        = 0x1F,
        REG_AFCLSB        = 0x20,
        REG_RSSIVALUE     = 0x24,
        REG_IRQFLAGS1     = 0x27,
        REG_IRQFLAGS2     = 0x28,
        REG_SYNCVALUE1    = 0x2F,
        REG_PKTCONFIG2    = 0x3D,
        REG_AESKEYMSB     = 0x3E,

        MODE_SLEEP        = 0<<2,
        MODE_STANDBY      = 1<<2,
        MODE_RECEIVE      = 4<<2,
    };

    void configure (const uint8_t* p);
    void setFrequency (uint32_t freq);
};

// driver implementation

template< typename SPI >
void RF69<SPI>::setFrequency (uint32_t hz) {
    // accept any frequency scale as input, including KHz and MHz
    // multiply by 10 until freq >= 100 MHz (don't specify 0 as input!)
    while (hz < 100000000)
        hz *= 10;

    // Frequency steps are in units of (32,000,000 >> 19) = 61.03515625 Hz
    // use multiples of 64 to avoid multi-precision arithmetic, i.e. 3906.25 Hz
    // due to this, the lower 6 bits of the calculated factor will always be 0
    // this is still 4 ppm, i.e. well below the radio's 32 MHz crystal accuracy
    // 868.0 MHz = 0xD90000, 868.3 MHz = 0xD91300, 915.0 MHz = 0xE4C000
    uint32_t frf = (hz << 2) / (32000000L >> 11);
    writeReg(REG_FRFMSB, frf >> 10);
    writeReg(REG_FRFMSB+1, frf >> 2);
    writeReg(REG_FRFMSB+2, frf << 6);
}

template< typename SPI >
void RF69<SPI>::configure (const uint8_t* p) {
    while (true) {
        uint8_t cmd = p[0];
        if (cmd == 0)
            break;
        writeReg(cmd, p[1]);
        p += 2;
    }
}

static const uint8_t configRegs [] = {
    // POR value is better for first rf_sleep  0x01, 0x00, // OpMode = sleep
    0x02, 0x00, // DataModul = packet mode, fsk
    0x03, 0x02, // BitRateMsb, data rate = 49,261 khz
    0x04, 0x8A, // BitRateLsb, divider = 32 MHz / 650
    0x05, 0x02, // FdevMsb = 45 KHz
    0x06, 0xE1, // FdevLsb = 45 KHz
    0x0B, 0x20, // Low M
    0x19, 0x4A, // RxBw 100 KHz
    0x1A, 0x42, // AfcBw 125 KHz
    0x1E, 0x0C, // AfcAutoclearOn, AfcAutoOn
    //0x25, 0x40, //0x80, // DioMapping1 = SyncAddress (Rx)
    0x26, 0x07, // disable clkout
    0x29, 0xC8, // RssiThresh -100 dB
    0x2D, 0x05, // PreambleSize = 5
    0x2E, 0x90, // SyncConfig = sync on, sync size = 3
    0x2F, 0xAA, // SyncValue1 = 0xAA
    0x30, 0x2D, // SyncValue2 = 0x2D
    0x37, 0xD0, // PacketConfig1 = fixed, white, no filtering
    0x38, 0x42, // PayloadLength = 0, unlimited
    0x3C, 0x82, // FifoTresh, not empty, level 2
    0x3D, 0x12, // 0x10, // PacketConfig2, interpkt = 1, autorxrestart off
    0x6F, 0x20, // TestDagc ...
    0x71, 0x02, // RegTestAfc
    0
};

template< typename SPI >
void RF69<SPI>::init (uint8_t id, uint8_t group, int freq) {
    myId = id;

    // b7 = group b7^b5^b3^b1, b6 = group b6^b4^b2^b0
    parity = group ^ (group << 4);
    parity = (parity ^ (parity << 2)) & 0xC0;

    do
        writeReg(REG_SYNCVALUE1, 0xAA);
    while (readReg(REG_SYNCVALUE1) != 0xAA);
    do
        writeReg(REG_SYNCVALUE1, 0x55);
    while (readReg(REG_SYNCVALUE1) != 0x55);

    configure(configRegs);
    setFrequency(freq);

    writeReg(REG_SYNCVALUE1+2, group);
    sleep();
}

template< typename SPI >
void RF69<SPI>::encrypt (const char* key) {
    uint8_t cfg = readReg(REG_PKTCONFIG2) & ~0x01;
    if (key) {
        for (int i = 0; i < 16; ++i) {
            writeReg(REG_AESKEYMSB + i, *key);
            if (*key != 0)
                ++key;
        }
        cfg |= 0x01;
    }
    writeReg(REG_PKTCONFIG2, cfg);
}

template< typename SPI >
void RF69<SPI>::txPower (uint8_t level) {
    writeReg(REG_PALEVEL, (readReg(REG_PALEVEL) & ~0x1F) | level);
}

template< typename SPI >
void RF69<SPI>::listen () {
    writeReg(0x3B, 0b01100100); // sleep mode until fifo empty
    setMode(MODE_RECEIVE);
}

template< typename SPI >
void RF69<SPI>::rssiCapture () {
    // fetch rssi, lnd, and afc
    rssi = readReg(REG_RSSIVALUE);
    lna = (readReg(REG_LNAVALUE) >> 3) & 0x7;
    afc = readReg(REG_AFCMSB) << 8;
    afc |= readReg(REG_AFCLSB);
}

template< typename SPI >
int RF69<SPI>::receive (void* ptr, int len) {
    SPI::enable();
    SPI::transfer(REG_FIFO);
    int count = SPI::transfer(0);
    for (int i = 0; i < count; ++i) {
        uint8_t v = SPI::transfer(0);
        if (i < len)
            ((uint8_t*) ptr)[i] = v;
    }
    SPI::disable();

    // only accept packets intended for us, or broadcasts
    // ... or any packet if we're the special catch-all node
    uint8_t dest = *(uint8_t*) ptr;
    if (count <= 66 && (dest & 0xC0) == parity) {
        uint8_t destId = dest & 0x3F;
        if (destId == myId || destId == 0 || myId == 63)
            return count;
    }

    return -1;
}

template< typename SPI >
void RF69<SPI>::send (uint8_t header, const void* ptr, int len) {
    writeReg(0x3B, 0b01011011); // automode: fifolevel + packetsent + tx

    SPI::enable();
    SPI::transfer(REG_FIFO | 0x80);
    SPI::transfer(len + 2);
    SPI::transfer((header & 0x3F) | parity);
    SPI::transfer((header & 0xC0) | myId);
    for (int i = 0; i < len; ++i)
        SPI::transfer(((const uint8_t*) ptr)[i]);
    SPI::disable();
}
