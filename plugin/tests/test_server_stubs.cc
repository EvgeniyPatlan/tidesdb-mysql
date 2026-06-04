/*
  Minimal server-symbol stubs for the atomic-DDL unit-test binary.

  sql_print_error/sql_print_warning/sql_print_information expand to
  log_message(...) (via log_errlog_formatted), which lives inside mysqld. The
  test harness does not link mysqld -- it only needs the binary to load -- so
  we provide a no-op log_message here. Tests assert on return values, not on
  log output.
*/

#include <cstdarg>

/* Matches the C++ signature in sql/log.h: `int log_message(int log_type, ...)`.
   The implementation is intentionally empty -- these tests assert on return
   values, not on log output. */
int log_message(int, ...) { return 0; }
