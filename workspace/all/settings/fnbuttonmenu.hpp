#pragma once

#include "menu.hpp"

// Builds the "Assignments" menu: one row per user-assignable button this
// device actually has (BTN_FN1/FN2/FN3, named by BTN_FN*_NAME in platform.h).
// Each row can be explicitly unassigned, bound to the application-runtime
// home/overlay action, or bound to a Tools pak. Left/Right cycles through the
// available actions and applies immediately; Confirm opens a submenu with the
// same choices for direct selection.
//
// Returns nullptr when the device has no assignable buttons, in which case the caller
// should leave the menu out entirely.
MenuList* buildFnButtonMenu();
