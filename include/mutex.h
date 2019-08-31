#ifndef DXVK_MUTEX_H_
#define DXVK_MUTEX_H_

#include <mutex>
#if !defined(_GLIBCXX_HAS_GTHREADS) && defined(__MINGW32__) && !defined(_MSC_VER)
#include <dxvk_mingw_private.h>
#include <mingw.mutex.h>
#endif

#endif /* DXVK_MUTEX_H_ */
