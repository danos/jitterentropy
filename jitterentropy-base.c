﻿/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014
 *
 * Design
 * ======
 *
 * See documentation in doc/ folder.
 *
 * Interface
 * =========
 *
 * See documentation in doc/ folder.
 *
 * License
 * =======
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "jitterentropy.h"

#ifndef CONFIG_CRYPTO_CPU_JITTERENTROPY_STAT
 /* only check optimization in a compilation for real work */
 #ifdef __OPTIMIZE__
  #error "The CPU Jitter random number generator must not be compiled with optimizations. See documentation. Use the compiler switch -O0 for compiling jitterentropy-base.c."
 #endif
#endif

/*
 * Update of the loop count used for the next round of
 * an entropy collection.
 *
 * Input:
 * @ec entropy collector struct -- may be NULL
 * @bits is the number of low bits of the timer to consider
 * @min is the number of bits we shift the timer value to the right at
 * 	the end to make sure we have a guaranteed minimum value
 *
 * Return:
 * Newly calculated loop counter
 */
static __u64 jent_loop_shuffle(struct rand_data *ec,
			       unsigned int bits, unsigned int min)
{
	__u64 time = 0;
	__u64 shuffle = 0;
	int i = 0;
	unsigned int mask = (1<<bits) - 1;

	jent_get_nstime(&time);
	/* mix the current state of the random number into the shuffle
	 * calculation to balance that shuffle a bit more */
	if (ec)
		time ^= ec->data;
	/* we fold the time value as much as possible to ensure that as many
	 * bits of the time stamp are included as possible */
	for(i = 0; (DATA_SIZE_BITS / bits) > i; i++) {
		shuffle ^= time & mask;
		time = time >> bits;
	}

	/* We add a lower boundary value to ensure we have a minimum
	 * RNG loop count. */
	return (shuffle + (1<<min));
}

/***************************************************************************
 * Noise sources
 ***************************************************************************/

/*
 * CPU Jitter noise source -- this is the noise source based on the CPU
 * 			      execution time jitter
 *
 * This function folds the time into TIME_ENTROPY_BITS bits by iterating
 * through the DATA_SIZE_BITS bit time value as follows: assume our time value
 * is 0xaaabbbcccddd, TIME_ENTROPY_BITS is 3
 * 1st loop, 1st shift generates 0xddd000000000
 * 1st loop, 2nd shift generates 0x000000000ddd
 * 2nd loop, 1st shift generates 0xcccddd000000
 * 2nd loop, 2nd shift generates 0x000000000ccc
 * 3rd loop, 1st shift generates 0xbbbcccddd000
 * 3rd loop, 2nd shift generates 0x000000000bbb
 * 4th loop, 1st shift generates 0xaaabbbcccddd
 * 4th loop, 2nd shift generates 0x000000000aaa
 * Now, the values at the end of the 2nd shifts are XORed together.
 * Note, the loop only performs (DATA_SIZE_BITS / TIME_SIZE) iterations. If the
 * division is not complete, it takes the lower bound (e.g. 64 / 3 would result
 * 21). Thus, the upmost bits that are less than TIME_SIZE in size (which are
 * assumed to have no entropy to begin with) are discarded.
 *
 * The code is deliberately inefficient and shall stay that way. This function
 * is the root cause why the code shall be compiled without optimization. This
 * function not only acts as folding operation, but this function's execution
 * is used to measure the CPU execution time jitter. Any change to the loop in
 * this function implies that careful retesting must be done.
 *
 * Input:
 * @ec entropy collector struct -- may be NULL
 * @time time stamp to be folded
 * @loop_cnt if a value not equal to 0 is set, use the given value as number of
 *	     loops to perform the folding
 *
 * Output:
 * @folded result of folding operation
 *
 * Return:
 * Number of loops the folding operation is performed
 */
