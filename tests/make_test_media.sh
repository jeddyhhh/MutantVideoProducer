#!/usr/bin/env bash
# Generates a corpus of short clips in many containers/codecs (modern, legacy
# and odd ones) for mvp_selftest. Requires an ffmpeg binary.
#
#   tests/make_test_media.sh [ffmpeg-binary] [output-dir]
set -u
FFMPEG="${1:-ffmpeg}"
OUT="${2:-testmedia}"
mkdir -p "$OUT"

V="-f lavfi -i testsrc2=size=640x360:rate=30:duration=4"
A="-f lavfi -i sine=frequency=440:sample_rate=48000:duration=4"
fail=0

gen() { # name, then ffmpeg output args
    local name="$1"; shift
    if "$FFMPEG" -hide_banner -loglevel error -y "$@" "$OUT/$name" </dev/null; then
        echo "made   $name"
    else
        echo "FAILED $name"; fail=$((fail+1))
    fi
}

# --- Modern ---------------------------------------------------------------
gen h264_aac.mp4            $V $A -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -shortest
gen hevc_opus.mkv           $V $A -c:v libx265 -preset ultrafast -c:a libopus -shortest
gen vp9_vorbis.webm         $V $A -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -c:a libvorbis -shortest
gen vp8_vorbis.webm         $V $A -c:v libvpx -deadline realtime -cpu-used 8 -c:a libvorbis -shortest
gen av1_flac.mkv            $V $A -c:v libsvtav1 -preset 12 -c:a flac -shortest
gen prores_pcm24.mov        $V $A -c:v prores_ks -profile:v 3 -c:a pcm_s24le -shortest
gen h264_10bit_422.mkv      $V $A -c:v libx264 -preset veryfast -pix_fmt yuv422p10le -c:a aac -shortest
gen hevc_10bit.mp4          $V $A -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -tag:v hvc1 -c:a aac -shortest
gen ffv1_pcm.nut            $V $A -c:v ffv1 -c:a pcm_s16le -shortest
gen mpeg2_mp2.ts            $V $A -c:v mpeg2video -c:a mp2 -shortest
gen h264_ac3.m2ts           $V $A -c:v libx264 -preset veryfast -c:a ac3 -f mpegts -shortest
gen mpeg2_pcm.mxf           $V $A -c:v mpeg2video -pix_fmt yuv422p -c:a pcm_s16le -shortest
# 4K clips for GPU / hardware-decoding benchmarks.
UHD="-f lavfi -i testsrc2=size=3840x2160:rate=30:duration=6"
gen uhd_h264.mp4            $UHD $A -c:v libx264 -preset ultrafast -b:v 50M -c:a aac -shortest
gen uhd_hevc10.mp4          $UHD $A -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -b:v 40M -c:a aac -shortest
gen png_alpha.mov           -f lavfi -i "testsrc2=size=320x240:rate=10:duration=3,format=rgba" -c:v png

# --- Legacy / odd -----------------------------------------------------------
# Old mobile phones: H.263 + AMR-NB in 3GP, MPEG-4 Part 2 + AAC in 3G2.
gen phone_h263_amr.3gp      $V $A -s 176x144 -r 15 -c:v h263 -c:a libopencore_amrnb -ar 8000 -ac 1 -b:a 12.2k -shortest
gen phone_mpeg4_aac.3g2     $V $A -s 176x144 -r 15 -c:v mpeg4 -c:a aac -ar 22050 -ac 1 -shortest
# Nintendo 3DS camera style: AVI, two MJPEG eye streams 480x240 @ 20 fps, IMA ADPCM.
gen 3ds_stereo_mjpeg.avi    $V $A -map 0:v -map 0:v -map 1:a -s 480x240 -r 20 -c:v mjpeg -q:v 3 -c:a adpcm_ima_wav -ar 16000 -ac 1 -shortest
gen divx3_msmpeg4.avi       $V $A -c:v msmpeg4 -c:a libmp3lame -shortest
gen xvid_mp3.avi            $V $A -c:v libxvid -c:a libmp3lame -shortest
gen mpeg1_mp2.mpg           $V $A -c:v mpeg1video -c:a mp2 -shortest
gen dvd_mpeg2_ac3.vob       $V $A -s 720x480 -r 30000/1001 -c:v mpeg2video -c:a ac3 -f dvd -shortest
gen dv_ntsc.dv              $V $A -s 720x480 -r 30000/1001 -pix_fmt yuv411p -c:v dvvideo -c:a pcm_s16le -ac 2 -shortest
gen flash_flv1_mp3.flv      $V $A -c:v flv1 -c:a libmp3lame -ar 44100 -shortest
gen flash.swf               $V $A -c:v flv1 -c:a libmp3lame -ar 44100 -shortest
gen windows_wmv2_wma.wmv    $V $A -c:v wmv2 -c:a wmav2 -shortest
gen windows_msvideo1.avi    $V -s 320x240 -c:v msvideo1
gen quicktime_cinepak.mov   $V $A -s 320x240 -c:v cinepak -c:a adpcm_ima_qt -shortest
gen quicktime_svq1.mov      $V $A -s 320x240 -c:v svq1 -c:a pcm_s16be -shortest
gen quicktime_animation.mov $V -s 320x240 -c:v qtrle
gen realmedia_rv20.rm       $V $A -s 320x240 -c:v rv20 -c:a ac3 -shortest
gen theora_vorbis.ogv       $V $A -c:v libtheora -c:a libvorbis -shortest
gen idroq.roq               $V $A -s 320x240 -r 30 -c:v roq -c:a roq_dpcm -ar 22050 -ac 1 -shortest
gen h261.h261               $V -s 352x288 -c:v h261 -f h261
gen raw_h264.h264           $V -c:v libx264 -preset veryfast -f h264
gen raw_mjpeg.mjpeg         $V -c:v mjpeg -f mjpeg
gen y4m_raw.y4m             $V -t 1 -pix_fmt yuv420p
gen animated.gif            $V -s 320x180 -r 10
gen animated.apng           $V -s 320x180 -r 10 -plays 0
gen smjpeg.mjpg             $V $A -s 320x240 -c:v mjpeg -c:a pcm_s16le -f smjpeg -shortest

