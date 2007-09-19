/*
 * For comparing the original libcore gmtime() implementation with my
 * optimized one, this file provides libcore's unmodified version.
 */

#include "libcore-gmtime.h"

typedef uint8_t xuint8;
typedef uint16_t xuint16;
typedef uint32_t xuint32;
typedef uint64_t xuint64;
typedef int xbool;

# define LIBCORE_d64C INT64_C

/*
// ---------- includes -----------------------------------------
*/

#include <time.h>
#include <stdio.h>

/*
// ---------- macros -------------------------------------------
*/

#define MILLIS_TO_1970 LIBCORE_d64C(62135596800000)

#define MILLIS_IN_GREG LIBCORE_d64C(12622780800000)

#define MAGIC(a, b, c) SYSX_MAGIC(a, b, c, '!')

#define GET_2DIGITS(x, v) do { \
		const char *s = (x); \
		(v) = (int) (s[0] - '0') * 10 \
			+ (int) (s[1] - '0'); \
	} while ( 0 )

#define GET_4DIGITS(x, v) do { \
		const char *s = (x); \
		(v) = (int) (s[0] - '0') * 1000 \
			+ (int) (s[1] - '0') * 100 \
			+ (int) (s[2] - '0') * 10 \
			+ (int) (s[3] - '0'); \
	} while ( 0 )

#define GET_MONTH(x, v) do { \
		const char *s = (x); \
		unsigned int _i, _m = MAGIC(s[0], s[1], s[2]); \
		for ( _i = 0 ; _i < 12 ; _i++ ) { \
			if (month_magic[_i] == _m) { \
				(v) = _i + 1; \
				break; \
			} \
		} \
	} while ( 0 )

/* table driven for values <= 400 */
#define LEAP_IN_GREG(y) ((leap_years[(y) >> 5] >> ((y) & 0x1f)) & 0x01)

/*
// ---------- data ---------------------------------------------
*/

static const xuint32					leap_years[13] = {

	0x88888888, 0x88888888, 0x88888888, 0x88888880, 0x88888888,
	0x88888888, 0x88888808, 0x88888888,	0x88888888, 0x88888088,
	0x88888888, 0x88888888,	0x00008888
};

