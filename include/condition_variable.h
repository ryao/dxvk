#ifndef DXVK_CONDITION_VARIABLE_H_
#define DXVK_CONDITION_VARIABLE_H_

#include <condition_variable>
#if !defined(_GLIBCXX_HAS_GTHREADS) && defined(__MINGW32__) && !defined(_MSC_VER)
#include <dxvk_mingw_private.h>
#include <mingw.condition_variable.h>
#endif

#endif /* DXVK_CONDITION_VARIABLE_H_ */
