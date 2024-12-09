//
// Created by hvz on 09/12/2024.
//

#ifndef PA_MAC_CORE_ATOMIC_H
#define PA_MAC_CORE_ATOMIC_H

#include "pa_mac_core_internal.h"

#ifdef MOSX_USE_NON_ATOMIC_FLAG_BITS
# define OSAtomicOr32( a, b ) ( (*(b)) |= (a) )
# define OSAtomicAnd32( a, b ) ( (*(b)) &= (a) )
#else
# include <libkern/OSAtomic.h>
#endif

#endif //PA_MAC_CORE_ATOMIC_H
