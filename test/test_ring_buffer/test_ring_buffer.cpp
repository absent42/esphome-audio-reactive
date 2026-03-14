#include "../../components/audio_reactive/ring_buffer.h"
#include <cassert>
#include <cstdio>

using namespace esphome::audio_reactive;

void test_basic_write_read() {
    RingBuffer<float, 512> buf;
    float samples[128];
    for (int i = 0; i < 128; i++) samples[i] = static_cast<float>(i);
    assert(buf.write(samples, 128) == 128);
    assert(buf.available() == 128);
    float out[128];
    assert(buf.read(out, 128) == 128);
    assert(out[0] == 0.0f);
    assert(out[127] == 127.0f);
    assert(buf.available() == 0);
    printf("PASS: test_basic_write_read\n");
}

void test_overlap_read() {
    RingBuffer<float, 1024> buf;
    float samples[512];
    for (int i = 0; i < 512; i++) samples[i] = static_cast<float>(i);
    buf.write(samples, 512);
    float window[512];
    assert(buf.peek(window, 512) == 512);
    assert(window[0] == 0.0f);
    assert(window[511] == 511.0f);
    buf.advance(128);
    assert(buf.available() == 384);
    float more[128];
    for (int i = 0; i < 128; i++) more[i] = static_cast<float>(512 + i);
    buf.write(more, 128);
    assert(buf.available() == 512);
    buf.peek(window, 512);
    assert(window[0] == 128.0f);
    assert(window[511] == 639.0f);
    printf("PASS: test_overlap_read\n");
}

void test_overflow_drops() {
    RingBuffer<float, 16> buf;
    float samples[20];
    for (int i = 0; i < 20; i++) samples[i] = static_cast<float>(i);
    size_t written = buf.write(samples, 20);
    assert(written == 16);
    printf("PASS: test_overflow_drops\n");
}

void test_clear() {
    RingBuffer<float, 64> buf;
    float s[10] = {};
    buf.write(s, 10);
    assert(buf.available() == 10);
    buf.clear();
    assert(buf.available() == 0);
    printf("PASS: test_clear\n");
}

int main() {
    test_basic_write_read();
    test_overlap_read();
    test_overflow_drops();
    test_clear();
    printf("All ring buffer tests passed!\n");
    return 0;
}
