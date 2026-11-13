#pragma once
#include "clipboard.h"
#include <string.h>
#include <stdlib.h>

static uint32_t clip_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void clip_put32(uint8_t *p, uint32_t n) {
    p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24);
}
// Bounded, uncompressed DIB only. No palette, masks or embedded profile parsing.
static int clip_valid_dib(const uint8_t *p, size_t n) {
    if (!p || n < 40 || n > PROBE_IMAGE_LIMIT || clip_u32(p)!=40) return 0;
    int32_t w=(int32_t)clip_u32(p+4), h=(int32_t)clip_u32(p+8);
    unsigned bits=p[14] | (unsigned)p[15]<<8;
    if (w<=0 || w>8192 || !h || h < -8192 || h>8192 || p[12]!=1 || p[13] ||
        (bits!=24 && bits!=32) || clip_u32(p+16) || clip_u32(p+32)) return 0;
    uint64_t stride=((uint64_t)w*bits+31)/32*4;
    return 40+stride*(h<0 ? -(int64_t)h : h)<=n;
}
// Normalize bounded V4/V5 and standard 32-bit bitfields into the wire's opaque
// 24-bit BI_RGB DIB. Never dereference linked profiles or arbitrary mask layouts.
static uint8_t *clip_normalize_dib(const uint8_t *p,size_t n,size_t *outSize) {
    *outSize=0;
    if(clip_valid_dib(p,n)){
        uint8_t *copy=(uint8_t *)malloc(n);if(copy){memcpy(copy,p,n);*outSize=n;}return copy;
    }
    if(!p || n<40 || n>PROBE_IMAGE_LIMIT)return NULL;
    uint32_t header=clip_u32(p),compression=clip_u32(p+16);
    int32_t w=(int32_t)clip_u32(p+4),h=(int32_t)clip_u32(p+8);
    unsigned bits=p[14]|(unsigned)p[15]<<8;
    if((header!=40 && header!=108 && header!=124) || header>n ||
       w<=0 || w>8192 || !h || h < -8192 || h>8192 || p[12]!=1 || p[13] ||
       (bits!=24 && bits!=32) || clip_u32(p+32) ||
       (compression!=0 && compression!=3 && compression!=6))return NULL;
    if(header==124 && (clip_u32(p+112) || clip_u32(p+116) ||
        clip_u32(p+56)==0x4c494e4b || clip_u32(p+56)==0x4d424544))return NULL;
    size_t offset=header;
    if(compression){
        if(bits!=32)return NULL;
        if(header==40)offset+=compression==6?16:12;
        if(offset>n || n<52 || clip_u32(p+40)!=0x00ff0000 ||
           clip_u32(p+44)!=0x0000ff00 || clip_u32(p+48)!=0x000000ff)return NULL;
        if((header>=108 || compression==6) && clip_u32(p+52)!=0 && clip_u32(p+52)!=0xff000000)return NULL;
    }
    uint64_t rows=h<0?-(int64_t)h:h,sourceStride=((uint64_t)w*bits+31)/32*4,
        targetStride=((uint64_t)w*24+31)/32*4,total=40+targetStride*rows;
    if(sourceStride*rows>n-offset || total>PROBE_IMAGE_LIMIT)return NULL;
    uint8_t *out=(uint8_t *)calloc(1,(size_t)total);if(!out)return NULL;
    clip_put32(out,40);clip_put32(out+4,(uint32_t)w);clip_put32(out+8,(uint32_t)h);
    out[12]=1;out[14]=24;clip_put32(out+20,(uint32_t)(targetStride*rows));
    for(size_t y=0;y<rows;y++)for(int32_t x=0;x<w;x++)
        memcpy(out+40+y*targetStride+(size_t)x*3,p+offset+y*sourceStride+(size_t)x*(bits/8),3);
    *outSize=(size_t)total;return out;
}
static int clip_has_png_signature(const uint8_t *p, size_t n) {
    static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    return p && n >= sizeof(signature) && !memcmp(p, signature, sizeof(signature));
}

// The local snapshot contains a PNG length followed by PNG and compatibility DIB.
// Each RDP request receives only the bytes of the format it asked for.
static size_t clip_image_png_size(const uint8_t *p, size_t n) {
    if (!p || n < 4) return 0;
    size_t pngSize = clip_u32(p);
    if (pngSize > PROBE_IMAGE_SEND_LIMIT || pngSize > n - 4 ||
        !clip_has_png_signature(p + 4, pngSize)) return 0;
    size_t dibSize = n - 4 - pngSize;
    return dibSize <= PROBE_IMAGE_SEND_LIMIT && clip_valid_dib(p + 4 + pngSize, dibSize)
        ? pngSize : 0;
}
#define CLIP_DESCRIPTOR_SIZE 592u
static uint64_t clip_file_size(const uint8_t *d) {
    return (uint64_t)clip_u32(d+64)<<32 | clip_u32(d+68);
}
// Flat regular files only. Names never become paths without native validation too.
static int clip_valid_files(const uint8_t *p, size_t n) {
    if (!p || n<4) return 0;
    uint32_t count=clip_u32(p); uint64_t total=0;
    if (!count || count>PROBE_FILE_COUNT || n!=4+(size_t)count*CLIP_DESCRIPTOR_SIZE) return 0;
    for (unsigned i=0;i<count;i++) {
        const uint8_t *d=p+4+i*CLIP_DESCRIPTOR_SIZE;
        if (!(clip_u32(d)&0x40) || (clip_u32(d+36)&0x410)) return 0;
        uint64_t size=clip_file_size(d);
        if(size>PROBE_FILE_LIMIT || total>PROBE_FILE_LIMIT-size)return 0;
        total+=size;
        unsigned len=0;
        for (;len<260;len++) {
            unsigned c=d[72+len*2] | (unsigned)d[73+len*2]<<8;
            if(!c) break;
            if(c<32 || c=='/' || c=='\\' || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|') return 0;
        }
        if(!len || len==260 || (d[72+(len-1)*2]=='.' && !d[73+(len-1)*2]) ||
           (d[72+(len-1)*2]==' ' && !d[73+(len-1)*2])) return 0;
        for(unsigned j=0;j<i;j++)
            if(!memcmp(d+72,p+4+j*CLIP_DESCRIPTOR_SIZE+72,520)) return 0;
    }
    return 1;
}
