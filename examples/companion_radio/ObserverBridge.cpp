#include "ObserverBridge.h"

#ifdef WITH_MQTT_BRIDGE

// IMPORTANT: this translation unit must never include companion_radio/NodePrefs.h.
// CommonCLI.h below declares its own `struct NodePrefs` with a different layout,
// and the two cannot coexist. See the comment in ObserverBridge.h.
#include <SPIFFS.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/RegionMap.h>
#include <helpers/TransportKeyStore.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/bridges/MQTTBridge.h>
#include <helpers/SensorManager.h>

static mesh::MainBoard*      s_board      = nullptr;
static mesh::Dispatcher*     s_dispatcher = nullptr;
static mesh::Radio*          s_radio      = nullptr;
static mesh::PacketManager*  s_mgr        = nullptr;
static mesh::RTCClock*       s_rtc        = nullptr;
static mesh::LocalIdentity*  s_self_id    = nullptr;

// Repeater-shaped prefs. MQTTBridge reads exactly five fields off this
// (freq, bw, sf, cr, disable_fwd) to fill the status message; CommonCLI needs
// the whole struct to exist. It is kept in sync with the companion's own prefs
// through setRadioInfo(), and is never persisted from here — CommonCLI writes
// /com_prefs and /mqtt_prefs, the companion writes /new_prefs, no overlap.
static NodePrefs          s_prefs;
static TransportKeyStore  s_key_store;
static RegionMap          s_region_map(s_key_store);
static ClientACL          s_acl;
static MQTTBridge*        s_bridge = nullptr;
static CommonCLI*         s_cli    = nullptr;

// FIRMWARE_VERSION / FIRMWARE_BUILD_DATE live in companion_radio/MyMesh.h, which
// this file must not include (it drags in the companion's NodePrefs). They are
// passed in from main.cpp instead so there is still one source of truth.
static const char* s_firmware_ver = "";
static const char* s_build_date   = "";

/**
 * Minimal CommonCLICallbacks for a companion.
 *
 * Only the observer surface (mqtt.*, wifi.*, timezone.*) has to work here; the
 * repeater-oriented commands that share the same CLI are stubbed out and report
 * "unsupported" rather than pretending. The companion's own settings stay under
 * the app protocol, which is where the phone expects them.
 */
class CompanionCLICallbacks : public CommonCLICallbacks {
public:
  void savePrefs() override { /* companion prefs are owned by MyMesh/DataStore */ }

  const char* getFirmwareVer() override { return s_firmware_ver; }
  const char* getBuildDate() override { return s_build_date; }
  const char* getRole() override { return "companion"; }

  bool formatFileSystem() override { return false; }

  void sendSelfAdvertisement(int, bool) override { }
  void updateAdvertTimer() override { }
  void updateFloodAdvertTimer() override { }

  void setLoggingOn(bool) override { }
  void eraseLogFile() override { }
  void dumpLogFile() override { }

  void setTxPower(int8_t) override { }

  void formatNeighborsReply(char* reply) override { strcpy(reply, "Not supported"); }
  void formatStatsReply(char* reply) override { strcpy(reply, "Not supported"); }
  void formatRadioStatsReply(char* reply) override { strcpy(reply, "Not supported"); }
  void formatPacketStatsReply(char* reply) override { strcpy(reply, "Not supported"); }

  mesh::LocalIdentity& getSelfId() override { return *s_self_id; }
  void saveIdentity(const mesh::LocalIdentity&) override { }

  void clearStats() override { }
  void applyTempRadioParams(float, float, uint8_t, uint8_t, int) override { }

  /* ---- the parts that actually matter ---- */

  bool isMqttBridgeRunning() override {
    return s_bridge && s_bridge->isRunning();
  }

  void restartBridge() override {
    if (!s_bridge || !s_bridge->isRunning()) return;
    s_bridge->end();
    applyMetadata();
    s_bridge->begin();
  }

  void restartBridgeSlot(int slot) override {
    if (!s_bridge || !s_bridge->isRunning()) return;
    s_bridge->setSlotPreset(slot, s_cli->getObserverPrefs()->mqtt_slot_preset[slot]);
  }

  bool syncMqttNtp() override {
    if (!s_bridge || !s_bridge->isRunning()) return false;
    // Marshal onto the MQTT task (Core 0); this runs on the CLI thread (Core 1).
    return s_bridge->requestForcedNtpSync();
  }

  bool runMqttNtpDiag(char* reply, size_t reply_size, bool verbose) override {
    if (!s_bridge || !s_bridge->isRunning()) return false;
    return s_bridge->ntpDiag(reply, reply_size, verbose);
  }

