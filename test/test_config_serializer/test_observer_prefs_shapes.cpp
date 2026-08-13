// Round-trip tests for the shapes the observer fork adds to NodePrefs:
// nested groups, hex blobs at the size we actually persist, and the
// loop-generated per-bucket keys used by the flood-retry bridge buckets.
//
// NodePrefs itself pulls in the whole firmware, so these mirror its structure
// rather than including it. Keep them in sync with the retry/flood/alert groups
// in src/helpers/CommonCLI.h.
//
// Two ConfigSerializer constraints are easy to violate and fail destructively —
// a violation is a parse ERROR that aborts the entire load, so every setting
// after the bad key is silently lost, not just the offending one:
//
//   1. is_key_char() accepts only [A-Za-z_]. A digit in a key is unparseable.
//   2. A value token longer than CONFIG_MAX_TOKEN_LEN-1 (127) chars is a parse
//      error. Blobs are hex, so a blob must stay under 63 bytes.

#include <gtest/gtest.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "helpers/ConfigSerializer.h"

// Sizes taken from CommonCLI.h's defaults.
#define T_PREFIX_LEN        3
#define T_PREFIX_SLOTS      8
#define T_BRIDGE_BUCKETS    6
#define T_BUCKET_PREFIXES  17

// A Stream that actually formats what it is given, so a save can be fed
// straight back into a load.
class StringStream : public Stream {
    char _buf[4096];
    int _len = 0, _pos = 0;
public:
    size_t write(uint8_t b) override {
        if (_len < (int)sizeof(_buf) - 1) { _buf[_len++] = (char)b; _buf[_len] = 0; return 1; }
        return 0;
    }
    size_t print(unsigned char v, int r = DEC) override { return printf_("%u", (unsigned)v); }
    size_t print(int v, int r = DEC) override { return printf_("%d", v); }
    size_t print(unsigned int v, int r = DEC) override { return printf_("%u", v); }
    size_t print(long v, int r = DEC) override { return printf_("%ld", v); }
    size_t print(unsigned long v, int r = DEC) override { return printf_("%lu", v); }
    size_t print(long long v, int r = DEC) override { return printf_("%lld", v); }
    size_t print(unsigned long long v, int r = DEC) override { return printf_("%llu", v); }
    size_t print(double v, int p = 2) override { return printf_("%.*f", p, v); }

    int available() override { return _len - _pos; }
    int read() override { return _pos < _len ? (uint8_t)_buf[_pos++] : -1; }
    int peek() override { return _pos < _len ? (uint8_t)_buf[_pos] : -1; }

    void rewind() { _pos = 0; }
    const char* c_str() const { return _buf; }
    int length() const { return _len; }
private:
    size_t printf_(const char* fmt, ...) {
        char tmp[64];
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
        va_end(ap);
        for (int i = 0; i < n; i++) write((uint8_t)tmp[i]);
        return n;
    }
};

// Mirrors the retry/flood groups: scalars, blobs, and per-bucket loop keys.
struct ForkPrefs : public ConfigSerializer {
    uint8_t  retry_preset = 0;
    uint16_t snr_margin_x4 = 0;
    int8_t   cr4 = 0, cr5 = 0, cr7 = 0, cr8 = 0;
    uint8_t  magic[2];
    uint8_t  prefixes[T_PREFIX_SLOTS][T_PREFIX_LEN];
    uint8_t  buckets[T_BRIDGE_BUCKETS][T_BUCKET_PREFIXES][T_PREFIX_LEN];
    uint8_t  batt_low = 0;

private:
    class Retry : public ConfigSerializer {
        ForkPrefs* p;
    protected:
        void structure() override {
            def("preset", p->retry_preset);
            def("snr_margin", p->snr_margin_x4);
            def("cr_four", p->cr4);
            def("cr_five", p->cr5);
            def("cr_seven", p->cr7);
            def("cr_eight", p->cr8);
            def("magic", (void *) p->magic, sizeof(p->magic));
        }
    public:
        Retry(ForkPrefs* parent) : p(parent) { }
    };
    Retry retry;

    class Flood : public ConfigSerializer {
        ForkPrefs* p;
    protected:
        void structure() override {
            def("prefixes", (void *) p->prefixes, sizeof(p->prefixes));
            char key[8];
            for (uint8_t i = 0; i < T_BRIDGE_BUCKETS; i++) {
                sprintf(key, "bkt_%c", (char)('a' + i));
                def(key, (void *) p->buckets[i], sizeof(p->buckets[i]));
            }
        }
    public:
        Flood(ForkPrefs* parent) : p(parent) { }
    };
    Flood flood;

    class Alert : public ConfigSerializer {
        ForkPrefs* p;
    protected:
        void structure() override { def("batt_low", p->batt_low); }
    public:
        Alert(ForkPrefs* parent) : p(parent) { }
    };
    Alert alert;

protected:
    void structure() override {
        def("retry", retry);
        def("flood", flood);
        def("alert", alert);
    }
public:
    ForkPrefs() : retry(this), flood(this), alert(this) {
        memset(magic, 0, sizeof(magic));
        memset(prefixes, 0, sizeof(prefixes));
        memset(buckets, 0, sizeof(buckets));
    }
};

