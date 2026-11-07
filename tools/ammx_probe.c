/* Standalone 68080-only correctness and EClock probe. Never auto-run. */
#include "dock_core.h"
#include <exec/types.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Device *TimerBase;
long __stack = 32768;
/* Also declared by current dock_core.h; explicit declarations keep this probe
   buildable while backend headers are being integrated independently. */
extern void dock_rgb565_to_rgba_c(unsigned char *, const unsigned char *, unsigned long);
extern void dock_rgb565_to_rgba_ammx(unsigned char *, const unsigned char *, unsigned long);
extern void dock_rgba_to_rgb565_c(unsigned char *, const unsigned char *, unsigned long);
extern void dock_rgba_to_rgb565_ammx(unsigned char *, const unsigned char *, unsigned long);
extern int dock_bytes_equal_c(const unsigned char *, const unsigned char *, unsigned long);
extern int dock_bytes_equal_ammx(const unsigned char *, const unsigned char *, unsigned long);
extern void dock_rgb565_key_c(unsigned char *, const unsigned char *, unsigned long);
extern void dock_rgb565_key_ammx(unsigned char *, const unsigned char *, unsigned long);

static int validate(void)
{
    unsigned char actual[160], expected[160], source[160], saved[160];
    unsigned int a, b, i, offset, count;
    for (a = 0; a < 256; ++a) {
        for (b = 0; b < 256; ++b) {
            memset(actual, 0xa5, sizeof(actual));
            memset(source, 0x5a, sizeof(source));
            for (i = 0; i < 8; ++i) {
                actual[8+i] = (unsigned char)(a + 37*i);
                source[8+i] = (unsigned char)(b + 19*i);
            }
            memcpy(expected, actual, sizeof(actual));
            memcpy(saved, source, sizeof(source));
            dock_mix_half_c(expected+8, source+8, 2);
            dock_mix_half_ammx(actual+8, source+8, 2);
            if (memcmp(actual, expected, sizeof(actual)) ||
                memcmp(source, saved, sizeof(source))) {
                printf("FAIL byte pair a=%u b=%u\n", a, b);
                return 0;
            }
        }
    }
    for (offset = 0; offset < 8; ++offset) {
        for (count = 0; count < 32; ++count) {
            for (i = 0; i < sizeof(actual); ++i) {
                actual[i] = (unsigned char)(i*137 + count*19);
                source[i] = (unsigned char)(i*47 + offset*11);
            }
            memcpy(expected, actual, sizeof(actual));
            memcpy(saved, source, sizeof(source));
            dock_mix_half_c(expected+8+offset, source+8+offset, count);
            dock_mix_half_ammx(actual+8+offset, source+8+offset, count);
            if (memcmp(actual, expected, sizeof(actual)) ||
                memcmp(source, saved, sizeof(source))) {
                printf("FAIL count=%u alignment=%u\n", count, offset);
                return 0;
            }
            memcpy(expected, actual, sizeof(actual));
            dock_mix_half_ammx(actual+8+offset, actual+8+offset, count);
            if (memcmp(actual, expected, sizeof(actual))) {
                printf("FAIL alias count=%u alignment=%u\n", count, offset);
                return 0;
            }
        }
    }
    /* Zero count is defined even with no accessible image memory. */
    dock_mix_half_ammx(NULL, NULL, 0);
    puts("PASS: 65536 byte pairs, all count 0..31/alignment 0..7 cases, canaries, source preservation, exact alias, null zero count");
    return 1;
}

