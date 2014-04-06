/*==LICENSE==*

CyanWorlds.com Engine - MMOG client, server and tools
Copyright (C) 2011  Cyan Worlds, Inc.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Additional permissions under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or
combining it with any of RAD Game Tools Bink SDK, Autodesk 3ds Max SDK,
NVIDIA PhysX SDK, Microsoft DirectX SDK, OpenSSL library, Independent
JPEG Group JPEG library, Microsoft Windows Media SDK, or Apple QuickTime SDK
(or a modified version of those libraries),
containing parts covered by the terms of the Bink SDK EULA, 3ds Max EULA,
PhysX SDK EULA, DirectX SDK EULA, OpenSSL and SSLeay licenses, IJG
JPEG Library README, Windows Media SDK EULA, or QuickTime SDK EULA, the
licensors of this Program grant you additional
permission to convey the resulting work. Corresponding Source for a
non-source form of such a combination shall include the source code for
the parts of OpenSSL and IJG JPEG Library used as well as that of the covered
work.

You can contact Cyan Worlds, Inc. by email legal@cyan.com
 or by snail mail at:
      Cyan Worlds, Inc.
      14617 N Newport Hwy
      Mead, WA   99021

*==LICENSE==*/
#include "hsThread.h"
#include "hsExceptions.h"
#include <sys/errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>

#define NO_POSIX_CLOCK 1

#if NO_POSIX_CLOCK
#include <sys/time.h>
#include <unistd.h>
#define CLOCK_REALTIME 0

//
// A linux hack b/c we're not quite POSIX
//
int clock_gettime(int clocktype, struct timespec* ts)
{
  struct timezone tz;
  struct timeval tv;

  int result = gettimeofday(&tv, &tz);
  ts->tv_sec = tv.tv_sec;
  ts->tv_nsec = tv.tv_usec * 1000 + 500;  // sice we're losing accuracy round up by 500 nanos

  return result;
}

#endif

extern "C" {
    static void* gEntryPoint(void* param)
    {
        pthread_mutex_lock(((hsThread*)param)->GetStartupMutex());
        void* ret = (void*)(uintptr_t)((hsThread*)param)->Run();
        pthread_mutex_unlock(((hsThread*)param)->GetStartupMutex());
        ((hsThread*)param)->OnQuit();
        pthread_exit(ret);
        return ret;
    }
}

#define kInvalidStackSize   uint32_t(~0)

hsThread::hsThread(uint32_t stackSize) : fStackSize(stackSize), fQuit(false)
{
    fIsValid = false;
    pthread_mutex_init(&fMutex,nil);
}

hsThread::~hsThread()
{
    this->Stop();
}

void hsThread::Start()
{
    if (fIsValid == false)
    {
        pthread_mutex_lock(GetStartupMutex());

        int status = ::pthread_create(&fPThread, nil, gEntryPoint, this);
        pthread_mutex_unlock(GetStartupMutex());

        hsThrowIfOSErr(status);

        fIsValid = true;
    }
    else
        hsDebugMessage("Calling hsThread::Start() more than once", 0);
}

void hsThread::Stop()
{
    if (fIsValid)
    {   this->fQuit = true;

        int status = ::pthread_join(fPThread, nil);
        hsThrowIfOSErr(status);

        fIsValid = false;
    }
}

//////////////////////////////////////////////////////////////////////////////

void* hsThread::Alloc(size_t size)
{
    return ::malloc(size);
}

void hsThread::Free(void* p)
{
    if (p)
        ::free(p);
}

void hsThread::ThreadYield()
{
//  ::sched_yield();
}

//////////////////////////////////////////////////////////////////////////////

//#define MUTEX_TIMING
#ifdef MUTEX_TIMING

#include <ctime>
#include <cstdio>
#include <unistd.h>
#include "hsWide.h"


static FILE * gMutexTimerFile = nil;
static void InitMutexTimerFile()
{
    if ( !gMutexTimerFile )
    {
        gMutexTimerFile = fopen( "log/MutexTimes.log", "wt" );
        if ( gMutexTimerFile )
            fprintf( gMutexTimerFile, "------------------------------------\n" );
    }
}

#endif

//#define EVENT_LOGGING
#ifdef EVENT_LOGGING

#include <ctime>
#include <cstdio>
#include <unistd.h>
#include "NucleusLib/inc/hsTimer.h"


