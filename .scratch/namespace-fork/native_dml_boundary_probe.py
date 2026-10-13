#!/usr/bin/env python3
"""Native DML plans/iterators: inherited rows, duplicates, savepoints and LOB."""
import argparse
import hashlib
import resource
import pymysql
from fork_parent_truncate_probe import BootstrapExperiment, connect


def run_minimal(binary):
    exp = BootstrapExperiment(binary, 'native_upsert_minimal', prototype=6)
    try:
        exp.start()
        exp.sql('CREATE DATABASE repro')
        exp.sql('CREATE TABLE repro.t(id INT PRIMARY KEY,v INT)')
        exp.sql('INSERT INTO repro.t VALUES(1,10)')
        exp.sql('INSERT INTO repro.t VALUES(1,20) ON DUPLICATE KEY UPDATE v=VALUES(v)')
        assert exp.sql('SELECT * FROM repro.t') == ((1,20),)
        exp.record('PASS', case='native_upsert_minimal')
    finally:
        exp.close()


def run(binary):
    exp = BootstrapExperiment(binary, 'native_dml_boundary', prototype=6)
    child = None
    try:
        exp.start()
        exp.sql('CREATE DATABASE native_dml')
        exp.sql('CREATE TABLE native_dml.t(id INT PRIMARY KEY,u INT UNIQUE,v INT,body LONGTEXT)')
        exp.sql('INSERT INTO native_dml.t VALUES' + ','.join(
            f"({i},{i},{i * 10},REPEAT('x',8192))" for i in range(1, 97)))
        parent = exp.sql('SELECT id,u,v,LENGTH(body),MD5(body) FROM native_dml.t ORDER BY id', log=False)
        exp.sql('FORK NAMESPACE native_dml_child FROM ns1')
        child = connect(exp, 'root@native_dml_child')

        def sql(statement):
            return exp.sql(statement, child)

        sql('BEGIN')
        # Larger than the old 32-row transport batch; UPDATE consumes old/new pairs.
        sql('UPDATE native_dml.t SET id=id+1000,u=u+1000,v=v+1')
        sql('SAVEPOINT keep_rows')
        sql("UPDATE native_dml.t SET v=v+100,body=REPEAT('z',24000)")
        sql('ROLLBACK TO SAVEPOINT keep_rows')
        assert sql('SELECT COUNT(*),SUM(v),MIN(LENGTH(body)),MAX(LENGTH(body)) FROM native_dml.t') == ((96, 46656, 8192, 8192),)
        try:
            sql("INSERT INTO native_dml.t VALUES(2000,1001,0,'rejected')")
        except pymysql.MySQLError as error:
            assert error.args[0] == 1062, error
        else:
            raise AssertionError('unique-index conflict accepted')
        sql("INSERT INTO native_dml.t VALUES(1001,1001,777,'new') "
            'ON DUPLICATE KEY UPDATE v=v+10,body=VALUES(body)')
        sql("REPLACE INTO native_dml.t VALUES(3000,1002,888,'replaced')")
        sql("INSERT IGNORE INTO native_dml.t VALUES(1003,1003,999,'ignored'),(4000,4000,40,'inserted')")
        sql('INSERT INTO native_dml.t SELECT id+10000,u+10000,v,body FROM native_dml.t WHERE id=1003')
        assert sql('SELECT v FROM native_dml.t WHERE id=1001 FOR UPDATE') == ((21,),)
        sql('COMMIT')

        rows = {i+1000: (i+1000, i*10+1, 'x'*8192) for i in range(1, 97)}
        rows[1001] = (1001, 21, 'new')
        del rows[1002]
        rows[3000] = (1002, 888, 'replaced')
        rows[4000] = (4000, 40, 'inserted')
        rows[11003] = (11003, 31, 'x'*8192)
        expected = tuple((i, u, v, len(body), hashlib.md5(body.encode()).hexdigest())
                         for i, (u, v, body) in sorted(rows.items()))

        def verify():
            actual = sql('SELECT id,u,v,LENGTH(body),MD5(body) FROM native_dml.t ORDER BY id')
            assert actual == expected, (actual, expected)
            indexed = sql('SELECT id,u FROM native_dml.t FORCE INDEX(u) WHERE u>0 ORDER BY id')
            assert indexed == tuple((row[0], row[1]) for row in expected), indexed
            assert exp.sql('SELECT id,u,v,LENGTH(body),MD5(body) FROM native_dml.t ORDER BY id', log=False) == parent

        verify()
        sql('BEGIN')
        sql('DELETE FROM native_dml.t WHERE id>=1050')
        sql('ROLLBACK')
        verify()
        child.close(); child = None
        exp.connection.close(); exp.connection = None
        exp.proc.kill(); exp.proc.wait(timeout=15)
        exp.start()
        child = connect(exp, 'root@native_dml_child')
        verify()
        exp.record('PASS', case='native_dml_boundary', restart=True, row_count=len(expected))
    finally:
        if child is not None and child.open:
            child.close()
        exp.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--minimal', action='store_true')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    (run_minimal if args.minimal else run)(args.binary)
