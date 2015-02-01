#include <Ecore_X.h>
#include <dlog.h>

#if defined(HAVE_LIVEBOX)
#include <dynamicbox_errno.h>
#else
#include "lite-errno.h"
#endif
#include "util.h"
#include "debug.h"

static struct info {
	int initialized;
} s_info = {
	.initialized = 0,
};

int util_screen_size_get(int *width, int *height)
{
	if (!s_info.initialized) {
		ErrPrint("Not initialized\n");
		return DBOX_STATUS_ERROR_FAULT;
	}
	ecore_x_window_size_get(0, width, height);
	return DBOX_STATUS_ERROR_NONE;
}

int util_screen_init(void)
{
	s_info.initialized = 1;
	return ecore_x_init(NULL);
}

int util_screen_fini(void)
{
	s_info.initialized = 0;
	return ecore_x_shutdown();
}

/* End of a file */