static FILE * gEventLoggingFile = nil;
static void InitEventLoggingFile()
{
    if ( !gEventLoggingFile )
    {
        char fname[256];
        sprintf(fname,"log/Events-%u.log",getpid());
        gEventLoggingFile = fopen( fname, "wt" );
        if ( gEventLoggingFile )
            fprintf( gEventLoggingFile, "------------------------------------\n" );
    }
}

#endif

/////////////////////////////////////////////////////////////////////////////

hsSemaphore::hsSemaphore(int initialValue, const char* name)
{
#ifdef USE_SEMA
    fPSema = nil;
    if ((fNamed = (name != nil))) {
        /* Named semaphore shared between processes */
        fPSema = sem_open(name, O_CREAT, 0666, initialValue);
        if (fPSema == SEM_FAILED)
        {
            hsAssert(0, "hsOSException");
            throw hsOSException(errno);
        }
    } else {
        /* Anonymous semaphore shared between threads */
        int shared = 0; // 1 if sharing between processes
        fPSema = new sem_t;
        int status = sem_init(fPSema, shared, initialValue);
        hsThrowIfOSErr(status);
    }
#else
    int status = ::pthread_mutex_init(&fPMutex, nil);
    hsThrowIfOSErr(status);

    status = ::pthread_cond_init(&fPCond, nil);
    hsThrowIfOSErr(status);

    fCounter = initialValue;
#endif
}

hsSemaphore::~hsSemaphore()
{
#ifdef USE_SEMA
    int status = 0;
    if (fNamed) {
        status = sem_close(fPSema);
    } else {
        status = sem_destroy(fPSema);
        delete fPSema;
    }
    hsThrowIfOSErr(status);
#else
    int status = ::pthread_cond_destroy(&fPCond);
    hsThrowIfOSErr(status);

    status = ::pthread_mutex_destroy(&fPMutex);
    hsThrowIfOSErr(status);
#endif
}

bool hsSemaphore::Wait(hsMilliseconds timeToWait)
{
#ifdef USE_SEMA  // SHOULDN'T THIS USE timeToWait??!?!? -rje
    // shouldn't this use sem_timedwait? -dpogue (2012-03-04)
    hsAssert( timeToWait==kPosInfinity32, "sem_t does not support wait with timeout. #undef USE_SEMA and recompile." );
    int status = sem_wait(fPSema);
    hsThrowIfOSErr(status);
    return true;
#else
    bool  retVal = true;
    int status = ::pthread_mutex_lock(&fPMutex);
    hsThrowIfOSErr(status);

    if (timeToWait == kPosInfinity32)
    {   while (fCounter == 0)
        {   status = ::pthread_cond_wait(&fPCond, &fPMutex);
            hsThrowIfOSErr(status);
        }
    }
    else
    {   timespec spec;
        int  result;

        result = ::clock_gettime(CLOCK_REALTIME, &spec);
        hsThrowIfFalse(result == 0);

        spec.tv_sec += timeToWait / 1000;
        spec.tv_nsec += (timeToWait % 1000) * 1000 * 1000;
        while (spec.tv_nsec >= 1000 * 1000 * 1000)
        {   spec.tv_sec += 1;
            spec.tv_nsec -= 1000 * 1000 * 1000;
        }

        while (fCounter == 0)
        {   status = ::pthread_cond_timedwait(&fPCond, &fPMutex, &spec);
            if (status == ETIMEDOUT)
            {   retVal = false;
                goto EXIT;
            }
            hsThrowIfOSErr(status);
        }
    }

    hsAssert(fCounter > 0, "oops");
    fCounter -= 1;
EXIT:
    status = ::pthread_mutex_unlock(&fPMutex);
    hsThrowIfOSErr(status);
    return retVal;
#endif
}

void hsSemaphore::Signal()
{
#ifdef USE_SEMA
    int status = sem_post(fPSema);
    hsThrowIfOSErr(status);
#else
    int status = ::pthread_mutex_lock(&fPMutex);
    hsThrowIfOSErr(status);

    fCounter += 1;

    status = ::pthread_mutex_unlock(&fPMutex);
    hsThrowIfOSErr(status);

    status = ::pthread_cond_signal(&fPCond);
    hsThrowIfOSErr(status);
#endif
}

void hsSleep::Sleep(uint32_t millis)
{
    uint32_t secs = millis / 1000;
    if (secs > 0)
    {
        millis %= 1000;
        ::sleep(secs);
    }
    usleep(millis*1000);
}
