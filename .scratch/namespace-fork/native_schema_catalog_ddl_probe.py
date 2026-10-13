#!/usr/bin/env python3
"""Exercise production native catalog writes with SQL readers and crash recovery."""
import argparse
import time
import resource
import pymysql
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


def exercise_foreign_keys(query):
    query('CREATE DATABASE native_fk')
    query('USE native_fk')
    query('SET SESSION recyclebin=OFF')
    query('SET SESSION foreign_key_checks=0')
    query('CREATE TABLE child(id INT PRIMARY KEY,a INT,b INT,KEY ix(a,b),'
          'CONSTRAINT fk_ab FOREIGN KEY(a,b) REFERENCES missing(a,b))')
    fk_id, first_mock = query("SELECT foreign_key_id,parent_table_id FROM oceanbase.__all_foreign_key "
                              "WHERE foreign_key_name='fk_ab'")[0]
    old_columns = query('SELECT child_column_id,parent_column_id,position '
                         f'FROM oceanbase.__all_foreign_key_column WHERE foreign_key_id={fk_id} ORDER BY position')
    assert len(old_columns) == 2, old_columns
    # A second child adds and then removes a distinct column of the same mock.
    query('CREATE TABLE extra_child(id INT PRIMARY KEY,c INT,KEY ix(c),'
          'CONSTRAINT fk_c FOREIGN KEY(c) REFERENCES missing(c))')
    assert query('SELECT COUNT(*) FROM oceanbase.__all_mock_fk_parent_table_column '
                 f'WHERE mock_fk_parent_table_id={first_mock}') == ((3,),)
    query('DROP TABLE extra_child')
    assert query('SELECT COUNT(*) FROM oceanbase.__all_mock_fk_parent_table_column '
                 f'WHERE mock_fk_parent_table_id={first_mock}') == ((2,),)
    # The added leading column forces real column IDs to differ from the mock.
    query('CREATE TABLE missing(pad INT,a INT,b INT,PRIMARY KEY(a,b))')
    resolved = query('SELECT child_column_id,parent_column_id,position '
                     f'FROM oceanbase.__all_foreign_key_column WHERE foreign_key_id={fk_id} ORDER BY position')
    assert resolved != old_columns, (resolved, old_columns)
    query('INSERT INTO missing VALUES(99,11,12)')
    query('SET SESSION foreign_key_checks=1')
    query('INSERT INTO child VALUES(1,11,12)')
    # Parent DROP first removes the FK row, then recreates it pointing at a mock.
    query('SET SESSION foreign_key_checks=0')
    query('DROP TABLE missing')
    second_mock, is_mock = query('SELECT parent_table_id,is_parent_table_mock FROM oceanbase.__all_foreign_key '
                                 f'WHERE foreign_key_id={fk_id}')[0]
    assert is_mock == 1 and second_mock != first_mock, (second_mock, is_mock)
    query('CREATE TABLE missing(a INT,b INT,PRIMARY KEY(a,b))')
    query('INSERT INTO missing VALUES(11,12)')
    query('SET SESSION foreign_key_checks=1')
    query('INSERT INTO child VALUES(2,11,12)')
    # Existing DDL validation leaves an enabled NOVALIDATE FK after 5785.
    # Check its persistent state, then explicitly drop it; owner rollback is
    # covered independently by the cold native transaction probe.
    query('CREATE TABLE bad_child(id INT PRIMARY KEY,a INT,b INT,KEY ix(a,b))')
    query('INSERT INTO bad_child VALUES(1,999,999)')
    try:
        query('ALTER TABLE bad_child ADD CONSTRAINT fk_rejected FOREIGN KEY(a,b) REFERENCES missing(a,b)')
    except pymysql.MySQLError as error:
        assert error.args[0] == 5785, error
    else:
        raise AssertionError('invalid existing row accepted by foreign key validation')
    rejected = query("SELECT foreign_key_id,enable_flag,validate_flag FROM oceanbase.__all_foreign_key "
                     "WHERE foreign_key_name='fk_rejected'")
    assert len(rejected) == 1 and rejected[0][1:] == (1, 0), rejected
    assert query('SELECT foreign_key_id,enable_flag,validate_flag FROM oceanbase.__all_foreign_key_history '
                 f'WHERE foreign_key_id={rejected[0][0]} ORDER BY schema_version DESC LIMIT 1') == rejected
    query('ALTER TABLE bad_child DROP FOREIGN KEY fk_rejected')
    # Both explicit DROP FOREIGN KEY and whole-child DROP publish deletions.
    dropped = [rejected[0][0]]
    for name, drop_table in (('drop_fk_child', False), ('drop_table_child', True)):
        query(f'CREATE TABLE {name}(id INT PRIMARY KEY,a INT,b INT,KEY ix(a,b),'
              f'CONSTRAINT {name}_fk FOREIGN KEY(a,b) REFERENCES missing(a,b))')
        fid = query('SELECT foreign_key_id FROM oceanbase.__all_foreign_key '
                     f"WHERE foreign_key_name='{name}_fk'")[0][0]
        dropped.append(fid)
        query(f'DROP TABLE {name}' if drop_table else f'ALTER TABLE {name} DROP FOREIGN KEY {name}_fk')
    return fk_id, (first_mock, second_mock), dropped


