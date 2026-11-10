#pragma once

/* OBS generates this header while building libobs. A plugin that is built
 * against the source tree outside that build needs the same defines, and the
 * generated values are installation paths rather than anything a plugin acts
 * on. */

#define OBS_DATA_PATH "data/obs-plugins/%module%"
#define OBS_PLUGIN_PATH "obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