static void fill(ForkPrefs& p) {
    p.retry_preset = 2;
    p.snr_margin_x4 = 60;
    p.cr4 = 40; p.cr5 = 30; p.cr7 = 10; p.cr8 = -10;
    p.magic[0] = 0xD1; p.magic[1] = 0x52;
    p.batt_low = 20;
    for (int i = 0; i < T_PREFIX_SLOTS; i++)
        for (int j = 0; j < T_PREFIX_LEN; j++)
            p.prefixes[i][j] = (uint8_t)(i * 16 + j + 1);
    for (int b = 0; b < T_BRIDGE_BUCKETS; b++)
        for (int i = 0; i < T_BUCKET_PREFIXES; i++)
            for (int j = 0; j < T_PREFIX_LEN; j++)
                p.buckets[b][i][j] = (uint8_t)(b * 41 + i * 3 + j + 1);
}

// ── the whole fork prefs block survives a save -> load cycle ────────────────

TEST(ObserverPrefsShapes, RoundTripsEveryField) {
    ForkPrefs saved;
    fill(saved);

    StringStream s;
    ASSERT_TRUE(saved.saveSerial(s));

    ForkPrefs loaded;
    s.rewind();
    ASSERT_TRUE(loaded.loadSerial(s)) << "parse failed for: " << s.c_str();

    EXPECT_EQ(saved.retry_preset, loaded.retry_preset);
    EXPECT_EQ(saved.snr_margin_x4, loaded.snr_margin_x4);
    EXPECT_EQ(saved.cr4, loaded.cr4);
    EXPECT_EQ(saved.cr5, loaded.cr5);
    EXPECT_EQ(saved.cr7, loaded.cr7);
    EXPECT_EQ(saved.cr8, loaded.cr8);
    EXPECT_EQ(saved.batt_low, loaded.batt_low);
    EXPECT_EQ(0, memcmp(saved.magic, loaded.magic, sizeof(saved.magic)));
    EXPECT_EQ(0, memcmp(saved.prefixes, loaded.prefixes, sizeof(saved.prefixes)));
    EXPECT_EQ(0, memcmp(saved.buckets, loaded.buckets, sizeof(saved.buckets)))
        << "per-bucket keys did not round-trip";
}

// Each bucket must land in its own slot, not all in the first one.
TEST(ObserverPrefsShapes, BucketKeysAreDistinct) {
    ForkPrefs saved;
    fill(saved);

    StringStream s;
    ASSERT_TRUE(saved.saveSerial(s));
    for (uint8_t i = 0; i < T_BRIDGE_BUCKETS; i++) {
        char key[8];
        sprintf(key, "bkt_%c", (char)('a' + i));
        EXPECT_NE(nullptr, strstr(s.c_str(), key)) << "missing key " << key;
    }

    ForkPrefs loaded;
    s.rewind();
    ASSERT_TRUE(loaded.loadSerial(s));
    for (int b = 1; b < T_BRIDGE_BUCKETS; b++) {
        EXPECT_NE(0, memcmp(loaded.buckets[0], loaded.buckets[b], sizeof(loaded.buckets[0])))
            << "bucket " << b << " aliased bucket 0";
    }
}

// ── the two constraints that make a bad key destructive ────────────────────

// A digit in a key is a parse error, and it takes the rest of the file with it.
// This is why the retry keys are spelled cr_four/cr_five and the buckets are
// suffixed bkt_a..bkt_f rather than cr4/b0.
TEST(ObserverPrefsShapes, DigitInKeyBreaksTheWholeLoad) {
    struct DigitKey : public ConfigSerializer {
        uint8_t first = 0, second = 0;
    protected:
        void structure() override {
            def("cr4", first);      // illegal: contains a digit
            def("after", second);
        }
    public:
        DigitKey() { }
    };

    DigitKey d;
    StringStream s;
    const char* json = "{cr4:7,after:9}";
    for (const char* p = json; *p; p++) s.write((uint8_t)*p);

    EXPECT_FALSE(d.loadSerial(s)) << "a digit key should fail to parse";
    EXPECT_EQ(0, d.second) << "keys after the bad one must not have loaded";
}

// A blob whose hex encoding exceeds the token buffer is also a whole-file error,
// which is what the static_assert on the bucket size guards against.
TEST(ObserverPrefsShapes, BucketBlobFitsTokenBuffer) {
    const int bucket_bytes = T_BUCKET_PREFIXES * T_PREFIX_LEN;
    EXPECT_LT(bucket_bytes * 2, CONFIG_MAX_TOKEN_LEN - 1)
        << "bucket hex blob would overflow the parser's token buffer";
}
