# Compatibility fix - 2026-09-23

## Compatibility / bootstrap

- Added a second bootstrap path for systems that load System32 `version.dll`
  instead of the local proxy. The installed path proxies the executable's
  `X3DAudio1_7.dll` import and forwards its two original exports.
- Added timestamped bootstrap logging and install diagnostics with the exact
  supported EXE SHA-256 check.
- Added a backup-aware installer that distinguishes copied files from runtime
  verification. No game EXE, original PAK/SIG, Steam files, or saves change.

## Grendel

- On Grendel only, AI Jason skips the entire strategic startup trap sequence
  and enters normal counselor hunting. Other maps retain frozen trap behavior.
- Normal combat, Morph, knives, doors, vehicles, and stuck recovery are not
  changed by this override.
- On 2026-09-23, Grendel loaded and Jason hunted, threw a knife, killed both
  counselors, and reached post-match cleanup without a crash.

## Car extraction and navigation

- Recovers from a stale driver-extraction component after 5.5 seconds rather
  than leaving Jason in a permanently stalled chase state.
- Preserves accepted movement paths during the door-navigation grace period.
- The tested Crystal Lake Small match continued after an extraction timeout,
  completed a Jason kill, and returned to the main menu.

# Frozen definitive packed Full Edition - 2026-09-19

- Makes a living counselor in the driver seat authoritative even when the car
  is already stopped or the counselor re-entered after an earlier pull-out.
- Re-arms the native driver extraction lane instead of allowing ordinary
  slash/grab combat to take priority.
- Preserves the successfully tested first extraction, car pursuit, Jason kill,
  match ending, mask/sweater power, objectives, radar, and spectator behavior.
- Contains no unrelated gameplay tools, test bridge, game executable, or base PAK.

## Previous definitive packed cut - 2026-09-17

- Preserves the current Jason and counselor AI, map-aware spawning, objectives,
  navigation recovery, knives, traps, car interception, and spectator behavior.
- Keeps the player-tested simplified Jason-kill sequence and prevents the
  normal cabin walk after a confirmed Jason-death cinematic.
- Restores the older working car-extraction state handling while the stock
  pull-out animation is active.
- Uses a local version API proxy plus runtime menu integration for a normal
  packed installation; no unsigned menu PAK or `-NoPak` option.
