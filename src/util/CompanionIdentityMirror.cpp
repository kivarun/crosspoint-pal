#include "CompanionIdentityMirror.h"

#include <Logging.h>
#include <Preferences.h>

namespace companionidentity {

namespace {
constexpr char NAMESPACE[] = "cpgate";
constexpr char KEY_ADDR[] = "peer";
constexpr char KEY_TYPE[] = "type";
}  // namespace

bool load(StoredIdentity& out) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, true)) return false;
  char addr[18] = {};
  const size_t read = prefs.getString(KEY_ADDR, addr, sizeof(addr));
  const uint8_t type = prefs.getUChar(KEY_TYPE, 0xFF);
  prefs.end();
  if (read != 17 || !textValid(addr, type)) return false;
  memcpy(out.addr, addr, sizeof(out.addr));
  out.type = type;
  return true;
}

bool save(const char* addr, const uint8_t type) {
  if (!textValid(addr, type)) return false;
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, false)) return false;
  const bool ok = prefs.putString(KEY_ADDR, addr) != 0 && prefs.putUChar(KEY_TYPE, type) != 0;
  prefs.end();
  return ok;
}

bool clear() {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, false)) return false;
  prefs.clear();
  prefs.end();
  return true;
}

void migrateFromSettings(const char* settingsAddr, const uint8_t settingsType) {
  StoredIdentity mirror{};
  const bool mirrorValid = load(mirror);
  if (migrationDecision(mirrorValid, settingsAddr, settingsType) != Migration::WriteMirrorFromSettings) return;
  if (save(settingsAddr, settingsType)) {
    LOG_INF("GATE", "Identity mirror migrated from settings");
  } else {
    LOG_ERR("GATE", "Identity mirror migration failed; the gate stays inactive");
  }
}

}  // namespace companionidentity
