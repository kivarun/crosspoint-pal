// The one file of the companion module that includes NimBLE. Real bodies under
// FREEINK_CAP_BLE_COMPANION; stub bodies otherwise, so callers never #ifdef.
//
// Ownership model: NimBLE callbacks only copy bounded data into the fixed
// ring (control packets and connection/authentication events) and return.
// ALL product logic — the identity policy, persistence callbacks, session
// progression, state and revision changes, disconnect decisions — runs on the
// main loop inside loop(). Nothing here calls into settings or the filesystem.
#include "BleCompanion.h"

#include <BoardConfig.h>
#include <Logging.h>

#if FREEINK_CAP_BLE_COMPANION

#include <NimBLEDevice.h>
#include <esp_random.h>

#include <array>
#include <cstring>

#include "BleCompanionJson.h"
#include "CompanionPeer.h"
#include "CompanionSession.h"

#endif

namespace blecompanion {

#if FREEINK_CAP_BLE_COMPANION

namespace {

constexpr const char* TAG = "BLECOMP";
constexpr const char* DEVICE_NAME = "CrossPoint Pal";
// The ring: NimBLE callbacks copy bounded entries here and return; the main
// loop processes. Six entries cover a 1.5 s phone retry budget.
constexpr size_t QUEUE_SIZE = 6;

}  // namespace

class BleCompanion::Impl {
 public:
  // One ring entry. `kind` decides which fields are meaningful; the 512-byte
  // payload is shared by all kinds so the ring stays one fixed-size array.
  enum class Kind : uint8_t { Control, Connected, Authenticated, Disconnected };

  struct Entry {
    Kind kind = Kind::Control;
    uint16_t length = 0;      // Control: payload bytes
    uint16_t connHandle = 0;  // Connected/Authenticated/Disconnected
    bool encrypted = false;   // Authenticated
    PeerId peer{};            // Authenticated: the bonded ID address
    uint8_t data[MAX_CONTROL_BYTES] = {};
  };

  // Defined after the callback classes below: they need the complete Impl.
  bool begin(const char* storedPeerAddr, uint8_t storedPeerAddrType, void (*savePeerAddr)(const char*, uint8_t),
             void (*forgetPeerAddr)());
  void stop();
  bool forgetBond();

  bool sendButtonEvent(const uint8_t button, const bool pressed) {
    if (state_ != RadioState::Connected || event_ == nullptr || !session_.helloed()) return false;
    if (button > static_cast<uint8_t>(Button::Down)) return false;
    char out[96];
    const size_t n = writeButtonEvent(static_cast<Button>(button), pressed, out, sizeof(out));
    if (n == 0) return false;
    return event_->notify(reinterpret_cast<const uint8_t*>(out), n);
  }

  void loop() {
    Entry entry;
    while (pop(entry)) {
      switch (entry.kind) {
        case Kind::Connected:
          handleConnected(entry);
          break;
        case Kind::Authenticated:
          handleAuthenticated(entry);
          break;
        case Kind::Disconnected:
          handleDisconnected(entry);
          break;
        case Kind::Control:
          handleControl(reinterpret_cast<const char*>(entry.data), entry.length);
          break;
      }
    }
    if (takeQueueOverflow()) {
      reject("queue_full", "Too many messages");
    }
  }

  Status status() const {
    Status s;
    s.compiledIn = true;
    s.state = state_;
    s.sessionActive = state_ == RadioState::Connected && session_.helloed();
    s.contentPresent = session_.contentPresent();
    s.passkey = passkey_;
    s.revision = revision_;
    return s;
  }

  uint32_t passkeyForDisplay() const { return passkey_; }

  ContentView content() const {
    if (!session_.contentPresent()) return {};
    const auto& c = session_.content();
    return {c.title, c.body};
  }

  // --- NimBLE callbacks (they run on the NimBLE host task; enqueues only) ---
  void radioConnected(NimBLEConnInfo& info) {
    Entry entry;
    entry.kind = Kind::Connected;
    entry.connHandle = info.getConnHandle();
    push(entry);
  }

  void radioDisconnected(NimBLEConnInfo& info) {
    Entry entry;
    entry.kind = Kind::Disconnected;
    entry.connHandle = info.getConnHandle();
    push(entry);
  }

