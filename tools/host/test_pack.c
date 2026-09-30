/* Host test of source/ssr_pack.h: where the pack is in each file given. */
#include <stdio.h>
#include "../../source/ssr_pack.h"

int main(int argc, char **argv) {
  int bad = 0;
  for (int i = 1; i < argc; i++) {
    SsrPack p;
    if (ssr_pack_probe(argv[i], &p) != 0) {
      printf("%s: no pack\n", argv[i]);
      continue;
    }
    FILE *f = fopen(p.path, "rb");
    uint8_t h[8];
    ssr_read_at(f, p.pack_off, h, 8);
    uint32_t hdr = ssr_rd32(h + 2), count = ssr_rd16(h + 6);
    uint8_t v[8];
    ssr_read_at(f, p.video_off, v, 8);
    fclose(f);
    int ok = p.pack_len == SSR_PACK_SIZE && count == SSR_PACK_ENTRIES && !memcmp(v + 4, "ftyp", 4);
    bad += !ok;
    printf("%s: %s; pack at %llu (%u bytes, header %u, %u entries); intro at %llu (%u bytes, %.4s) %s\n", argv[i],
           p.how, (unsigned long long)p.pack_off, p.pack_len, hdr, count, (unsigned long long)p.video_off,
           p.video_len, (const char *)v + 4, ok ? "OK" : "UNEXPECTED");
  }
  return bad;
}
