// The one file of the companion module that includes NimBLE. Real bodies under
// FREEINK_CAP_BLE_COMPANION; stub bodies otherwise, so callers never #ifdef.
//
// Ownership model: NimBLE callbacks only copy bounded data into the fixed
// ring (control packets and connection/authentication events) and return.
// ALL product logic — the identity policy, persistence callbacks, session
// progression, state and revision changes, disconnect decisions — runs on the
// main loop inside loop(). Nothing here calls into settings or the filesystem.
// Control entries carry the writer's connection handle; the main loop drops
// everything that is not the accepted companion session, so a rejected or
// pairing phone can never move product state with a control packet.
#include "BleCompanion.h"

#include <BoardConfig.h>
#include <Logging.h>

#if FREEINK_CAP_BLE_COMPANION

#include <Memory.h>
#include <NimBLEDevice.h>
#include <esp_random.h>

#include <array>
#include <cstring>
#include <memory>

#include "BleCompanionJson.h"
#include "CompanionPeer.h"
#include "CompanionSession.h"
#include "ConnectionHandle.h"

#endif

namespace blecompanion {

// The persisted identity contract has one home in the public header; these
// private constants must agree with it.
#if FREEINK_CAP_BLE_COMPANION
static_assert(ADDR_TYPE_PUBLIC == CompanionAddrTypePublic);
static_assert(ADDR_TYPE_RANDOM == CompanionAddrTypeRandom);
static_assert(ADDR_TYPE_UNSET == CompanionAddrTypeUnset);
// The no-session sentinel is NimBLE's own; 0x0000 is a valid connection
// handle and must work like any other accepted handle.
static_assert(kNoSessionHandle == BLE_HS_CONN_HANDLE_NONE);
#endif

#if FREEINK_CAP_BLE_COMPANION

namespace {

constexpr const char* TAG = "BLECOMP";
constexpr const char* DEVICE_NAME = "CrossPoint Pal";
// The ring: NimBLE callbacks copy bounded entries here and return; the main
// loop processes. Six entries cover a 1.5 s phone retry budget.
constexpr size_t QUEUE_SIZE = 6;

class ServerCallbacks;
class ControlCallbacks;

// PeerId stores the address in display (MSB-first) order — the byte order a
// phone shows and a user types; NimBLE's ble_addr_t stores it LSB-first. This
// helper is the single place the two representations are translated.
ble_addr_t bleAddress(const PeerId& peer) {
  ble_addr_t addr{};
  addr.type = peer.type;
  for (size_t i = 0; i < sizeof(addr.val); i++) {
    addr.val[i] = peer.addr[sizeof(addr.val) - 1 - i];
  }
  return addr;
}

}  // namespace

class BleCompanion::Impl {
 public:
  // One ring entry. `kind` decides which fields are meaningful; the 512-byte
  // payload is shared by all kinds so the ring stays one fixed-size array.
  enum class Kind : uint8_t { Control, Connected, Authenticated, Disconnected };

  struct Entry {
    Kind kind = Kind::Control;
    uint16_t length = 0;      // Control: payload bytes
    uint16_t connHandle = 0;  // the connection the entry belongs to
    bool encrypted = false;   // Authenticated
    bool bonded = false;      // Authenticated: a persistent bond was established
    PeerId peer{};            // Authenticated: the bonded ID address
    uint8_t data[MAX_CONTROL_BYTES] = {};
  };

  // Defined after the callback classes below: they need the complete Impl.
  bool begin(const char* storedPeerAddr, uint8_t storedPeerAddrType, void (*savePeerAddr)(const char*, uint8_t),
             void (*forgetPeerAddr)());
  void stop();
  bool forgetBond();

