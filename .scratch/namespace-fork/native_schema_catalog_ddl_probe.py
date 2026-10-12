#!/usr/bin/env python3
"""Exercise production native catalog writes with SQL readers and crash recovery."""
import argparse
import resource
from fork_parent_truncate_probe import BootstrapExperiment, connect


def exercise_constraints(query):
    query('CREATE DATABASE native_constraints')
    query('CREATE TABLE native_constraints.state_t(id INT PRIMARY KEY,v INT,'
          'CONSTRAINT ck CHECK(v>=0) NOT ENFORCED,CONSTRAINT ck_id CHECK(id>0))')
    query('INSERT INTO native_constraints.state_t VALUES(1,-1)')
    query('UPDATE native_constraints.state_t SET v=1')
    query('ALTER TABLE native_constraints.state_t ALTER CHECK ck ENFORCED')
    tid = query("SELECT table_id FROM oceanbase.__all_table WHERE table_name='state_t'")[0][0]
    assert query(f"SELECT enable_flag FROM oceanbase.__all_constraint WHERE table_id={tid} "
                 "AND constraint_name='ck'") == ((1,),)
    query('ALTER TABLE native_constraints.state_t ALTER CHECK ck NOT ENFORCED')
    query('ALTER TABLE native_constraints.state_t CHANGE COLUMN v value INT')
    query('ALTER TABLE native_constraints.state_t ADD CONSTRAINT ck_pair CHECK(id<=value)')
    pair_id = query(f"SELECT constraint_id FROM oceanbase.__all_constraint WHERE table_id={tid} "
                    "AND constraint_name='ck_pair'")[0][0]
    assert query(f'SELECT COUNT(*) FROM oceanbase.__all_constraint_column WHERE table_id={tid} '
                 f'AND constraint_id={pair_id}') == ((2,),)
    query('ALTER TABLE native_constraints.state_t DROP CONSTRAINT ck_pair')
    dropped = []
    for recycle in (True, False):
        name = 'recycled' if recycle else 'dropped'
        query(f'CREATE TABLE native_constraints.{name}(id INT PRIMARY KEY,v INT,'
              'CONSTRAINT ck_keep CHECK(v>=0) NOT ENFORCED,CONSTRAINT ck_positive CHECK(id>0))')
        dropped_tid = query(f"SELECT table_id FROM oceanbase.__all_table WHERE table_name='{name}'")[0][0]
        before = query('SELECT constraint_id,constraint_name,rely_flag,enable_flag,validate_flag,name_generated_type '
                       f'FROM oceanbase.__all_constraint WHERE table_id={dropped_tid} ORDER BY constraint_id')
        column_keys = query('SELECT constraint_id,column_id FROM oceanbase.__all_constraint_column '
                            f'WHERE table_id={dropped_tid} ORDER BY constraint_id,column_id')
        query('SET SESSION recyclebin=' + ('ON' if recycle else 'OFF'))
        query(f'DROP TABLE native_constraints.{name}')
        if recycle:
            current = query('SELECT constraint_id,constraint_name,rely_flag,enable_flag,validate_flag,name_generated_type '
                            f'FROM oceanbase.__all_constraint WHERE table_id={dropped_tid} ORDER BY constraint_id')
            assert len(current) == len(before) == 2, (current, before)
            for old, row in zip(before, current):
                assert old[0] == row[0] and old[1] != row[1] and old[2:] == row[2:], (old, row)
                history = query('SELECT constraint_id,constraint_name,rely_flag,enable_flag,validate_flag,name_generated_type '
                                f'FROM oceanbase.__all_constraint_history WHERE table_id={dropped_tid} '
                                f'AND constraint_id={row[0]} ORDER BY schema_version DESC LIMIT 1')
                assert history == (row,), (history, row)
            query('PURGE RECYCLEBIN')
        dropped.append((dropped_tid, before, column_keys))
    return tid, pair_id, dropped