static __u64 jent_fold_time(struct rand_data *ec, __u64 time,
			    __u64 *folded, __u64 loop_cnt)
{
	int i, j;
	__u64 new = 0;
#define MAX_FOLD_LOOP_BIT 4
#define MIN_FOLD_LOOP_BIT 0
	__u64 fold_loop_cnt =
		jent_loop_shuffle(ec, MAX_FOLD_LOOP_BIT, MIN_FOLD_LOOP_BIT);

	/* testing purposes -- allow test app to set the counter, not
	 * needed during runtime */
	if (loop_cnt)
		fold_loop_cnt = loop_cnt;
	for (j = 0; j < fold_loop_cnt; j++) {
		new = 0;
		for (i = 1; (DATA_SIZE_BITS / TIME_ENTROPY_BITS) >= i; i++) {
			__u64 tmp = time <<
				(DATA_SIZE_BITS - (TIME_ENTROPY_BITS * i));
			tmp = tmp >> (DATA_SIZE_BITS - TIME_ENTROPY_BITS);
			new ^= tmp;
		}
	}
	*folded = new;
	return fold_loop_cnt;
}

/* 
 * Memory Access noise source -- this is a noise source based on variations in
 * 				 memory access times
 *
 * This function performs memory accesses which will add to the timing
 * variations due to an unknown amount of CPU wait states that need to be
 * added when accessing memory. The memory size should be larger than the L1
 * caches as outlined in the documentation and the associated testing.
 *
 * The L1 cache has a very high bandwidth, albeit its access rate is  usually
 * slower than accessing CPU registers. Therefore, L1 accesses only add minimal
 * variations as the CPU has hardly to wait. Starting with L2, significant
 * variations are added because L2 typically does not belong to the CPU any more
 * and therefore a wider range of CPU wait states is necessary for accesses.
 * L3 and real memory accesses have even a wider range of wait states. However,
 * to reliably access either L3 or memory, the ec->mem memory must be quite large
 * which is usually not desirable.
 *
 * Input:
 * @ec Reference to the entropy collector with the memory access data -- if
 *     the reference to the memory block to be accessed is NULL, this noise
 *     source is disabled
 *
 * Output:
 * nothing -- the state of the memory access data in @ec is updated
 *
 * Return:
 * Number of memory access operations
 */
static unsigned int jent_memaccess(struct rand_data *ec)
{
	unsigned char *tmpval = NULL;
	unsigned int wrap = 0;
	unsigned int i = 0;

	if (NULL == ec || NULL == ec->mem)
		return 0;
	wrap = ec->memblocksize * ec->memblocks;
	for (i = 0; i < ec->memaccessloops; i++) {
		tmpval = ec->mem + ec->memlocation;
		/* memory access: just add 1 to one byte,
		 * wrap at 255 -- memory access implies read
		 * from and write to memory location */
		*tmpval = (*tmpval + 1) & 0xff;
		/* Addition of memblocksize - 1 to pointer
		 * with wrap around logic to ensure that every
		 * memory location is hit evenly
		 */
		ec->memlocation = ec->memlocation + ec->memblocksize - 1;
		ec->memlocation = ec->memlocation % wrap;
	}
	return i;
}

/***************************************************************************
 * Start of entropy processing logic
 ***************************************************************************/

/*
 * This is the heart of the entropy generation: calculate time deltas and
 * use the CPU jitter in the time deltas. The jitter is folded into one
 * bit. You can call this function the "random bit generator" as it
 * produces one random bit per invocation.
 *
 * WARNING: ensure that ->prev_time is primed before using the output
 * 	    of this function! This can be done by calling this function
 * 	    and not using its result.
 *
 * Input:
 * @entropy_collector Reference to entropy collector
 *
 * Return:
 * One random bit
 *
 */
static __u64 jent_measure_jitter(struct rand_data *entropy_collector)
{
	__u64 time = 0;
	__u64 delta = 0;
	__u64 data = 0;

	/* Invoke one noise source before time measurement to add variations */
	jent_memaccess(entropy_collector);

	/* Get time stamp and calculate time delta to previous invocation 
	 * to measure the timing variations with the previous invocation */
	jent_get_nstime(&time);
	delta = time - entropy_collector->prev_time;
	entropy_collector->prev_time = time;

	/* Now call the next noise sources which also folds the data */
	jent_fold_time(entropy_collector, delta, &data, 0);

	return data;
}