  bool sendButtonEvent(const uint8_t button, const bool pressed) {
    if (state_ != RadioState::Connected || sessionHandle_ == kNoSessionHandle || event_ == nullptr ||
        !session_.helloed()) {
      return false;
    }
    if (button > static_cast<uint8_t>(Button::Down)) return false;
    char out[96];
    const size_t n = writeButtonEvent(static_cast<Button>(button), pressed, out, sizeof(out));
    if (n == 0) return false;
    return notifyTo(sessionHandle_, out, n);
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
          handleControl(entry);
          break;
      }
    }
    if (takeQueueOverflow()) {
      // Dropped entries can include lifecycle events, so the accepted link is
      // no longer trustworthy: fail closed. Force the exact accepted handle
      // down and clear the session. No notification goes out: the handle may
      // already be stale or reused, so it is not a trustworthy recipient.
      if (sessionHandle_ != kNoSessionHandle) {
        if (server_ != nullptr) server_->disconnect(sessionHandle_);
        sessionHandle_ = kNoSessionHandle;
      }
      session_.reset();
      revision_++;
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
    entry.bonded = info.isBonded();
    // The bonded IDENTITY (stable across Android RPA changes), never the
    // over-the-air address. NimBLEAddress is taken by value, so nothing
    // dangles; its raw bytes are LSB-first and PeerId wants display order.
    const ble_addr_t* id = info.getIdAddress().getBase();
    for (size_t i = 0; i < sizeof(entry.peer.addr); i++) {
      entry.peer.addr[i] = id->val[sizeof(id->val) - 1 - i];
    }
    entry.peer.type = id->type;
    push(entry);
  }