  static void applyMetadata() {
    if (!s_bridge) return;
    char device_id[65];
    mesh::Utils::toHex(device_id, s_self_id->pub_key, PUB_KEY_SIZE);
    s_bridge->setDeviceID(device_id);
    s_bridge->setFirmwareVersion(s_firmware_ver);
    s_bridge->setBoardModel(s_board->getManufacturerName());
    s_bridge->setBuildDate(s_build_date);
    s_bridge->setStatsSources(s_dispatcher, s_radio, s_board, nullptr);
  }
};

static CompanionCLICallbacks s_callbacks;

void ObserverBridge::begin(mesh::MainBoard* board, mesh::Dispatcher* dispatcher, mesh::Radio* radio,
                           mesh::PacketManager* mgr, mesh::RTCClock* rtc, mesh::LocalIdentity* self_id,
                           SensorManager* sensors, const char* node_name,
                           const ObserverRadioInfo& radio_info,
                           const char* firmware_ver, const char* build_date) {
  s_firmware_ver = firmware_ver;
  s_build_date   = build_date;
  s_board      = board;
  s_dispatcher = dispatcher;
  s_radio      = radio;
  s_mgr        = mgr;
  s_rtc        = rtc;
  s_self_id    = self_id;

  memset(&s_prefs, 0, sizeof(s_prefs));
  s_prefs.disable_fwd = 1;  // a companion never repeats
  setRadioInfo(radio_info);
  setNodeName(node_name);

  s_cli = new CommonCLI(*board, *rtc, *sensors, s_region_map, s_acl, &s_prefs, &s_callbacks);
  s_cli->loadPrefs(&SPIFFS);   // reads /com_prefs + /mqtt_prefs

  s_bridge = new MQTTBridge(&s_prefs, s_cli->getObserverPrefs(), mgr, rtc, self_id);
  CompanionCLICallbacks::applyMetadata();
  s_bridge->begin();
}

void ObserverBridge::loop() {
  if (s_bridge) s_bridge->loop();
}

void ObserverBridge::setRadioInfo(const ObserverRadioInfo& info) {
  s_prefs.freq = info.freq;
  s_prefs.bw   = info.bw;
  s_prefs.sf   = info.sf;
  s_prefs.cr   = info.cr;
}

void ObserverBridge::setNodeName(const char* name) {
  if (name) StrHelper::strzcpy(s_prefs.node_name, name, sizeof(s_prefs.node_name));
}

void ObserverBridge::onRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
  // Argument order is deliberately swapped relative to the Dispatcher hook.
  if (s_bridge) s_bridge->storeRawRadioData(raw, len, snr, rssi);
}

void ObserverBridge::onRx(mesh::Packet* pkt) {
  if (s_bridge) s_bridge->onPacketReceived(pkt);
}

void ObserverBridge::onTx(mesh::Packet* pkt) {
  if (s_bridge) s_bridge->sendPacket(pkt);
}

void ObserverBridge::getStatusLine(char* buf, size_t buf_size) {
  if (buf == nullptr || buf_size == 0) return;
  if (!s_bridge) {
    strncpy(buf, "MQTT: off", buf_size);
  } else if (WiFi.status() != WL_CONNECTED) {
    strncpy(buf, "WiFi: connecting", buf_size);
  } else {
    IPAddress ip = WiFi.localIP();
    snprintf(buf, buf_size, "%u.%u.%u.%u  MQTT:%d",
             ip[0], ip[1], ip[2], ip[3], s_bridge->getConnectedBrokers());
  }
  buf[buf_size - 1] = 0;   // strncpy does not terminate on truncation
}

bool ObserverBridge::handleCliLine(uint32_t sender_timestamp, char* line, char* reply) {
  if (!s_cli) return false;
  s_cli->handleCommand(sender_timestamp, line, reply);
  return true;
}

#else  /* !WITH_MQTT_BRIDGE — compile the whole facade away */

void ObserverBridge::begin(mesh::MainBoard*, mesh::Dispatcher*, mesh::Radio*, mesh::PacketManager*,
                           mesh::RTCClock*, mesh::LocalIdentity*, SensorManager*, const char*,
                           const ObserverRadioInfo&, const char*, const char*) { }
void ObserverBridge::loop() { }
void ObserverBridge::setRadioInfo(const ObserverRadioInfo&) { }
void ObserverBridge::setNodeName(const char*) { }
void ObserverBridge::onRxRaw(float, float, const uint8_t[], int) { }
void ObserverBridge::onRx(mesh::Packet*) { }
void ObserverBridge::onTx(mesh::Packet*) { }
void ObserverBridge::getStatusLine(char* buf, size_t buf_size) {
  if (buf && buf_size) buf[0] = 0;
}
bool ObserverBridge::handleCliLine(uint32_t, char*, char*) { return false; }

#endif
