// Single source of truth for the app version (used by app.rc and the update checker).
// Bump these before tagging a new GitHub release (tag format: v<major>.<minor>[.<patch>]).
#pragma once
#define VER_MAJOR 1
#define VER_MINOR 0
#define VER_PATCH 0
#define VER_STR   "1.0.0"
#define VER_WSTR  L"1.0.0"
#define VER_RC    VER_MAJOR,VER_MINOR,VER_PATCH,0