/*
 * Von Neuman unbias as explained in RFC 4086 section 4.2. As shown in the
 * documentation of that RNG, the bits from jent_measure_jitter are considered
 * independent which implies that the Von Neuman unbias operation is applicable.
 * A proof of the Von-Neumann unbias operation to remove skews is given in the
 * document "A proposal for: Functionality classes for random number
 * generators", version 2.0 by Werner Schindler, section 5.4.1.
 *
 * Input:
 * @entropy_collector Reference to entropy collector
 *
 * Return:
 * One random bit
 */
static __u64 jent_unbiased_bit(struct rand_data *entropy_collector)
{
	if (1 == entropy_collector->disable_unbias)
		return (jent_measure_jitter(entropy_collector));
	do {
		__u64 a = jent_measure_jitter(entropy_collector);
		__u64 b = jent_measure_jitter(entropy_collector);
		if (a == b)
			continue;
		if (1 == a)
			return 1;
		else
			return 0;
	} while (1);
}

/*
 * Shuffle the pool a bit by mixing some value with a bijective function (XOR)
 * into the pool.
 *
 * The function generates a mixer value that depends on the bits set and the
 * location of the set bits in the random number generated by the entropy
 * source. Therefore, based on the generated random number, this mixer value
 * can have 2**64 different values. That mixer value is initialized with the
 * first two SHA-1 constants. After obtaining the mixer value, it is XORed into
 * the random number.
 *
 * The mixer value is not assumed to contain any entropy. But due to the XOR
 * operation, it can also not destroy any entropy present in the entropy pool.
 *
 * Input:
 * @entropy_collector Reference to entropy collector
 *
 * Output:
 * nothing
 */
static void jent_stir_pool(struct rand_data *entropy_collector)
{
	/* to shut up GCC on 32 bit, we have to initialize the 64 variable
	 * with two 32 bit variables */
	union c {
		__u64 u64;
		__u32 u32[2];
	};
	/* This constant is derived from the first two 32 bit initialization
	 * vectors of SHA-1 as defined in FIPS 180-4 section 5.3.1 */
	union c constant;
	/* The start value of the mixer variable is derived from the third
	 * and fourth 32 bit initialization vector of SHA-1 as defined in
	 * FIPS 180-4 section 5.3.1 */
	union c mixer;
	int i = 0;

	/* Store the SHA-1 constants in reverse order to make up the 64 bit
	 * value -- this applies to a little endian system, on a big endian
	 * system, it reverses as expected. But this really does not matter
	 * as we do not rely on the specific numbers. We just pick the SHA-1
	 * constants as they have a good mix of bit set and unset. */
	constant.u32[1] = 0x67452301;
	constant.u32[0] = 0xefcdab89;
	mixer.u32[1] = 0x98badcfe;
	mixer.u32[0] = 0x10325476;

	for (i = 0; i < DATA_SIZE_BITS; i++) {
		/* get the i-th bit of the input random number and only XOR
		 * the constant into the mixer value when that bit is set */
		if ((entropy_collector->data >> i) & 0x0000000000000001)
			mixer.u64 ^= constant.u64;
		mixer.u64 = rol64(mixer.u64, 1);
	}
	entropy_collector->data ^= mixer.u64;
}

/*
 * Generator of one 64 bit random number
 * Function fills rand_data->data
 *
 * Input:
 * @entropy_collector Reference to entropy collector
 *
 * Return:
 * Number of loops the entropy collection is performed.
 */
