# Companion Observer

A MeshCore companion that is also an MQTT observer. The phone app connects to it
over WiFi exactly as it would to any companion radio, and at the same time every
packet the radio hears — plus every packet it sends — is published to an MQTT
broker, where [CoreScope](https://github.com/Kpa-clawbot/CoreScope) decodes it
and serves a web UI (live feed, map, packet tracing, per-node analytics).

Existing MeshCore MQTT observers are all built on the repeater or room-server
roles, so you cannot use them from the app. This target is the companion.

## Repo layout

Three remotes, three roles:

| Remote | Points at | Used for |
|---|---|---|
| `origin` | `BadCactus634/meshcore-companion-observer` | this fork, where the work is published |
| `upstream` | `meshcore-dev/MeshCore` | the canonical firmware; base for rebases |
| `agessaman` | `agessaman/MeshCore` | source of `MQTTBridge` and the observer CLI |

Branch: `companion-observer`, forked from `agessaman/mqtt-observer-plus` at
**`f35341c`**. Use that commit when diffing our changes against the fork.

## Build

```bash
pio run -e Tbeam_SX1262_companion_radio_wifi_mqtt
pio run -t upload -e Tbeam_SX1262_companion_radio_wifi_mqtt
```

Board: LilyGO T-Beam with **SX1262** (ESP32 classic, 4 MB flash). The image uses
the `min_spiffs.csv` partition table, giving a 1.875 MB app slot.

There is also `Tbeam_SX1262_companion_radio_wifi` — the same companion over WiFi
but without any MQTT — which upstream ships for the S3 Supreme but not for this
board.

## Configuration

Nothing is compiled in: no SSID, no broker, no credentials live in the repo.
Everything is set at runtime and stored in `/mqtt_prefs` on the device.

In this build the companion protocol runs over TCP port 5000, which leaves the
USB serial console free — that is where the config CLI lives. Open it at
**115200 baud** and type commands followed by Enter.

### Minimum to get running

```bash
set wifi.ssid MyHomeNetwork
set wifi.pwd  MyPassword
set mqtt.iata MXP                    # any code; it is a topic segment + UI filter

set mqtt1.preset custom
set mqtt1.server mqtt://192.168.1.50:1883
set timezone Europe/Rome
reboot
```

Then check:

```bash
get wifi.status
get mqtt.status
```

> The value of a `set` is everything after the first space — do **not** quote it.
> Quotes are stored literally. For an open network: `set wifi.pwd ` with nothing
> after the space.

### TLS

TLS is not an on/off switch — it follows the URL scheme in `mqttN.server`:

| Scheme | Transport | Certificates |
|---|---|---|
| `mqtt://host:1883` | plain MQTT | not checked |
| `ws://host:9001/mqtt` | plain WebSocket | not checked |
| `mqtts://host:8883` | MQTT over TLS | verified against the bundle |
| `wss://host:443/mqtt` | WebSocket Secure | verified against the bundle |

For a Mosquitto on your LAN, `mqtt://` is the right choice.

`MAX_MQTT_BROKERS` is 1 in this target. The T-Beam has no PSRAM and each TLS/WSS
connection needs ~40 KB of contiguous internal heap, so a second concurrent TLS
slot is known to fail (`mbedtls_ssl_setup`).

### Publishing this node's own transmissions

On by default, restricted to self-originated adverts:

```bash
set mqtt.tx advert     # default: only our own adverts
set mqtt.tx on         # everything this node transmits
set mqtt.tx off        # nothing
```

TX packets have no raw bytes off the radio, so the bridge re-serialises them —
the `raw` field reaches the analyzer for TX exactly as it does for RX.

### Other useful commands

```bash
set mqtt.origin <name>          set mqtt.rx on|off
set mqtt.packets on|off         set mqtt.raw on|off
set mqtt.interval <minutes>     set mqtt.ntp <hostname>
set mqtt1.username <u>          set mqtt1.password <p>
set mqtt1.topic <template>      get mqtt.presets
```

Topic template placeholders: `{iata}`, `{device}`, `{token}`, `{type}`. Custom
slots default to `meshcore/{iata}/{device}/{type}`, which is already the layout
CoreScope subscribes to (`meshcore/+/+/packets`).

The observer CLI is shared with the repeater firmware, so it also accepts
repeater-oriented commands. Those that make no sense on a companion answer
`Not supported` — the companion's own settings belong to the phone app.

## Analyzer

```bash
docker run -d --name corescope --restart=unless-stopped \
  -p 8080:80 -p 1883:1883 \
  -v /path/to/data:/app/data \
  ghcr.io/kpa-clawbot/corescope:latest
```

`config.json`:

```json
{
  "port": 3000,
  "mqtt": { "broker": "mqtt://localhost:1883", "topic": "meshcore/+/+/packets" },
  "channelKeys": { "public": "8b3387e9c5cdea6ac9e5edbaa115cd72" },
  "defaultRegion": "MXP"
}
```

CoreScope only decrypts channels whose key is listed in `channelKeys`; everything
else stays as metadata (type, path, SNR, hash, routing). Direct messages are
never readable.

## How it works

Three virtual hooks on `Dispatcher` carry packets to the bridge. The companion's
`MyMesh` overrides all three:

| Hook | Bridge call | Purpose |
|---|---|---|
| `logRxRaw(snr, rssi, raw, len)` | `storeRawRadioData(raw, len, snr, rssi)` | stage the wire bytes |
| `logRx(pkt, len, score)` | `onPacketReceived(pkt)` | enqueue RX, consuming the staged bytes |
| `logTx(pkt, len)` | `sendPacket(pkt)` | enqueue TX |

Two things are easy to get wrong here. The argument order differs between the
hook and the bridge call. And the staging slot holds exactly one frame: it is
written on Core 1 and consumed by the next enqueue on the same core, with no
mutex, so `logRxRaw` must stay immediately before its matching `logRx`.

Publishing never happens on the radio path — the bridge hands packets to a
FreeRTOS queue drained by its own task, so a slow broker cannot disturb CAD or
TX scheduling.

### Why `ObserverBridge` exists

`struct NodePrefs` is declared twice in the tree with different layouts, once in
`helpers/CommonCLI.h` (repeater, room server) and once in
`examples/companion_radio/NodePrefs.h`. `MQTTBridge.h` reaches the first through
`BridgeBase.h`, so a companion file that included `MQTTBridge.h` would not
compile.

`ObserverBridge.cpp` is the only companion file that includes `CommonCLI.h` and
`MQTTBridge.h`, and it never includes the companion's `NodePrefs.h`. The header
exposes static functions and a plain `ObserverRadioInfo { freq, bw, sf, cr }`, so
`MyMesh.cpp` and `main.cpp` never see both structs at once. The bridge reads only
five fields off the repeater-side struct, which `ObserverBridge` keeps in sync.

## Fixes carried on top of the fork

Two defects in `agessaman/mqtt-observer-plus` blocked every companion target:

- `MyMesh::getCADEnabled()` was defined twice in `companion_radio/MyMesh.cpp`
  (bad merge).
- `arduino_base` globs `helpers/*.cpp`, which pulls in `MQTTMessageBuilder.cpp`.
  It has no `WITH_MQTT_BRIDGE` guard and hard-includes `<Timezone.h>`, so any
  target without that `lib_dep` fails to compile.

## Status

Builds clean. **Not yet verified on hardware** — see the checklist below.

1. Flash, open the console at 115200, run the configuration above.
2. `get wifi.status` reports an IP.
3. `mosquitto_sub -h <broker> -t 'meshcore/#' -v` shows one message per heard
   packet, with a populated `raw` field.
4. CoreScope lists the observer under `/api/observers`; no decode errors in the
   ingestor log (those would mean malformed hex).
5. The phone app connects to `<device-ip>:5000` and works normally — contacts,
   messages, channels — while MQTT keeps publishing.
6. Trigger an advert and confirm this node appears in CoreScope.
7. Leave it running 24h: no watchdog reboots, no clock-skew warnings in the UI.

## Known limits

- **WiFi means no BLE.** Upstream selects one companion transport at compile time
  (`#ifdef WIFI_SSID / #elif BLE_PIN_CODE` in `main.cpp`). Away from the WiFi
  network, the app cannot reach the node.
- **A companion does not repeat.** It observes; it does not extend coverage.
- **One node hears one place.** Real coverage needs several observers on the same
  broker.
- **Keep it on USB power.** WiFi inhibits sleep (`board.setInhibitSleep(true)`),
  so the battery is a backup, not the supply.
