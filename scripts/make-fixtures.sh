#!/bin/bash
# Generates reproducible audio fixtures for the Snake Puppy test suite.
# Requires ffmpeg (present on this machine). Creates Tests/Fixtures/ (not
# committed) with nested albums, tags, embedded artwork, duplicates and
# deliberately tricky files.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
FIX="$ROOT/Tests/Fixtures"
rm -rf "$FIX"
mkdir -p "$FIX/Library/Album A" "$FIX/Library/Album B" "$FIX/Library/Album B/Disc 2" "$FIX/Library/Duplicates" "$FIX/Playlists" "$FIX/Artwork" "$FIX/Broken"

FF=${FFMPEG:-ffmpeg}
$FF -hide_banner -loglevel error -f lavfi -i "color=c=0x1a2b3c:s=512x512:d=1" -frames:v 1 "$FIX/Artwork/cover.jpg"
$FF -hide_banner -loglevel error -f lavfi -i "color=c=0x8a3324:s=512x512:d=1" -frames:v 1 "$FIX/Artwork/cover2.jpg"

tone() { # freq duration
  echo "sine=frequency=$1:duration=$2:sample_rate=96000"
}

# FLAC 24/96 with full tags + embedded artwork
$FF -hide_banner -loglevel error -f lavfi -i "$(tone 440 2)" -i "$FIX/Artwork/cover.jpg" \
  -map 0:a -map 1:v -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 -ar 96000 -c:v copy -disposition:v attached_pic \
  -metadata title="Track One" -metadata artist="Test Artist" -metadata album_artist="Test Artist" \
  -metadata album="Album A" -metadata genre="Jazz" -metadata date="1975" -metadata track="1/3" -metadata disc="1" \
  "$FIX/Library/Album A/01 - Track One.flac"

# FLAC 16/44.1
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=523:duration=2:sample_rate=44100" \
  -c:a flac -sample_fmt s16 -ar 44100 \
  -metadata title="Track Two" -metadata artist="Test Artist" -metadata album_artist="Test Artist" \
  -metadata album="Album A" -metadata genre="Jazz" -metadata date="1975" -metadata track="2/3" \
  "$FIX/Library/Album A/02 - Track Two.flac"

# MP3 320 kbps
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=659:duration=2:sample_rate=44100" \
  -c:a libmp3lame -b:a 320k -ar 44100 \
  -metadata title="Track Three" -metadata artist="Test Artist" -metadata album_artist="Test Artist" \
  -metadata album="Album A" -metadata genre="Soul" -metadata date="1975" -metadata track="3/3" \
  "$FIX/Library/Album A/03 - Track Three.mp3"

# WAV 24/48
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=880:duration=2:sample_rate=48000" \
  -c:a pcm_s24le -ar 48000 \
  -metadata title="Rhythm Section" -metadata artist="Second Artist" -metadata album_artist="Second Artist" \
  -metadata album="Album B" -metadata genre="Funk" -metadata date="1981" -metadata track="1/4" \
  "$FIX/Library/Album B/01 - Rhythm Section.wav"

# AIFF 16/44.1
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=330:duration=2:sample_rate=44100" \
  -c:a pcm_s16be -ar 44100 \
  -metadata title="Big Room" -metadata artist="Second Artist" -metadata album_artist="Second Artist" \
  -metadata album="Album B" -metadata genre="Funk" -metadata date="1981" -metadata track="2/4" \
  "$FIX/Library/Album B/02 - Big Room.aiff"

# ALAC in M4A
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=196:duration=2:sample_rate=44100" \
  -c:a alac -ar 44100 \
  -metadata title="Slow Burn" -metadata artist="Second Artist" -metadata album_artist="Second Artist" \
  -metadata album="Album B" -metadata genre="Classic R&B" -metadata date="1981" -metadata track="3/4" \
  "$FIX/Library/Album B/03 - Slow Burn.m4a"

# AAC in M4A
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=294:duration=2:sample_rate=44100" \
  -c:a aac -b:a 192k -ar 44100 \
  -metadata title="Late Night" -metadata artist="Second Artist" -metadata album_artist="Second Artist" \
  -metadata album="Album B/Disc 2" -metadata genre="Modern R&B" -metadata date="2020" -metadata track="1/1" \
  "$FIX/Library/Album B/Disc 2/01 - Late Night.m4a"

# OGG Vorbis
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=392:duration=2:sample_rate=44100" \
  -ac 2 -c:a vorbis -strict experimental -ar 44100 \
  -metadata title="Pocket Groove" -metadata artist="Third Artist" -metadata album_artist="Third Artist" \
  -metadata album="Album B" -metadata genre="Electronic" -metadata date="2005" -metadata track="4/4" \
  "$FIX/Library/Album B/04 - Pocket Groove.ogg"

# Duplicate of the first FLAC (different filename) for duplicate detection
cp "$FIX/Library/Album A/01 - Track One.flac" "$FIX/Library/Duplicates/Track One (copy).flac"

# A 1 kHz test signal used by DSP measurement tests, plus a loud/clipping-prone file.
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=1000:duration=5:sample_rate=44100" \
  -c:a pcm_s24le -ar 44100 "$FIX/test-1k-44100.wav"
$FF -hide_banner -loglevel error -f lavfi -i "sine=frequency=60:duration=3:sample_rate=44100" \
  -af "volume=1.0" -c:a pcm_s24le -ar 44100 "$FIX/test-60hz-44100.wav"
$FF -hide_banner -loglevel error -f lavfi -i "aevalsrc=0.99*sin(2*PI*100*t)+0.99*sin(2*PI*1000*t)+0.99*sin(2*PI*6000*t):s=44100:d=2" \
  -c:a pcm_s24le -ar 44100 "$FIX/test-loud-complex.wav"

# A broken file for error-handling tests.
printf 'not an audio file' > "$FIX/Broken/broken.flac"
FLAC_BYTES=$(wc -c < "$FIX/Library/Album A/01 - Track One.flac" | tr -d ' ')
head -c $((FLAC_BYTES / 2)) "$FIX/Library/Album A/01 - Track One.flac" > "$FIX/Broken/truncated.flac"

# Playlists (absolute paths; tests rewrite them to the current fixture location).
cat > "$FIX/Playlists/fixture.m3u8" <<EOF
#EXTM3U
#EXTINF:2,Test Artist - Track One
$FIX/Library/Album A/01 - Track One.flac
#EXTINF:2,Test Artist - Track Two
$FIX/Library/Album A/02 - Track Two.flac
#EXTINF:2,Second Artist - Rhythm Section
$FIX/Library/Album B/01 - Rhythm Section.wav
EOF

cat > "$FIX/Playlists/fixture.pls" <<EOF
[playlist]
NumberOfEntries=2
File1=$FIX/Library/Album A/01 - Track One.flac
Title1=Track One
Length1=2
File2=$FIX/Library/Album B/01 - Rhythm Section.wav
Title2=Rhythm Section
Length2=2
Version=2
EOF

# DSD64 DSF/DFF fixtures generated by Python (ffmpeg cannot encode DSD).
python3 "$ROOT/scripts/make_dsd_fixtures.py" "$FIX"

# IR file for convolution tests (short exponentially decaying noise).
$FF -hide_banner -loglevel error -f lavfi -i "anoisesrc=d=0.25:c=pink:a=0.4:r=48000" \
  -af "afade=t=out:st=0:d=0.25" -c:a pcm_s16le -ar 48000 "$FIX/ir-short.wav"

echo "Fixtures written to $FIX"
find "$FIX" -type f | sed "s|$FIX/||" | sort
