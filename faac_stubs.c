/*
 *  Symbols libfaac references that the solvers do not export. A side module
 *  whose imports are not all resolved never instantiates at all - emscripten
 *  reports it by crashing inside its own reportUndefinedSymbols, which names
 *  nothing - so each has to be defined here.
 *
 *  The maths ones are the single-precision variants: the solver exports the
 *  double-precision functions but not their float counterparts, so each is
 *  routed through the double version. That is what a libm without a dedicated
 *  float path does, and costs nothing in accuracy. Same pattern as
 *  libcodec2/codec2_stubs.c.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

float asinf(float x) { return (float)asin((double)x); }
float sinf(float x) { return (float)sin((double)x); }
float expf(float x) { return (float)exp((double)x); }
float log10f(float x) { return (float)log10((double)x); }

void __assert_fail(const char *expr, const char *file, unsigned int line, const char *func)
{
	fprintf(stderr, "[FaacEnc] assertion failed: %s at %s:%u in %s\n",
	        expr ? expr : "?", file ? file : "?", line, func ? func : "?");
	abort();
}