  void radioAuthenticated(NimBLEConnInfo& info) {
    Entry entry;
    entry.kind = Kind::Authenticated;
    entry.connHandle = info.getConnHandle();
    entry.encrypted = info.isEncrypted();
    // The bonded IDENTITY (stable across Android RPA changes), never the
    // over-the-air address: NimBLEAddress is taken by value, so nothing
    // dangles.
    const NimBLEAddress id = info.getIdAddress();
    memcpy(entry.peer.addr, id.getVal(), sizeof(entry.peer.addr));
    entry.peer.type = id.getType();
    push(entry);
  }

  void controlWritten(NimBLECharacteristic* characteristic) {
    const NimBLEAttValue value = characteristic->getValue();
    const size_t length = value.size();
    if (length == 0 || length > MAX_CONTROL_BYTES) {
      flagQueueOverflow();
      return;
    }
    Entry entry;
    entry.kind = Kind::Control;
    entry.length = static_cast<uint16_t>(length);
    memcpy(entry.data, value.data(), length);
    push(entry);
  }

 private:
  // The ring: one producer (NimBLE host task), one consumer (main loop).
  // Indices and the overflow flag are only mutated inside the portMUX
  // critical section; that section is the synchronization contract.
  bool push(const Entry& entry) {
    bool accepted = false;
    portENTER_CRITICAL(&queueMux_);
    const uint8_t next = (queueHead_ + 1) % QUEUE_SIZE;
    if (next != queueTail_) {
      queue_[queueHead_] = entry;
      queueHead_ = next;
      accepted = true;
    } else {
      queueOverflow_ = true;
    }
    portEXIT_CRITICAL(&queueMux_);
    return accepted;
  }

  bool pop(Entry& entry) {
    bool available = false;
    portENTER_CRITICAL(&queueMux_);
    if (queueTail_ != queueHead_) {
      entry = queue_[queueTail_];
      queueTail_ = (queueTail_ + 1) % QUEUE_SIZE;
      available = true;
    }
    portEXIT_CRITICAL(&queueMux_);
    return available;
  }

  bool takeQueueOverflow() {
    bool overflow = false;
    portENTER_CRITICAL(&queueMux_);
    overflow = queueOverflow_;
    queueOverflow_ = false;
    portEXIT_CRITICAL(&queueMux_);
    return overflow;
  }

  static void flagQueueOverflow() {
    portENTER_CRITICAL(&queueMux_);
    queueOverflow_ = true;
    portEXIT_CRITICAL(&queueMux_);
  }

  // --- Main-loop event handling (the single product-state owner) ---
  void handleConnected(const Entry& entry) {
    if (server_ != nullptr) {
      server_->updateConnParams(entry.connHandle, 12, 24, 0, 300);
    }
    setState(RadioState::AwaitingAuthentication);
  }

  void handleAuthenticated(const Entry& entry) {
    if (!entry.encrypted) {
      LOG_ERR(TAG, "Rejecting unencrypted connection");
      if (server_ != nullptr) server_->disconnect(entry.connHandle);
      return;
    }
    switch (CompanionSession::authenticate(entry.peer, storedPeer_)) {
      case AuthDecision::Allow:
        break;
      case AuthDecision::Adopt: {
        char text[PEER_ID_TEXT_LEN + 1];
        if (peerIdToString(entry.peer, text, sizeof(text)) && savePeerAddr_ != nullptr) {
          savePeerAddr_(text, entry.peer.type);
        }
        storedPeer_ = entry.peer;
        break;
      }
      case AuthDecision::Reject: {
        char text[PEER_ID_TEXT_LEN + 1];
        if (peerIdToString(entry.peer, text, sizeof(text))) {
          LOG_ERR(TAG, "Rejecting bond from %s: another phone is paired", text);
        }
        if (server_ != nullptr) server_->disconnect(entry.connHandle);
        return;
      }
    }
    setState(RadioState::Connected);
  }

  void handleDisconnected(const Entry&) {
    session_.reset();  // ephemeral state dies with the link
    if (state_ != RadioState::Stopped) setState(RadioState::Advertising);
  }

