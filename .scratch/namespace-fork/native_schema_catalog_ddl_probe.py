#!/usr/bin/env python3
"""Exercise production native catalog writes with SQL readers and crash recovery."""
import argparse
import resource
from fork_parent_truncate_probe import BootstrapExperiment, connect


def run(binary):
    experiment = BootstrapExperiment(binary, 'native_catalog_ddl', prototype=6)
    child = None
    try:
        experiment.start()
        experiment.sql('CREATE DATABASE native_ddl')
        definition = ("id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
                      "u BIGINT UNSIGNED DEFAULT 18446744073709551615, n INT DEFAULT NULL, "
                      "e VARCHAR(100) DEFAULT '', c ENUM('a','b') DEFAULT 'b', body LONGTEXT")
        experiment.sql(f'CREATE TABLE native_ddl.t({definition})')
        experiment.sql("INSERT INTO native_ddl.t(id,body) VALUES(1,REPEAT('a',90000))")
        experiment.sql("CREATE TABLE native_ddl.p(id INT PRIMARY KEY,v VARCHAR(100),KEY ix(v),"
                       "CONSTRAINT ck CHECK(id >= 0)) PARTITION BY HASH(id) PARTITIONS 8")
        experiment.sql('USE native_ddl')
        experiment.sql('SET SESSION foreign_key_checks=0')
        experiment.sql('CREATE TABLE native_ddl.mock_child(id INT PRIMARY KEY,fk_id INT,KEY ix(fk_id),'
                       'CONSTRAINT fk_mock FOREIGN KEY(fk_id) REFERENCES mock_parent(id))')
        experiment.sql('SET SESSION foreign_key_checks=1')
        experiment.sql('FORK NAMESPACE native_ddl_child FROM ns1')
        child = connect(experiment, 'root@native_ddl_child')
        experiment.sql('CREATE TABLE native_ddl.mock_parent(id INT PRIMARY KEY)')
        experiment.sql('USE native_ddl', child)
        experiment.sql('SET SESSION foreign_key_checks=0', child)
        experiment.sql('ALTER TABLE native_ddl.mock_child ADD CONSTRAINT fk_mock_2 '
                       'FOREIGN KEY(id) REFERENCES mock_parent(id)', child)
        experiment.sql('SET SESSION foreign_key_checks=1', child)
        experiment.sql("ALTER TABLE native_ddl.t COMMENT='parent changed'")
        experiment.sql(f'CREATE TABLE native_ddl.local_table({definition})', child)
        experiment.sql("INSERT INTO native_ddl.local_table(id,body) VALUES(2,REPEAT('b',120000))", child)
        experiment.sql('ALTER TABLE native_ddl.t ADD extra INT DEFAULT 17', child)
        experiment.sql('TRUNCATE TABLE native_ddl.p', child)
        experiment.sql('DROP TABLE native_ddl.p', child)

        def verify():
            assert experiment.sql('SELECT id,v,u,n,e,c,LENGTH(body) FROM native_ddl.t') == (
                (1, '初始', 18446744073709551615, None, '', 'b', 90000),)
            assert experiment.sql('SELECT id,extra,LENGTH(body) FROM native_ddl.t', child) == ((1, 17, 90000),)
            assert experiment.sql('SELECT id,LENGTH(body) FROM native_ddl.local_table', child) == ((2, 120000),)
            assert experiment.sql('SELECT COUNT(*) FROM native_ddl.p') == ((0,),)
            for connection, expected_comment in ((None, 'parent changed'), (child, '')):
                db_id = experiment.sql("SELECT database_id FROM oceanbase.__all_database "
                                       "WHERE database_name='native_ddl'", connection)[0][0]
                mock = experiment.sql('SELECT mock_fk_parent_table_name FROM oceanbase.__all_mock_fk_parent_table '
                                      f'WHERE database_id={db_id}', connection)
                assert mock == (() if connection is None else (('mock_parent',),)), mock
                fks = experiment.sql('SELECT COUNT(*) FROM oceanbase.__all_foreign_key f '
                                     'JOIN oceanbase.__all_table t ON f.child_table_id=t.table_id '
                                     f"WHERE t.database_id={db_id} AND t.table_name='mock_child'", connection)
                assert fks == ((1 if connection is None else 2,),), fks
                rows = experiment.sql('SELECT table_id,table_name,comment FROM oceanbase.__all_table '
                                      f'FORCE INDEX (idx_db_tb_name) WHERE database_id={db_id} '
                                      "AND table_name='t'", connection)
                assert len(rows) == 1 and rows[0][1:] == ('t', expected_comment), rows
                tid = rows[0][0]
                columns = experiment.sql('SELECT column_name,cur_default_value_v2 FROM oceanbase.__all_column '
                                         f'FORCE INDEX (idx_tb_column_name) WHERE table_id={tid} '
                                         "AND column_name='v'", connection)
                assert columns == (('v', '初始'.encode('utf-8')),), columns
                latest = experiment.sql('SELECT table_name,comment FROM oceanbase.__all_table_history '
                                        f'WHERE table_id={tid} ORDER BY schema_version DESC LIMIT 1', connection)
                assert latest == (('t', expected_comment),), latest
            parent_local = experiment.sql("SELECT COUNT(*) FROM oceanbase.__all_table WHERE table_name='local_table'")
            assert parent_local == ((0,),), parent_local

        verify()
        experiment.record('PASS', case='native_catalog_ddl_sql_indexes_and_fork')
        child.close()
        child = None
        experiment.connection.close()
        experiment.connection = None
        experiment.proc.kill()
        experiment.proc.wait(timeout=15)
        experiment.start()
        child = connect(experiment, 'root@native_ddl_child')
        verify()
        experiment.record('PASS', case='native_catalog_ddl_crash_recovery')
        experiment.sql('CREATE TABLE native_ddl.mock_parent(id INT PRIMARY KEY)', child)
        experiment.sql('INSERT INTO native_ddl.mock_parent VALUES(1)', child)
        experiment.sql('INSERT INTO native_ddl.mock_child VALUES(1,1)', child)
        assert experiment.sql('SELECT id,fk_id FROM native_ddl.mock_child', child) == ((1, 1),)
        assert experiment.sql("SELECT COUNT(*) FROM oceanbase.__all_mock_fk_parent_table "
                              "WHERE mock_fk_parent_table_name='mock_parent'", child) == ((0,),)
        assert experiment.sql('SELECT COUNT(*) FROM native_ddl.mock_child') == ((0,),)
        experiment.record('PASS', case='native_catalog_mock_parent_resolution_after_restart')
    finally:
        if child is not None:
            child.close()
        experiment.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    run(args.binary)