def verify_foreign_keys(query, evidence):
    fk_id, mocks, dropped = evidence
    assert query('SELECT id,a,b FROM native_fk.child ORDER BY id') == ((1, 11, 12), (2, 11, 12))
    fields = ('foreign_key_id,foreign_key_name,child_table_id,parent_table_id,update_action,delete_action,'
              'enable_flag,validate_flag,rely_flag,ref_cst_type,ref_cst_id,is_parent_table_mock,name_generated_type')
    current = query(f'SELECT {fields} FROM oceanbase.__all_foreign_key WHERE foreign_key_id={fk_id}')
    history = query(f'SELECT {fields} FROM oceanbase.__all_foreign_key_history WHERE foreign_key_id={fk_id} '
                     'ORDER BY schema_version DESC LIMIT 1')
    assert current == history and len(current) == 1 and current[0][-2] == 0, (current, history)
    parent_id = current[0][3]
    columns = query('SELECT child_column_id,parent_column_id,position FROM oceanbase.__all_foreign_key_column '
                     f'WHERE foreign_key_id={fk_id} ORDER BY position')
    parent_columns = dict(query('SELECT column_name,column_id FROM oceanbase.__all_column '
                                f"WHERE table_id={parent_id} AND column_name IN ('a','b')"))
    assert [r[1] for r in columns] == [parent_columns['a'], parent_columns['b']], (columns, parent_columns)
    for fid in (fk_id, *dropped):
        latest = query('SELECT h.child_column_id,h.parent_column_id,h.is_deleted FROM oceanbase.__all_foreign_key_column_history h '
                        f'WHERE h.foreign_key_id={fid} AND h.schema_version=(SELECT MAX(v.schema_version) '
                        'FROM oceanbase.__all_foreign_key_column_history v WHERE v.foreign_key_id=h.foreign_key_id '
                        'AND v.child_column_id=h.child_column_id AND v.parent_column_id=h.parent_column_id)')
        active = query('SELECT child_column_id,parent_column_id FROM oceanbase.__all_foreign_key_column '
                        f'WHERE foreign_key_id={fid}')
        assert {r[:2] for r in latest if r[2] == 0} == set(active), (latest, active)
        if fid in dropped:
            assert not active and latest and all(r[2] == 1 for r in latest), (active, latest)
            assert query(f'SELECT COUNT(*) FROM oceanbase.__all_foreign_key WHERE foreign_key_id={fid}') == ((0,),)
            assert query('SELECT is_deleted FROM oceanbase.__all_foreign_key_history '
                         f'WHERE foreign_key_id={fid} ORDER BY schema_version DESC LIMIT 1') == ((1,),)
    for mid in mocks:
        for inner in ('__all_mock_fk_parent_table', '__all_mock_fk_parent_table_column'):
            assert query(f'SELECT COUNT(*) FROM oceanbase.{inner} WHERE mock_fk_parent_table_id={mid}') == ((0,),)
        assert query('SELECT is_deleted FROM oceanbase.__all_mock_fk_parent_table_history '
                     f'WHERE mock_fk_parent_table_id={mid} ORDER BY schema_version DESC LIMIT 1') == ((1,),)
        latest = query('SELECT h.is_deleted FROM oceanbase.__all_mock_fk_parent_table_column_history h '
                        f'WHERE h.mock_fk_parent_table_id={mid} AND h.schema_version=(SELECT MAX(v.schema_version) '
                        'FROM oceanbase.__all_mock_fk_parent_table_column_history v '
                        'WHERE v.mock_fk_parent_table_id=h.mock_fk_parent_table_id AND v.parent_column_id=h.parent_column_id)')
        assert latest and all(row == (1,) for row in latest), latest


