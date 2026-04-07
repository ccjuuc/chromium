#include "xenon_overlay/chrome/browser/xenon_prefs.h"

#include "components/pref_registry/pref_registry_syncable.h"

namespace xenon::prefs {

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterBooleanPref(kAppSessionLoggedIn, false);
  registry->RegisterIntegerPref(kReloginPresentation, 0);
}

}  // namespace xenon::prefs
