/* GLIB sliced memory - fast concurrent memory chunk allocator
 * Copyright (C) 2005 Tim Janik
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
/* MT safe */

#include "config.h"
#include "glibconfig.h"

#include <stdlib.h>             /* posix_memalign() */
#include <string.h>
#include <errno.h>

#ifdef G_OS_UNIX
#include <unistd.h>             /* sysconf() */
#endif
#ifdef G_OS_WIN32
#include <windows.h>
#include <process.h>
#endif

#include <stdio.h>              /* fputs */

#include "gslice.h"

#include "gmem.h"               /* gslice.h */
#include "gstrfuncs.h"

/* optimized version of ALIGN (size, P2ALIGNMENT) */
#if     GLIB_SIZEOF_SIZE_T * 2 == 8  /* P2ALIGNMENT */
#define P2ALIGN(size)   (((size) + 0x7) & ~(gsize) 0x7)
#elif   GLIB_SIZEOF_SIZE_T * 2 == 16 /* P2ALIGNMENT */
#define P2ALIGN(size)   (((size) + 0xf) & ~(gsize) 0xf)
#else
#define P2ALIGN(size)   ALIGN (size, P2ALIGNMENT)
#endif
gpointer
g_slice_alloc (gsize mem_size)
{
  gpointer mem;
  mem = g_malloc (mem_size);

  return mem;
}

gpointer
g_slice_alloc0 (gsize mem_size)
{
  gpointer mem = g_slice_alloc (mem_size);
  if (mem)
    memset (mem, 0, mem_size);
  return mem;
}

void
g_slice_free1 (gsize    mem_size,
               gpointer mem_block)
{
   g_free (mem_block);
}
void
g_slice_free_chain_with_offset (gsize    mem_size,
                                gpointer mem_chain,
                                gsize    next_offset)
{
  gpointer slice = mem_chain;
  /* while the thread magazines and the magazine cache are implemented so that
   * they can easily be extended to allow for free lists containing more free
   * lists for the first level nodes, which would allow O(1) freeing in this
   * function, the benefit of such an extension is questionable, because:
   * - the magazine size counts will become mere lower bounds which confuses
   *   the code adapting to lock contention;
   * - freeing a single node to the thread magazines is very fast, so this
   *   O(list_length) operation is multiplied by a fairly small factor;
   * - memory usage histograms on larger applications seem to indicate that
   *   the amount of released multi node lists is negligible in comparison
   *   to single node releases.
   * - the major performance bottle neck, namely g_private_get() or
   *   g_mutex_lock()/g_mutex_unlock() has already been moved out of the
   *   inner loop for freeing chained slices.
   */
  gsize chunk_size = P2ALIGN (mem_size);
    while (slice)
      {
        guint8 *current = slice;
        slice = *(gpointer*) (current + next_offset);
        g_free (current);
      }
}
