// Host-side checks for the learning decoder (irdecode.c): captures recorded on a 3DS from a Flipper Zero's
// `ir tx`, and every protocol encoded by irenc.c, reduced to mark starts and jittered.
//
//   cc -O2 -Isource tests/decode_test.c source/irdecode.c source/irenc.c -o decode_test && ./decode_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "irdecode.h"
#include "irenc.h"

// Mark start times (us) the 3DS's RX FIFO polling saw (tools/irnet.py rx), Flipper `ir tx` at ~5 cm.
static const struct {
    const char* protocol;
    uint32_t address, command;
    uint32_t t[80];
} captures[] = {
    {"NEC", 0x04, 0x08, {40718, 54080, 55121, 56348, 58250, 59687, 60815, 61891, 62925, 64283, 66534, 68589, 69491, 71951,
                         74316, 76496, 78846, 81098, 82180, 83306, 84378, 86510, 87846, 88867, 90120, 91009, 93443, 95759,
                         97858, 98936, 101189, 103411, 105908, 108066}},
    {"SIRC", 0x01, 0x15, {491565, 494769, 496350, 497373, 499390, 500677, 502494, 503525, 504740, 506688, 507809, 508624,
                          510283, 536583, 539548, 541340, 542539, 544571, 545592, 547506, 548548, 549829, 551695, 552493,
                          553995, 555262, 581604, 584724, 586227, 587773, 589335, 590764, 592471, 593777, 594838, 596562,
                          597818, 599084, 600374}},
    {"Samsung32", 0x07, 0x02, {758096, 767067, 769195, 771536, 773729, 774928, 775975, 777080, 778022, 779145, 781502, 783538,
                               785793, 786974, 788009, 789076, 790105, 791334, 792392, 794480, 795840, 796791, 797818, 799121,
                               800062, 801112, 803493, 804441, 806849, 808999, 811134, 813183, 815517, 817780}},
    {"Kaseikyo", 0x0100, 0x51, {750838, 756010, 757875, 758477, 759487, 760321, 761092, 761933, 762462, 763639, 764700,
                                765495, 766385, 767153, 767983, 768983, 769904, 770490, 772449, 773230, 774153, 774882,
                                775975, 776809, 777576, 778383, 779203, 780028, 781065, 781828, 783691, 784356, 785263,
                                786311, 787929, 788757, 790528, 791456, 792337, 793156, 793910, 794919, 795690, 796657,
                                798375, 799162, 800975, 801830, 802681, 803462}},
    {"RCA", 0x0F, 0x54, {637722, 645927, 648274, 650688, 653366, 655925, 657301, 658925, 661214, 662916, 665292, 666790, 669224,
                         670787, 672270, 673833, 675222, 676778, 679363, 681746, 683296, 685823, 687208, 689857, 691270,
                         693931}},
    {"Pioneer", 0xAA, 0x1C, {670768, 683566, 684461, 686569, 687635, 689401, 690592, 692467, 693481, 695523, 697648, 698688,
                             700557, 701538, 703505, 704610, 706506, 707602, 708624, 709480, 711714, 713531, 715467, 716655,
                             717464, 718568, 720723, 722531, 723497, 724624, 725677, 727612, 729652, 731727, 758638, 771383,
                             772458, 774255, 775298, 777405, 778375, 780264, 781342, 783370, 785453, 786281, 788450, 789378,
                             791344, 792378, 794326, 795184, 796324, 797349, 799231, 801310, 803245, 804370, 805201, 806220,
                             808375, 810150, 810988, 812448, 813255, 815329, 817183, 819192}},
    // 36 kHz: a stream of bytes, no usable edges
    {"RC5", 0, 0, {661259, 662164, 663063, 663651, 664238, 664862, 665529, 666124, 666679, 667468, 668063, 669042, 669586,
                   670145, 670937, 671449, 672007, 673634, 674206, 674773, 675361, 676132, 676853, 677430, 679006, 679520,
                   680070, 680614, 681198, 681923, 682542, 683358, 684320}},
};

