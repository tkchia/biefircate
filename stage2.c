/*
 * Copyright (c) 2020--2021 TK Chia
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "truckload.h"

INIT_TEXT static void set_up_trampolines()
{
	void *reserved_base_mem = mem_map_reserve_page(0xf0000ULL);
	cprintf("setting up long mode \u2194 real mode trampolines @%p\n",
	    reserved_base_mem);
	lm86_rm86_init(reserved_base_mem);
}

INIT_TEXT void stage2(void)
{
	set_up_trampolines();
}