  void controlWritten(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) {
    const NimBLEAttValue value = characteristic->getValue();
    const size_t length = value.size();
    if (length == 0 || length > MAX_CONTROL_BYTES) {
      // Unqueueable writes are dropped silently: the writer is not yet known
      // to the main loop's policy (it may be a pairing-time peer), so no
      // error can be scoped to it, and treating a validation failure as ring
      // pressure would tear down an unrelated live session.
      return;
    }
    Entry entry;
    entry.kind = Kind::Control;
    entry.connHandle = info.getConnHandle();
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

  // --- Main-loop event handling (the single product-state owner) ---
  void handleConnected(const Entry& entry) {
    if (sessionHandle_ != kNoSessionHandle && entry.connHandle != sessionHandle_) {
      // A second peer connected while a session is active. The one-phone
      // policy rejects it at authentication; do not disturb the live session.
      return;
    }
    if (server_ != nullptr) {
      server_->updateConnParams(entry.connHandle, 12, 24, 0, 300);
    }
    LOG_DBG(TAG, "Peer connected, handle %u", entry.connHandle);
    setState(RadioState::AwaitingAuthentication);
  }

  void handleAuthenticated(const Entry& entry) {
    if (!entry.encrypted || !entry.bonded) {
      // The policy requires an encrypted persistent bond: a paired-only link
      // has no stable identity, so it can never be adopted.
      LOG_ERR(TAG, "Rejecting a %s connection", entry.bonded ? "unencrypted" : "unbonded");
      if (server_ != nullptr) server_->disconnect(entry.connHandle);
      return;
    }
    if (sessionHandle_ != kNoSessionHandle && entry.connHandle != sessionHandle_) {
      LOG_ERR(TAG, "Rejecting a second connection while a session is active");
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
    sessionHandle_ = entry.connHandle;
    setState(RadioState::Connected);
  }

  void handleDisconnected(const Entry& entry) {
    if (sessionHandle_ != kNoSessionHandle && entry.connHandle != sessionHandle_) {
      return;  // a rejected or foreign peer left; the accepted session stays
    }
    sessionHandle_ = kNoSessionHandle;
    session_.reset();  // ephemeral state dies with the link
    if (state_ != RadioState::Stopped) setState(RadioState::Advertising);
  }

  void handleControl(const Entry& entry) {
    // Only the accepted companion session reaches product state; anything
    // else (rejected phone, pairing-time writes) is dropped here. Handle 0 is
    // a valid accepted session; the sentinel is kNoSessionHandle.
    if (!sessionBound(sessionHandle_, entry.connHandle)) return;
    const SessionEvent event = session_.onControl(reinterpret_cast<const char*>(entry.data), entry.length);
    switch (event) {
      case SessionEvent::HelloAccepted: {
        char out[64];
        const size_t n = writeHelloAck(out, sizeof(out));
        if (n != 0) notifyTo(sessionHandle_, out, n);
        revision_++;  // sessionActive changed: the snapshot must repaint
        break;
      }
      case SessionEvent::ContentStored:
      case SessionEvent::ContentRepeat: {
        char out[80];
        const size_t n = writeContentAck(session_.content().contentId, out, sizeof(out));
        if (n != 0) notifyTo(sessionHandle_, out, n);
        if (event == SessionEvent::ContentStored) revision_++;
        break;
      }
      case SessionEvent::Rejected: {
        switch (session_.lastRejection()) {
          case Rejection::Version:
            reject(sessionHandle_, "version", "Unsupported protocol version");
            break;
          case Rejection::NotHello:
            reject(sessionHandle_, "hello_first", "Send hello first");
            break;
          case Rejection::BadMessage:
          case Rejection::None:
            reject(sessionHandle_, "bad_message", "Malformed control message");
            break;
        }
        break;
      }
      case SessionEvent::None:
        break;
    }
  }

  // All protocol egress goes through here, scoped to one connection handle.
  // NimBLE's handle-less notify would broadcast to every connected peer,
  // including rejected or pairing ones.
  bool notifyTo(uint16_t handle, const char* data, const size_t length) {
    if (event_ == nullptr || handle == kNoSessionHandle) return false;
    return event_->notify(reinterpret_cast<const uint8_t*>(data), length, handle);
  }

  void reject(uint16_t handle, const char* code, const char* message) {
    char out[MAX_CONTROL_BYTES];
    const size_t n = writeError(code, message, out, sizeof(out));
    if (n != 0) notifyTo(handle, out, n);
  }

  void setState(const RadioState state) {
    if (state_ == state) return;
    state_ = state;
    revision_++;
  }

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* control_ = nullptr;
  NimBLECharacteristic* event_ = nullptr;
  std::unique_ptr<ServerCallbacks> serverCallbacks_;
  std::unique_ptr<ControlCallbacks> controlCallbacks_;

  void (*savePeerAddr_)(const char*, uint8_t) = nullptr;
  void (*forgetPeerAddr_)() = nullptr;
  PeerId storedPeer_{};                        // main-loop-owned identity policy state
  uint16_t sessionHandle_ = kNoSessionHandle;  // accepted session's connection handle

  CompanionSession session_;
  uint32_t passkey_ = 0;
  RadioState state_ = RadioState::Stopped;
  uint32_t revision_ = 0;

  std::array<Entry, QUEUE_SIZE> queue_{};
  volatile uint8_t queueHead_ = 0;
  volatile uint8_t queueTail_ = 0;
  volatile bool queueOverflow_ = false;
  portMUX_TYPE queueMux_ = portMUX_INITIALIZER_UNLOCKED;

 public:
  // Single owner of the two NimBLE callback objects (the server's delete
  // flag is off). Defined after the callback classes so they are complete.
  ~Impl();
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
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    owner_.controlWritten(characteristic, info);
  }

 private:
  BleCompanion::Impl& owner_;
};

}  // namespace

// The Impl destructor destroys the callback objects after their types are
// complete; their unique_ptr members free them exactly once, on every path.
// Defined here, directly in blecompanion: a nested anonymous namespace does
// not enclose BleCompanion::Impl.
BleCompanion::Impl::~Impl() = default;

