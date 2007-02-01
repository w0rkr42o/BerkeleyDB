# See the file LICENSE for redistribution information.
#
# Copyright (c) 2006 Oracle.  All rights reserved.
#
# $Id: rep068.tcl,v 1.4 2006/12/07 19:37:44 carol Exp $
#
# TEST	rep068
# TEST	Verify replication of dbreg operations does not hang clients.
# TEST  In a simple replication group, create a database with very
# TEST  little data.  With DB_TXN_NOSYNC the database can be created
# TEST  at the client even though the log is not flushed.  If we crash
# TEST  and restart, the application of the log starts over again, even
# TEST  though the database is still there.  The application can open
# TEST  the database before replication tries to re-apply the create.
# TEST  This causes a hang as replication waits to be able to get a
# TEST  handle lock.
# TEST
# TEST	Run for btree only because access method shouldn't matter.
# TEST
proc rep068 { method { tnum "068" } args } {

	source ./include.tcl

	if { $is_windows9x_test == 1 } {
		puts "Skipping replication test on Win9x platform."
		return
	}

	# Run for btree methods only.
	if { $checking_valid_methods } {
		set test_methods {}
		foreach meth $valid_methods {
			if { [is_btree $method] } {
				lappend test_methods $meth
			}
		}
		return $test_methods
	}
	if { [is_btree $method] == 0 } {
		puts "Rep$tnum: skipping for non-btree method."
		return
	}

	set args [convert_args $method $args]

	# Run the body of the test with/without recovery and txn nosync.
	foreach s {"nosync" ""} {
		foreach r $test_recopts {
			puts "Rep$tnum ($method $r $s):\
			    Test of dbreg lock conflicts at client"
			rep068_sub $method $tnum $r $s $args
		}
	}
}

# Temporary note: at the moment, this test fails when both "-recover" and
# "nosync" are in use, because of problems described in SR #15071.
#
proc rep068_sub { method tnum recargs nosync largs } {
	global testdir
	global rep_verbose
 
	set verbargs ""
	if { $rep_verbose == 1 } {
		set verbargs " -verbose {rep on} "
	}
 
	set KEY "any old key"
	set DATA "arbitrary data"
	set DBNAME "test.db"

	set nosync_args [subst {-txn $nosync}]

	env_cleanup $testdir

	replsetup $testdir/MSGQUEUEDIR

	set masterdir $testdir/MASTERDIR
	set clientdir $testdir/CLIENTDIR

	file mkdir $masterdir
	file mkdir $clientdir

	# Open a master.
	repladd 1
	set ma_envcmd "berkdb_env_noerr -create $verbargs -errpfx MASTER \
	    -home $masterdir -rep_transport \[list 1 replsend\]"
	set masterenv [eval $ma_envcmd $recargs $nosync_args -rep_master]

	# Open a client
	repladd 2
	set cl_envcmd "berkdb_env_noerr -create $verbargs -errpfx CLIENT \
	    -home $clientdir -rep_transport \[list 2 replsend\]"
	set clientenv [eval $cl_envcmd $recargs $nosync_args -rep_client]

	# Bring the client online by processing the startup messages.
	set envlist "{$masterenv 1} {$clientenv 2}"
	process_msgs $envlist
					
	# Open/create a database, maybe put just one record in it
	# abandon the client env, and restart it.  Before trying to sync,
	# open the database at the client.

	set db [berkdb_open_noerr -auto_commit \
	     -btree -create -env $masterenv $DBNAME]
	set ret [$db put $KEY $DATA]
	error_check_good initial_insert $ret 0
	process_msgs $envlist

	# Simulate a crash and restart of the client, by simply abandoning
	# the old environment handle and opening a new one.
	# 
	puts "\tRep$tnum.a: Open a fresh handle onto the client env."
	set clientenv [eval $cl_envcmd $recargs $nosync_args -rep_client]
	set envlist "{$masterenv 1} {$clientenv 2}"

	# We expect the db creation operation to have been flushed to the log,
	# so that at this point recovery will have removed the database (since
	# we expect the transaction did not commit).  But the bug we are testing
	# for is that the applying of replicated transactions hangs if the
	# database turns out to be present.  Thus, for a stringent test, we want
	# to at least try to open the database, and "dare ourselves" not to hang
	# if it turns out to be present.
	#
	if {[catch {set client_db [berkdb_open_noerr \
	    -auto_commit -unknown -env $clientenv $DBNAME]} result] == 0} {
		puts "\t\tRep$tnum.a(ii): warning: db open at restarted client\
		    succeeded unexpectedly"
	} else {
		set client_db "NULL"
	}
	
	puts "\tRep$tnum.b: Attempting sync-up with db handle open."
	process_msgs $envlist
	puts "\tRep$tnum.c: Sync-up completed."

	if {$client_db == "NULL"} {
		set client_db [berkdb_open_noerr \
		    -auto_commit -unknown -env $clientenv $DBNAME]
	}
	set result [$client_db get $KEY]
	error_check_good one_pair [llength $result] 1
	set val [lindex $result 0 1]
	error_check_good "value still matches" $val $DATA
	puts "\tRep$tnum.d: Confirmed correct data."

 	$client_db close
	$clientenv close

	$db close
	$masterenv close
	replclose $testdir/MSGQUEUEDIR
}