static void jent_gen_entropy(struct rand_data *entropy_collector)
{
	unsigned int k;

	/* number of loops for the entropy collection depends on the size of
	 * the random number and the size of the folded value. We want to
	 * ensure that we pass over each bit of the random value once with the
	 * folded value.  E.g. if we have a random value of 64 bits and 2 bits
	 * of folded size, we need 32 entropy collection loops. If the random
	 * value size is not divisible by the folded value size, we have as
	 * many loops to cover each random number value bit at least once. E.g.
	 * 64 bits random value size and the folded value is 3 bits, we need 22
	 * loops to cover the 64 bits at least once. */
	/* We multiply the loop value with ->osr to obtain the oversampling
	 * rate requested by the caller */
	for (k = 0;
	     k < ((((DATA_SIZE_BITS - 1) / TIME_ENTROPY_BITS) + 1) *
		  entropy_collector->osr);
	     k++) {
		__u64 data = 0;
		__u64 prev_data = entropy_collector->data;
		/* priming of the ->prev_time value in first loop iteration */
		if (!k)
			jent_measure_jitter(entropy_collector);

		data = jent_unbiased_bit(entropy_collector);
		entropy_collector->data ^= data;
		entropy_collector->data = rol64(entropy_collector->data,
						TIME_ENTROPY_BITS);

		/* statistics testing only */
		jent_bit_count(entropy_collector, prev_data);
	}
	if (entropy_collector->stir)
		jent_stir_pool(entropy_collector);
}

/* the continuous test required by FIPS 140-2 -- the function automatically
 * primes the test if needed.
 *
 * Return:
 * 0 if FIPS test passed
 * < 0 if FIPS test failed
 */
static int jent_fips_test(struct rand_data *entropy_collector)
{
	if (!jent_fips_enabled())
		return 0;

	/* shall we somehow allow the caller to reset that? Probably
	 * not, because the caller can de-allocate the entropy collector
	 * instance and set up a new one. */
	if (entropy_collector->fips_fail)
		return -1;

	/* prime the FIPS test */
	if (!entropy_collector->old_data) {
		entropy_collector->old_data = entropy_collector->data;
		jent_gen_entropy(entropy_collector);
	}

	if (entropy_collector->data == entropy_collector->old_data) {
		entropy_collector->fips_fail = 1;
		return -1;
	}
	entropy_collector->old_data = entropy_collector->data;

	return 0;
}

/*
 * Entry function: Obtain entropy for the caller.
 *
 * This function invokes the entropy gathering logic as often to generate
 * as many bytes as requested by the caller. The entropy gathering logic
 * creates 64 bit per invocation.
 *
 * This function truncates the last 64 bit entropy value output to the exact
 * size specified by the caller.
 *
 * @data: pointer to buffer for storing random data -- buffer must already
 *        exist
 * @len: size of the buffer, specifying also the requested number of random
 *       in bytes
 *
 * return: number of bytes returned when request is fulfilled or an error
 *
 * The following error codes can occur:
 * 	-1	FIPS 140-2 continuous self test failed
 * 	-2	entropy_collector is NULL
 */
int jent_read_entropy(struct rand_data *entropy_collector,
		      char *data, size_t len)
{
	char *p = data;
	int ret = 0;
	size_t orig_len = len;

	if (NULL == entropy_collector)
		return -2;

	while (0 < len) {
		size_t tocopy;
		jent_gen_entropy(entropy_collector);
		ret = jent_fips_test(entropy_collector);
		if (0 > ret)
			return ret;

		if ((DATA_SIZE_BITS / 8) < len)
			tocopy = (DATA_SIZE_BITS / 8);
		else
			tocopy = len;
		memcpy(p, &entropy_collector->data, tocopy);

		len -= tocopy;
		p += tocopy;
	}

	/* To be on the safe side, we generate one more round of entropy
	 * which we do not give out to the caller. That round shall ensure
	 * that in case the calling application crashes, memory dumps, pages
	 * out, or due to the CPU Jitter RNG lingering in memory for long
	 * time without being moved and an attacker cracks the application,
	 * all he reads in the entropy pool is a value that is NEVER EVER
	 * being used for anything. Thus, he does NOT see the previous value
	 * that was returned to the caller for cryptographic purposes.
	 */
	/* If we use secured memory, do not use that precaution as the secure
	 * memory protects the entropy pool. Moreover, note that using this
	 * call reduces the speed of the RNG by up to half */
#ifndef CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
	jent_gen_entropy(entropy_collector);
#endif
	return orig_len;
}
#if defined(__KERNEL__) && !defined(MODULE)
EXPORT_SYMBOL(jent_read_entropy);
#endif

/***************************************************************************
 * Initialization logic
 ***************************************************************************/

struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
					       unsigned int flags)
{
	struct rand_data *entropy_collector;

	entropy_collector = jent_zalloc(sizeof(struct rand_data));
	if (NULL == entropy_collector)
		return NULL;

	if (!(flags & JENT_DISABLE_MEMORY_ACCESS)) {
		/* Allocate memory for adding variations based on memory
		 * access
		 */
		entropy_collector->mem = 
			(unsigned char *)jent_zalloc(JENT_MEMORY_SIZE);
		if (NULL == entropy_collector->mem) {
			jent_zfree(entropy_collector, sizeof(struct rand_data));
			return NULL;
		}
		entropy_collector->memblocksize = JENT_MEMORY_BLOCKSIZE;
		entropy_collector->memblocks = JENT_MEMORY_BLOCKS;
		entropy_collector->memaccessloops = JENT_MEMORY_ACCESSLOOPS;
	}

	/* verify and set the oversampling rate */
	if (0 == osr)
		osr = 1; /* minimum sampling rate is 1 */
	entropy_collector->osr = osr;

	entropy_collector->stir = 1;
	if (flags & JENT_DISABLE_STIR)
		entropy_collector->stir = 0;
	if (flags & JENT_DISABLE_UNBIAS)
		entropy_collector->disable_unbias = 1;

	/* fill the data pad with non-zero values */
	jent_gen_entropy(entropy_collector);

	/* initialize the FIPS 140-2 continuous test if needed */
	jent_fips_test(entropy_collector);

	return entropy_collector;
}
#if defined(__KERNEL__) && !defined(MODULE)
EXPORT_SYMBOL(jent_entropy_collector_alloc);
#endif

void jent_entropy_collector_free(struct rand_data *entropy_collector)
{
	if (NULL != entropy_collector->mem)
		jent_zfree(entropy_collector->mem, JENT_MEMORY_SIZE);
	entropy_collector->mem = NULL;
	if (NULL != entropy_collector)
		jent_zfree(entropy_collector, sizeof(struct rand_data));
	entropy_collector = NULL;
}
#if defined(__KERNEL__) && !defined(MODULE)
EXPORT_SYMBOL(jent_entropy_collector_free);
#endif

