# Poor Man's Concatenator

Compile it. Point it at a voice bank. Give it some words. Get a WAV back. That's the whole pitch.

---

## Building

```sh
cc -o pmc main.c
```

or if you're a C89 purist with strong opinions:

```sh
cc -ansi -o pmc mainc89.c
```

---

## Quick start

```sh
# Say something
./pmc -vb voice_bank_4b.vbc -o out.wav -i "hello world"

# Read from a file
./pmc -vb voice_bank_4b.vbc -o out.wav -i speech.txt

# Read from stdin, feel very unix about it
echo "hello world" | ./pmc -vb voice_bank_4b.vbc -o out.wav -i -
```

There's an `examples/example.wav` in the repo if you want to hear what you're getting into idrk

---

## Voice banks

Three voice banks are included, differing only in how aggressively the audio is packed. Pick your poison:

| File | Bit depth |
|---|---|
| `voice_bank_1b.vbc` | 1-bit |
| `voice_bank_2b.vbc` | 2-bit |
| `voice_bank_4b.vbc` | 4-bit |
Voice banks 6-16 refused to upload :(

Each bank has 3408 entries. The sweet spot is somewhere in the middle, probably idk xd. Lower bit depths = smaller files but more "crunch." Higher bit depths = larger files but smoother audio. 4-bit is usually the best balance for me lol

---

## Help text

```
Usage: pmc [options]

========================================
 Poor Man's Concatenator (Generator & PMC Concatenator)
========================================

MODES
-----
  1. Concatenate (Default)
     Reads a .vbc file, tokenizes input text, and stitches audio to a WAV file.
     Required: -vb <bank.vbc> -o <out.wav> -i <text|file|->

  2. Generate (-g flag)
     Builds a .vbc file from SAM phonemes or pre-existing WAV files.
     Required: -g -o <bank.vbc> (-sg <map> | -cvb <dir> <map>)

COMMON OPTIONS
--------------
  -v          Verbose output.
  -werror     Treat warnings as fatal errors.
  -iwarn      Ignore (suppress) all warnings.
  -h / -help  Print this help and exit.

CONCATENATE OPTIONS (Default Mode)
----------------------------------
  -vb <file.vbc>       Path to input .vbc voice bank.
  -o  <output.wav>     Path for output WAV file.
  -i  <text|file|->    Input text to synthesize. Use '-' for stdin.
  -wsm <float>         Word silence in ms (default: 50.0).
  -psm <float>         Phoneme silence in ms (default: 0.0).
  -obd <int>           Output WAV bit depth: 8 or 16 (default: 2).

  BOUNDARY SMOOTHING
  -cf  <float>         Crossfade duration in ms for voiced segment boundaries
                       (default: 0.0 = disabled).
                       When enabled, also activates:
                         * Zero-Crossing Editing: cuts are snapped to the
                           nearest zero-crossing (within half the cf window)
                           so there is no sudden DC jump at the edit point.
                         * Pitch Smoothing: the boundary region of the higher-
                           pitched segment is fractionally resampled to match
                           the pitch of its neighbour, eliminating beating.
                       Silence segments are never crossfaded.

GENERATE OPTIONS (-g Mode)
--------------------------
  -g                   Enable Generation mode.
  -o  <output.vbc>     Path for output .vbc file.
  -sg <map_file>       Synthesize phonemes using SAM. Map: KEY:SAM_PHONETIC_STRING.
                       Requires 'sam' (or 'sam.exe' on Windows) in current dir.
  -cvb <wav_dir> <map> Pack existing WAVs. Map: KEY:WAV_FILENAME.
  -bd  <int>           Packed audio bit depth (default: 4). Supported: 1 2 4 6 8 10 12 14 16.
  -speed <int>         SAM speech speed (default: 100).

EXAMPLES
--------
  Basic concatenation:
    pmc -vb bank.vbc -o out.wav -i "hello world"

  With crossfade + ZCR + pitch smoothing:
    pmc -vb bank.vbc -o out.wav -i "hello world" -cf 10

  Generate (SAM):
    pmc -g -o bank.vbc -sg phonemes.map

  Generate (WAVs):
    pmc -g -o bank.vbc -cvb ./wavs words.map -bd 4
```

---

## Rolling your own voice bank

You can build a `.vbc` from your own WAV files using a map file. The map format is dead simple:

```
key:filename.wav
hello:hello.wav
world:world.wav
how are you:howareyou.wav
```

Then:

```sh
./pmc -g -o mybank.vbc -cvb ./my_wavs mymap.map -bd 4
```

Or if you have SAM handy and want to synthesize directly from phonemes:

```sh
./pmc -g -o mybank.vbc -sg phonemes.map
```

SAM needs to be in the current directory as `./sam` (or `sam.exe` on Windows). The map file format is the same, just with SAM phonetic strings on the right side instead of filenames.

---

## License

MIT. See `LICENSE`.
