/*
 * Copyright (C) 2016-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
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
#include "stress-ng.h"
#include "core-asm-ppc64.h"
#include "core-asm-x86.h"
#include "core-builtin.h"
#include "core-cpu.h"
#include "core-cpu-cache.h"
#include "core-put.h"

#define MIN_PREFETCH_L3_SIZE      (4 * KB)
#define MAX_PREFETCH_L3_SIZE      (MAX_MEM_LIMIT)
#define DEFAULT_PREFETCH_L3_SIZE  (4 * MB)

#define STRESS_PREFETCH_OFFSETS	(128)
#define STRESS_CACHE_LINE_SIZE	(64)


static const stress_help_t help[] = {
	{ NULL,	"prefetch N" ,		"start N workers exercising memory prefetching " },
	{ NULL,	"prefetch-l3-size N",	"specify the L3 cache size of the CPU" },
	{ NULL, "prefetch-method M",	"specify the prefetch method" },
	{ NULL,	"prefetch-ops N",	"stop after N bogo prefetching operations" },
	{ NULL,	NULL,                   NULL }
};

typedef struct {
	size_t	offset;
	uint64_t count;
	double	duration;
	double 	bytes;
	double 	rate;
} stress_prefetch_info_t;

typedef struct {
	char *name;
	int method;
	bool (*available)(void);
	bool check_prefetch_rate;
} stress_prefetch_method_t;

#define STRESS_PREFETCH_BUILTIN		(0)
#define STRESS_PREFETCH_BUILTIN_L0	(1)
#define STRESS_PREFETCH_BUILTIN_L3	(2)
#define STRESS_PREFETCH_X86_PREFETCHT0	(3)
#define STRESS_PREFETCH_X86_PREFETCHT1	(4)
#define STRESS_PREFETCH_X86_PREFETCHT2	(5)
#define STRESS_PREFETCH_X86_PREFETCHNTA	(6)
#define STRESS_PREFETCH_PPC64_DCBT	(7)
#define STRESS_PREFETCH_PPC64_DCBTST	(8)

static inline bool stress_prefetch_true(void)
{
	return true;
}

stress_prefetch_method_t prefetch_methods[] = {
	{ "builtin",		STRESS_PREFETCH_BUILTIN,	stress_prefetch_true,	false },
	{ "builtinl0",		STRESS_PREFETCH_BUILTIN_L0,	stress_prefetch_true,	false },
	{ "builtinl3",		STRESS_PREFETCH_BUILTIN_L3,	stress_prefetch_true,	false },
#if defined(HAVE_ASM_X86_PREFETCHT0)
	{ "prefetcht0",		STRESS_PREFETCH_X86_PREFETCHT0,	stress_cpu_x86_has_sse,	true },
#endif
#if defined(HAVE_ASM_X86_PREFETCHT1)
	{ "prefetcht1",		STRESS_PREFETCH_X86_PREFETCHT1, stress_cpu_x86_has_sse,	true },
#endif
#if defined(HAVE_ASM_X86_PREFETCHT2)
	{ "prefetcht2",		STRESS_PREFETCH_X86_PREFETCHT2, stress_cpu_x86_has_sse,	true },
#endif
#if defined(HAVE_ASM_X86_PREFETCHNTA)
	{ "prefetchnta",	STRESS_PREFETCH_X86_PREFETCHNTA, stress_cpu_x86_has_sse, true },
#endif
#if defined(HAVE_ASM_PPC64_DCBT)
	{ "dcbt",		STRESS_PREFETCH_PPC64_DCBT,	stress_prefetch_true,	true },
#endif
#if defined(HAVE_ASM_PPC64_DCBTST)
	{ "dcbtst",		STRESS_PREFETCH_PPC64_DCBTST,	stress_prefetch_true,	true },
#endif
};

static int stress_set_prefetch_L3_size(const char *opt)
{
	uint64_t prefetch_L3_size;
	size_t sz;

	prefetch_L3_size = stress_get_uint64_byte(opt);
	stress_check_range_bytes("prefetch-L3-size", prefetch_L3_size,
		MIN_PREFETCH_L3_SIZE, MAX_PREFETCH_L3_SIZE);
	sz = (size_t)prefetch_L3_size;

	return stress_set_setting("prefetch-L3-size", TYPE_ID_SIZE_T, &sz);
}

static int stress_set_prefetch_method(const char *opt)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(prefetch_methods); i++) {
		if (!strcmp(prefetch_methods[i].name, opt)) {
			return stress_set_setting("prefetch-method", TYPE_ID_SIZE_T, &i);
		}
	}

	(void)fprintf(stderr, "prefetch-method must be one of:");
	for (i = 0; i < SIZEOF_ARRAY(prefetch_methods); i++) {
		(void)fprintf(stderr, " %s", prefetch_methods[i].name);
	}
	(void)fprintf(stderr, "\n");
	return -1;
}

static inline uint64_t get_prefetch_L3_size(const stress_args_t *args)
{
	uint64_t cache_size = DEFAULT_PREFETCH_L3_SIZE;
#if defined(__linux__)
	stress_cpu_cache_cpus_t *cpu_caches;
	stress_cpu_cache_t *cache = NULL;
	uint16_t max_cache_level;

	cpu_caches = stress_cpu_cache_get_all_details();
	if (!cpu_caches) {
		if (!args->instance)
			pr_inf("%s: using built-in defaults as unable to "
				"determine cache details\n", args->name);
		return cache_size;
	}
	max_cache_level = stress_cpu_cache_get_max_level(cpu_caches);
	if ((max_cache_level > 0) && (max_cache_level < 3) && (!args->instance))
		pr_inf("%s: no L3 cache, using L%" PRIu16 " size instead\n",
			args->name, max_cache_level);

	cache = stress_cpu_cache_get(cpu_caches, max_cache_level);
	if (!cache) {
		if (!args->instance)
			pr_inf("%s: using built-in defaults as no suitable "
				"cache found\n", args->name);
		stress_free_cpu_caches(cpu_caches);
		return cache_size;
	}
	if (!cache->size) {
		if (!args->instance)
			pr_inf("%s: using built-in defaults as unable to "
				"determine cache size\n", args->name);
		stress_free_cpu_caches(cpu_caches);
		return cache_size;
	}
	cache_size = cache->size;

	stress_free_cpu_caches(cpu_caches);
#else
	if (!args->instance)
		pr_inf("%s: using built-in defaults as unable to "
			"determine cache details\n", args->name);
#endif
	return cache_size;
}

static inline void stress_prefetch_builtin(const void *addr)
{
	shim_builtin_prefetch(addr);
}

static inline void stress_prefetch_builtin_locality0(const void *addr)
{
	shim_builtin_prefetch(addr, 0, 0);
}

static inline void stress_prefetch_builtin_locality3(const void *addr)
{
	shim_builtin_prefetch(addr, 0, 3);
}

static inline void stress_prefetch_none(const void *addr)
{
	(void)addr;
}

#define STRESS_PREFETCH_LOOP(func, type)				\
	if (verify) {							\
		checksum = 0;						\
		while (ptr < l3_data_end) {				\
			func(pre_ptr);					\
			checksum += *(ptr + 0);				\
			checksum += *(ptr + 1);				\
			checksum += *(ptr + 2);				\
			checksum += *(ptr + 3);				\
			pre_ptr += 8;					\
			checksum += *(ptr + 4);				\
			checksum += *(ptr + 5);				\
			checksum += *(ptr + 6);				\
			checksum += *(ptr + 7);				\
			ptr += 8;					\
		}							\
		if (checksum != checksum_sane) {			\
			pr_fail("%s: %s method: checksum failure, got "	\
				"0x%" PRIx64 ", expected 0x%" PRIx64 "\n",\
				args->name, type,			\
				checksum, checksum_sane);		\
			*success = false;				\
		}							\
	} else {							\
		while (ptr < l3_data_end) {				\
			func(pre_ptr);					\
			(void)(*(ptr + 0));				\
			(void)(*(ptr + 1));				\
			(void)(*(ptr + 2));				\
			(void)(*(ptr + 3));				\
			pre_ptr += 8;					\
			(void)(*(ptr + 4));				\
			(void)(*(ptr + 5));				\
			(void)(*(ptr + 6));				\
			(void)(*(ptr + 7));				\
			ptr += 8;					\
		}							\
	}

static inline void OPTIMIZE3 stress_prefetch_benchmark(
	const stress_args_t *args,
	stress_prefetch_info_t *prefetch_info,
	const size_t prefetch_method,
	const size_t i,
	const uint64_t checksum_sane,
	uint64_t *RESTRICT l3_data,
	uint64_t *RESTRICT l3_data_end,
	uint64_t *total_count,
	const bool verify,
	bool *success)
{
	double t1, t2, t3, t4;
	const size_t l3_data_size = (uintptr_t)l3_data_end - (uintptr_t)l3_data;
	volatile uint64_t *ptr;
	uint64_t *pre_ptr;
	register uint64_t checksum;

	shim_cacheflush((char *)l3_data, (int)l3_data_size, SHIM_DCACHE);
#if defined(HAVE_BUILTIN___CLEAR_CACHE)
	__builtin___clear_cache((void *)l3_data, (void *)l3_data_end);
#endif

	/* Benchmark loop */
	ptr = l3_data;
	pre_ptr = l3_data + prefetch_info[i].offset;
	t1 = stress_time_now();
	while (ptr < l3_data_end) {
		ptr += 8;
		pre_ptr += 8;
		shim_mb();
	}
	t2 = stress_time_now();
	stress_void_ptr_put((volatile void *)ptr);
	stress_void_ptr_put((volatile void *)pre_ptr);

	shim_cacheflush((char *)l3_data, (int)l3_data_size, SHIM_DCACHE);