def prepare_partition_tables(query):
    query('CREATE DATABASE native_part')
    query('CREATE TABLE native_part.r(id INT PRIMARY KEY,v INT) PARTITION BY RANGE(id) '
          '(PARTITION p0 VALUES LESS THAN(10),PARTITION p1 VALUES LESS THAN(20))')
    query('INSERT INTO native_part.r VALUES(1,1),(11,2)')
    query("CREATE TABLE native_part.l(id INT,k VARCHAR(32),PRIMARY KEY(id,k)) PARTITION BY LIST COLUMNS(k) "
          "(PARTITION p0 VALUES IN('a''b','中文'),PARTITION p1 VALUES IN('plain'))")
    query("INSERT INTO native_part.l VALUES(1,'a''b'),(2,'中文'),(3,'plain')")
    query('CREATE TABLE native_part.t(id INT,s INT,v INT,PRIMARY KEY(id,s)) '
          'PARTITION BY RANGE(id) SUBPARTITION BY LIST(s) SUBPARTITION TEMPLATE '
          '(SUBPARTITION s0 VALUES IN(1,3),SUBPARTITION s1 VALUES IN(2,4)) '
          '(PARTITION p0 VALUES LESS THAN(10),PARTITION p1 VALUES LESS THAN(20))')
    query('INSERT INTO native_part.t VALUES(1,1,10),(11,2,20)')
    query('CREATE TABLE native_part.n(id INT,s INT,v INT,PRIMARY KEY(id,s)) '
          'PARTITION BY RANGE(id) SUBPARTITION BY RANGE(s) '
          '(PARTITION p0 VALUES LESS THAN(10) '
          '(SUBPARTITION s00 VALUES LESS THAN(10),SUBPARTITION s01 VALUES LESS THAN(20)),'
          'PARTITION p1 VALUES LESS THAN(20) '
          '(SUBPARTITION s10 VALUES LESS THAN(10),SUBPARTITION s11 VALUES LESS THAN(20)))')
    query('INSERT INTO native_part.n VALUES(1,5,10),(1,15,20),(11,5,30)')
    # Cross the former SQL batch boundary while retaining bounded native state.
    query('CREATE TABLE native_part.h(id INT PRIMARY KEY,v INT) PARTITION BY HASH(id) PARTITIONS 130')
    query('INSERT INTO native_part.h VALUES(1,1),(129,2),(260,3)')


def exercise_partitions(query, prepared=False):
    if not prepared:
        prepare_partition_tables(query)
    ids = dict(query('SELECT table_name,table_id FROM oceanbase.__all_table '
                      "WHERE database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='native_part') "
                      "AND table_name IN ('r','l','t','n','h')"))
    assert len(ids) == 5, ids
    query('ALTER TABLE native_part.r ADD PARTITION (PARTITION p2 VALUES LESS THAN(30))')
    query('INSERT INTO native_part.r VALUES(24,3)')
    query('ALTER TABLE native_part.r TRUNCATE PARTITION p0')
    query('ALTER TABLE native_part.r DROP PARTITION p1')
    query("ALTER TABLE native_part.l ADD PARTITION (PARTITION p2 VALUES IN('extra'))")
    query("INSERT INTO native_part.l VALUES(4,'extra')")
    query('ALTER TABLE native_part.l TRUNCATE PARTITION p0')
    query('ALTER TABLE native_part.l DROP PARTITION p1')
    query('ALTER TABLE native_part.t ADD PARTITION (PARTITION p2 VALUES LESS THAN(30))')
    query('INSERT INTO native_part.t VALUES(21,3,30)')
    query('ALTER TABLE native_part.t TRUNCATE PARTITION p0')
    subname = query('SELECT sub_part_name FROM oceanbase.__all_sub_part '
                    f"WHERE table_id={ids['t']} AND sub_part_idx=1 AND part_id="
                    f"(SELECT part_id FROM oceanbase.__all_part WHERE table_id={ids['t']} AND part_name='p1')")[0][0]
    query(f'ALTER TABLE native_part.t TRUNCATE SUBPARTITION `{subname}`')
    query('ALTER TABLE native_part.t DROP PARTITION p0')
    query('ALTER TABLE native_part.n MODIFY PARTITION p0 ADD SUBPARTITION '
          '(SUBPARTITION s02 VALUES LESS THAN(30))')
    query('INSERT INTO native_part.n VALUES(1,25,40)')
    query('ALTER TABLE native_part.n TRUNCATE SUBPARTITION s00')
    query('ALTER TABLE native_part.n DROP SUBPARTITION s01')
    query('ALTER TABLE native_part.n ADD PARTITION (PARTITION p2 VALUES LESS THAN(30) '
          '(SUBPARTITION s20 VALUES LESS THAN(10),SUBPARTITION s21 VALUES LESS THAN(20)))')
    query('INSERT INTO native_part.n VALUES(21,15,50)')
    query('ALTER TABLE native_part.n DROP PARTITION p1')
    query('SET SESSION recyclebin=OFF')
    query('DROP TABLE native_part.h')
    return ids


