//============================================================================
//   Stella - Atari 2600 VCS emulator
//============================================================================
//
// TRIMMED for the LyraZeroW composite build: the original 3250-entry game
// properties database (~600 KB) was removed so Stella fits in the ota_0
// partition alongside the Atari 800. Cart bankswitch types are now resolved by
// Stella's auto-detection in Cartridge::create() instead of this MD5 database.
// A single empty row keeps PropsSet's binary search valid (it matches no MD5,
// so every cart falls through to default properties + auto-detect).
//============================================================================

#ifndef DEF_PROPS_HXX
#define DEF_PROPS_HXX

#define DEF_PROPS_SIZE 1

static const char* DefProps[DEF_PROPS_SIZE][21] = {
  { "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" }
};

#endif