bool BleCompanion::Impl::begin(const char* storedPeerAddr, const uint8_t storedPeerAddrType,
                               void (*savePeerAddr)(const char*, uint8_t), void (*forgetPeerAddr)()) {
  // One radio owner: the adapter is process-global and shared with the BLE
  // page turner, so starting on a live adapter would corrupt both.
  if (NimBLEDevice::isInitialized()) {
    LOG_ERR(TAG, "The BLE adapter is already in use; not starting the companion");
    return false;
  }
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
  // Allocate the callback objects before any NimBLE state exists: a failed
  // allocation here leaves nothing to tear down, and Impl owns them for the
  // whole session (no bare new, no abort under -fno-exceptions).
  serverCallbacks_ = makeUniqueNoThrow<ServerCallbacks>(*this);
  controlCallbacks_ = makeUniqueNoThrow<ControlCallbacks>(*this);
  if (serverCallbacks_ == nullptr || controlCallbacks_ == nullptr) {
    LOG_ERR(TAG, "Not enough heap for the companion callbacks");
    return false;
  }

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityPasskey(passkey_);

  server_ = NimBLEDevice::createServer();
  server_->setCallbacks(serverCallbacks_.get(), false);  // Impl owns the object
  NimBLEService* service = server_->createService(SERVICE_UUID);
  control_ = service->createCharacteristic(CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
                                           MAX_CONTROL_BYTES);
  event_ =
      service->createCharacteristic(EVENT_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC, MAX_CONTROL_BYTES);
  control_->setCallbacks(controlCallbacks_.get());
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
  // NimBLE teardown first: deinit deletes the server/characteristics the
  // callbacks are registered with, so no callback can fire into a dying Impl.
  if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
  server_ = nullptr;
  control_ = nullptr;
  event_ = nullptr;
  // serverCallbacks_/controlCallbacks_ stay alive: their unique_ptr members
  // release them exactly once in ~Impl.
  portENTER_CRITICAL(&queueMux_);
  queueHead_ = 0;
  queueTail_ = 0;
  queueOverflow_ = false;
  portEXIT_CRITICAL(&queueMux_);
  sessionHandle_ = kNoSessionHandle;
  session_.reset();
  setState(RadioState::Stopped);
}

bool BleCompanion::Impl::forgetBond() {
  // The seam decides: only a configured companion identity selects a bond,
  // and nothing else in the store is ever touched. The radio must be up: the
  // companion activity owns it for the whole reset.
  if (!NimBLEDevice::isInitialized()) {
    LOG_ERR(TAG, "The companion radio is not running; cannot reset pairing");
    return false;
  }
  PeerId target;
  if (!companionBondTarget(storedPeer_, target)) {
    LOG_ERR(TAG, "No companion bond configured; nothing to delete");
    return false;
  }
  if (server_ != nullptr) {
    while (server_->getConnectedCount() > 0) {
      if (!server_->disconnect(server_->getPeerInfo(0))) {
        LOG_ERR(TAG, "Could not drop the companion connection");
        break;
      }
      delay(50);
    }
  }
  // ble_gap_unpair refuses to run while advertising is active when the peer
  // distributed an IRK (every Android phone does), so pause it around the
  // deletion and restore the idle state afterwards.
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->stop();
  const NimBLEAddress targetAddress(bleAddress(target));
  bool deleted = true;
  if (!NimBLEDevice::isBonded(targetAddress)) {
    // The phone forgot us first: the reset goal — this identity is no longer
    // bonded — already holds.
    LOG_INF(TAG, "Companion bond not present in the store (already removed)");
  } else if (!(deleted = NimBLEDevice::deleteBond(targetAddress))) {
    LOG_ERR(TAG, "Failed to delete the companion bond");
  }
  if (!advertising->start()) {
    LOG_ERR(TAG, "Advertising restart failed");
  }
  if (!deleted) return false;
  if (forgetPeerAddr_ != nullptr) forgetPeerAddr_();
  storedPeer_ = PeerId{};
  return true;
}

BleCompanion::~BleCompanion() = default;

bool BleCompanion::begin(const char* storedPeerAddr, const uint8_t storedPeerAddrType,
                         void (*savePeerAddr)(const char*, uint8_t), void (*forgetPeerAddr)()) {
  if (impl_ != nullptr) return true;
  auto impl = makeUniqueNoThrow<Impl>();
  if (impl == nullptr) {
    LOG_ERR(TAG, "Not enough heap for the companion radio");
    return false;
  }
  if (!impl->begin(storedPeerAddr, storedPeerAddrType, savePeerAddr, forgetPeerAddr)) {
    return false;  // the unique_ptr destroys the half-built Impl
  }
  impl_ = impl.release();
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
