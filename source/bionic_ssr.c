/* bionic_ssr.c -- the few bionic imports of libssasr.so the shared runtime
 * did not have yet (tools/gen_imports.py lists them). MIT. */
#include <math.h>
#include <switch.h>

#include "error.h"
#include "util.h"

/* bionic's libm: the isfinite() of a float (NDK r5-r8 <math.h> calls it). */
int b___isfinitef(float f) { return isfinite(f); }

/* libstdc++ / libsupc++: a pure virtual method called (a destroyed object's
 * vtable). On Android: abort with this message. */
void b___cxa_pure_virtual(void) {
  debugPrintf("[abi] pure virtual method called\n");
  log_flush_ring();
  fatal_error("The game called a pure virtual method (an internal error of the engine).");
}
