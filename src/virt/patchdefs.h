/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Definitions of which patch functions handle which syscalls
// Uses macros, assumes you'll include this from somewhere else

// Unconditional patches

// File system -- fs.cpp
PF(SYS_open, PatchOpen);
PF(SYS_openat, PatchOpen);

// Port virtualization -- ports.cpp
PF(SYS_bind, PatchBind);
PF(SYS_getsockname, PatchGetsockname);
PF(SYS_connect, PatchConnect);

// CPU virtualization -- cpu.cpp
PF(SYS_getcpu, PatchGetcpu);
PF(SYS_sched_getaffinity, PatchSchedGetaffinity);
PF(SYS_sched_setaffinity, PatchSchedSetaffinity);

// NUMA virtualization -- numa.cpp
PF(SYS_get_mempolicy, PatchGetMempolicy);
PF(SYS_set_mempolicy, PatchSetMempolicy);
PF(SYS_mbind, PatchMbind);
PF(SYS_migrate_pages, PatchMigratePages);
PF(SYS_move_pages, PatchMovePages);
PF(SYS_munmap, PatchMunmap);
PF(SYS_mremap, PatchMremap);

// Thread control -- control.cpp
PF(SYS_exit_group, PatchExitGroup);

// Conditional patches, only when not fast-forwarded

// Time virtualization -- time.cpp
PF(SYS_gettimeofday, PatchGettimeofday);
PF(SYS_time, PatchTime);
PF(SYS_clock_gettime, PatchClockGettime);
PF(SYS_nanosleep, PatchNanosleep);
PF(SYS_clock_nanosleep, PatchNanosleep);

// Timeout virtualization -- timeout.cpp
PF(SYS_futex, PatchTimeoutSyscall);
PF(SYS_epoll_wait, PatchTimeoutSyscall);
PF(SYS_epoll_pwait, PatchTimeoutSyscall);
PF(SYS_poll, PatchTimeoutSyscall);

