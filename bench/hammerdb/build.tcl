#!/bin/tclsh
# HammerDB 5.0 TPROC-C schema build on the TidesDB engine.
# Form taken verbatim from the bundled
# scripts/tcl/mysql/tprocc/mysql_tprocc_buildschema.tcl: in
# `hammerdbcli auto` mode `buildschema` is synchronous and the CLI
# exits on its own -- NO waittocomplete / quit (those raise a Tcl
# error in 5.0). Placeholders @X@ filled by run-hammerdb.sh.
puts "SETTING CONFIGURATION"
dbset db mysql
dbset bm TPC-C
diset connection mysql_host @DBHOST@
diset connection mysql_port 3306
diset connection mysql_socket /tmp/mysql.sock
diset tpcc mysql_count_ware @WARE@
diset tpcc mysql_num_vu @BUILDVU@
diset tpcc mysql_user @USER@
diset tpcc mysql_pass @PASS@
diset tpcc mysql_dbase tpcc
diset tpcc mysql_storage_engine tidesdb
diset tpcc mysql_partition false
puts "SCHEMA BUILD STARTED"
buildschema
puts "SCHEMA BUILD COMPLETED"