def verify_constraints(query, evidence):
    tid, pair_id, dropped = evidence
    assert query('SELECT id,value FROM native_constraints.state_t') == ((1, 1),)
    current = query('SELECT constraint_id,constraint_name,check_expr,rely_flag,enable_flag,validate_flag '
                    f'FROM oceanbase.__all_constraint WHERE table_id={tid} ORDER BY constraint_id')
    assert len(current) == 2, current
    for row in current:
        if row[1] == 'ck':
            assert row[4] == 0 and 'value' in row[2], row
        history = query('SELECT constraint_id,constraint_name,check_expr,rely_flag,enable_flag,validate_flag '
                        f'FROM oceanbase.__all_constraint_history WHERE table_id={tid} '
                        f'AND constraint_id={row[0]} ORDER BY schema_version DESC LIMIT 1')
        assert history == (row,), (history, row)
    assert query(f'SELECT is_deleted FROM oceanbase.__all_constraint_history WHERE table_id={tid} '
                 f'AND constraint_id={pair_id} ORDER BY schema_version DESC LIMIT 1') == ((1,),)
    for dropped_tid, constraints, column_keys in dropped:
        for inner in ('__all_constraint', '__all_constraint_column'):
            assert query(f'SELECT COUNT(*) FROM oceanbase.{inner} WHERE table_id={dropped_tid}') == ((0,),)
        for row in constraints:
            assert query('SELECT is_deleted FROM oceanbase.__all_constraint_history '
                         f'WHERE table_id={dropped_tid} AND constraint_id={row[0]} '
                         'ORDER BY schema_version DESC LIMIT 1') == ((1,),)
        for cid, col in column_keys:
            assert query('SELECT is_deleted FROM oceanbase.__all_constraint_column_history '
                         f'WHERE table_id={dropped_tid} AND constraint_id={cid} AND column_id={col} '
                         'ORDER BY schema_version DESC LIMIT 1') == ((1,),)


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
        experiment.sql('CREATE TABLE native_ddl.drop_columns(id INT PRIMARY KEY,v INT,body LONGTEXT,'
                       'CONSTRAINT drop_ck CHECK(v >= 0))')
        experiment.sql("INSERT INTO native_ddl.drop_columns VALUES(1,7,REPEAT('d',90000))")
        drop_tid = experiment.sql("SELECT table_id FROM oceanbase.__all_table WHERE table_name='drop_columns'")[0][0]
        dropped_cid = experiment.sql(f"SELECT column_id FROM oceanbase.__all_column WHERE table_id={drop_tid} "
                                    "AND column_name='v'")[0][0]
        constraint_id = experiment.sql(f'SELECT constraint_id FROM oceanbase.__all_constraint '
                                      f'WHERE table_id={drop_tid}')[0][0]
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
        experiment.sql("ALTER TABLE native_ddl.t CHANGE COLUMN v title VARCHAR(120) DEFAULT '新默认'", child)
        experiment.sql("INSERT INTO native_ddl.t(id,body) VALUES(3,'new child')", child)
        experiment.sql('RENAME TABLE native_ddl.local_table TO native_ddl.local_renamed', child)
        experiment.sql('ALTER TABLE native_ddl.drop_columns DROP CONSTRAINT drop_ck', child)
        experiment.sql('ALTER TABLE native_ddl.drop_columns DROP COLUMN v', child)
        assert experiment.sql('SELECT COUNT(*) FROM oceanbase.__all_column FORCE INDEX(idx_tb_column_name) '
                              f"WHERE table_id={drop_tid} AND column_name='v'", child) == ((0,),)
        # Reuse the old name: a stale name-index entry must not survive deletion.
        experiment.sql('ALTER TABLE native_ddl.drop_columns ADD v INT DEFAULT 23', child)
        assert experiment.sql('SELECT column_id FROM oceanbase.__all_column FORCE INDEX(idx_tb_column_name) '
                              f"WHERE table_id={drop_tid} AND column_name='v'", child)[0][0] != dropped_cid
        experiment.sql('ALTER TABLE native_ddl.drop_columns DROP COLUMN body', child)
        # Dropping LOB can rebuild the table; subsequent current-row checks use
        # its new identity while history checks retain the original identity.
        current_drop_tid = experiment.sql("SELECT table_id FROM oceanbase.__all_table "
                                          "WHERE table_name='drop_columns'", child)[0][0]
        experiment.sql('CREATE TABLE native_ddl.to_drop(id INT PRIMARY KEY,a INT,b INT,body LONGTEXT)', child)
        purge_tid = experiment.sql("SELECT table_id FROM oceanbase.__all_table WHERE table_name='to_drop'", child)[0][0]
        experiment.sql('SET SESSION recyclebin=ON', child)
        experiment.sql('DROP TABLE native_ddl.to_drop', child)
        assert experiment.sql('SELECT COUNT(*) FROM oceanbase.__all_table '
                              f'WHERE table_id={purge_tid}', child) == ((1,),)
        # PURGE completes catalog deletion after the recycle-bin rename.
        experiment.sql('PURGE RECYCLEBIN', child)
        experiment.sql('TRUNCATE TABLE native_ddl.p', child)
        experiment.sql('DROP TABLE native_ddl.p', child)
        constraint_evidence = exercise_constraints(lambda statement: experiment.sql(statement, child))

        def verify():
            verify_constraints(lambda statement: experiment.sql(statement, child), constraint_evidence)
            assert experiment.sql('SELECT id,v,u,n,e,c,LENGTH(body) FROM native_ddl.t') == (
                (1, '初始', 18446744073709551615, None, '', 'b', 90000),)
            assert experiment.sql('SELECT id,title,extra,LENGTH(body) FROM native_ddl.t ORDER BY id', child) == (
                (1, '初始', 17, 90000), (3, '新默认', 17, 9))
            assert experiment.sql('SELECT id,LENGTH(body) FROM native_ddl.local_renamed', child) == ((2, 120000),)
            assert experiment.sql('SELECT COUNT(*) FROM native_ddl.p') == ((0,),)
            assert experiment.sql('SELECT id,v,LENGTH(body) FROM native_ddl.drop_columns') == ((1, 7, 90000),)
            assert experiment.sql('SELECT id,v FROM native_ddl.drop_columns', child) == ((1, 23),)
            assert experiment.sql('SELECT COUNT(*) FROM oceanbase.__all_constraint '
                                  f'WHERE table_id={drop_tid} AND constraint_id={constraint_id}', child) == ((0,),)
            assert experiment.sql('SELECT is_deleted FROM oceanbase.__all_constraint_history '
                                  f'WHERE table_id={drop_tid} AND constraint_id={constraint_id} '
                                  'ORDER BY schema_version DESC LIMIT 1', child) == ((1,),)
            assert experiment.sql('SELECT COUNT(*) FROM oceanbase.__all_constraint_column '
                                  f'WHERE table_id={drop_tid} AND constraint_id={constraint_id}', child) == ((0,),)
            assert experiment.sql('SELECT is_deleted FROM oceanbase.__all_column_history '
                                  f'WHERE table_id={drop_tid} AND column_id={dropped_cid} '
                                  'ORDER BY schema_version DESC LIMIT 1', child) == ((1,),)
            indexed = experiment.sql('SELECT column_id,column_name FROM oceanbase.__all_column FORCE INDEX(idx_tb_column_name) '
                                     f"WHERE table_id={current_drop_tid} AND column_name='v'", child)
            primary = experiment.sql('SELECT column_id,column_name FROM oceanbase.__all_column IGNORE INDEX(idx_tb_column_name) '
                                     f"WHERE table_id={current_drop_tid} AND column_name='v'", child)
            assert len(indexed) == 1 and indexed == primary, (indexed, primary)
            for inner, predicate in (('__all_table', f'table_id={purge_tid}'),
                                     ('__all_column', f'table_id={purge_tid}')):
                assert experiment.sql(f'SELECT COUNT(*) FROM oceanbase.{inner} WHERE {predicate}', child) == ((0,),)
            assert experiment.sql('SELECT is_deleted FROM oceanbase.__all_table_history '
                                  f'WHERE table_id={purge_tid} ORDER BY schema_version DESC LIMIT 1', child) == ((1,),)
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
                assert experiment.sql('SELECT gmt_modified>gmt_create FROM oceanbase.__all_table '
                                      f'WHERE table_id={tid}', connection) == ((1,),)
                column_name, default = ('v', '初始') if connection is None else ('title', '新默认')
                columns = experiment.sql('SELECT column_name,cur_default_value_v2 FROM oceanbase.__all_column '
                                         f'FORCE INDEX (idx_tb_column_name) WHERE table_id={tid} '
                                         f"AND column_name='{column_name}'", connection)
                assert columns == ((column_name, default.encode('utf-8')),), columns
                if connection is not None:
                    assert experiment.sql('SELECT COUNT(*) FROM oceanbase.__all_column FORCE INDEX (idx_tb_column_name) '
                                          f"WHERE table_id={tid} AND column_name='v'", connection) == ((0,),)
                    assert experiment.sql('SELECT table_name FROM oceanbase.__all_table FORCE INDEX (idx_db_tb_name) '
                                          f"WHERE database_id={db_id} AND table_name IN ('local_table','local_renamed')",
                                          connection) == (('local_renamed',),)
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
