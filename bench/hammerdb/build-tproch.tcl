#!/bin/tclsh
# HammerDB 5.0 TPROC-H (TPC-H) schema build on the TidesDB engine.
# Form taken from the bundled mysql_tproch_buildschema.tcl: in
# `hammerdbcli auto` mode `buildschema` is synchronous and the CLI
# exits on its own -- NO waittocomplete / quit (those raise a Tcl
# error in 5.0). Placeholders @X@ filled by run-hammerdb-tproch.sh.
#
# mysql_scale_fact:
#   1   -> ~1GB raw / ~6M lineitem rows (TPC-H spec for SF=1)
#   10  -> ~10GB / ~60M lineitem rows
puts "SETTING CONFIGURATION"
dbset db mysql
dbset bm TPC-H
diset connection mysql_host @DBHOST@
diset connection mysql_port 3306
diset connection mysql_socket /tmp/mysql.sock
diset tpch mysql_scale_fact @SCALE@
diset tpch mysql_num_tpch_threads @BUILDVU@
diset tpch mysql_tpch_user @USER@
diset tpch mysql_tpch_pass @PASS@
diset tpch mysql_tpch_dbase tpch
diset tpch mysql_tpch_storage_engine tidesdb
puts "SCHEMA BUILD STARTED"
buildschema
puts "SCHEMA BUILD COMPLETED"
