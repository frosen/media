#include "UtilsWanakaFramework.h"
#include <sys/timeb.h>
#include <time.h>

#include "typedef.h"

#ifdef WIN32
#include <wtypesbase.h>
#endif

double UtilsWanakaFramework::getUnixTimestamp()
{
#ifdef WIN32
	SYSTEMTIME systime;
	GetSystemTime(&systime);
	time_t t;
	t = time(NULL);
	uint64 timestamp = t * 1000 + systime.wMilliseconds;
	return (double)timestamp / 1000;
#else
	struct timeb tp;
	ftime(&tp);
	uint64 sec = uint64(tp.time) * 1000;
	uint64 ms = uint64(tp.millitm);

	return (double)uint64(sec + ms) / 1000;
#endif
}