def verify_partitions(query, ids):
    assert query('SELECT * FROM native_part.r ORDER BY id') == ((24, 3),)
    assert query('SELECT * FROM native_part.l ORDER BY id') == ((4, 'extra'),)
    assert query('SELECT * FROM native_part.t ORDER BY id,s') == ((21, 3, 30),)
    assert query('SELECT * FROM native_part.n ORDER BY id,s') == ((1, 25, 40), (21, 15, 50))
    for inner, keys in (('__all_part_info', ('table_id',)), ('__all_part', ('table_id','part_id')),
                        ('__all_sub_part', ('table_id','part_id','sub_part_id')),
                        ('__all_def_sub_part', ('table_id','sub_part_id'))):
        columns = [row[0] for row in query(f'SHOW COLUMNS FROM oceanbase.{inner}')
                   if row[0] not in ('gmt_create', 'gmt_modified')]
        fields = ','.join(columns)
        matches = ' AND '.join(f'v.{key}=h.{key}' for key in keys)
        for name, tid in ids.items():
            current = query(f'SELECT {fields} FROM oceanbase.{inner} WHERE table_id={tid} ORDER BY '+','.join(keys))
            history = query('SELECT '+','.join('h.'+column for column in columns)+f' FROM oceanbase.{inner}_history h '
                            f'WHERE h.table_id={tid} AND h.is_deleted=0 AND h.schema_version='
                            f'(SELECT MAX(v.schema_version) FROM oceanbase.{inner}_history v WHERE {matches}) '
                            'ORDER BY '+','.join('h.'+key for key in keys))
            assert current == history, (name, inner, current, history)
            index = {'__all_part': 'idx_part_name', '__all_sub_part': 'idx_sub_part_name',
                     '__all_def_sub_part': 'idx_def_sub_part_name'}.get(inner)
            if index:
                indexed = query(f'SELECT {fields} FROM oceanbase.{inner} FORCE INDEX ({index}) '
                                f'WHERE table_id={tid} ORDER BY '+','.join(keys))
                assert indexed == current, (name, inner, indexed, current)
            if name == 'h':
                assert not current, (inner, current)
    assert query(f"SELECT COUNT(*) FROM oceanbase.__all_part_history WHERE table_id={ids['h']} AND is_deleted=1") == ((130,),)


def read_operation_rows(query, lower, upper, index=False):
    hint = 'FORCE INDEX(idx_ddl_type)' if index else 'IGNORE INDEX(idx_ddl_type)'
    return query('SELECT schema_version,operation_type,user_id,database_id,column_id,table_id,'
                 'database_name,table_name,LENGTH(ddl_stmt_str),CRC32(ddl_stmt_str) '
                 f'FROM oceanbase.__all_ddl_operation {hint} '
                 f'WHERE schema_version>{lower} AND schema_version<={upper} ORDER BY schema_version')


def schema_watermarks(query):
    rows = query("SELECT column_name,CAST(column_value AS SIGNED) FROM oceanbase.__all_core_table "
                 "WHERE table_name='__all_global_stat' AND column_name IN "
                 "('core_schema_version','sys_schema_version','normal_schema_version','baseline_schema_version')")
    assert len(rows) == 4, rows
    versions = dict(rows)
    assert 0 < versions['core_schema_version'] <= versions['sys_schema_version'] <= versions['normal_schema_version'], versions
    assert versions['baseline_schema_version'] >= -1, versions
    return versions


def exercise_operations(query):
    lower = query('SELECT MAX(schema_version) FROM oceanbase.__all_ddl_operation')[0][0]
    query('CREATE DATABASE native_operations')
    query('CREATE TABLE native_operations.t(id INT PRIMARY KEY,v INT,body LONGTEXT,KEY ix(v))')
    query("INSERT INTO native_operations.t VALUES(1,7,REPEAT('a',90000))")
    query('ALTER TABLE native_operations.t ADD extra INT DEFAULT 19')
    query('CREATE VIEW native_operations.v AS SELECT id,v,extra FROM native_operations.t')
    query('CREATE USER native_operation_user')
    query('DROP USER native_operation_user')
    upper, last_type = query('SELECT schema_version,operation_type FROM oceanbase.__all_ddl_operation '
                             'ORDER BY schema_version DESC LIMIT 1')[0]
    # OB_DDL_END_SIGN: each completed DDL has a final record in its owner.
    assert last_type == 1503, (upper, last_type)
    before = read_operation_rows(query, lower, upper)
    versions = schema_watermarks(query)
    assert versions['normal_schema_version'] == upper, (versions, upper)
    try:
        query('ALTER TABLE native_operations.t ADD v INT')
        raise AssertionError('duplicate column unexpectedly accepted')
    except pymysql.MySQLError as error:
        assert error.args[0] == 1060, error.args
    assert query('SELECT MAX(schema_version) FROM oceanbase.__all_ddl_operation') == ((upper,),)
    assert schema_watermarks(query) == versions
    assert sum(row[1] == 1503 for row in before) >= 6, before
    return lower, upper, before


