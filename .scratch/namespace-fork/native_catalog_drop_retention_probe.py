#!/usr/bin/env python3
"""Unfinished F must retain schema tablets even after their Namespace drops."""
import argparse
import os
import time
import pymysql

from fork_parent_truncate_probe import BootstrapExperiment, connect, namespace_id, physical_id


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', required=True)
args = parser.parse_args()
experiment = BootstrapExperiment(args.binary, 'native_catalog_drop_retention', prototype=6)
gate = experiment.base / 'round.pause'
os.environ['SEEKDB_PHYSICAL_REPORT_GATE'] = str(gate)
read_control = experiment.base / 'historical.read'
os.environ['SEEKDB_CATALOG_RETENTION_READ'] = str(read_control)
gc_trigger = experiment.base / 'catalog.gc'
os.environ['SEEKDB_CATALOG_GC_TRIGGER'] = str(gc_trigger)
child = None


def wait_for(fn, message, timeout=120):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        assert experiment.proc.poll() is None, ('instance exited', experiment.base)
        result = fn()
        if result:
            return result
        time.sleep(.25)
    raise AssertionError(message)


def progress():
    return experiment.sql('SELECT frozen_scn,last_scn FROM oceanbase.DBA_OB_MAJOR_COMPACTION',
                          log=False)[0]


try:
    experiment.start()
    experiment.connection._read_timeout = 330
    experiment.sql('SET ob_query_timeout=300000000')
    experiment.sql("ALTER SYSTEM SET ob_compaction_schedule_interval='3s'")
    experiment.sql('FORK NAMESPACE retained_catalog FROM ns1')
    owner = namespace_id(experiment, 'retained_catalog')
    child = connect(experiment, 'root@retained_catalog')
    experiment.sql('CREATE DATABASE retained_catalog_db', child)
    experiment.sql('CREATE TABLE retained_catalog_db.t(id INT PRIMARY KEY,v INT)', child)
    experiment.sql('INSERT INTO retained_catalog_db.t VALUES(1,10)', child)
    table_id, version = experiment.sql("SELECT table_id,schema_version FROM oceanbase.__all_table "
                                       "WHERE table_name='t'", child)[0]
    ids = [physical_id(owner, logical) for logical in (12, 13)]

    def state():
        return experiment.sql('SELECT tablet_id,tablet_status,is_committed,is_empty_shell '
                              'FROM oceanbase.__all_virtual_tablet_info WHERE tablet_id IN ('
                              + ','.join(map(str, ids)) + ') ORDER BY tablet_id', log=False)

    assert len(state()) == 2 and all(row[1:] == (1, 1, 0) for row in state()), state()
    gate.touch()
    previous = progress()[0]
    experiment.sql('ALTER SYSTEM MAJOR FREEZE')
    frozen = wait_for(lambda: (lambda row: row[0] if row[0] > previous and row[1] < row[0]
                              else None)(progress()), 'freeze not published')
    child.close()
    child = None
    # COM_QUIT is asynchronous with server-side admission release.
    def drop_after_disconnect():
        try:
            experiment.sql('DROP NAMESPACE retained_catalog')
            return True
        except pymysql.MySQLError as error:
            if error.args[0] != 4179:
                raise
            return False
    wait_for(drop_after_disconnect, 'closed connection still prevents Namespace drop', 10)
    wait_for(lambda: len(state()) == 2 and all(row[1:3] == (3, 1) for row in state()),
             'metadata tablets did not receive committed DELETE')
    until = time.monotonic() + 16
    while time.monotonic() < until:
        assert progress()[1] < frozen, ('round gate failed', progress())
        current = state()
        assert len(current) == 2 and all(row[1:] == (3, 1, 0) for row in current), (
            'schema tablet reclaimed while historical F is still required', frozen, current)
        time.sleep(.25)
    experiment.record('catalog_tablets_retained', frozen=frozen, tablets=ids, seconds=16)
    gc_trigger.touch()
    wait_for(lambda: any('CATALOG_GC_PROBE ret=0' in log.read_text(errors='replace')
                        for log in (experiment.base / 'log').glob('seekdb.log*')),
             'directory page GC did not finish before historical read')
    read_control.write_text(f'{owner} {table_id} {frozen} {version}\n')
    # The fork hook only supplies an execution entry. Its native reader opens
    # the deleted owner's original F, independently of the new Namespace.
    experiment.sql('FORK NAMESPACE catalog_retention_read FROM ns1')
    proof = f'NATIVE_CATALOG_DROPPED_READ ns={owner} table={table_id} F={frozen} V={version} ret=0'
    assert any(proof in log.read_text(errors='replace')
               for log in (experiment.base / 'log').glob('seekdb.log*')), proof
    experiment.record('historical_catalog_read_after_drop', evidence=proof)
    gate.unlink()
    wait_for(lambda: progress()[1] >= frozen, 'round did not complete after release')
    wait_for(lambda: not state() or all(row[3] == 1 for row in state()),
             'schema tablets were retained after F completed')
    experiment.record('PASS', case='native_catalog_drop_retention', frozen=frozen,
                      protected_without_active_reader=True, released_after_completion=True)
finally:
    if child is not None:
        child.close()
    experiment.close()
