#include "speaker.h"
#include "timer.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0,%1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

#define PIT_CHAN2   0x42
#define PIT_CMD     0x43
#define PIT_HZ      1193182u
#define SPEAKER_GATE 0x61

void speaker_on(uint32_t freq_hz) {
    if (freq_hz == 0) return;
    uint32_t div = PIT_HZ / freq_hz;

    outb(PIT_CMD, 0xB6);
    outb(PIT_CHAN2, (uint8_t)(div & 0xFF));
    outb(PIT_CHAN2, (uint8_t)((div >> 8) & 0xFF));

    uint8_t cur = inb(SPEAKER_GATE);
    outb(SPEAKER_GATE, cur | 0x03);
}

void speaker_off(void) {
    uint8_t cur = inb(SPEAKER_GATE);
    outb(SPEAKER_GATE, cur & (uint8_t)~0x03);
}

void speaker_beep(uint32_t freq_hz, uint32_t ms) {
    speaker_on(freq_hz);
    timer_sleep(ms);
    speaker_off();
}

void speaker_jingle(void) {
    static const uint32_t notes[] = {523, 659, 784, 1047};
    for (int i = 0; i < 4; i++) {
        speaker_on(notes[i]);
        timer_sleep(90);
        speaker_off();
        timer_sleep(15);
    }
}