static int validate_rgb24(void)
{
    unsigned char actual[160], expected[160], source[160], saved[160];
    unsigned int dst_offset, src_offset, count, i, pixel;
    /* Independent source/destination alignments exercise cross-boundary stores. */
    for (src_offset = 0; src_offset < 8; ++src_offset) {
        for (dst_offset = 0; dst_offset < 8; ++dst_offset) {
            for (count = 0; count < 32; ++count) {
                memset(actual, 0xa5, sizeof(actual));
                memset(expected, 0xa5, sizeof(expected));
                for (i = 0; i < sizeof(source); ++i)
                    source[i] = (unsigned char)(i*47 + count*19 + src_offset*11);
                memcpy(saved, source, sizeof(source));
                for (pixel = 0; pixel < count; ++pixel) {
                    for (i = 0; i < 3; ++i)
                        expected[8+dst_offset+pixel*3+i] = source[8+src_offset+pixel*4+i];
                }
                dock_rgba_to_rgb24_ammx(actual+8+dst_offset, source+8+src_offset, count);
                if (memcmp(actual, expected, sizeof(actual)) ||
                    memcmp(source, saved, sizeof(source))) {
                    printf("FAIL RGB24 count=%u srcalign=%u dstalign=%u\n",
                           count, src_offset, dst_offset);
                    return 0;
                }
            }
        }
    }
    dock_rgba_to_rgb24_ammx(NULL, NULL, 0);
    puts("PASS RGB24: all count 0..31/source alignment 0..7/destination alignment 0..7 cases, canaries, source preservation, null zero count");
    return 1;
}