  void handleControl(const char* data, const size_t length) {
    const SessionEvent event = session_.onControl(data, length);
    switch (event) {
      case SessionEvent::HelloAccepted: {
        char out[64];
        const size_t n = writeHelloAck(out, sizeof(out));
        if (n != 0 && event_ != nullptr) event_->notify(reinterpret_cast<const uint8_t*>(out), n);
        break;
      }
      case SessionEvent::ContentStored:
      case SessionEvent::ContentRepeat: {
        char out[80];
        const size_t n = writeContentAck(session_.content().contentId, out, sizeof(out));
        if (n != 0 && event_ != nullptr) event_->notify(reinterpret_cast<const uint8_t*>(out), n);
        if (event == SessionEvent::ContentStored) revision_++;
        break;
      }
      case SessionEvent::Rejected: {
        switch (session_.lastRejection()) {
          case Rejection::Version:
            reject("version", "Unsupported protocol version");
            break;
          case Rejection::NotHello:
            reject("hello_first", "Send hello first");
            break;
          case Rejection::BadMessage:
          case Rejection::None:
            reject("bad_message", "Malformed control message");
            break;
        }
        break;
      }
      case SessionEvent::None:
        break;
    }
  }

  void reject(const char* code, const char* message) {
    char out[MAX_CONTROL_BYTES];
    const size_t n = writeError(code, message, out, sizeof(out));
    if (n != 0 && event_ != nullptr) event_->notify(reinterpret_cast<const uint8_t*>(out), n);
  }

  void setState(const RadioState state) {
    if (state_ == state) return;
    state_ = state;
    revision_++;
  }

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* control_ = nullptr;
  NimBLECharacteristic* event_ = nullptr;
  ServerCallbacks* serverCallbacks_ = nullptr;
  ControlCallbacks* controlCallbacks_ = nullptr;

  const void (*savePeerAddr_)(const char*, uint8_t) = nullptr;
  void (*forgetPeerAddr_)() = nullptr;
  PeerId storedPeer_{};  // main-loop-owned identity policy state

  CompanionSession session_;
  uint32_t passkey_ = 0;
  RadioState state_ = RadioState::Stopped;
  uint32_t revision_ = 0;

  std::array<Entry, QUEUE_SIZE> queue_{};
  volatile uint8_t queueHead_ = 0;
  volatile uint8_t queueTail_ = 0;
  volatile bool queueOverflow_ = false;
  portMUX_TYPE queueMux_ = portMUX_INITIALIZER_UNLOCKED;
};

namespace {

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleCompanion::Impl& owner) : owner_(owner) {}
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override { owner_.radioConnected(info); }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override { owner_.radioDisconnected(info); }
  uint32_t onPassKeyDisplay() override { return owner_.passkeyForDisplay(); }
  void onAuthenticationComplete(NimBLEConnInfo& info) override { owner_.radioAuthenticated(info); }

 private:
  BleCompanion::Impl& owner_;
};

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit ControlCallbacks(BleCompanion::Impl& owner) : owner_(owner) {}
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    owner_.controlWritten(characteristic);
  }

 private:
  BleCompanion::Impl& owner_;
};

}  // namespace

bool BleCompanion::Impl::begin(const char* storedPeerAddr, const uint8_t storedPeerAddrType,
                               void (*savePeerAddr)(const char*, uint8_t), void (*forgetPeerAddr)()) {
  savePeerAddr_ = savePeerAddr;
  forgetPeerAddr_ = forgetPeerAddr;
  // Parse the persisted identity once; a malformed pair degrades to "none
  // configured" so a corrupt settings file can never lock the phone out or
  // let a bogus identity through.
  if (storedPeerAddr != nullptr && storedPeerAddr[0] != '\0' &&
      peerIdFromString(storedPeerAddr, storedPeerAddrType, storedPeer_)) {
    // parsed
  } else {
    storedPeer_ = PeerId{};
    if (storedPeerAddr != nullptr && storedPeerAddr[0] != '\0') {
      LOG_ERR(TAG, "Stored companion identity is malformed; treating as unpaired");
    }
  }
  session_.reset();
  passkey_ = 100000 + esp_random() % 900000;

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityPasskey(passkey_);

  server_ = NimBLEDevice::createServer();
  serverCallbacks_ = new ServerCallbacks(*this);
  server_->setCallbacks(serverCallbacks_, true);  // freed with the server
  NimBLEService* service = server_->createService(SERVICE_UUID);
  control_ = service->createCharacteristic(CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
                                           MAX_CONTROL_BYTES);
  event_ =
      service->createCharacteristic(EVENT_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC, MAX_CONTROL_BYTES);
  controlCallbacks_ = new ControlCallbacks(*this);
  control_->setCallbacks(controlCallbacks_);
  server_->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName(DEVICE_NAME);
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->enableScanResponse(true);
  if (!advertising->start()) {
    LOG_ERR(TAG, "Advertising failed");
    stop();
    return false;
  }
  setState(RadioState::Advertising);
  LOG_INF(TAG, "Advertising, passkey %06u", passkey_);
  return true;
}