def verify_operations(query, evidence):
    lower, upper, expected = evidence
    assert query('SELECT * FROM native_operations.v') == ((1,7,19),)
    current = read_operation_rows(query, lower, upper)
    indexed = read_operation_rows(query, lower, upper, True)
    assert current == indexed == expected, (current, indexed, expected)
    assert schema_watermarks(query)['normal_schema_version'] >= upper


def prepare_triggers(query):
    query('CREATE DATABASE native_triggers')
    query('USE native_triggers')
    query('CREATE TABLE t(id INT PRIMARY KEY,v INT)')
    query('CREATE TABLE audit_t(id INT PRIMARY KEY,v INT)')
    query('CREATE TRIGGER ti BEFORE INSERT ON t FOR EACH ROW SET NEW.v=NEW.v+1')
    query('CREATE TRIGGER ta AFTER INSERT ON t FOR EACH ROW INSERT INTO audit_t VALUES(NEW.id,NEW.v)')
    query('INSERT INTO t VALUES(1,3)')
    assert query('SELECT * FROM t') == ((1,4),)


def exercise_triggers(query, prepared=False):
    if not prepared:
        prepare_triggers(query)
    query('USE native_triggers')
    old = query("SELECT trigger_id FROM oceanbase.__all_trigger WHERE trigger_name='ti'")[0][0]
    query('DROP TRIGGER ti')
    # Exercise LONGTEXT storage and full definition loading, including UTF-8.
    body = 'BEGIN /*中文' + 'x'*90000 + "*/ SET NEW.v=NEW.v+10; END"
    query('CREATE TRIGGER ti BEFORE INSERT ON t FOR EACH ROW ' + body)
    query('CREATE TRIGGER ti_second BEFORE INSERT ON t FOR EACH ROW PRECEDES ti SET NEW.v=NEW.v*2')
    second = query("SELECT trigger_id FROM oceanbase.__all_trigger WHERE trigger_name='ti_second'")[0][0]
    query('RENAME TABLE t TO renamed')
    query('INSERT INTO renamed VALUES(2,3)')
    query('DROP TRIGGER ti_second')
    query('INSERT INTO renamed VALUES(3,3)')
    query('CREATE TABLE recycled(id INT PRIMARY KEY,v INT)')
    query('CREATE TRIGGER recycled_ti BEFORE INSERT ON recycled FOR EACH ROW SET NEW.v=NEW.v+1')
    recycled = query("SELECT trigger_id FROM oceanbase.__all_trigger WHERE trigger_name='recycled_ti'")[0][0]
    query('SET SESSION recyclebin=ON')
    query('DROP TABLE recycled')
    query('FLASHBACK TABLE recycled TO BEFORE DROP RENAME TO restored')
    query('INSERT INTO restored VALUES(1,8)')
    assert query('SELECT * FROM restored') == ((1,9),)
    query('DROP TABLE restored')
    query('PURGE RECYCLEBIN')
    tid = query("SELECT table_id FROM oceanbase.__all_table WHERE table_name='renamed' AND database_id="
                "(SELECT database_id FROM oceanbase.__all_database WHERE database_name='native_triggers')")[0][0]
    return tid, (old, second, recycled)


def verify_triggers(query, evidence):
    tid, dropped = evidence
    expected = ((1,4),(2,16),(3,13))
    assert query('SELECT * FROM native_triggers.renamed ORDER BY id') == expected
    assert query('SELECT * FROM native_triggers.audit_t ORDER BY id') == expected
    columns = [row[0] for row in query('SHOW COLUMNS FROM oceanbase.__all_trigger')
               if row[0] not in ('gmt_create','gmt_modified')]
    fields = ','.join(columns)
    current = query(f'SELECT {fields} FROM oceanbase.__all_trigger WHERE base_object_id={tid} ORDER BY trigger_id')
    history = query('SELECT '+','.join('h.'+col for col in columns)+' FROM oceanbase.__all_trigger_history h '
                    f'WHERE h.base_object_id={tid} AND h.is_deleted=0 AND h.schema_version='
                    '(SELECT MAX(v.schema_version) FROM oceanbase.__all_trigger_history v '
                    'WHERE v.trigger_id=h.trigger_id) ORDER BY h.trigger_id')
    assert len(current) == 2 and current == history, (len(current), len(history))
    indexed = query(f'SELECT {fields} FROM oceanbase.__all_trigger FORCE INDEX(idx_trigger_base_obj_id) '
                    f'WHERE base_object_id={tid} ORDER BY trigger_id')
    assert indexed == current
    for name in ('ti','ta'):
        ids = query('SELECT trigger_id FROM oceanbase.__all_trigger FORCE INDEX(idx_trigger_name) '
                    f"WHERE trigger_name='{name}' AND base_object_id={tid}")
        assert len(ids) == 1, (name, ids)
    assert query('SELECT LENGTH(trigger_body_v2)>90000,trigger_body IS NULL '
                 f"FROM oceanbase.__all_trigger WHERE base_object_id={tid} AND trigger_name='ti'") == ((1,1),)
    for trigger_id in dropped:
        assert query(f'SELECT COUNT(*) FROM oceanbase.__all_trigger WHERE trigger_id={trigger_id}') == ((0,),)
        assert query('SELECT is_deleted FROM oceanbase.__all_trigger_history '
                     f'WHERE trigger_id={trigger_id} ORDER BY schema_version DESC LIMIT 1') == ((1,),)


