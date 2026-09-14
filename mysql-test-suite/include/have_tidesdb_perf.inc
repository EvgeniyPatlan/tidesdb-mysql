# Skip unless the plugin was built with -DTIDESDB_PERF=1.
#
# The tidesdb_perf_* sysvars are registered in both builds -- they are
# inert without the instrumentation -- so a test cannot detect the build
# from them. The tidesdb_perf_compiled status variable is the
# compile-time constant, and this guard is the perf equivalent of
# have_debug.inc.
--let $tidesdb_perf_compiled = query_get_value(SHOW GLOBAL STATUS LIKE 'tidesdb_perf_compiled', Value, 1)
if ($tidesdb_perf_compiled == 0)
{
  --skip TidesDB perf instrumentation not compiled in (rebuild with -DTIDESDB_PERF=1)
}