int jent_entropy_init(void)
{
	int i;
	__u64 delta_sum = 0;
	__u64 old_delta = 0;
	int time_backwards = 0;
	int count_var = 0;
	int count_mod = 0;

	/* We could perform statistical tests here, but the problem is
	 * that we only have a few loop counts to do testing. These
	 * loop counts may show some slight skew and we produce
	 * false positives.
	 *
	 * Moreover, only old systems show potentially problematic
	 * jitter entropy that could potentially be caught here. But
	 * the RNG is intended for hardware that is available or widely
	 * used, but not old systems that are long out of favor. Thus,
	 * no statistical tests.
	 */

	/* We could add a check for system capabilities such as clock_getres or
	 * check for CONFIG_X86_TSC, but it does not make much sense as the
	 * following sanity checks verify that we have a high-resolution
	 * timer. */
	/* TESTLOOPCOUNT needs some loops to identify edge systems. 100 is
	 * definitely too little. */
#define TESTLOOPCOUNT 300
#define CLEARCACHE 100
	for (i = 0; (TESTLOOPCOUNT + CLEARCACHE) > i; i++) {
		__u64 time = 0;
		__u64 time2 = 0;
		__u64 folded = 0;
		__u64 delta = 0;

		jent_get_nstime(&time);
		jent_fold_time(NULL, time, &folded, 1<<MIN_FOLD_LOOP_BIT);
		jent_get_nstime(&time2);

		/* test whether timer works */
		if (!time || !time2)
			return ENOTIME;
		delta = time2 - time;
		/* test whether timer is fine grained enough to provide
		 * delta even when called shortly after each other -- this
		 * implies that we also have a high resolution timer */
		if (!delta)
			return ECOARSETIME;
		/* TIME_ENTROPY_BITS states the absolute minimum entropy we
		 * assume the time variances have. As we also check for
		 * delta of deltas, we ensure that there is a varying delta
		 * value, preventing identical time spans */
		if (TIME_ENTROPY_BITS > delta)
			return EMINVARIATION;

		/* up to here we did not modify any variable that will be
		 * evaluated later, but we already performed some work. Thus we
		 * already have had an impact on the caches, branch prediction,
		 * etc. with the goal to clear it to get the worst case
		 * measurements. */
		if (CLEARCACHE > i)
			continue;

		/* test whether we have an increasing timer */
		if (!(time2 > time))
			time_backwards++;

		if (!(delta % 100))
			count_mod++;

		/* ensure that we have a varying delta timer which is necessary
		 * for the calculation of entropy -- perform this check
		 * only after the first loop is executed as we need to prime
		 * the old_data value */
		if (i) {
			if (delta != old_delta)
				count_var++;
			if (delta > old_delta)
				delta_sum += (delta - old_delta);
			else
				delta_sum += (old_delta - delta);
		}
		old_delta = delta;
	}

	/* we allow up to three times the time running backwards.
	 * CLOCK_REALTIME is affected by adjtime and NTP operations. Thus,
	 * if such an operation just happens to interfere with our test, it
	 * should not fail. The value of 3 should cover the NTP case being
	 * performed during our test run. */
	if (3 < time_backwards)
		return ENOMONOTONIC;
	/* Error if the time variances are always identical */
	if (!delta_sum)
		return EVARVAR;

	/* Variations of deltas of time must on average be larger
	 * than TIME_ENTROPY_BITS to ensure the entropy estimation
	 * implied with TIME_ENTROPY_BITS is preserved */
	if (!(delta_sum / TESTLOOPCOUNT) > TIME_ENTROPY_BITS)
		return EMINVARVAR;

	/* Ensure that we have variations in the time stamp below 10 for at least
	 * 10% of all checks -- on some platforms, the counter increments in
	 * multiples of 100, but not always */
	if ((TESTLOOPCOUNT/10 * 9) < count_mod)
		return ECOARSETIME;

	return 0;
}
#if defined(__KERNEL__) && !defined(MODULE)
EXPORT_SYMBOL(jent_entropy_init);
#endif

/***************************************************************************
 * Statistical test logic not compiled for regular operation
 ***************************************************************************/

#ifdef CONFIG_CRYPTO_CPU_JITTERENTROPY_STAT
/* Statistical tests: invoke the entropy collector and sample time results
 * for it, the random data is never returned - every call to this function
 * generates one random number.
 * This function is only meant for statistical analysis purposes and not
 * for general use
 */
void jent_gen_entropy_stat(struct rand_data *entropy_collector,
	       		   struct entropy_stat *stat)
{
	/* caller is allowed to set the entropy collection loop to a fixed
	 * value -- we still call shuffle for the time measurements */
	jent_init_statistic(entropy_collector);
	jent_gen_entropy(entropy_collector);
	jent_calc_statistic(entropy_collector, stat, DATA_SIZE_BITS);
}

/* Statistical test: obtain the distribution of the folded time value from
 * jent_fold_time */
void jent_fold_time_stat(struct rand_data *ec, __u64 *fold, __u64 *loop_cnt)
{
	__u64 time = 0;
	__u64 time2 = 0;
	__u64 folded = 0;
	jent_get_nstime(&time);
	jent_memaccess(ec);
	/* implement the priming logic */
	jent_fold_time(ec, time, &folded, 0);
	jent_get_nstime(&time2);
	time2 = time2 - time;
	*loop_cnt = jent_fold_time(ec, time2, &folded, 0);
	*fold = folded;
}

/* Statistical test: return the time duration for the folding operation. If min
 * is set, perform the given number of foldings. Otherwise, allow the
 * loop count shuffling to define the number of foldings. */
__u64 jent_fold_var_stat(struct rand_data *ec, unsigned int min)
{
	__u64 time = 0;
	__u64 time2 = 0;
	__u64 folded = 0;
	jent_get_nstime(&time);
	jent_memaccess(ec);
	jent_fold_time(NULL, time, &folded, min);
	jent_get_nstime(&time2);
	return ((time2 - time));
}
#endif /* CONFIG_CRYPTO_CPU_JITTERENTROPY_STAT */