static const xuint16					day_to_day[366] = {

	0x0101, 0x0202, 0x0303, 0x0404, 0x0505, 0x0606, 0x0707, 0x0808,
	0x0909, 0x0a0a, 0x0b0b, 0x0c0c, 0x0d0d, 0x0e0e, 0x0f0f, 0x1010,
	0x1111, 0x1212, 0x1313, 0x1414, 0x1515, 0x1616, 0x1717, 0x1818,
	0x1919, 0x1a1a, 0x1b1b, 0x1c1c, 0x1d1d, 0x1e1e, 0x1f1f, 0x0101,
	0x0202, 0x0303, 0x0404, 0x0505, 0x0606, 0x0707, 0x0808, 0x0909,
	0x0a0a, 0x0b0b, 0x0c0c, 0x0d0d, 0x0e0e, 0x0f0f, 0x1010, 0x1111,
	0x1212, 0x1313, 0x1414, 0x1515, 0x1616, 0x1717, 0x1818, 0x1919,
	0x1a1a, 0x1b1b, 0x1c1c, 0x1d01, 0x0102, 0x0203, 0x0304, 0x0405,
	0x0506, 0x0607, 0x0708, 0x0809, 0x090a, 0x0a0b, 0x0b0c, 0x0c0d,
	0x0d0e, 0x0e0f, 0x0f10, 0x1011, 0x1112, 0x1213, 0x1314, 0x1415,
	0x1516, 0x1617, 0x1718, 0x1819, 0x191a, 0x1a1b, 0x1b1c, 0x1c1d,
	0x1d1e, 0x1e1f, 0x1f01, 0x0102, 0x0203, 0x0304, 0x0405, 0x0506,
	0x0607, 0x0708, 0x0809, 0x090a, 0x0a0b, 0x0b0c, 0x0c0d, 0x0d0e,
	0x0e0f, 0x0f10, 0x1011, 0x1112, 0x1213, 0x1314, 0x1415, 0x1516,
	0x1617, 0x1718, 0x1819, 0x191a, 0x1a1b, 0x1b1c, 0x1c1d, 0x1d1e,
	0x1e01, 0x0102, 0x0203, 0x0304, 0x0405, 0x0506, 0x0607, 0x0708,
	0x0809, 0x090a, 0x0a0b, 0x0b0c, 0x0c0d, 0x0d0e, 0x0e0f, 0x0f10,
	0x1011, 0x1112, 0x1213, 0x1314, 0x1415, 0x1516, 0x1617, 0x1718,
	0x1819, 0x191a, 0x1a1b, 0x1b1c, 0x1c1d, 0x1d1e, 0x1e1f, 0x1f01,
	0x0102, 0x0203, 0x0304, 0x0405, 0x0506, 0x0607, 0x0708, 0x0809,
	0x090a, 0x0a0b, 0x0b0c, 0x0c0d, 0x0d0e, 0x0e0f, 0x0f10, 0x1011,
	0x1112, 0x1213, 0x1314, 0x1415, 0x1516, 0x1617, 0x1718, 0x1819,
	0x191a, 0x1a1b, 0x1b1c, 0x1c1d, 0x1d1e, 0x1e01, 0x0102, 0x0203,
	0x0304, 0x0405, 0x0506, 0x0607, 0x0708, 0x0809, 0x090a, 0x0a0b,
	0x0b0c, 0x0c0d, 0x0d0e, 0x0e0f, 0x0f10, 0x1011, 0x1112, 0x1213,
	0x1314, 0x1415, 0x1516, 0x1617, 0x1718, 0x1819, 0x191a, 0x1a1b,
	0x1b1c, 0x1c1d, 0x1d1e, 0x1e1f, 0x1f01, 0x0102, 0x0203, 0x0304,
	0x0405, 0x0506, 0x0607, 0x0708, 0x0809, 0x090a, 0x0a0b, 0x0b0c,
	0x0c0d, 0x0d0e, 0x0e0f, 0x0f10, 0x1011, 0x1112, 0x1213, 0x1314,
	0x1415, 0x1516, 0x1617, 0x1718, 0x1819, 0x191a, 0x1a1b, 0x1b1c,
	0x1c1d, 0x1d1e, 0x1e1f, 0x1f01, 0x0102, 0x0203, 0x0304, 0x0405,
	0x0506, 0x0607, 0x0708, 0x0809, 0x090a, 0x0a0b, 0x0b0c, 0x0c0d,
	0x0d0e, 0x0e0f, 0x0f10, 0x1011, 0x1112, 0x1213, 0x1314, 0x1415,
	0x1516, 0x1617, 0x1718, 0x1819, 0x191a, 0x1a1b, 0x1b1c, 0x1c1d,
	0x1d1e, 0x1e01, 0x0102, 0x0203, 0x0304, 0x0405, 0x0506, 0x0607,
	0x0708, 0x0809, 0x090a, 0x0a0b, 0x0b0c, 0x0c0d, 0x0d0e, 0x0e0f,
	0x0f10, 0x1011, 0x1112, 0x1213, 0x1314, 0x1415, 0x1516, 0x1617,
	0x1718, 0x1819, 0x191a, 0x1a1b, 0x1b1c, 0x1c1d, 0x1d1e, 0x1e1f,
	0x1f01, 0x0102, 0x0203, 0x0304, 0x0405, 0x0506, 0x0607, 0x0708,
	0x0809, 0x090a, 0x0a0b, 0x0b0c, 0x0c0d, 0x0d0e, 0x0e0f, 0x0f10,
	0x1011, 0x1112, 0x1213, 0x1314, 0x1415, 0x1516, 0x1617, 0x1718,
	0x1819, 0x191a, 0x1a1b, 0x1b1c, 0x1c1d, 0x1d1e, 0x1e01, 0x0102,
	0x0203, 0x0304, 0x0405, 0x0506, 0x0607, 0x0708, 0x0809, 0x090a,
	0x0a0b, 0x0b0c, 0x0c0d, 0x0d0e, 0x0e0f, 0x0f10, 0x1011, 0x1112,
	0x1213, 0x1314, 0x1415, 0x1516, 0x1617, 0x1718, 0x1819, 0x191a,
	0x1a1b, 0x1b1c, 0x1c1d, 0x1d1e, 0x1e1f, 0x1f00
};

static const xuint8						day_to_mon[366] = {

	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
	0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
	0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
	0x23, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
	0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
	0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
	0x34, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
	0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
	0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
	0x44, 0x45, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	0x55, 0x56, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x67, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
	0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
	0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
	0x77, 0x77, 0x77, 0x78, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
	0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
	0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
	0x88, 0x88, 0x88, 0x89, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99,
	0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99,
	0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99,
	0x99, 0x99, 0x99, 0x99, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xab, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb,
	0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb,
	0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb,
	0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xb0
};

