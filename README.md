# serpantinum-audio

A small PipeWire audio processor for the [Serpantinum](https://github.com/ilyamiro/serpantinum) shell. It replaces EasyEffects and depends only on PipeWire.

It creates a virtual output called **Serpantinum** (`serpantinum_sink`), processes everything sent to it and plays the result on your real output device. The shell talks to it over a local socket, so changing a setting is instant.

## Features

- 10-band equalizer (32 Hz to 16 kHz, peaking filters, Q 1)
- Balance, mono and channel swap
- Compressor and look-ahead limiter in two modes, balanced and night
- Dynamic EQ, an adaptive tonal balance that follows the music
- Global bypass

When the daemon starts it becomes the default output and follows the device you pick: choosing another output moves the processed audio there. Your previous default is restored when it stops.

## Usage

```
serpantinum-audio daemon          run in the foreground
serpantinum-audio start           start in the background unless already running, wait until it answers
serpantinum-audio ctl <command>   send one command and print the reply
serpantinum-audio version         print the version
```

Without arguments it prints the usage and exits with status 64.

### Commands

| Command | Arguments |
| --- | --- |
| `set` | 10 band gains in dB, balance (-1 to 1), mono, swap, dynamics (`off`, `balanced`, `night`), bypass, and optionally dynamic EQ |
| `eq` | 10 band gains in dB |
| `band` | band number (1 to 10) and gain in dB |
| `balance` | -1 to 1 |
| `mono`, `swap`, `bypass`, `dyneq` | `1`/`0` or `true`/`false` |
| `dynamics` | `off`, `balanced` or `night` |
| `get` | prints the current settings as a `set` line |
| `status` | prints the sink, the current output device, the underrun count and the settings |
| `ping` | answers `ok` |
| `quit` | stops the daemon |

Every change replies `ok` or `error`. Example:

```bash
serpantinum-audio ctl set 2 1 0 0 0 0 0 1 2 3 0 0 0 off 0 1
```

### Files

| Path | Content |
| --- | --- |
| `$XDG_RUNTIME_DIR/serpantinum-audio.sock` | control socket |
| `$XDG_STATE_HOME/serpantinum/audio.state` | last settings, restored on start |
| `$XDG_STATE_HOME/serpantinum/audio.log` | log of the background daemon |
| `$XDG_RUNTIME_DIR/serpantinum/audio-target` | name and PipeWire id of the real output device |

## Dynamic EQ

The dynamic EQ listens to the last few seconds of audio and gently moves the tonal balance toward the typical balance of commercial recordings. It replaces the manual equalizer while it is on. It leaves well balanced music alone, never changes loudness and never touches the extreme bass or treble, where there is no reliable reference.

1. **Analysis.** A 16384-point FFT (about 170 ms hops) measures the stereo input in one-third-octave bands from 40 Hz to 16 kHz.
2. **Gating.** Following ITU-R BS.1770, blocks below -70 dB are ignored, and so are blocks more than 10 dB below the running average.
3. **Averaging.** The spectrum is averaged over about 8 seconds, shortening to 1.5 seconds when the content changes sharply, for example at a track change.
4. **Target.** Each band is compared with a target slope of -2.5 dB per octave. Only the shape matters, not the absolute level.
5. **Correction.** The difference passes through a smooth deadzone and a partial-strength correction, then through asymmetric limits.
6. **Realization.** Seventeen peaking filters, a half-octave apart, are fitted by regularized least squares so their combined response matches the wanted curve.
7. **Loudness.** A K-weighted makeup gain keeps the perceived loudness unchanged.
8. **Safety.** A look-ahead limiter at -0.1 dBFS only catches overs.

| Setting | Value | Basis |
| --- | --- | --- |
| Target slope | -2.5 dB per octave band | Average spectra of popular recordings fall at about 5 to 6 dB per octave in power density between 100 Hz and 10 kHz, 2 to 3 dB per octave in octave bands (Pestana et al., AES 135th Convention, 2013; Adrian and Bitzer, DAGA 2014). |
| Correction range | 100 Hz to 10 kHz, fading out to 50 Hz and 16 kHz | The range the published measurements cover. |
| Deadzone | 2.5 dB | Listeners detect narrow spectral changes from about 1 to 3 dB and wider ones from 3 to 6 dB. |
| Strength | 50% | A track's departure from the average is partly intentional. |
| Cut limit | 6 dB | Cuts are harder to hear than boosts. |
| Boost limit | 3 dB | Boosts are easier to notice, cost headroom and lift noise. |
| Bass preference | +1 dB below 80 Hz, fading to 0 at 250 Hz | A listening preference: studies on headphone and speaker targets favor a gentle bass lift. |
| Gate | -70 dB absolute, -10 dB relative | ITU-R BS.1770 gating. |
| Averaging | 8 s steady, 1.5 s on change | Longer than the 3 s EBU R128 short-term window, so drum fills do not move the balance. |
| Loudness match | K-weighted | ITU-R BS.1770 K-weighting. |

The constants are at the top of `src/dsp/dynamic_eq.h`.

## Building

You need a C++17 compiler, `make`, `pkg-config` and the PipeWire development files.

| Distribution | Packages |
| --- | --- |
| Arch Linux | `base-devel pipewire` |
| Fedora | `gcc-c++ make pkgconf-pkg-config pipewire-devel` |
| Void Linux | `gcc make pkg-config pipewire-devel` |
| Debian, Ubuntu | `build-essential pkg-config libpipewire-0.3-dev` |

```bash
make
make test
sudo make install
```

`make install` honours `PREFIX` (default `/usr/local`) and `DESTDIR`.

`make eval` builds a tool that runs the dynamic EQ over real music and reports how far it moved the balance:

```bash
ffmpeg -i track.mp3 -f f32le -ac 2 -ar 48000 - | ./eval
```

### Nix

```nix
environment.systemPackages = [ (import (builtins.fetchTarball "https://github.com/ilyamiro/serpantinum-audio/archive/master.tar.gz") { inherit pkgs; }) ];
```

The Serpantinum flake already includes it.

## Releases

Pushing a `v*` tag that matches `VERSION` builds `x86_64` and `aarch64` binaries on Ubuntu 24.04, so they run on any distribution with glibc 2.39 and PipeWire 0.3.49 or newer, and publishes them with a `SHA256SUMS` file. Each archive holds `usr/bin/serpantinum-audio` and the license.

## License

AGPL-3.0-only, like Serpantinum.
