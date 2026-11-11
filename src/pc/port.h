#pragma once

// The game this runtime is built into, defined by that game's CMake (xport.cmake, xport_identity): XPORT_NAME
// ("mc3port": the data folder and the names of the log, the settings and the crash files), XPORT_NAME_UPPER and
// XPORT_ENV_PREFIX ("MC3_": the environment switches).
#if !defined(XPORT_NAME) || !defined(XPORT_NAME_UPPER) || !defined(XPORT_ENV_PREFIX)
#error "XPORT_NAME, XPORT_NAME_UPPER and XPORT_ENV_PREFIX come from the game's CMake (xport_identity in xport.cmake)"
#endif

// An environment switch's name: XPORT_ENV("PROFILE") is "MC3_PROFILE" in mc3port.
#define XPORT_ENV(name) XPORT_ENV_PREFIX name
#define XPORT_ENV_W(name) L"" XPORT_ENV_PREFIX name
