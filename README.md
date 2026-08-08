# MeshCore Companion Observer - standalone, no Raspberry Pi

A custom [MeshCore](https://meshcore.co.uk/) firmware for ESP32 LoRa boards that
does two jobs at once: it is a **normal companion radio** you connect to from the
MeshCore phone app over your home WiFi, **and** an **observer node** that records
every packet it hears (and every packet it sends) to an MQTT broker, where a
web dashboard turns it into a live map, a searchable packet feed and per-node
statistics.

If you know Meshtastic, this firmware can be used to populate CoreScope (the MeshCore equivalent of Malla).

---

## What problem does this solve?

MeshCore firmware comes in a few flavours (companion, repeater and room server).
Turning a node into an *observer* — one that reports mesh traffic for analysis —
normally costs you two things. This firmware costs you neither.

### You keep using the node from your phone

Nearly every observer firmware is built on the repeater or room-server role,
which turns the node into infrastructure: **you can no longer use it from your
phone.** So you need two devices, or you give up one for the other.

Here the node stays a companion. Your phone connects to it over WiFi and it
behaves like any other companion radio — contacts, direct messages, channels,
everything — while it quietly reports what it hears in the background.

### No computer attached to the radio

The usual observer setup is a chain: the node is flashed with packet logging,
a **USB cable** runs from it to a Raspberry Pi or a mini PC, and a Python bridge
(`meshcoretomqtt`) on that machine reads the serial output and forwards it to
MQTT. The radio has to live wherever the computer is, and if the computer sleeps,
reboots or the cable is knocked out, you stop collecting.

This firmware does that job itself. It joins your WiFi and talks MQTT directly,
so the node needs **nothing but a USB power supply** — a phone charger in the
attic, by a window, in the garden, wherever the antenna does best. No cable to a
computer, no Python, no Raspberry Pi.

You still need somewhere to run the dashboard that *displays* the data, but that
is any machine on your network — a NAS, a mini PC, the computer you already
have — and it does not have to be anywhere near the radio. It can even be
somewhere else entirely, or you can publish to one of the public community
brokers and run nothing at all.

Nothing leaves your house unless you choose it to: point the node at your own
broker and the data stays on your LAN.

## What you need

| | |
|---|---|
| **A board** | Ready-made targets for **LilyGO T-Beam SX1262**, **Heltec WiFi LoRa 32 V3** and **LilyGO T3-S3**. Any other **ESP32** MeshCore board is one config block away, no code changes — see [OBSERVER.md](OBSERVER.md#porting-to-another-board). nRF52 boards (T1000-E, Wio Tracker L1, T-Echo, RAK4631) have no WiFi and cannot run this. |
| **WiFi and USB power** | That is the whole hardware requirement for the node itself. |
| **Somewhere to run the dashboard** | Any machine on your network with Docker. Not attached to the radio, and not needed at all if you publish to a public broker. |

## How it fits together

```
   LoRa mesh                  your home WiFi             your server (if you setup one)
 ┌───────────┐               ┌──────────────┐                 ┌──────────────┐
 │ other     │  ))))  ────▶  │  this node   │  ── MQTT ── ▶  │  CoreScope   │
 │ nodes     │               │              │                 │  (Docker)    │
 └───────────┘               └──────┬───────┘                 └──────┬───────┘
                                    │                                │
                                phone app                       web browser
                            (TCP, port 5000)                   (maps, stats)
```

## Getting started

1. **Flash the firmware** - [OBSERVER.md § Build](OBSERVER.md#build)
2. **Configure it** by typing commands in the node's CLI over USB - WiFi name and
   password, and where your broker lives. Nothing is baked into the firmware, so
   no passwords ever end up in this repository.
3. **Run the analyzer** - [deploy/corescope-portainer-stack.yml](deploy/corescope-portainer-stack.yml)
   is a ready-made Portainer/Docker Compose stack.
4. **Open the dashboard** in a browser and **point the phone app** at the node's
   IP address on port 5000.

Full walkthrough in **[OBSERVER.md](OBSERVER.md)**.

## What you get to see

The analyzer ([CoreScope](https://github.com/Kpa-clawbot/CoreScope)) decodes what
the node reports and shows a live packet feed, a map with signal traces, decoded
channel chat, per-node statistics and health, and packet tracing across multiple
observers if you run more than one.

Two honest limits worth knowing before you start:

- **You only see what your antenna hears.** One node is one listening post. Real
  coverage maps need several observers reporting to the same broker.
- **Encrypted stays encrypted.** Channel messages are only readable if you supply
  that channel's key. Everything else remains metadata - packet type, routing
  path, signal strength. Direct messages between other people are never readable.

---

# Technical details

## Relationship to upstream

This is a fork of [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore)
(MIT). The MQTT machinery is **not new code**: `MQTTBridge`,
`MQTTMessageBuilder`, `JWTHelper` and the observer CLI are reused as-is from
[agessaman/MeshCore](https://github.com/agessaman/MeshCore), branch
`mqtt-observer-plus`, which is where the community observer firmwares come from.

What this fork adds is the wiring: the same bridge, driven from the **companion**
role instead of the repeater. Work lives on the `companion-observer` branch.

### Prior art

[Dreikor17/MeshCore-Observer-Companion](https://github.com/Dreikor17/MeshCore-Observer-Companion)
got to the same idea first, in June 2026, and arrived at the same structure —
including an identical fix for the `NodePrefs` clash described below. Worth
reading; it also has an OLED status line this build lacks.

The two differ in what the companion side can do. That project is a **USB**
companion: WiFi carries the MQTT uplink only, and the phone connects by cable.
Its configuration is compile-time (`OBSERVER_WIFI_SSID` and friends as build
flags), with runtime configuration listed on its roadmap.

Here the companion protocol runs over **TCP port 5000**, so the phone app
connects over WiFi, and *all* configuration is runtime over the USB console —
which the TCP transport leaves free. Target board is the T-Beam SX1262 rather
than the Heltec V3.

| Remote | Points at | Role |
|---|---|---|
| `origin` | this repository | published work |
| `upstream` | `meshcore-dev/MeshCore` | canonical firmware, rebase base |
| `agessaman` | `agessaman/MeshCore` | source of the MQTT bridge |

Upstream's own README is preserved as [`README.upstream.md`](README.upstream.md).

## What was changed

**Three radio hooks**, overridden in `examples/companion_radio/MyMesh.cpp`:

| Hook | Bridge call | Purpose |
|---|---|---|
| `logRxRaw(snr, rssi, raw, len)` | `storeRawRadioData(raw, len, snr, rssi)` | stage the wire bytes |
| `logRx(pkt, len, score)` | `onPacketReceived(pkt)` | enqueue RX |
| `logTx(pkt, len)` | `sendPacket(pkt)` | enqueue TX |

Transmitted packets have no raw bytes off the radio, so the bridge re-serialises
them - the analyzer sees this node's own traffic exactly as it sees everyone
else's. Controlled by `set mqtt.tx on|advert|off`.

Publishing never happens on the radio path. The bridge hands packets to a
FreeRTOS queue drained by its own task, so a slow or unreachable broker cannot
disturb channel-activity detection or transmit scheduling.

**`examples/companion_radio/ObserverBridge.{h,cpp}`** exists to work around a
name clash. `struct NodePrefs` is declared twice in the tree with different
layouts - once in `helpers/CommonCLI.h`, once in
`examples/companion_radio/NodePrefs.h` - and `MQTTBridge.h` reaches the first
through `BridgeBase.h`. Any companion file including the bridge directly fails to
compile. `ObserverBridge.cpp` is the single translation unit that sees both
worlds; its header exposes static functions and a plain POD, so `MyMesh.cpp` and
`main.cpp` never meet both structs.

**Configuration is runtime-only.** In this build the companion protocol runs over
TCP, which frees the USB console for the shared observer CLI (`set wifi.ssid`,
`set mqtt1.server`, `get mqtt.status`, …). Repeater-oriented commands that reach
the same CLI answer `Not supported`.

**Uplink status on the display.** Boards with a screen show one line carrying the
IP address and the number of connected brokers, so the uplink can be checked
without a serial cable. Borrowed from Dreikor17's project, credited above.

**Two upstream-fork defects fixed**, both of which broke *every* companion target
in `mqtt-observer-plus`:

- `MyMesh::getCADEnabled()` defined twice in `companion_radio/MyMesh.cpp`.
- `MQTTMessageBuilder.cpp` has no `WITH_MQTT_BRIDGE` guard while `arduino_base`
  globs `helpers/*.cpp`, so targets without `JChristensen/Timezone` in
  `lib_deps` failed to compile.

## Build targets

Three ready-made observer targets, all built and measured:

| Environment | Board | Flash | App slot | Used |
|---|---|---|---|---|
| `Tbeam_SX1262_companion_radio_wifi_mqtt` | T-Beam SX1262 | 1 601 065 B | 1.875 MB | 81.4 % |
| `Heltec_v3_companion_radio_wifi_mqtt` | Heltec WiFi LoRa 32 V3 | 1 438 881 B | 3.19 MB | 43.1 % |
| `LilyGo_T3S3_companion_radio_wifi_mqtt` | LilyGO T3-S3 | 1 371 709 B | 1.875 MB | 69.8 % |

The T-Beam image is the largest because that variant also pulls in XPowersLib and
MicroNMEA for its PMU and GPS.

Two details worth knowing if you add a target of your own:

- **T3-S3 needs an explicit partition override.** `boards/t3_s3_v1_x.json`
  defaults to `default.csv`, a 1.25 MB app slot this image does not fit in, so
  the env sets `min_spiffs.csv`. The T-Beam gets the same table from its variant
  base; the Heltec V3 has 8 MB and needs nothing.
- **Keep env names short on Windows.** PlatformIO unpacks RadioLib's examples
  under `.pio/libdeps/<env-name>/`, and with a deep project directory a long name
  pushes those paths past `MAX_PATH`, leaving the library installer looping on
  "cannot find the path specified". That is why the T3-S3 env drops the `sx1262`
  infix its siblings carry.

`MAX_MQTT_BROKERS` is 1 on the T-Beam, which has no PSRAM: each TLS/WSS
connection needs roughly 40 KB of contiguous internal heap, so a second
concurrent slot fails. The T3-S3 has PSRAM and is set to 2.

There is also `Tbeam_SX1262_companion_radio_wifi` - a plain WiFi companion with
no MQTT, which upstream ships for the S3 Supreme but not for the classic T-Beam.

## Status

Builds clean; **hardware verification is still outstanding** - the checklist is
in [OBSERVER.md § Status](OBSERVER.md#status).

## Licence

MIT, inherited from MeshCore. See [`license.txt`](license.txt).

*Keywords: MeshCore firmware, MeshCore observer node, MeshCore MQTT, MeshCore
WiFi companion, LoRa mesh network analyzer, packet sniffer, CoreScope, T-Beam,
Heltec, ESP32, self-hosted mesh monitoring.*