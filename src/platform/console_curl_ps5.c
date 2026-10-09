/*
 * SwanStationPS5 - pulls in the boilerplate's console_curl helpers for the PS5 build only.
 */
#if defined(__PROSPERO__) && defined(SwanStationPS5_HAVE_CURL)
#include "../../examples/update-check/console_curl.c"
#endif