def exercise_recovered_triggers(query):
    query('INSERT INTO native_triggers.renamed VALUES(4,5)')
    assert query('SELECT v FROM native_triggers.renamed WHERE id=4') == ((15,),)
    assert query('SELECT v FROM native_triggers.audit_t WHERE id=4') == ((15,),)


def dependency_rows(query, ids):
    fields = ('dep_obj_type,dep_obj_id,dep_order,schema_version,dep_timestamp,'
              'ref_obj_type,ref_obj_id,ref_timestamp,dep_obj_owner_id,property,'
              'HEX(dep_attrs),HEX(dep_reason),ref_obj_name')
    condition = 'dep_obj_id IN (' + ','.join(map(str, ids)) + ')'
    order = ' ORDER BY dep_obj_type,dep_obj_id,dep_order'
    rows = query(f'SELECT {fields} FROM oceanbase.__all_dependency WHERE {condition}{order}')
    refs = sorted({row[6] for row in rows})
    if refs:
        indexed = query(f'SELECT {fields} FROM oceanbase.__all_dependency FORCE INDEX(idx_dependency_ref_obj) '
                        f'WHERE {condition} AND ref_obj_id IN (' + ','.join(map(str, refs)) + ')' + order)
        assert indexed == rows, (rows, indexed)
    return rows


def dependency_id(query, name):
    return query("SELECT table_id FROM oceanbase.__all_table WHERE database_id="
                 "(SELECT database_id FROM oceanbase.__all_database WHERE database_name='native_dependency') "
                 f"AND table_name='{name}'")[0][0]


def prepare_dependencies(query):
    query('CREATE DATABASE native_dependency')
    query('CREATE TABLE native_dependency.t(id INT PRIMARY KEY,v INT)')
    query('INSERT INTO native_dependency.t VALUES(1,7)')
    tid = dependency_id(query, 't')
    version = query(f'SELECT schema_version FROM oceanbase.__all_table WHERE table_id={tid}')[0][0]
    query('CREATE VIEW native_dependency.v1 AS SELECT * FROM native_dependency.t')
    v1 = dependency_id(query, 'v1')
    rows = dependency_rows(query, [v1])
    assert len(rows) == 1 and rows[0][6:8] == (tid, version), (rows, tid, version)
    query('CREATE VIEW native_dependency.v2 AS SELECT * FROM native_dependency.v1')
    v2 = dependency_id(query, 'v2')
    assert query('SELECT * FROM native_dependency.v2') == ((1, 7),)
    return [v1, v2], dependency_rows(query, [v1, v2])


def exercise_dependencies(query, prepared=False):
    if not prepared:
        prepare_dependencies(query)
    old_v1 = dependency_id(query, 'v1')
    v2 = dependency_id(query, 'v2')
    query('CREATE OR REPLACE VIEW native_dependency.v1 AS SELECT id,v+10 AS v FROM native_dependency.t')
    new_v1 = dependency_id(query, 'v1')
    assert new_v1 != old_v1
    deadline = time.monotonic() + 30
    while True:
        assert query('SELECT * FROM native_dependency.v2') == ((1, 17),)
        rows = dependency_rows(query, [v2])
        if rows and any(row[6] == new_v1 for row in rows):
            break
        assert time.monotonic() < deadline, ('dependent view did not refresh references', rows)
        time.sleep(.05)
    assert not dependency_rows(query, [old_v1])
    # The transitive dependency walk invalidates v1 and v2. Recreating the
    # referenced table must recompile both and replace their old reference keys.
    query('DROP TABLE native_dependency.t')
    query('CREATE TABLE native_dependency.t(id INT PRIMARY KEY,v BIGINT)')
    query('INSERT INTO native_dependency.t VALUES(1,9)')
    new_t = dependency_id(query, 't')
    # Capture the invalidation boundary before reading/recompiling the views.
    # Filling a view's column metadata can subsequently advance table.schema_version
    # without invalidating its dependents again.
    versions = dict(query('SELECT table_id,schema_version FROM oceanbase.__all_table '
                          f'WHERE table_id IN ({new_v1},{new_t})'))
    deadline = time.monotonic() + 30
    while True:
        assert query('SELECT * FROM native_dependency.v2') == ((1, 19),)
        rows = dependency_rows(query, [new_v1, v2])
        # Both background updates must finish before preserving exact evidence.
        # v1 -> new_t can finish while v2 still references v1's old version.
        expected = {(new_v1, new_t), (v2, new_v1)}
        if {(row[1], row[6]) for row in rows} == expected and all(row[7] >= versions[row[6]] for row in rows):
            break
        assert time.monotonic() < deadline, ('dependency chain not refreshed', rows, expected)
        time.sleep(.05)
    query('CREATE VIEW native_dependency.disposable AS SELECT * FROM native_dependency.v2')
    disposable = dependency_id(query, 'disposable')
    assert dependency_rows(query, [disposable])
    query('DROP VIEW native_dependency.disposable')
    assert not dependency_rows(query, [disposable])
    return [new_v1, v2], dependency_rows(query, [new_v1, v2]), [old_v1, disposable]


