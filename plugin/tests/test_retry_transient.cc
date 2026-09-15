/*
  Unit tests for tdb_retry_transient (plugin/tidesdb_retry.h).

  The wrapper exists to absorb TDB_ERR_LOCKED on read paths. Its behaviour is
  otherwise invisible: on a healthy engine it never fires, so a mistake in the
  attempt count or the exit condition would sit unnoticed until the one
  workload that needs it. These drive it directly with a callable that reports
  whatever the case under test needs, no engine involved.

  Timing is asserted only as a lower bound. The ladder sleeps a known minimum,
  and a loaded machine can only overshoot, so a bound is stable where an
  equality would be flaky.
*/
#include <gtest/gtest.h>

#include <chrono>

#include "tidesdb_retry.h"

namespace {

/* Succeeds immediately: exactly one call, no sleeping. */
TEST(RetryTransient, SuccessCallsOnce) {
    int calls = 0;
    const int rc = tdb_retry_transient([&] {
        calls++;
        return TDB_SUCCESS;
    });
    EXPECT_EQ(TDB_SUCCESS, rc);
    EXPECT_EQ(1, calls);
}

/* A non-LOCKED failure is returned on the spot -- the wrapper must not retry
   errors that are not contention, or a corrupt read becomes a stall. */
TEST(RetryTransient, NonTransientErrorIsNotRetried) {
    int calls = 0;
    const int rc = tdb_retry_transient([&] {
        calls++;
        return TDB_ERR_CORRUPTION;
    });
    EXPECT_EQ(TDB_ERR_CORRUPTION, rc);
    EXPECT_EQ(1, calls);
}

/* TDB_ERR_NOT_FOUND is a real answer, not a failure. Retrying it would turn
   every miss into the full ladder's dwell. */
TEST(RetryTransient, NotFoundIsNotRetried) {
    int calls = 0;
    const int rc = tdb_retry_transient([&] {
        calls++;
        return TDB_ERR_NOT_FOUND;
    });
    EXPECT_EQ(TDB_ERR_NOT_FOUND, rc);
    EXPECT_EQ(1, calls);
}

/* Contention that clears: the wrapper keeps asking and returns the success,
   so the caller never sees TDB_ERR_LOCKED at all. */
TEST(RetryTransient, LockedThenSuccess) {
    int calls = 0;
    const int rc = tdb_retry_transient([&] {
        calls++;
        return calls < 3 ? TDB_ERR_LOCKED : TDB_SUCCESS;
    });
    EXPECT_EQ(TDB_SUCCESS, rc);
    EXPECT_EQ(3, calls);
}

/* Contention that does not clear: bounded, and the last rc is handed back so
   tdb_rc_to_ha maps it exactly as it would have without the retry. */
TEST(RetryTransient, LockedIsBoundedAndSurfaces) {
    int calls = 0;
    const int rc = tdb_retry_transient([&] {
        calls++;
        return TDB_ERR_LOCKED;
    });
    EXPECT_EQ(TDB_ERR_LOCKED, rc);
    EXPECT_EQ(TDB_READ_RETRY_MAX_ATTEMPTS, calls);
}

/* The ladder actually sleeps. A wrapper that spun without backing off would
   pass every count assertion above while hammering a contended engine. */
TEST(RetryTransient, ExhaustionSleepsTheLadder) {
    int expected_us = 0;
    for (int i = 0; i < TDB_READ_RETRY_MAX_ATTEMPTS - 1; i++) {
        expected_us += TDB_READ_RETRY_BACKOFF_US[i];
    }

    const auto start = std::chrono::steady_clock::now();
    (void)tdb_retry_transient([] { return TDB_ERR_LOCKED; });
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    EXPECT_GE(elapsed, expected_us);
}

/* One sleep short of the full ladder when the last attempt succeeds -- the
   wrapper must not sleep after a result it is going to return. */
TEST(RetryTransient, NoSleepAfterFinalAttempt) {
    int all_but_last = 0;
    for (int i = 0; i < TDB_READ_RETRY_MAX_ATTEMPTS - 1; i++) {
        all_but_last += TDB_READ_RETRY_BACKOFF_US[i];
    }

    int calls = 0;
    const auto start = std::chrono::steady_clock::now();
    const int rc = tdb_retry_transient([&] {
        calls++;
        return calls < TDB_READ_RETRY_MAX_ATTEMPTS ? TDB_ERR_LOCKED : TDB_SUCCESS;
    });
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    EXPECT_EQ(TDB_SUCCESS, rc);
    EXPECT_EQ(TDB_READ_RETRY_MAX_ATTEMPTS, calls);
    EXPECT_GE(elapsed, all_but_last);
}

}  // namespace