#if defined(HAVE_BUILTIN___CLEAR_CACHE)
	__builtin___clear_cache((void *)l3_data, (void *)l3_data_end);
#endif

	ptr = l3_data;
	pre_ptr = l3_data + prefetch_info[i].offset;
	t3 = stress_time_now();

	/* Benchmark reads */
	if (prefetch_info[i].offset == 0) {
		/* Benchmark no prefetch */
		STRESS_PREFETCH_LOOP(stress_prefetch_none, "no prefetch");
	} else {
		switch (prefetch_method) {
		default:
		case STRESS_PREFETCH_BUILTIN:
			STRESS_PREFETCH_LOOP(stress_prefetch_builtin, "builtin_prefetch");
			break;
		case STRESS_PREFETCH_BUILTIN_L0:
			STRESS_PREFETCH_LOOP(stress_prefetch_builtin_locality0, "builtin_prefetch locality 0");
			break;
		case STRESS_PREFETCH_BUILTIN_L3:
			STRESS_PREFETCH_LOOP(stress_prefetch_builtin_locality3, "builtin_prefetch locality 3");
			break;
#if defined(HAVE_ASM_X86_PREFETCHT0)
		case STRESS_PREFETCH_X86_PREFETCHT0:
			STRESS_PREFETCH_LOOP(stress_asm_x86_prefetcht0, "x86 prefetcht0");
			break;
#endif
#if defined(HAVE_ASM_X86_PREFETCHT1)
		case STRESS_PREFETCH_X86_PREFETCHT1:
			STRESS_PREFETCH_LOOP(stress_asm_x86_prefetcht1, "x86 prefetcht1");
			break;
#endif
#if defined(HAVE_ASM_X86_PREFETCHT2)
		case STRESS_PREFETCH_X86_PREFETCHT2:
			STRESS_PREFETCH_LOOP(stress_asm_x86_prefetcht2, "x86 prefetcht2");
			break;
#endif
#if defined(HAVE_ASM_X86_PREFETCHNTA)
		case STRESS_PREFETCH_X86_PREFETCHNTA:
			STRESS_PREFETCH_LOOP(stress_asm_x86_prefetchnta, "x86 prefetchnta");
			break;
#endif
#if defined(HAVE_ASM_PPC64_DCBT)
		case STRESS_PREFETCH_PPC64_DCBT:
			STRESS_PREFETCH_LOOP(stress_asm_ppc64_dcbt, "ppc64 dcbt");
			break;
#endif
#if defined(HAVE_ASM_PPC64_DCBTST)
		case STRESS_PREFETCH_PPC64_DCBTST:
			STRESS_PREFETCH_LOOP(stress_asm_ppc64_dcbtst, "ppc64 dcbtst");
			break;
#endif
		}
	}
	stress_void_ptr_put(pre_ptr);
	t4 = stress_time_now();

	/* Update stats */
	prefetch_info[i].bytes += (double)l3_data_size;
	prefetch_info[i].duration += (t4 - t3) - (t2 - t1);
	prefetch_info[i].count++;
	(*total_count)++;
}