def verify_dependencies(query, evidence):
    ids, rows, deleted = evidence
    assert query('SELECT * FROM native_dependency.v2') == ((1, 19),)
    actual = dependency_rows(query, ids)
    assert actual == rows, (actual, rows)
    assert not dependency_rows(query, deleted)


def prepare_views(query):
    query('CREATE DATABASE native_views')
    query('CREATE TABLE native_views.base(id INT PRIMARY KEY,v INT,KEY ix(v))')
    query('INSERT INTO native_views.base VALUES(1,7)')
    query('CREATE VIEW native_views.v AS SELECT id,v FROM native_views.base')
    assert query('SELECT * FROM native_views.v') == ((1, 7),)


def exercise_views(query, prepared=False):
    if not prepared:
        prepare_views(query)
    ids = query('SELECT table_id,schema_version FROM oceanbase.__all_table WHERE database_id='
                "(SELECT database_id FROM oceanbase.__all_database WHERE database_name='native_views')")
    # Creation records include the main table, its physical index and the view.
    original_versions = []
    for tid, _ in ids:
        original = query(f'SELECT ori_schema_version FROM oceanbase.__all_ori_schema_version WHERE table_id={tid}')
        # A batched creation shares its final publication version; individual
        # table histories may start at earlier versions within the same DDL.
        assert len(original) == 1 and original[0][0] > 0, (tid, original)
        original_versions.append((tid, original[0][0]))
    view_id = query("SELECT table_id FROM oceanbase.__all_table WHERE table_name='v' AND database_id="
                    "(SELECT database_id FROM oceanbase.__all_database WHERE database_name='native_views')")[0][0]
    original_type = query(f"SELECT data_type FROM oceanbase.__all_column WHERE table_id={view_id} AND column_name='v'")
    query('ALTER TABLE native_views.base MODIFY v BIGINT')
    # Recompilation can be queued after a client resolves the changed view.
    import time
    deadline = time.monotonic() + 30
    while True:
        assert query('SELECT * FROM native_views.v') == ((1, 7),)
        current_type = query(f"SELECT data_type FROM oceanbase.__all_column WHERE table_id={view_id} AND column_name='v'")
        if current_type and current_type != original_type:
            break
        assert time.monotonic() < deadline, ('view columns were not recompiled', original_type, current_type)
        time.sleep(.2)
    return view_id, original_versions, current_type


def verify_views(query, evidence):
    view_id, original_versions, current_type = evidence
    assert query('SELECT * FROM native_views.v') == ((1, 7),)
    assert query(f"SELECT data_type FROM oceanbase.__all_column WHERE table_id={view_id} AND column_name='v'") == current_type
    columns = [row[0] for row in query('SHOW COLUMNS FROM oceanbase.__all_column')
               if row[0] not in ('gmt_create', 'gmt_modified')]
    fields = ','.join(columns)
    current = query(f'SELECT {fields} FROM oceanbase.__all_column WHERE table_id={view_id} ORDER BY column_id')
    history = query('SELECT '+','.join('h.'+column for column in columns)+' FROM oceanbase.__all_column_history h '
                    f'WHERE h.table_id={view_id} AND h.is_deleted=0 AND h.schema_version='
                    '(SELECT MAX(v.schema_version) FROM oceanbase.__all_column_history v '
                    'WHERE v.table_id=h.table_id AND v.column_id=h.column_id) ORDER BY h.column_id')
    assert len(current) == 2 and current == history, (current, history)
    indexed = query(f'SELECT {fields} FROM oceanbase.__all_column FORCE INDEX (idx_tb_column_name) '
                    f'WHERE table_id={view_id} ORDER BY column_id')
    assert indexed == current, (indexed, current)
    for tid, version in original_versions:
        assert query(f'SELECT ori_schema_version FROM oceanbase.__all_ori_schema_version WHERE table_id={tid}') == ((version,),)


