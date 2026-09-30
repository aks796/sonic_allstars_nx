# portlibs32

The 32-bit (AArch32) libraries the program links, not included here. Build
them and copy `include/` and `lib/` from their `prefix/` here:

* [mesa32](https://github.com/aks796/mesa32): Mesa (nouveau, EGL, GLES 1/2),
  libdrm_nouveau and libglapi, the AArch32 build.
* [ffmpeg32](https://github.com/aks796/ffmpeg32), FFmpeg for AArch32 (LGPL): the MOV demuxer and the H.264, AAC and MP3
  decoders, built with `-fno-short-enums`. Its default build has no H.264 or
  MP3, so build it with:

  ```bash
  FFMPEG_COMPONENTS="--enable-demuxer=mov --enable-decoder=h264 --enable-parser=h264 \
    --enable-decoder=aac --enable-parser=aac --enable-decoder=mp3 --enable-parser=mpegaudio" ./build.sh
  ```

Without Mesa the program builds with a null renderer (it runs, draws
nothing). Without FFmpeg it has no intro movie and no music.
