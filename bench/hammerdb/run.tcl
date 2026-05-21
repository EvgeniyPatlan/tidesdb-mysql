#!/bin/tclsh
# HammerDB 5.0 TPROC-C timed run against the TidesDB engine.
# Form taken from the bundled mysql_tprocc_run.tcl: `vurun` itself
# blocks for the full rampup+duration, then we vudestroy/tcstop. NO
# runtimer / vustop / quit (5.0 CLI exits on its own). The standard
# TPC-C mix includes Order-Status -> exercises the reverse-ref
# (ORDER BY o_id DESC LIMIT 1) path the fix addresses.
# raiseerror=false so expected TidesDB OCC conflicts don't abort the
# run; the orchestrator scans run.log for handler-unsupported errors.
puts "SETTING CONFIGURATION"
dbset db mysql
dbset bm TPC-C
diset connection mysql_host @DBHOST@
diset connection mysql_port 3306
diset connection mysql_socket /tmp/mysql.sock
diset tpcc mysql_user @USER@
diset tpcc mysql_pass @PASS@
diset tpcc mysql_dbase tpcc
diset tpcc mysql_storage_engine @ENGINE@
diset tpcc mysql_count_ware @WARE@
diset tpcc mysql_driver timed
diset tpcc mysql_rampup @RAMP@
diset tpcc mysql_duration @DUR@
diset tpcc mysql_allwarehouse true
diset tpcc mysql_raiseerror false
loadscript
puts "TEST STARTED"
vuset vu @RUNVU@
vucreate
tcstart
tcstatus
vurun
vudestroy
tcstop
puts "TEST COMPLETE"