static size_t count(const uint32_t* t, size_t max) {
    size_t n = 0;
    while(n < max && t[n]) n++;
    return n;
}

static int fails;

static void expect(const char* label, const uint32_t* t, size_t n, const char* protocol, uint32_t a, uint32_t c) {
    IrDecoded d;
    bool ok = ir_decode(t, n, &d);
    if(!protocol) {
        if(ok) {
            printf("FAIL %s: decoded %s %X/%X, expected nothing\n", label, d.protocol, d.address, d.command);
            fails++;
        }
        return;
    }
    if(!ok || strcmp(d.protocol, protocol) || d.address != a || d.command != c) {
        printf("FAIL %s: got %s %X/%X, expected %s %X/%X (%s)\n", label, ok ? d.protocol : "nothing", ok ? d.address : 0,
               ok ? d.command : 0, protocol, a, c, ok ? "" : ir_decode_hint(t, n));
        fails++;
    }
}

static uint32_t rng = 12345;
static int jitter(int amp) {
    rng = rng * 1103515245 + 12345;
    return (int)((rng >> 8) % (2 * amp + 1)) - amp;
}

// One press as mark starts, each moved by up to +-amp us (the 3DS sees them +-150, a few +-350).
static size_t starts(const char* protocol, uint32_t a, uint32_t c, int amp, uint32_t* out, size_t max) {
    IrTimings tm;
    ir_timings_init(&tm);
    size_t n = 0;
    if(ir_encode_parsed(protocol, a, c, &tm)) {
        uint32_t at = 100000;
        for(size_t i = 0; i < tm.n && n < max; i++) {
            if(i % 2 == 0) out[n++] = at + jitter(amp);
            at += tm.t[i];
        }
    }
    ir_timings_free(&tm);
    return n;
}

int main(void) {
    for(size_t i = 0; i < sizeof(captures) / sizeof(captures[0]); i++) {
        size_t n = count(captures[i].t, 80);
        bool none = !strcmp(captures[i].protocol, "RC5");
        expect(captures[i].protocol, captures[i].t, n, none ? NULL : captures[i].protocol, captures[i].address,
               captures[i].command);
        // starting inside the first frame: only the captures with a second frame (SIRC, Pioneer) decode
        bool more = !strcmp(captures[i].protocol, "SIRC") || !strcmp(captures[i].protocol, "Pioneer");
        if(!none)
            expect("late start", captures[i].t + 3, n - 3, more ? captures[i].protocol : NULL, captures[i].address,
                   captures[i].command);
    }
    static const struct {
        const char* p;
        uint32_t a, c;
    } codes[] = {
        {"NEC", 0x04, 0x08},          {"NEC", 0x00, 0xFF},         {"NECext", 0x7F00, 0x0E1F},  {"NECext", 0x0586, 0xF708},
        {"NEC42", 0x1C, 0x13},        {"NEC42ext", 0x123456, 0xABCD}, {"Samsung32", 0x07, 0x02}, {"Samsung32", 0x0E, 0xE6},
        {"SIRC", 0x01, 0x15},         {"SIRC15", 0x77, 0x33},      {"SIRC20", 0x1A3, 0x7F},     {"Kaseikyo", 0x1544A0, 0x51},
        {"Kaseikyo", 0x3002020, 0x3FF}, {"Pioneer", 0xAA, 0x1C},   {"RCA", 0x0F, 0x54},         {"RCA", 0x03, 0x00},
    };
    uint32_t t[400];
    for(size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        for(int amp = 0; amp <= 200; amp += 100) {
            for(int rep = 0; rep < 20; rep++) {
                size_t n = starts(codes[i].p, codes[i].a, codes[i].c, amp, t, 400);
                char label[64];
                snprintf(label, sizeof(label), "%s %X/%X +-%d", codes[i].p, codes[i].a, codes[i].c, amp);
                expect(label, t, n, codes[i].p, codes[i].a, codes[i].c);
                if(fails > 20) return 1;
            }
        }
    }
    printf("%s\n", fails ? "FAILED" : "all decoded");
    return fails != 0;
}
