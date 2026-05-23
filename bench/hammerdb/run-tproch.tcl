#!/bin/tclsh
# HammerDB 5.0 TPROC-H power test against the TidesDB engine.
# Single VU runs all 22 ad-hoc analytics queries sequentially; measures
# per-query latency + geometric mean (the "power test" half of TPC-H).
# Stresses TidesDB's LSM read path: sequential SSTable iteration,
# block-cache hit rate, bloom-filter effectiveness, range scans, joins.
# Form taken from the bundled mysql_tproch_run.tcl.
puts "SETTING CONFIGURATION"
dbset db mysql
dbset bm TPC-H
diset connection mysql_host @DBHOST@
diset connection mysql_port 3306
diset connection mysql_socket /tmp/mysql.sock
diset tpch mysql_scale_fact @SCALE@
diset tpch mysql_tpch_user @USER@
diset tpch mysql_tpch_pass @PASS@
diset tpch mysql_tpch_dbase tpch
diset tpch mysql_tpch_storage_engine tidesdb
loadscript
puts "TEST STARTED"
vuset vu @RUNVU@
vucreate
vurun
vudestroy
puts "TEST COMPLETE"