# --- Stills -----------------------------------------------------------------
gen still.png               $V -frames:v 1
gen still.jpg               $V -frames:v 1
gen still.bmp               $V -frames:v 1
gen still.tiff              $V -frames:v 1
gen still.webp              $V -frames:v 1
gen still_portrait.png      -f lavfi -i "testsrc2=size=360x640" -frames:v 1

# --- Audio only ---------------------------------------------------------------
# Test signals for the audio effects self-test.
gen audio_sine_loud.wav     -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t):s=48000:d=4" -c:a pcm_s16le
gen audio_noise.wav         -f lavfi -i "anoisesrc=color=white:amplitude=0.05:r=48000:d=4" -ac 2 -c:a pcm_s16le
gen audio_sine_noise.wav    -f lavfi -i "aevalsrc=0.25*sin(2*PI*440*t)+0.04*(random(0)*2-1)|0.25*sin(2*PI*440*t)+0.04*(random(1)*2-1):s=48000:d=4" -c:a pcm_s16le
gen audio.mp3               $A -c:a libmp3lame
gen audio.wav               $A -c:a pcm_s16le
gen audio.flac              $A -c:a flac
gen audio.ogg               $A -c:a libvorbis
gen audio.opus              $A -c:a libopus
gen audio.aac               $A -c:a aac
gen audio_alac.m4a          $A -c:a alac
gen audio.wma               $A -c:a wmav2
gen audio.ac3               $A -c:a ac3
gen audio.aiff              $A -c:a pcm_s16be
gen audio.au                $A -c:a pcm_s16be
gen audio.caf               $A -c:a pcm_s16le
gen audio.wv                $A -c:a wavpack
gen audio.tta               $A -c:a tta
gen audio.mp2               $A -c:a mp2
gen audio.voc               $A -c:a pcm_u8 -ar 22050 -ac 1
gen audio_amrnb.amr         $A -c:a libopencore_amrnb -ar 8000 -ac 1 -b:a 12.2k
gen audio_gamecube.ast      $A -c:a pcm_s16be_planar
gen audio_mp3_with_cover.mp3 $A -f lavfi -i "testsrc2=size=300x300" -map 0:a -map 1:v -frames:v 1 -c:a libmp3lame -c:v mjpeg -disposition:v attached_pic -shortest

# HDR as phones and HDR10 masters tag it: 10-bit HEVC in BT.2020 with PQ or HLG.
gen hdr_pq.mkv              $V -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -x265-params colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:log-level=error
gen hdr_hlg.mp4             $V -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc -x265-params colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc:log-level=error -tag:v hvc1
# Interlaced: a moving picture's 50 fields a second woven into 25 frames, flagged top field first; and the same weave
# saved as plain progressive frames (what it looks like when nothing deinterlaces it).
FIELDS="-f lavfi -i testsrc2=size=720x576:rate=50:duration=4"
gen interlaced_mpeg2.ts     $FIELDS -vf tinterlace=interleave_top,setfield=tff -c:v mpeg2video -flags +ilme+ildct -top 1 -b:v 12M
gen woven_progressive.mkv   $FIELDS -vf tinterlace=interleave_top,setfield=prog -c:v ffv1
# Phone video with rotation metadata (portrait recording).
"$FFMPEG" -hide_banner -loglevel error -y -display_rotation 90 -i "$OUT/h264_aac.mp4" -c copy "$OUT/phone_rotated.mp4" \
    && echo "made   phone_rotated.mp4" || { echo "FAILED phone_rotated.mp4"; fail=$((fail+1)); }

echo "done, $fail failed"
