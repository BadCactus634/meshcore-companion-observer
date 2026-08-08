#pragma once

#include <Mesh.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Companion-side facade over MQTTBridge + the observer CLI.
 *
 * Why this exists: `struct NodePrefs` is declared twice in the tree with
 * different layouts — once in helpers/CommonCLI.h (repeater/room server) and
 * once in companion_radio/NodePrefs.h. MQTTBridge.h reaches CommonCLI.h through
 * BridgeBase.h, so any companion translation unit that included MQTTBridge.h
 * directly would hit a redefinition error.
 *
 * Everything that needs the repeater-side NodePrefs is therefore confined to
 * ObserverBridge.cpp, which never includes the companion's NodePrefs.h. This
 * header stays free of both, so MyMesh/main can talk to the bridge with no
 * collision. Radio parameters cross the boundary as plain scalars.
 */

namespace mesh {
  class MainBoard;
  class Radio;
  class Dispatcher;
}
class SensorManager;

/** Radio settings the MQTT status message reports, passed as scalars. */
struct ObserverRadioInfo {
  float   freq;
  float   bw;
  uint8_t sf;
  uint8_t cr;
};

class ObserverBridge {
public:
  /**
   * Wire up the bridge. Safe to call when WITH_MQTT_BRIDGE is undefined — the
   * whole class then compiles to no-ops.
   *
   * Does not connect: WiFi and broker come up from loop() once credentials are
   * present, so a device with no configuration yet still boots as a companion.
   */
  static void begin(mesh::MainBoard* board, mesh::Dispatcher* dispatcher, mesh::Radio* radio,
                    mesh::PacketManager* mgr, mesh::RTCClock* rtc, mesh::LocalIdentity* self_id,
                    SensorManager* sensors, const char* node_name, const ObserverRadioInfo& radio_info,
                    const char* firmware_ver, const char* build_date);

  static void loop();

  /** Keep the reported radio params in sync after the app changes them. */
  static void setRadioInfo(const ObserverRadioInfo& info);
  static void setNodeName(const char* name);

  /* ---- radio hooks, called from MyMesh ---- */

  /**
   * Stage the raw wire bytes of a received frame.
   *
   * Note the argument order differs from Dispatcher::logRxRaw(): the bridge
   * takes (raw, len, snr, rssi). Must be followed by onRx() for the same
   * packet with nothing in between — the staging slot holds one frame and is
   * consumed by the next enqueue.
   */
  static void onRxRaw(float snr, float rssi, const uint8_t raw[], int len);

  /** Enqueue a received packet, consuming the raw bytes staged by onRxRaw(). */
  static void onRx(mesh::Packet* pkt);

  /**
   * Enqueue a transmitted packet. The bridge re-serialises it, so `raw` reaches
   * the analyzer for TX exactly as it does for RX. Honours `mqtt.tx`
   * (on / advert / off) internally.
   */
  static void onTx(mesh::Packet* pkt);

  /**
   * One-line uplink status for the device display:
   *   "MQTT: off"         bridge never started
   *   "WiFi: connecting"  associating, no IP yet
   *   "<ip>  MQTT:<n>"    associated; n = brokers currently connected
   *
   * Approach borrowed from Dreikor17/MeshCore-Observer-Companion.
   */
  static void getStatusLine(char* buf, size_t buf_size);

  /* ---- serial console ---- */

  /**
   * Feed one line from the USB console to the observer CLI.
   * `reply` is filled with the response; returns false when the bridge is not
   * compiled in.
   */
  static bool handleCliLine(uint32_t sender_timestamp, char* line, char* reply);
};
