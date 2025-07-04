/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*	hfs_dbg.h
 *
 *	(c) 1997 Apple Computer, Inc.  All Rights Reserved
 *
 *	hfs_dbg.h -- debugging macros for HFS file system.
 *
 *	HISTORY
 *	10-Nov-1998 Pat Dirks		Cleaned up definition of DBG_ASSERT to handle embedded '%' correctly.
 *	28-Apr-1998	Scott Roberts	Reorganized and added HFS_DEBUG_STAGE
 *	17-Nov-1997	Pat Dirks		Pat Dirks at Apple Computer
 *								Derived from old hfs version.
 */

struct componentname;

/* Define the debugging stage...
		4 -> Do all, aggresive, call_kdp
		3 -> debug asserts and debug err, panic instead of call_kdp
		2 -> debug error, no kdb
		1 -> very little, panic only
*/
#ifndef HFS_DIAGNOSTIC
#define HFS_DIAGNOSTIC 0
#endif /* HFS_DIAGNOSTIC */

#ifndef HFS_DEBUG_STAGE
#if HFS_DIAGNOSTIC
#define HFS_DEBUG_STAGE 4
#else
#define HFS_DEBUG_STAGE 1
#endif /* _KERNEL */
#endif /* HFS_DEBUG_STAGE */

#ifdef DARWIN
#ifdef _KERNEL
#define PRINTIT kprintf
#else /* _KERNEL */
#define PRINTIT printf
#endif /* _KERNEL */
#else
#define PRINTIT printf
#endif /* DARWIN */

#if (HFS_DEBUG_STAGE > 3)
#define DEBUG_BREAK Debugger("");
#else
#define DEBUG_BREAK
#endif

#if (HFS_DEBUG_STAGE == 4)
#define DEBUG_BREAK_MSG(PRINTF_ARGS) \
	{                            \
		PRINTIT PRINTF_ARGS; \
		DEBUG_BREAK          \
	};
#elif (HFS_DEBUG_STAGE == 3)
#define DEBUG_BREAK_MSG(PRINTF_ARGS) \
	{                            \
		panic PRINTF_ARGS;   \
	};
#else
#define DEBUG_BREAK_MSG(PRINTF_ARGS) \
	{                            \
		PRINTIT PRINTF_ARGS; \
	};
#endif

// #define PRINT_DELAY (void) tsleep((caddr_t)&lbolt, PPAUSE, "hfs kprintf", 0)
#define PRINT_DELAY

/*
 * Debugging macros.
 */
#if HFS_DIAGNOSTIC
extern int hfs_dbg_all;
extern int hfs_dbg_err;

#ifdef _KERNEL
#if (HFS_DEBUG_STAGE == 4)
#define DBG_ASSERT(a)                                              \
	{                                                          \
		if (!(a)) {                                        \
			char gDebugAssertStr[255];                 \
			sprintf(gDebugAssertStr,                   \
			    "Oops - File "__FILE__                 \
			    ", line %d: assertion '%s' failed.\n", \
			    __LINE__, #a);                         \
			Debugger(gDebugAssertStr);                 \
		}                                                  \
	}
#else
#define DBG_ASSERT(a)                                                \
	{                                                            \
		if (!(a)) {                                          \
			panic("File "__FILE__                        \
			      ", line %d: assertion '%s' failed.\n", \
			    __LINE__, #a);                           \
		}                                                    \
	}
#endif /* HFS_DEBUG_STAGE */
#else
#define DBG_ASSERT(a) assert(a)
#endif /* _KERNEL */

#define DBG_ERR(x)                                                  \
	{                                                           \
		if (hfs_dbg_all || hfs_dbg_err) {                   \
			PRINTIT("%X: ", curthread->td_proc->p_pid); \
			PRINTIT("HFS ERROR: ");                     \
			PRINTIT x;                                  \
			PRINT_DELAY;                                \
		};                                                  \
	}

#else /* HFS_DIAGNOSTIC */

#define DBG_ASSERT(a)
#define DBG_ERR(x)

#endif /* HFS_DIAGNOSTIC */