/* Independent byte-wise oracle, not a call to the optimized C kernels. */
static void expected_decode(unsigned char *dst, const unsigned char *src, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; ++i) {
        unsigned int v = ((unsigned int)src[0] << 8) | src[1];
        unsigned int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        dst[0] = (unsigned char)((r << 3) | (r >> 2));
        dst[1] = (unsigned char)((g << 2) | (g >> 4));
        dst[2] = (unsigned char)((b << 3) | (b >> 2)); dst[3] = 255;
        src += 2; dst += 4;
    }
}
static void expected_encode(unsigned char *dst, const unsigned char *src, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; ++i) {
        unsigned int v = ((unsigned int)(src[0] >> 3) << 11) |
            ((unsigned int)(src[1] >> 2) << 5) | (src[2] >> 3);
        dst[0] = (unsigned char)(v >> 8); dst[1] = (unsigned char)v;
        src += 4; dst += 2;
    }
}
static int validate_rgb565(void)
{
    unsigned char actual[160], expected[160], source[160], saved[160], packed[160];
    unsigned int value, i, src_offset, dst_offset, count;
    for (value = 0; value < 65536UL; ++value) {
        memset(source, 0x5a, sizeof(source));
        for (i = 0; i < 4; ++i) {
            unsigned int v = (value + i*19873UL) & 65535UL;
            source[8+i*2] = (unsigned char)(v >> 8); source[9+i*2] = (unsigned char)v;
        }
        memcpy(saved, source, sizeof(saved));
        memset(actual, 0xa5, sizeof(actual)); memset(expected, 0xa5, sizeof(expected));
        expected_decode(expected+8, source+8, 4);
        dock_rgb565_to_rgba_ammx(actual+8, source+8, 4);
        if (memcmp(actual, expected, sizeof(actual)) || memcmp(source, saved, sizeof(source))) {
            printf("FAIL RGB565 exhaustive decode value=%u firstRGBA=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                value, actual[8],actual[9],actual[10],actual[11],expected[8],expected[9],expected[10],expected[11]);
            return 0;
        }
        memset(packed, 0xa5, sizeof(packed)); memset(expected, 0xa5, sizeof(expected));
        memcpy(expected+8, source+8, 8);
        dock_rgba_to_rgb565_ammx(packed+8, actual+8, 4);
        if (memcmp(packed, expected, sizeof(packed))) {
            printf("FAIL RGB565 exhaustive roundtrip value=%u\n",value); return 0;
        }
    }
    for (src_offset = 0; src_offset < 8; ++src_offset) {
        for (dst_offset = 0; dst_offset < 8; ++dst_offset) {
            for (count = 0; count < 32; ++count) {
                for (i = 0; i < sizeof(source); ++i)
                    source[i] = (unsigned char)(i*137 + count*19 + src_offset*47);
                memcpy(saved, source, sizeof(saved));
                memset(actual, 0xa5, sizeof(actual)); memset(expected, 0xa5, sizeof(expected));
                expected_decode(expected+8+dst_offset, source+8+src_offset, count);
                dock_rgb565_to_rgba_ammx(actual+8+dst_offset, source+8+src_offset, count);
                if (memcmp(actual, expected, sizeof(actual)) || memcmp(source,saved,sizeof(source))) {
                    printf("FAIL RGB565 decode count=%u srcalign=%u dstalign=%u\n",count,src_offset,dst_offset); return 0;
                }
                memset(actual, 0xa5, sizeof(actual)); memset(expected, 0xa5, sizeof(expected));
                expected_encode(expected+8+dst_offset, source+8+src_offset, count);
                dock_rgba_to_rgb565_ammx(actual+8+dst_offset, source+8+src_offset, count);
                if (memcmp(actual, expected, sizeof(actual)) || memcmp(source,saved,sizeof(source))) {
                    printf("FAIL RGB565 encode count=%u srcalign=%u dstalign=%u\n",count,src_offset,dst_offset); return 0;
                }
            }
        }
    }
    dock_rgb565_to_rgba_ammx(NULL,NULL,0); dock_rgba_to_rgb565_ammx(NULL,NULL,0);
    puts("PASS RGB565: all 65536 colors/roundtrips, counts 0..31, independent alignments 0..7, canaries, source preservation, null zero");
    return 1;
}
static int validate_equal(void)
{
    unsigned char a[160], b[160], saved_a[160], saved_b[160];
    unsigned int ao, bo, count, i, mismatch;
    if (!dock_bytes_equal_ammx(NULL,NULL,0)) { puts("FAIL equality nullzero"); return 0; }
    for (ao=0;ao<8;++ao) for (bo=0;bo<8;++bo) for (count=0;count<64;++count) {
        memset(a,0x5a,sizeof(a)); memset(b,0xa5,sizeof(b));
        for (i=0;i<count;++i) a[8+ao+i]=b[8+bo+i]=(unsigned char)(i*137+count*19);
        memcpy(saved_a,a,sizeof(a)); memcpy(saved_b,b,sizeof(b));
        if (!dock_bytes_equal_ammx(a+8+ao,b+8+bo,count) ||
            !dock_bytes_equal_ammx(a+8+ao,a+8+ao,count)) {
            printf("FAIL equality equal count=%u align=%u/%u\n",count,ao,bo); return 0;
        }
        for (mismatch=0;mismatch<count;++mismatch) {
            b[8+bo+mismatch]^=1;
            if (dock_bytes_equal_ammx(a+8+ao,b+8+bo,count)) {
                printf("FAIL equality mismatch count=%u align=%u/%u byte=%u\n",count,ao,bo,mismatch); return 0;
            }
            b[8+bo+mismatch]^=1;
        }
        if (memcmp(a,saved_a,sizeof(a)) || memcmp(b,saved_b,sizeof(b))) {
            puts("FAIL equality modified source"); return 0;
        }
    }
    puts("PASS equality: counts 0..63, independent alignments 0..7, every mismatch position, exact alias, unchanged inputs, null zero");
    return 1;
}

static void expected_key(unsigned char *dst, const unsigned char *src, unsigned int count)
{
    unsigned int i;
    for (i=0; i<count; ++i) {
        unsigned int d=((unsigned int)dst[0]<<8)|dst[1];
        if (!*src) d=0xf81f;
        else if (d==0xf81f) d=0xf81e;
        dst[0]=(unsigned char)(d>>8); dst[1]=(unsigned char)d;
        dst+=2; ++src;
    }
}

static void put565(unsigned char *dst, unsigned int value)
{
    dst[0]=(unsigned char)(value>>8); dst[1]=(unsigned char)value;
}

static int validate_key565(void)
{
    unsigned char actual[160], expected[160], source[160], saved[160], original[160];
    unsigned int value, i, ao, so, count;
    for (value=0; value<65536UL; ++value) {
        static const unsigned char masks[8]={0,1,255,0,128,2,254,0};
        memset(actual,0xa5,sizeof(actual)); memset(source,0x5a,sizeof(source));
        for (i=0; i<8; ++i) {
            put565(actual+8+i*2,value); source[8+i]=masks[i];
        }
        memcpy(expected,actual,sizeof(actual)); memcpy(original,actual,sizeof(actual));
        memcpy(saved,source,sizeof(source));
        expected_key(expected+8,source+8,8);
        dock_rgb565_key_ammx(actual+8,source+8,8);
        if (memcmp(actual,expected,sizeof(actual)) || memcmp(source,saved,sizeof(source))) {
            printf("FAIL RGB565 key exhaustive value=%u\n",value); return 0;
        }
        dock_rgb565_key_c(original+8,source+8,8);
        if (memcmp(original,expected,sizeof(original)) || memcmp(source,saved,sizeof(source))) {
            printf("FAIL RGB565 key C exhaustive value=%u\n",value); return 0;
        }
    }
    for (ao=0; ao<8; ++ao) for (so=0; so<8; ++so) for (count=0; count<32; ++count) {
        memset(actual,0xa5,sizeof(actual)); memset(source,0x5a,sizeof(source));
        for (i=0; i<count; ++i) {
            unsigned int d=(i*19873UL+count*137+ao*47)&65535UL;
            if (i%4>=2) d=0xf81f;
            put565(actual+8+ao+i*2,d);
            source[8+so+i]=i%4==0 ? 0 : i%4==1 ? 1 : i%4==2 ? 255 : 0;
        }
        memcpy(expected,actual,sizeof(actual)); memcpy(original,actual,sizeof(actual));
        memcpy(saved,source,sizeof(source));
        expected_key(expected+8+ao,source+8+so,count);
        dock_rgb565_key_ammx(actual+8+ao,source+8+so,count);
        dock_rgb565_key_c(original+8+ao,source+8+so,count);
        if (memcmp(actual,expected,sizeof(actual)) || memcmp(original,expected,sizeof(original)) ||
            memcmp(source,saved,sizeof(source))) {
            printf("FAIL RGB565 key count=%u align=%u/%u\n",count,ao,so); return 0;
        }
    }
    /* Every possible nonzero coverage byte retains foreground colors. In
       particular matching gray foreground/background is not a transparent hole. */
    for (value=0; value<256; ++value) {
        memset(actual,0xa5,sizeof(actual)); memset(source,(int)value,sizeof(source));
        for (i=0; i<8; ++i) put565(actual+8+i*2,i%2 ? 0xf81f : 0x8410);
        memcpy(expected,actual,sizeof(actual)); memcpy(saved,source,sizeof(source));
        expected_key(expected+8,source+8,8);
        dock_rgb565_key_ammx(actual+8,source+8,8);
        if (memcmp(actual,expected,sizeof(actual)) || memcmp(source,saved,sizeof(source))) {
            printf("FAIL RGB565 key coverage byte=%u\n",value); return 0;
        }
    }
    dock_rgb565_key_ammx(NULL,NULL,0); dock_rgb565_key_c(NULL,NULL,0);
    puts("PASS RGB565 key: all 65536 colors and key collisions, every coverage byte, counts 0..31, independent alignments 0..7, canaries, coverage preservation, null zero");
    return 1;
}
static void measure_native(const char *name, DockMixHalf convert,
                           unsigned char *dst, const unsigned char *src)
{
    struct EClockVal start,end;
    unsigned long frequency,ticks,repeat;
    frequency=ReadEClock(&start);
    for(repeat=0;repeat<250;++repeat) convert(dst,src,16384);
    ReadEClock(&end); ticks=end.ev_lo-start.ev_lo;
    printf("%s: %lu EClock ticks at %lu Hz, 4096000 pixels\n",name,ticks,frequency);
}
static void measure_equal(const char *name,
    int (*equal)(const unsigned char *, const unsigned char *, unsigned long),
    const unsigned char *a, const unsigned char *b)
{
    struct EClockVal start,end;
    unsigned long frequency,ticks,repeat;
    volatile unsigned long checksum=0;
    frequency=ReadEClock(&start);
    for(repeat=0;repeat<250;++repeat) checksum+=(unsigned long)equal(a,b,65536);
    ReadEClock(&end); ticks=end.ev_lo-start.ev_lo;
    printf("%s: %lu EClock ticks at %lu Hz, 16384000 bytes, checksum%lu\n",name,ticks,frequency,checksum);
}

static void measure(const char *name, DockMixHalf mix,
                    unsigned char *dst, const unsigned char *src)
{
    struct EClockVal start, end;
    unsigned long frequency, ticks, repeat;
    frequency = ReadEClock(&start);
    for (repeat = 0; repeat < 1000; ++repeat)
        mix(dst, src, 16384);
    ReadEClock(&end);
    ticks = end.ev_lo - start.ev_lo;
    printf("%s: %lu EClock ticks at %lu Hz, 16384000 pixels\n",
           name, ticks, frequency);
}

int main(void)
{
    struct MsgPort *port;
    struct timerequest *request;
    unsigned char *dst, *source;
    unsigned int i;
    int result = 20;
    if (!validate() || !validate_rgb24() || !validate_rgb565() || !validate_equal() || !validate_key565()) return result;
    port = CreateMsgPort();
    if (!port) return result;
    request = (struct timerequest *)CreateIORequest(port, sizeof(*request));
    if (!request) { DeleteMsgPort(port); return result; }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, &request->tr_node, 0)) {
        DeleteIORequest(&request->tr_node);
        DeleteMsgPort(port);
        return result;
    }
    TimerBase = request->tr_node.io_Device;
    dst = (unsigned char *)malloc(65536);
    source = (unsigned char *)malloc(65536);
    if (dst && source) {
        memset(dst, 17, 65536);
        memset(source, 93, 65536);
        measure("C", dock_mix_half_c, dst, source);
        memset(dst, 17, 65536);
        measure("AMMX", dock_mix_half_ammx, dst, source);
        measure("RGB24 C", dock_rgba_to_rgb24_c, dst, source);
        measure("RGB24 AMMX", dock_rgba_to_rgb24_ammx, dst, source);
        measure_native("RGB565 decode C",dock_rgb565_to_rgba_c,dst,source);
        measure_native("RGB565 decode AMMX",dock_rgb565_to_rgba_ammx,dst,source);
        measure_native("RGB565 encode C",dock_rgba_to_rgb565_c,dst,source);
        measure_native("RGB565 encode AMMX",dock_rgba_to_rgb565_ammx,dst,source);
        memcpy(dst,source,65536);
        measure_equal("Equal C",dock_bytes_equal_c,dst,source);
        measure_equal("Equal AMMX",dock_bytes_equal_ammx,dst,source);
        memset(source,0,65536);
        for (i=0; i<32768; ++i) put565(dst+i*2,0xf81f);
        measure_native("RGB565 key clear C",dock_rgb565_key_c,dst,source);
        measure_native("RGB565 key clear AMMX",dock_rgb565_key_ammx,dst,source);
        memset(source,255,65536);
        measure_native("RGB565 key covered C",dock_rgb565_key_c,dst,source);
        measure_native("RGB565 key covered AMMX",dock_rgb565_key_ammx,dst,source);
        result = 0;
    }
    free(source);
    free(dst);
    CloseDevice(&request->tr_node);
    DeleteIORequest(&request->tr_node);
    DeleteMsgPort(port);
    return result;
}
