#include "codec2_internal.h"
#include "util.h"

int32_t fp_acos_q24(int32_t x)
{
	/* Q31 coefficients of the Horner polynomial */
	static const int64_t c5 = -10160094LL;
	static const int64_t c4 = 43248558LL;
	static const int64_t c3 = -97651852LL;
	static const int64_t c2 = 188956439LL;
	static const int64_t c1 = -460667158LL;
	static const int64_t c0 = 3373255746LL;

	int neg = 0;
	int64_t a = x;

	if (a < -(1LL << 31))
		a = -(1LL << 31);
	if (a > (1LL << 31) - 1)
		a = (1LL << 31) - 1;
	if (a < 0) {
		a = -a;
		neg = 1;
	}

	/* p = (((((c5*a + c4)*a + c3)*a + c2)*a + c1)*a + c0), Q31 */
	int64_t p = c5 * a;
	p = ((p >> 31) + c4) * a;
	p = ((p >> 31) + c3) * a;
	p = ((p >> 31) + c2) * a;
	p = ((p >> 31) + c1) * a;
	p = (p >> 31) + c0;

	/* sqrt(1 - a) in Q31 */
	int64_t om = (1LL << 31) - a;          /* in [0, 2^31] */
	int64_t sq = fp_isqrt64((uint64_t)om << 31);   /* Q31 of sqrt(1-a) */

	int64_t y = ((sq * p) >> 31) >> 7;     /* radians Q24 */

	return neg ? (PI_Q24 - (int32_t)y) : (int32_t)y;
}