def run(binary):
    experiment = BootstrapExperiment(binary, 'native_catalog_ddl', prototype=6)
    child = None
    peer = None
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
        prepare_partition_tables(experiment.sql)
        parent_dependency = prepare_dependencies(experiment.sql)
        prepare_views(experiment.sql)
        prepare_triggers(experiment.sql)
        experiment.sql('FORK NAMESPACE native_ddl_child FROM ns1')
        experiment.sql('FORK NAMESPACE native_view_peer FROM ns1')
        child = connect(experiment, 'root@native_ddl_child')
        peer = connect(experiment, 'root@native_view_peer')
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
        partition_evidence = exercise_partitions(lambda statement: experiment.sql(statement, child), prepared=True)
        fk_evidence = exercise_foreign_keys(lambda statement: experiment.sql(statement, child))
        dependency_evidence = exercise_dependencies(lambda statement: experiment.sql(statement, child), prepared=True)
        operation_evidence = exercise_operations(lambda statement: experiment.sql(statement, child))
        trigger_evidence = exercise_triggers(lambda statement: experiment.sql(statement, child), prepared=True)
        constraint_evidence = exercise_constraints(lambda statement: experiment.sql(statement, child))
        # Same view ID in two Namespaces must identify two independent tasks.
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=2) as workers:
            jobs = [workers.submit(exercise_views,
                                   lambda statement, c=c: experiment.sql(statement, c), True)
                    for c in (child, peer)]
            view_evidence, peer_view_evidence = [job.result() for job in jobs]

        def verify():
            verify_dependencies(lambda statement: experiment.sql(statement, child), dependency_evidence)
            assert experiment.sql('SELECT * FROM native_dependency.v2') == ((1, 7),)
            assert dependency_rows(experiment.sql, parent_dependency[0]) == parent_dependency[1]
            verify_operations(lambda statement: experiment.sql(statement, child), operation_evidence)
            verify_triggers(lambda statement: experiment.sql(statement, child), trigger_evidence)
            assert experiment.sql('SELECT * FROM native_triggers.t') == ((1,4),)
            assert experiment.sql('SELECT * FROM native_triggers.audit_t') == ((1,4),)
            verify_views(lambda statement: experiment.sql(statement, child), view_evidence)
            verify_views(lambda statement: experiment.sql(statement, peer), peer_view_evidence)
            assert experiment.sql('SELECT * FROM native_views.v') == ((1, 7),)
            assert experiment.sql(f'SELECT data_type FROM oceanbase.__all_column WHERE table_id={view_evidence[0]} '
                                  "AND column_name='v'") != view_evidence[2]
            verify_partitions(lambda statement: experiment.sql(statement, child), partition_evidence)
            assert experiment.sql('SELECT * FROM native_part.r ORDER BY id') == ((1, 1), (11, 2))
            assert experiment.sql('SELECT COUNT(*) FROM native_part.h') == ((3,),)
            verify_foreign_keys(lambda statement: experiment.sql(statement, child), fk_evidence)
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
        peer.close()
        peer = None
        experiment.connection.close()
        experiment.connection = None
        experiment.proc.kill()
        experiment.proc.wait(timeout=15)
        experiment.start()
        child = connect(experiment, 'root@native_ddl_child')
        peer = connect(experiment, 'root@native_view_peer')
        verify()
        experiment.record('PASS', case='native_catalog_ddl_crash_recovery')
        exercise_recovered_triggers(lambda statement: experiment.sql(statement, child))
        experiment.sql('INSERT INTO native_triggers.t VALUES(4,5)')
        assert experiment.sql('SELECT v FROM native_triggers.t WHERE id=4') == ((6,),)
        assert experiment.sql('SELECT v FROM native_triggers.audit_t WHERE id=4') == ((6,),)
        experiment.record('PASS', case='native_catalog_trigger_execution_after_restart')
        experiment.sql('CREATE TABLE native_ddl.mock_parent(id INT PRIMARY KEY)', child)
        experiment.sql('INSERT INTO native_ddl.mock_parent VALUES(1)', child)
        experiment.sql('INSERT INTO native_ddl.mock_child VALUES(1,1)', child)
        assert experiment.sql('SELECT id,fk_id FROM native_ddl.mock_child', child) == ((1, 1),)
        assert experiment.sql("SELECT COUNT(*) FROM oceanbase.__all_mock_fk_parent_table "
                              "WHERE mock_fk_parent_table_name='mock_parent'", child) == ((0,),)
        assert experiment.sql('SELECT COUNT(*) FROM native_ddl.mock_child') == ((0,),)
        experiment.record('PASS', case='native_catalog_mock_parent_resolution_after_restart')
    finally:
        if peer is not None:
            peer.close()
        if child is not None:
            child.close()
        experiment.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    run(args.binary)