void BleCompanion::Impl::stop() {
  if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
  server_ = nullptr;
  serverCallbacks_ = nullptr;
  control_ = nullptr;
  controlCallbacks_ = nullptr;
  event_ = nullptr;
  portENTER_CRITICAL(&queueMux_);
  queueHead_ = 0;
  queueTail_ = 0;
  queueOverflow_ = false;
  portEXIT_CRITICAL(&queueMux_);
  session_.reset();
  setState(RadioState::Stopped);
}

bool BleCompanion::Impl::forgetBond() {
  // The seam decides: only a configured companion identity selects a bond,
  // and nothing else in the store is ever touched.
  PeerId target;
  if (!companionBondTarget(storedPeer_, target)) {
    LOG_ERR(TAG, "No companion bond configured; nothing to delete");
    return false;
  }
  const bool wasUp = NimBLEDevice::isInitialized();
  if (!wasUp && !begin(nullptr, ADDR_TYPE_UNSET, savePeerAddr_, forgetPeerAddr_)) return false;
  if (server_ != nullptr) {
    while (server_->getConnectedCount() > 0) {
      server_->disconnect(server_->getPeerInfo(0));
      delay(50);
    }
  }
  const NimBLEAddress targetAddress(target.addr, target.type);
  const bool deleted = NimBLEDevice::deleteBond(targetAddress);
  if (!deleted) {
    // With the stack up, false means the bond is already absent from the
    // store (the phone forgot us first): the reset goal — this identity is
    // no longer bonded — still holds. Log it either way.
    LOG_INF(TAG, "Companion bond not present in the store (already removed)");
  }
  if (forgetPeerAddr_ != nullptr) forgetPeerAddr_();
  storedPeer_ = PeerId{};
  if (!wasUp) stop();
  return true;
}

BleCompanion::~BleCompanion() = default;

bool BleCompanion::begin(const char* storedPeerAddr, const uint8_t storedPeerAddrType,
                         void (*savePeerAddr)(const char*, uint8_t), void (*forgetPeerAddr)()) {
  if (impl_ != nullptr) return true;
  impl_ = new Impl();
  if (!impl_->begin(storedPeerAddr, storedPeerAddrType, savePeerAddr, forgetPeerAddr)) {
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  return true;
}

void BleCompanion::loop() {
  if (impl_ != nullptr) impl_->loop();
}

void BleCompanion::stop() {
  if (impl_ == nullptr) return;
  impl_->stop();
  delete impl_;
  impl_ = nullptr;
}

bool BleCompanion::forgetBond() { return impl_ != nullptr && impl_->forgetBond(); }

bool BleCompanion::sendButtonEvent(const uint8_t button, const bool pressed) {
  return impl_ != nullptr && impl_->sendButtonEvent(button, pressed);
}

Status BleCompanion::status() const {
  if (impl_ == nullptr) return Status{};
  return impl_->status();
}

ContentView BleCompanion::content() const {
  if (impl_ == nullptr) return {};
  return impl_->content();
}

bool compiledIn() { return true; }

#else  // capability off: stub bodies, no BLE linked

BleCompanion::~BleCompanion() = default;
bool BleCompanion::begin(const char*, uint8_t, void (*)(const char*, uint8_t), void (*)()) { return false; }
void BleCompanion::loop() {}
void BleCompanion::stop() {}
bool BleCompanion::forgetBond() { return false; }
bool BleCompanion::sendButtonEvent(uint8_t, bool) { return false; }
Status BleCompanion::status() const { return Status{}; }
ContentView BleCompanion::content() const { return {}; }

bool compiledIn() { return false; }

#endif

}  // namespace blecompanion
