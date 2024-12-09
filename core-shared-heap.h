/*
 * Copyright (C) 2023-2024 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#ifndef CORE_SHARED_HEAP_H
#define CORE_SHARED_HEAP_H

#include "stress-ng.h"
#include "core-setting.h"

extern WARN_UNUSED void *stress_shared_heap_init(void);
extern void stress_shared_heap_free(void);
extern WARN_UNUSED void *stress_shared_heap_malloc(const size_t size);
extern WARN_UNUSED char *stress_shared_heap_dup_const(const char *str);

#endif
