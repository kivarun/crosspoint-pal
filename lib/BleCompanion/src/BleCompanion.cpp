// The one file of the companion module that includes NimBLE. Real bodies under
// FREEINK_CAP_BLE_COMPANION; stub bodies otherwise, so callers never #ifdef.
#include "BleCompanion.h"

#include <BoardConfig.h>
#include <Logging.h>

#if FREEINK_CAP_BLE_COMPANION

#include <NimBLEDevice.h>
#include <esp_random.h>

#include <array>
#include <cstring>

#include "BleCompanionJson.h"
#include "CompanionSession.h"

#endif

namespace blecompanion {

#if FREEINK_CAP_BLE_COMPANION

namespace {

constexpr const char* TAG = "BLECOMP";
constexpr const char* DEVICE_NAME = "CrossPoint Pal";
// The control ring: NimBLE callbacks copy bytes here and return; the main loop
// parses. Six 512 B packets, matching a 1.5 s phone retry budget.
constexpr size_t QUEUE_SIZE = 6;

struct Packet {
  uint16_t length = 0;
  std::array<uint8_t, MAX_CONTROL_BYTES> data{};
};

class ServerCallbacks;
class ControlCallbacks;

}  // namespace

class BleCompanion::Impl {
 public:
  // Defined after the callback classes below: they need the complete Impl.
  bool begin(const char* storedPeerAddr, void (*savePeerAddr)(const char*), void (*forgetPeerAddr)());
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
    if (queueOverflow_) {
      portENTER_CRITICAL(&queueMux_);
      queueOverflow_ = false;
      portEXIT_CRITICAL(&queueMux_);
      reject("queue_full", "Too many control messages");
      return;
    }
    Packet packet;
    for (int processed = 0; processed < 3 && dequeue(packet); processed++) {
      handleControl(reinterpret_cast<const char*>(packet.data.data()), packet.length);
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

  // --- NimBLE callbacks (they run on the NimBLE host task) ---
  void radioConnected(NimBLEServer* server, NimBLEConnInfo& info) {
    server->updateConnParams(info.getConnHandle(), 12, 24, 0, 300);
    setState(RadioState::AwaitingAuthentication);
  }

  void radioDisconnected(NimBLEServer*, NimBLEConnInfo&, int) {
    session_.reset();  // ephemeral state dies with the link
    if (state_ != RadioState::Stopped) setState(RadioState::Advertising);
  }

  void authenticated(NimBLEConnInfo& info) {
    if (!info.isEncrypted()) {
      if (server_ != nullptr) server_->disconnect(info.getConnHandle());
      return;
    }
    const char* addr = info.getAddress().toString().c_str();
    switch (CompanionSession::authenticate(addr, storedPeerAddr_)) {
      case AuthDecision::Allow:
        break;
      case AuthDecision::Adopt:
        if (savePeerAddr_ != nullptr) savePeerAddr_(addr);
        storedPeerAddr_ = addr;
        break;
      case AuthDecision::Reject:
        LOG_ERR(TAG, "Rejecting bond from %s: another phone is paired", addr);
        if (server_ != nullptr) server_->disconnect(info.getConnHandle());
        return;
    }
    setState(RadioState::Connected);
  }

  void controlWritten(NimBLECharacteristic* characteristic) {
    const NimBLEAttValue value = characteristic->getValue();
    if (!enqueue(reinterpret_cast<const uint8_t*>(value.data()), value.size())) {
      // enqueue() flags the overflow itself; loop() reports it to the phone.
    }
  }

 private:
  void setState(const RadioState state) {
    if (state_ == state) return;
    state_ = state;
    revision_++;  // volatile: the NimBLE task bumps it too
  }

  bool enqueue(const uint8_t* data, const size_t length) {
    if (data == nullptr || length == 0 || length > MAX_CONTROL_BYTES) {
      queueOverflow_ = true;
      return false;
    }
    bool accepted = false;
    portENTER_CRITICAL(&queueMux_);
    const uint8_t next = (queueHead_ + 1) % QUEUE_SIZE;
    if (next != queueTail_) {
      Packet& packet = queue_[queueHead_];
      packet.length = length;
      memcpy(packet.data.data(), data, length);
      queueHead_ = next;
      accepted = true;
    }
    portEXIT_CRITICAL(&queueMux_);
    if (!accepted) queueOverflow_ = true;
    return accepted;
  }

  bool dequeue(Packet& packet) {
    bool available = false;
    portENTER_CRITICAL(&queueMux_);
    if (queueTail_ != queueHead_) {
      packet = queue_[queueTail_];
      queueTail_ = (queueTail_ + 1) % QUEUE_SIZE;
      available = true;
    }
    portEXIT_CRITICAL(&queueMux_);
    return available;
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

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* control_ = nullptr;
  NimBLECharacteristic* event_ = nullptr;
  ServerCallbacks* serverCallbacks_ = nullptr;
  ControlCallbacks* controlCallbacks_ = nullptr;

  const char* storedPeerAddr_ = "";
  void (*savePeerAddr_)(const char*) = nullptr;
  void (*forgetPeerAddr_)() = nullptr;

  CompanionSession session_;
  uint32_t passkey_ = 0;
  RadioState state_ = RadioState::Stopped;
  volatile uint32_t revision_ = 0;

  std::array<Packet, QUEUE_SIZE> queue_{};
  volatile uint8_t queueHead_ = 0;
  volatile uint8_t queueTail_ = 0;
  volatile bool queueOverflow_ = false;
  portMUX_TYPE queueMux_ = portMUX_INITIALIZER_UNLOCKED;
};

namespace {

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleCompanion::Impl& owner) : owner_(owner) {}
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override { owner_.radioConnected(server, info); }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
    owner_.radioDisconnected(server, info, reason);
  }
  uint32_t onPassKeyDisplay() override { return owner_.passkeyForDisplay(); }
  void onAuthenticationComplete(NimBLEConnInfo& info) override { owner_.authenticated(info); }

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

bool BleCompanion::Impl::begin(const char* storedPeerAddr, void (*savePeerAddr)(const char*),
                               void (*forgetPeerAddr)()) {
  storedPeerAddr_ = storedPeerAddr;
  savePeerAddr_ = savePeerAddr;
  forgetPeerAddr_ = forgetPeerAddr;
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
  const bool wasUp = NimBLEDevice::isInitialized();
  if (!wasUp && !begin(storedPeerAddr_, savePeerAddr_, forgetPeerAddr_)) return false;
  if (server_ != nullptr) {
    while (server_->getConnectedCount() > 0) {
      server_->disconnect(server_->getPeerInfo(0));
      delay(50);
    }
  }
  if (forgetPeerAddr_ != nullptr) {
    forgetPeerAddr_();
    storedPeerAddr_ = "";
  }
  if (!NimBLEDevice::deleteAllBonds()) {
    LOG_ERR(TAG, "Bond deletion failed");
    if (!wasUp) stop();
    return false;
  }
  if (!wasUp) stop();
  return true;
}

BleCompanion::~BleCompanion() = default;

bool BleCompanion::begin(const char* storedPeerAddr, void (*savePeerAddr)(const char*), void (*forgetPeerAddr)()) {
  if (impl_ != nullptr) return true;
  impl_ = new Impl();
  if (!impl_->begin(storedPeerAddr, savePeerAddr, forgetPeerAddr)) {
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
bool BleCompanion::begin(const char*, void (*)(const char*), void (*)()) { return false; }
void BleCompanion::loop() {}
void BleCompanion::stop() {}
bool BleCompanion::forgetBond() { return false; }
bool BleCompanion::sendButtonEvent(uint8_t, bool) { return false; }
Status BleCompanion::status() const { return Status{}; }
ContentView BleCompanion::content() const { return {}; }

bool compiledIn() { return false; }

#endif

}  // namespace blecompanion
