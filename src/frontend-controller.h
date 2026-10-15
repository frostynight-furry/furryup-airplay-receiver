#pragma once

#include <stdbool.h>

/* Registers a visible entry in OBS' Tools menu.  The implementation resolves
 * the frontend API from the already-running OBS process, so the receiver stays
 * independent of Qt and does not need a separate player window. */
bool airplay_frontend_controller_register(void);