static uint64_t stress_prefetch_data_set(uint64_t *l3_data, uint64_t *l3_data_end)
{
        register uint32_t const a = 16843009;
        register uint32_t const c = 826366247;
        register uint32_t seed = 123456789;
	register uint64_t checksum = 0;

	while (l3_data < l3_data_end) {
		uint64_t val;

		seed = (a * seed + c);
		val = seed;
		seed = (a * seed + c);
		val |= (uint64_t)seed << 32;;

		*(l3_data++) = val;
		checksum += val;
	}
	return checksum;
}

/*
 *  stress_prefetch()
 *	stress cache/memory/CPU with stream stressors
 */
static int stress_prefetch(const stress_args_t *args)
{
	uint64_t *l3_data, *l3_data_end, total_count = 0, checksum_sane;
	size_t l3_data_size = 0, l3_data_mmap_size;
	stress_prefetch_info_t prefetch_info[STRESS_PREFETCH_OFFSETS];
	size_t i, best;
	size_t prefetch_method = STRESS_PREFETCH_BUILTIN;
	double best_rate, ns, non_prefetch_rate;
	bool success = true;
	bool check_prefetch_rate;
	const bool verify = !!(g_opt_flags & OPT_FLAGS_VERIFY);

	(void)stress_get_setting("prefetch-method", &prefetch_method);

	if (!prefetch_methods[prefetch_method].available()) {
		(void)pr_inf("%s: prefetch-method '%s' is not available on this CPU, skipping stressor\n",
			args->name, prefetch_methods[prefetch_method].name);
		return EXIT_NO_RESOURCE;
	}
#if defined(STRESS_ARCH_X86_64)
	check_prefetch_rate = true;
#else
	check_prefetch_rate = prefetch_methods[prefetch_method].check_prefetch_rate;
#endif

	(void)stress_get_setting("prefetch-L3-size", &l3_data_size);
	if (l3_data_size == 0)
		l3_data_size = get_prefetch_L3_size(args);

	l3_data_mmap_size = l3_data_size + (STRESS_PREFETCH_OFFSETS * STRESS_CACHE_LINE_SIZE);

	l3_data = (uint64_t *)mmap(NULL, l3_data_mmap_size,
		PROT_READ | PROT_WRITE,
#if defined(MAP_POPULATE)
		MAP_POPULATE |
#endif
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (l3_data == MAP_FAILED) {
		pr_inf_skip("%s: cannot allocate %zu bytes, skipping stressor\n",
			args->name, l3_data_mmap_size);
		return EXIT_NO_RESOURCE;
	}

	l3_data_end = (uint64_t *)((uintptr_t)l3_data + l3_data_size);
	checksum_sane = stress_prefetch_data_set(l3_data, l3_data_end);

	for (i = 0; i < SIZEOF_ARRAY(prefetch_info); i++) {
		prefetch_info[i].offset = i * STRESS_CACHE_LINE_SIZE;
		prefetch_info[i].count = 0;
		prefetch_info[i].duration = 0.0;
		prefetch_info[i].bytes = 0.0;
		prefetch_info[i].rate = 0.0;
	}
	if (args->instance == 0) {
		pr_inf("%s: using a %zd KB L3 cache with prefetch method '%s'\n",
		args->name, l3_data_size >> 10, prefetch_methods[prefetch_method].name);
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		for (i = 0; i < SIZEOF_ARRAY(prefetch_info); i++) {
			stress_prefetch_benchmark(args, prefetch_info,
				prefetch_method, i, checksum_sane,
				l3_data, l3_data_end, &total_count,
				verify, &success);
			if (!success)
				break;
		}
		stress_bogo_inc(args);
	} while (success && stress_continue(args));

	best = 0;
	best_rate = 0.0;
	for (i = 0; i < SIZEOF_ARRAY(prefetch_info); i++) {
		if (prefetch_info[i].duration > 0.0)
			prefetch_info[i].rate = prefetch_info[i].bytes / prefetch_info[i].duration;
		else
			prefetch_info[i].rate = 0.0;

		if (prefetch_info[i].rate > best_rate) {
			best_rate = prefetch_info[i].rate;
			best = i;
		}
	}

	non_prefetch_rate = prefetch_info[0].rate / (double)GB;
	stress_metrics_set(args, 0, "GB per sec non-prefetch read rate", non_prefetch_rate);

	if (best_rate > 0.0)
		ns = STRESS_DBL_NANOSECOND * (double)prefetch_info[best].offset / best_rate;
	else
		ns = 0.0;

	pr_dbg("%s: best prefetch read rate @ %.2f GB per sec at offset %zd (~%.2f nanosecs)\n",
		args->name, best_rate / (double)GB,
		prefetch_info[best].offset, ns);

	best_rate /= (double)GB;
	stress_metrics_set(args, 1, "GB per sec best read rate", best_rate);

	/* sanity check prefetch rates */
	if (verify && check_prefetch_rate && (best_rate < non_prefetch_rate)) {
		pr_fail("%s: non-prefetch rate %.2f GB per sec higher "
			"than best prefetch rate %.2f GB per sec\n",
			args->name, non_prefetch_rate, best_rate);
		success = false;
	}

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	(void)munmap((void *)l3_data, l3_data_mmap_size);

	return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_prefetch_l3_size,	stress_set_prefetch_L3_size },
	{ OPT_prefetch_method,	stress_set_prefetch_method  },
	{ 0,			NULL }
};

stressor_info_t stress_prefetch_info = {
	.stressor = stress_prefetch,
	.class = CLASS_CPU | CLASS_CPU_CACHE | CLASS_MEMORY,
	.opt_set_funcs = opt_set_funcs,
	.verify = VERIFY_OPTIONAL,
	.help = help
};
