# Companion Observer - operator's guide

How to build, flash, configure and verify this firmware. For what it is and how
it works internally, see the [README](README.md).

Branch `companion-observer`, forked from `agessaman/mqtt-observer-plus` at
**`f35341c`** - diff against that commit to see our changes.

## Build and flash

### Pre-built images (no toolchain needed)

Merged firmware images for the three boards are attached to every
[release](https://github.com/BadCactus634/meshcore-companion-observer/releases).
To use one:

1. Download the file for your board — `…-tbeam-sx1262-…-merged.bin`,
   `…-heltec-v3-…-merged.bin` or `…-lilygo-t3s3-…-merged.bin`.
2. Open the [MeshCore web flasher](https://flasher.meshcore.co.uk/), connect the
   board over USB, and choose **custom firmware** instead of one of the listed
   builds.
3. Upload the file and flash.

These are *merged* images — bootloader, partition table and application in one
file written at offset `0x0` — which is what the flasher expects for a custom
build. Nothing is baked in: no WiFi credentials, no broker. Configure the node
over the USB console afterwards, as below.

> The **T3-S3** image changes the partition table (`min_spiffs`, which the stock
> T3-S3 companion builds do not use), so its first flash resets stored settings
> including the node identity. The T-Beam and Heltec V3 images keep the layout
> their stock builds already use.

Releases are produced by
[`.github/workflows/release-companion-observer.yml`](.github/workflows/release-companion-observer.yml),
which builds all three targets and attaches them when a `v*` tag is pushed.

### Building it yourself

Pick the target for your board:

| Board | Environment | Flash used |
|---|---|---|
| LilyGO T-Beam SX1262 | `Tbeam_SX1262_companion_radio_wifi_mqtt` | 81.4 % of 1.875 MB |
| Heltec WiFi LoRa 32 V3 | `Heltec_v3_companion_radio_wifi_mqtt` | 43.1 % of 3.19 MB |
| LilyGO T3-S3 | `LilyGo_T3S3_companion_radio_wifi_mqtt` | 69.8 % of 1.875 MB |

```bash
pio run -e <environment>
pio run -t upload -e <environment>
pio device monitor -b 115200
```

A plain upload keeps the device's stored settings — none of these targets change
the partition layout relative to the other MeshCore builds for the same board, so
there is nothing to migrate.

The T-Beam variant also gained `Tbeam_SX1262_companion_radio_wifi`: the same WiFi
companion without any MQTT. Upstream ships that env for the S3 Supreme but not
for the classic T-Beam.

On boards with a display, one line shows the uplink state — `MQTT: off`,
`WiFi: connecting`, or the IP address followed by the number of connected
brokers — so you can check it without plugging in a serial cable.

### Flashing with a full erase

Wipes bootloader, partition table, NVS and filesystem:

```bash
pio run -t erase  -e Tbeam_SX1262_companion_radio_wifi_mqtt
pio run -t upload -e Tbeam_SX1262_companion_radio_wifi_mqtt
```

The upload rewrites bootloader, partition table, `boot_app0` and firmware, so
this is safe - but **it destroys the node's identity**. The private key lives in
the filesystem, so the device generates a new one on next boot: new public key,
new observer ID in the analyzer, contacts have to re-add you. Export the key from
the phone app first if you want to keep it. (`set prv.key` belongs to the
repeater CLI and does nothing here - `saveIdentity()` is a stub in the companion
wiring.)

To clear only the stored configuration, without reflashing, use `rebuild`
(reformat, then rewrite identity and prefs) or `erase` (reformat only) from the
USB console.

### Porting to another board

No C++ changes. Everything board-specific - radio pins, LoRa chip, display, power
management - lives in the base section of that board's
`variants/<board>/platformio.ini`; this work sits at the role level. Copy one of
the three env blocks above into the other variant file and point `extends =` at
that board's base.

Two traps the three existing targets already show:

- **Check the partition table.** The T3-S3 env has to set
  `board_build.partitions = min_spiffs.csv` because `boards/t3_s3_v1_x.json`
  defaults to `default.csv` — a 1.25 MB app slot the image does not fit in. The
  T-Beam inherits `min_spiffs` from its variant base; the Heltec V3 has 8 MB and
  needs nothing.
- **Keep the env name short on Windows.** PlatformIO unpacks RadioLib's examples
  under `.pio/libdeps/<env-name>/`. With a deep project directory a long name
  pushes those paths past `MAX_PATH`, and the library installer loops forever on
  "cannot find the path specified" instead of failing usefully. That is why the
  T3-S3 env drops the `sx1262` infix its sibling targets carry.

Many boards already ship a `companion_radio_wifi` env - Heltec V3 and V4, Station
G2 and G3, Xiao S3 WIO, T-Beam Supreme, T-Beam 1W, T-LoRa V2.1, Thinknode M2 and
M5. Start from that one and add the MQTT parts: `WITH_MQTT_BRIDGE`,
`MQTT_MAX_PACKET_SIZE`, `MAX_MQTT_BROKERS`, the cert-bundle
`extra_scripts`/`embed_files`, `+<helpers/bridges/MQTTBridge.cpp>` in the source
filter, and the five MQTT `lib_deps`.

What genuinely differs per board:

- **ESP32 only.** The bridge needs FreeRTOS queues, the ESP32 `WiFi.h` and
  PsychicMqttClient (ESP-IDF). nRF52 boards (T1000-E, Wio Tracker L1, RAK4631,
  T-Echo) have no WiFi; RP2040 targets are out too.
- **Flash.** The image is ~1.6 MB, which only matters on the 4 MB boards (T-Beam
  SX1262, T3-S3) - and both already set `min_spiffs.csv` in their variant base.
  Heltec V3 defaults to `default_8MB.csv` (~3.3 MB app slot); 16 MB boards have
  far more.
- **PSRAM.** `MAX_MQTT_BROKERS=1` is a T-Beam limitation: each TLS/WSS connection
  wants ~40 KB of contiguous internal heap and this board has no PSRAM, so a
  second concurrent slot fails (`mbedtls_ssl_setup`). Raise it on T3-S3, Supreme
  or Xiao S3 to publish to several brokers at once.

## Configuration

Nothing is compiled in - no SSID, no broker, no credentials in the repo.
Everything is set at runtime and stored in `/mqtt_prefs` on the device.

The companion protocol runs over TCP port 5000 in this build, which leaves the
USB serial console free for the config CLI. Open it at **115200 baud**.

### Minimum to get running

```
set wifi.ssid MyHomeNetwork
set wifi.pwd  MyPassword
set mqtt.iata MXP                 # any code; a topic segment and a UI filter
set mqtt1.preset custom
set mqtt1.server mqtt://192.168.1.50:1883
set timezone Europe/Rome
reboot
```

Then `get wifi.status` and `get mqtt.status` to check.

> A `set` value is everything after the first space - do **not** quote it, quotes
> are stored literally. For an open network: `set wifi.pwd ` with nothing after
> the space.

### Which IATA code?

`mqtt.iata` is the three-letter **IATA airport code** of the large airport
nearest your node - the convention observers use to group a region. It is only a
topic segment and a filter in the analyzer's UI, so it does not have to be exact;
it just has to match what other observers in your area use, and what you put in
the analyzer's `defaultRegion`.

Look yours up on [Wikipedia's IATA airport code index](https://en.wikipedia.org/wiki/IATA_airport_code)
or IATA's own code search - or simply search "*your city* airport IATA code".
Milan Malpensa is `MXP`, Rome Fiumicino `FCO`, Bologna `BLQ`.

The value is stored uppercase automatically. Leaving it empty or setting it to
`XXX` means "not configured": the firmware treats that as unset and will not
publish until you give it a real code.

### TLS

Not a switch - it follows the URL scheme in `mqttN.server`:

| Scheme | Transport | Certificates |
|---|---|---|
| `mqtt://host:1883` | plain MQTT | not checked |
| `ws://host:9001/mqtt` | plain WebSocket | not checked |
| `mqtts://host:8883` | MQTT over TLS | verified against the bundle |
| `wss://host:443/mqtt` | WebSocket Secure | verified against the bundle |

For a broker on your own LAN, `mqtt://` is the right choice.

### Publishing this node's own transmissions

```
set mqtt.tx advert     # default: only our own adverts
set mqtt.tx on         # everything this node transmits
set mqtt.tx off        # nothing
```

### Other commands

```
set mqtt.origin <name>          set mqtt.rx on|off
set mqtt.packets on|off         set mqtt.raw on|off
set mqtt.interval <minutes>     set mqtt.ntp <hostname>
set mqtt1.username <u>          set mqtt1.password <p>
set mqtt1.topic <template>      get mqtt.presets
```

Topic template placeholders: `{iata}`, `{device}`, `{token}`, `{type}`. Custom
slots default to `meshcore/{iata}/{device}/{type}`.

This CLI is shared with the repeater firmware, so it also accepts
repeater-oriented commands. Those that make no sense on a companion answer
`Not supported` - the companion's own settings belong to the phone app.

## Analyzer

Install and configure your analyzer from its own documentation -
[CoreScope](https://github.com/Kpa-clawbot/CoreScope) is the one this was built
against. The only thing that has to line up on both sides is the topic: the
default `meshcore/{iata}/{device}/{type}` is already what CoreScope subscribes to
(`meshcore/+/+/packets`), and it is the format `meshcoretomqtt` produces, so
other consumers of that feed work too.

[`deploy/corescope-portainer-stack.yml`](deploy/corescope-portainer-stack.yml) is
a ready-made Portainer stack if you want one - it publishes the MQTT port (the
upstream example does not) and turns off HTTPS, since it is meant to stay on the
LAN.

## Verifying a new install

Builds clean; **not yet verified on hardware**.

1. Flash, open the console at 115200, run the configuration above.
2. `get wifi.status` reports an IP.
3. `mosquitto_sub -h <broker> -t 'meshcore/#' -v` shows one message per heard
   packet, with a populated `raw` field.
4. The analyzer lists the observer, with no decode errors in its ingestor log
   (those would mean malformed hex).
5. The phone app connects to `<device-ip>:5000` and works normally - contacts,
   messages, channels - while MQTT keeps publishing.
6. Trigger an advert and confirm this node appears in the analyzer.
7. Leave it running 24h: no watchdog reboots, no clock-skew warnings.

## Known limits

- **WiFi means no BLE.** Upstream picks one companion transport at compile time
  (`#ifdef WIFI_SSID / #elif BLE_PIN_CODE` in `main.cpp`). Off the WiFi network,
  the app cannot reach the node.
- **A companion does not repeat.** It observes; it does not extend coverage.
- **One node hears one place.** Real coverage needs several observers on the same
  broker.
- **Keep it on USB power.** WiFi inhibits sleep (`board.setInhibitSleep(true)`),
  so the battery is a backup, not the supply.