static const xuint8						years_to_leap_days[401] = {

	0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
	5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9,
	10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13,
	13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17,
	17, 17, 18, 18, 18, 18, 19, 19, 19, 19, 20, 20, 20, 20, 21,
	21, 21, 21, 22, 22, 22, 22, 23, 23, 23, 23, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 26, 26, 26, 26, 27, 27, 27,
	27, 28, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30, 31, 31,
	31, 31, 32, 32, 32, 32, 33, 33, 33, 33, 34, 34, 34, 34, 35,
	35, 35, 35, 36, 36, 36, 36, 37, 37, 37, 37, 38, 38, 38, 38,
	39, 39, 39, 39, 40, 40, 40, 40, 41, 41, 41, 41, 42, 42, 42,
	42, 43, 43, 43, 43, 44, 44, 44, 44, 45, 45, 45, 45, 46, 46,
	46, 46, 47, 47, 47, 47, 48, 48, 48, 48, 48, 48, 48, 48, 49,
	49, 49, 49, 50, 50, 50, 50, 51, 51, 51, 51, 52, 52, 52, 52,
	53, 53, 53, 53, 54, 54, 54, 54, 55, 55, 55, 55, 56, 56, 56,
	56, 57, 57, 57, 57, 58, 58, 58, 58, 59, 59, 59, 59, 60, 60,
	60, 60, 61, 61, 61, 61, 62, 62, 62, 62, 63, 63, 63, 63, 64,
	64, 64, 64, 65, 65, 65, 65, 66, 66, 66, 66, 67, 67, 67, 67,
	68, 68, 68, 68, 69, 69, 69, 69, 70, 70, 70, 70, 71, 71, 71,
	71, 72, 72, 72, 72, 72, 72, 72, 72, 73, 73, 73, 73, 74, 74,
	74, 74, 75, 75, 75, 75, 76, 76, 76, 76, 77, 77, 77, 77, 78,
	78, 78, 78, 79, 79, 79, 79, 80, 80, 80, 80, 81, 81, 81, 81,
	82, 82, 82, 82, 83, 83, 83, 83, 84, 84, 84, 84, 85, 85, 85,
	85, 86, 86, 86, 86, 87, 87, 87, 87, 88, 88, 88, 88, 89, 89,
	89, 89, 90, 90, 90, 90, 91, 91, 91, 91, 92, 92, 92, 92, 93,
	93, 93, 93, 94, 94, 94, 94, 95, 95, 95, 95, 96, 96, 96, 96,
	97
};

/*
// ---------- implementation (public) --------------------------
*/

/**
 * This is an implementation of the "slender"
 * algorithm described in the feb. 1993 paper
 * "efficient timestamp input and output" by
 * C. Dyreson and R. Snodgrass. (Chapter 4.3).
 */
LIBCORE__STDCALL(xbrokentime *)			sysx_time_gmtime_orig(xtime tm64, xbrokentime *tmrec) {

	long tm_greg, days, year, secs;

	unsigned int leap;

	tm64 += MILLIS_TO_1970;
	tm_greg = (long) (tm64 / MILLIS_IN_GREG);
	tm64 %= MILLIS_IN_GREG;

	if (tm64 < 0) {
		tm_greg--;
		tm64 += MILLIS_IN_GREG;
	}

	days = (long) (tm64 / 86400000);
	secs = (long) (tm64 % 86400000);

	tmrec->tm_wday = (days + 1) % 7;

	year = days / 365;
	days = days % 365 - years_to_leap_days[year];

	if (days < 0) {
		year--;
		leap = LEAP_IN_GREG(year);
		days += 365 + leap;
	} else
		leap = LEAP_IN_GREG(year);

	tmrec->tm_year = tm_greg * 400 + year + 1 - 1900;
	tmrec->tm_mon  = day_to_mon[days] >> 4 * leap & 0x0f;
	tmrec->tm_mday = day_to_day[days] >> 8 * leap & 0xff;
	tmrec->tm_yday = days;

	tmrec->tm_hour = secs / 3600000;
	secs %= 3600000;
	tmrec->tm_min  = secs / 60000;
	tmrec->tm_sec  = secs % 60000;
	tmrec->tm_sec /= 1000;

	return tmrec;
}										/* × */
