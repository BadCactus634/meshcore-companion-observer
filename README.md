# MeshCore Companion Observer — use your node *and* analyse the whole mesh

A custom [MeshCore](https://meshcore.co.uk/) firmware for ESP32 LoRa boards that
does two jobs at once: it is a **normal companion radio** you connect to from the
MeshCore phone app over your home WiFi, **and** an **observer node** that records
every packet it hears — and every packet it sends — to an MQTT broker, where a
web dashboard turns it into a live map, a searchable packet feed and per-node
statistics.

If you know Meshtastic, this is the MeshCore equivalent of running Malla or
MeshMap on your own node.

*Keywords: MeshCore firmware, MeshCore observer node, MeshCore MQTT, MeshCore
WiFi companion, LoRa mesh network analyzer, packet sniffer, CoreScope, T-Beam,
Heltec, ESP32, self-hosted mesh monitoring.*

---

## What problem does this solve?

MeshCore firmware comes in a few flavours. A **companion** is the radio your
phone talks to — you use it to chat. A **repeater** or **room server** sits
somewhere high up and forwards traffic for everyone else.

People who want to *analyse* their mesh — see which nodes are reachable, how far
signals travel, what the traffic looks like — normally flash an observer
firmware. But every observer firmware available today is built on the repeater or
room-server role. That means the node becomes infrastructure: **you can no longer
use it from your phone.** So you need two devices, or you give up one for the
other.

This firmware removes that trade-off. One board, one antenna:

- Your phone connects to it over WiFi and it behaves like any other companion —
  contacts, direct messages, channels, everything.
- At the same time it quietly reports what it hears to your own server, so you
  get the maps and statistics too.

Nothing leaves your house unless you tell it to: the analyzer runs in Docker on
your own machine, and the node talks to it over your LAN.

## What you need

| | |
|---|---|
| **A board** | LilyGO T-Beam with SX1262 radio. Any other **ESP32** MeshCore board works too — porting is one config block, no code changes, see [OBSERVER.md](OBSERVER.md#porting-to-another-board). nRF52 boards (T1000-E, Wio Tracker L1, T-Echo, RAK4631) have no WiFi and cannot run this. |
| **WiFi** | The node stays on your home network and on USB power. |
| **A computer that stays on** | To run the analyzer — a NAS, a mini PC, a Raspberry Pi, anything with Docker. |

## How it fits together

```
   LoRa mesh                  your home WiFi                your server
 ┌───────────┐               ┌──────────────┐            ┌──────────────┐
 │ other     │  ))))  ────▶  │  this node   │  ── MQTT ▶ │  CoreScope   │
 │ nodes     │               │              │            │  (Docker)    │
 └───────────┘               └──────┬───────┘            └──────┬───────┘
                                    │                            │
                              phone app                    web browser
                            (TCP, port 5000)              (maps, stats)
```

## Getting started

1. **Flash the firmware** — [OBSERVER.md § Build](OBSERVER.md#build)
2. **Configure it** by typing a handful of commands over USB — WiFi name and
   password, and where your broker lives. Nothing is baked into the firmware, so
   no passwords ever end up in this repository.
3. **Run the analyzer** — [deploy/corescope-portainer-stack.yml](deploy/corescope-portainer-stack.yml)
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
  that channel's key. Everything else remains metadata — packet type, routing
  path, signal strength. Direct messages between other people are never readable.

---

# Technical details

## Relationship to upstream

This is a fork of [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore)
(MIT). The MQTT machinery is **not new code**: `MQTTBridge`,
`MQTTMessageBuilder`, `JWTHelper` and the observer CLI are reused as-is from
[agessaman/MeshCore](https://github.com/agessaman/MeshCore), branch
`mqtt-observer-plus`, which is where the community observer firmwares come from.

What this fork adds is the wiring that was missing: the same bridge, driven from
the **companion** role instead of the repeater. Work lives on the
`companion-observer` branch.

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
them — the analyzer sees this node's own traffic exactly as it sees everyone
else's. Controlled by `set mqtt.tx on|advert|off`.

Publishing never happens on the radio path. The bridge hands packets to a
FreeRTOS queue drained by its own task, so a slow or unreachable broker cannot
disturb channel-activity detection or transmit scheduling.

**`examples/companion_radio/ObserverBridge.{h,cpp}`** exists to work around a
name clash. `struct NodePrefs` is declared twice in the tree with different
layouts — once in `helpers/CommonCLI.h`, once in
`examples/companion_radio/NodePrefs.h` — and `MQTTBridge.h` reaches the first
through `BridgeBase.h`. Any companion file including the bridge directly fails to
compile. `ObserverBridge.cpp` is the single translation unit that sees both
worlds; its header exposes static functions and a plain POD, so `MyMesh.cpp` and
`main.cpp` never meet both structs.

**Configuration is runtime-only.** In this build the companion protocol runs over
TCP, which frees the USB console for the shared observer CLI (`set wifi.ssid`,
`set mqtt1.server`, `get mqtt.status`, …). Repeater-oriented commands that reach
the same CLI answer `Not supported`.

**Two upstream-fork defects fixed**, both of which broke *every* companion target
in `mqtt-observer-plus`:

- `MyMesh::getCADEnabled()` defined twice in `companion_radio/MyMesh.cpp`.
- `MQTTMessageBuilder.cpp` has no `WITH_MQTT_BRIDGE` guard while `arduino_base`
  globs `helpers/*.cpp`, so targets without `JChristensen/Timezone` in
  `lib_deps` failed to compile.

## Build targets

`variants/lilygo_tbeam_SX1262/platformio.ini`:

| Environment | Flash | App slot used |
|---|---|---|
| `Tbeam_SX1262_companion_radio_wifi` | 1 244 097 B | 63.3 % |
| `Tbeam_SX1262_companion_radio_wifi_mqtt` | 1 600 849 B | 81.4 % |

Both use `min_spiffs.csv` (1.875 MB app slot) on this 4 MB board. The plain WiFi
companion target is also new — upstream ships one for the S3 Supreme but not for
the classic T-Beam.

`MAX_MQTT_BROKERS` is 1: the board has no PSRAM and each TLS/WSS connection needs
roughly 40 KB of contiguous internal heap, so a second concurrent TLS slot fails.

## Status

Builds clean; **hardware verification is still outstanding** — the checklist is
in [OBSERVER.md § Status](OBSERVER.md#status).

## Licence

MIT, inherited from MeshCore. See [`license.txt`](license.txt).
