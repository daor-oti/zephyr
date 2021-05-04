/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ztest.h>
#include <psa/crypto.h>
#include <kernel.h>

#ifndef EXC_RETURN_S
/* bit [6] stack used to push registers: 0=Non-secure 1=Secure */
#define EXC_RETURN_S               (0x00000040UL)
#endif

#define HASH_LEN 32

static struct k_work_delayable interrupting_work;
static volatile bool work_done;
static char dummy_string[0x1000];
static char dummy_digest_correct[HASH_LEN];
static const uint32_t delay_ms = 4;
static volatile const struct k_thread *main_thread;

static void do_hash(char *hash)
{
	size_t len;

	/* Calculate correct hash. */
	psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, dummy_string,
			sizeof(dummy_string), hash, HASH_LEN, &len);

	zassert_equal(PSA_SUCCESS, status, NULL);
	zassert_equal(HASH_LEN, len, NULL);
}

static void work_func(struct k_work *work)
{
#ifdef CONFIG_ARM_NONSECURE_PREEMPTIBLE_SECURE_CALLS
	/* Check that the main thread was executing in secure mode. */
	zassert_true(main_thread->arch.mode_exc_return & EXC_RETURN_S,
		"EXC_RETURN not secure: 0x%x\n", main_thread->arch.mode_exc_return);

#else
	/* Check that the main thread was executing in nonsecure mode. */
	zassert_false(main_thread->arch.mode_exc_return & EXC_RETURN_S,
		"EXC_RETURN not nonsecure: 0x%x\n", main_thread->arch.mode_exc_return);
#endif

	work_done = true;

	/* If FPU is available, clobber FPU context in this thread to check that
	 * the correct context is restored in the other thread.
	 */
#ifdef CONFIG_CPU_HAS_FPU
	uint32_t clobber_val[16] = {
		0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef,
		0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef,
		0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef,
		0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef,
	};

	__asm__ volatile(
		"vldmia %0, {s0-s15}\n"
		"vldmia %0, {s16-s31}\n"
		:: "r" (clobber_val) :
	);
#endif /* CONFIG_CPU_HAS_FPU */

	/* Call a secure service here as well, to test the added complexity of
	 * calling secure services from two threads.
	 */
	psa_status_t status = psa_hash_compare(PSA_ALG_SHA_256, dummy_string,
			sizeof(dummy_string), dummy_digest_correct, HASH_LEN);

	zassert_equal(PSA_SUCCESS, status, NULL);
}

void test_thread_swap(void)
{
	int err;
	char dummy_digest[HASH_LEN];
	k_timeout_t delay = K_MSEC(delay_ms);
	k_tid_t curr;
	psa_status_t status;

	curr = k_current_get();
	main_thread = (struct k_thread *)curr;

	status = psa_crypto_init();
	zassert_equal(PSA_SUCCESS, status, NULL);

	/* Calculate correct hash. */
	do_hash(dummy_digest_correct);

	/* Set up interrupting_work to fire while call_tfm() is executing.
	 * This tests that it is safe to switch threads while a secure service
	 * is running.
	 */
	k_work_init_delayable(&interrupting_work, work_func);

	err = k_work_reschedule(&interrupting_work, delay);
	zassert_equal(1, err, "k_work_reschedule failed: %d\n", err);

	/* If FPU is available, check that FPU context is preserved when calling
	 * a secure function.
	 */
#ifdef CONFIG_CPU_HAS_FPU
	uint32_t test_val[16] = {
		0x1a2b3c4d, 0x1a2b3c4d, 0x1a2b3c4d, 0x1a2b3c4d,
		0x1a2b3c4d, 0x1a2b3c4d, 0x1a2b3c4d, 0x1a2b3c4d,
		0x1a2b3c4d, 0x1a2b3c4d, 0x1a2b3c4d, 0x1a2b3c4d,
		0x1a2b3c4d, 0x1a2b3c4d, 0x1a2b3c4d, 0x1a2b3c4d,
	};
	uint32_t test_val_res0[16];
	uint32_t test_val_res1[16];

	__asm__ volatile(
		"vldmia %0, {s0-s15}\n"
		"vldmia %0, {s16-s31}\n"
		:: "r" (test_val) :
	);
#endif /* CONFIG_CPU_HAS_FPU */

	work_done = false;
	do_hash(dummy_digest);
	zassert_true(work_done, "Interrupting work never happened\n");

#ifdef CONFIG_CPU_HAS_FPU
	__asm__ volatile(
		"vstmia %0, {s0-s15}\n"
		"vstmia %1, {s16-s31}\n"
		:: "r" (test_val_res0), "r" (test_val_res1) :
	);

	zassert_mem_equal(dummy_digest, dummy_digest_correct, HASH_LEN, NULL);
	zassert_mem_equal(test_val, test_val_res0, sizeof(test_val), NULL);
	zassert_mem_equal(test_val, test_val_res1, sizeof(test_val), NULL);
#endif /* CONFIG_CPU_HAS_FPU */
}

void test_main(void)
{
	ztest_test_suite(test_thread_swap,
			ztest_unit_test(test_thread_swap)
			);
	ztest_run_test_suite(test_thread_swap);
}