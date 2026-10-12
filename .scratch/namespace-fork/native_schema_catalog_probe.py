#!/usr/bin/env python3
"""Verify native table/column history reads before any child Runtime loads."""
import argparse
import os
import resource
from fork_parent_truncate_probe import BootstrapExperiment, namespace_id


def run(binary, selected=None):
    experiment = BootstrapExperiment(binary, 'native_schema_catalog', prototype=6)
    control = experiment.base / 'native-schema-catalog-control'
    os.environ['SEEKDB_NATIVE_SCHEMA_CATALOG_CONTROL'] = str(control)
    try:
        experiment.start()
        cases = [
            ('typed', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "u BIGINT UNSIGNED DEFAULT 18446744073709551615, n INT DEFAULT NULL, "
             "e VARCHAR(100) DEFAULT '', c ENUM('a','b') DEFAULT 'b'", '', (6, 0, 0, 0, 0, 0)),
            ('indexed', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'", '', (2, 0, 0, 0, 0, 0)),
            ('plain', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'", '', (2, 0, 0, 0, 0, 0)),
            ('versions', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'", '', (2, 0, 0, 0, 0, 0)),
            ('part', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', KEY ix(v), "
             "CONSTRAINT ck CHECK (id >= 0)", ' PARTITION BY HASH(id) PARTITIONS 8', (2, 8, 1, 1, 0, 0)),
            ('constraint', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT ck CHECK (id >= 0) NOT ENFORCED", '', (2, 0, 0, 1, 0, 0)),
            ('part2', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'",
             ' PARTITION BY RANGE(id) SUBPARTITION BY HASH(id) SUBPARTITIONS 2 '
             '(PARTITION p0 VALUES LESS THAN(10), PARTITION p1 VALUES LESS THAN MAXVALUE)',
             (2, 2, 0, 0, 0, 0)),
            ('drop_index', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', KEY ix(v)", '',
             (2, 0, 0, 0, 0, 0)),
            ('fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT fk FOREIGN KEY(id) REFERENCES parent(id)", '', (2, 0, 0, 0, 1, 0)),
            ('mock_fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT fk FOREIGN KEY(id) REFERENCES missing_parent(id)", '', (2, 0, 0, 0, 1, 0)),
            ('mock_fk_multi', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', KEY ix(v), "
             "CONSTRAINT fk FOREIGN KEY(v,id) REFERENCES missing_parent(b,a)", '', (2, 0, 1, 0, 1, 0)),
            ('old_mock_fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT fk FOREIGN KEY(id) REFERENCES missing_parent(id)", '', (2, 0, 0, 0, 1, 0)),
            ('self_fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', parent_id INT, "
             "KEY parent_idx(parent_id), CONSTRAINT fk FOREIGN KEY(parent_id) REFERENCES t(id)",
             '', (3, 0, 1, 0, 1, 0)),
            ('drop_fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT fk FOREIGN KEY(id) REFERENCES parent(id)", '', (2, 0, 0, 0, 0, 0)),
            ('old_fk', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "CONSTRAINT fk FOREIGN KEY(id) REFERENCES parent(id)", '', (2, 0, 0, 0, 1, 0)),
            ('history', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始'", '', (2, 0, 0, 0, 0, 0)),
            ('dropped', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', n INT", '', (2, 0, 0, 0, 0, 0)),
            ('generated', "id INT PRIMARY KEY, v VARCHAR(100) DEFAULT '初始', "
             "n INT GENERATED ALWAYS AS (LENGTH(v)) STORED", '', (3, 0, 0, 0, 0, 0)),
            ('lob', "id INT PRIMARY KEY, v VARCHAR(20000) DEFAULT '" + 'x' * 10000 + "'", '',
             (2, 0, 0, 0, 0, 10000)),
        ]
        for label, columns, options, expected in cases:
            if selected and label not in selected:
                continue
            database = f'native_catalog_{label}'
            mock_case = label in ('mock_fk', 'mock_fk_multi', 'old_mock_fk')
            experiment.sql(f'CREATE DATABASE {database}')
            if label in ('fk', 'drop_fk', 'old_fk'):
                experiment.sql(f'CREATE TABLE {database}.parent(id INT PRIMARY KEY)')
                columns = columns.replace('REFERENCES parent', f'REFERENCES {database}.parent')
            if label == 'self_fk':
                columns = columns.replace('REFERENCES t', f'REFERENCES {database}.t')
            if mock_case:
                columns = columns.replace('REFERENCES missing_parent', f'REFERENCES {database}.missing_parent')
                experiment.sql(f'USE {database}')
                experiment.sql('SET SESSION foreign_key_checks=0')
            experiment.sql(f'CREATE TABLE {database}.t({columns}){options}')
            if mock_case:
                experiment.sql('SET SESSION foreign_key_checks=1')
            table_id = experiment.sql("SELECT t.table_id FROM oceanbase.__all_table t JOIN oceanbase.__all_database d "
                                      f"ON t.database_id=d.database_id WHERE d.database_name='{database}' "
                                      "AND t.table_name='t'")[0][0]
            requested_version = 2**63 - 1
            if label == 'versions':
                for version in range(20):
                    experiment.sql(f"ALTER TABLE {database}.t COMMENT = 'history {version}'")
            elif label == 'history':
                requested_version = experiment.sql(f'SELECT schema_version FROM oceanbase.__all_table '
                                                   f'WHERE table_id={table_id}')[0][0]
                experiment.sql(f'ALTER TABLE {database}.t ADD n INT DEFAULT 17')
            elif label in ('drop_fk', 'old_fk', 'old_mock_fk'):
                if label in ('old_fk', 'old_mock_fk'):
                    requested_version = experiment.sql(f'SELECT schema_version FROM oceanbase.__all_table '
                                                       f'WHERE table_id={table_id}')[0][0]
                    if label == 'old_mock_fk':
                        requested_version = experiment.sql('SELECT MAX(schema_version) FROM oceanbase.__all_ddl_operation')[0][0]
                experiment.sql(f'ALTER TABLE {database}.t DROP FOREIGN KEY fk')
            elif label == 'drop_index':
                experiment.sql(f'ALTER TABLE {database}.t DROP INDEX ix')
            elif label == 'dropped':
                experiment.sql(f'ALTER TABLE {database}.t DROP COLUMN n')
                stored_columns = experiment.sql(f'SELECT column_name, is_hidden FROM oceanbase.__all_column '
                                                f'WHERE table_id={table_id} ORDER BY column_id')
                assert not any(row[0] == 'n' for row in stored_columns), stored_columns
                expected = (len(stored_columns), *expected[1:])
            name = f'native_catalog_cold_{label}'
            version_count, minimum = experiment.sql(
                f'SELECT COUNT(*), MIN(schema_version) FROM oceanbase.__all_table_history '
                f'WHERE table_id={table_id} AND schema_version<={requested_version}')[0]
            if label == 'versions':
                assert version_count > 16, version_count
            latest = experiment.sql(f'SELECT MAX(schema_version) FROM oceanbase.__all_table_history '
                                    f'WHERE table_id={table_id}')[0][0]
            original = experiment.sql(f'SELECT ori_schema_version FROM oceanbase.__all_ori_schema_version '
                                      f'WHERE table_id={table_id}')[0][0]
            current_version = experiment.sql(f'SELECT schema_version FROM oceanbase.__all_table WHERE table_id={table_id}')[0][0]
            current_indexes = experiment.sql('SELECT table_id, table_name, schema_version, index_type FROM oceanbase.__all_table '
                                             f'WHERE data_table_id={table_id} AND index_type>0')
            auxiliary = experiment.sql('SELECT a.table_id,a.table_type,a.index_type '
                'FROM oceanbase.__all_table_history a '
                f'WHERE a.data_table_id={table_id} AND a.is_deleted=0 '
                'AND a.schema_version=(SELECT MAX(b.schema_version) FROM oceanbase.__all_table_history b '
                f'WHERE b.table_id=a.table_id AND b.schema_version<={requested_version}) ORDER BY a.table_id')
            if mock_case:
                mock_id, mock_version = experiment.sql('SELECT mock_fk_parent_table_id, schema_version '
                    'FROM oceanbase.__all_mock_fk_parent_table_history '
                    f'WHERE database_id=(SELECT database_id FROM oceanbase.__all_table WHERE table_id={table_id}) '
                    f'AND schema_version<={requested_version} ORDER BY schema_version DESC LIMIT 1')[0]
                mock_columns = experiment.sql('SELECT c.parent_column_id,c.parent_column_name '
                    'FROM oceanbase.__all_mock_fk_parent_table_column_history c '
                    f'WHERE c.mock_fk_parent_table_id={mock_id} AND c.is_deleted=0 '
                    'AND c.schema_version=(SELECT MAX(h.schema_version) '
                    'FROM oceanbase.__all_mock_fk_parent_table_column_history h '
                    'WHERE h.mock_fk_parent_table_id=c.mock_fk_parent_table_id '
                    f'AND h.parent_column_id=c.parent_column_id AND h.schema_version<={requested_version}) '
                    'ORDER BY c.parent_column_id')
            if label == 'plain':
                core_self = experiment.sql("SELECT table_id, schema_version FROM oceanbase.__all_virtual_core_all_table WHERE table_name='__all_core_table'")
                core_tables = experiment.sql('SELECT COUNT(*) FROM oceanbase.__all_virtual_core_all_table '
                                             'WHERE table_id<>1')[0][0]
                core_columns = experiment.sql('SELECT COUNT(*) FROM oceanbase.__all_virtual_core_column_table '
                    'WHERE table_id IN (SELECT table_id FROM oceanbase.__all_virtual_core_all_table '
                    'WHERE table_id<>1)')[0][0]
            control.write_text(f'{name} {table_id} ' + ' '.join(map(str, expected))
                               + f' {requested_version}\n')
            experiment.sql(f'FORK NAMESPACE {name} FROM ns1')
            control.unlink()
            child_id = namespace_id(experiment, name)
            evidence = [line for log in (experiment.base / 'log').glob('seekdb.log*')
                        for line in log.read_text(errors='replace').splitlines()
                        if f'NATIVE_SCHEMA_CATALOG_FULL namespace={child_id} table={table_id}' in line]
            assert any('ret=0' in line for line in evidence), evidence
            simple = [line for log in (experiment.base / 'log').glob('seekdb.log*')
                      for line in log.read_text(errors='replace').splitlines()
                      if f'NATIVE_SCHEMA_CATALOG_SIMPLE namespace={child_id} table={table_id}' in line]
            assert any('ret=0' in line for line in simple), simple
            trace = '\n'.join(log.read_text(errors='replace') for log in (experiment.base / 'log').glob('seekdb.log*'))
            assert (f'NATIVE_SCHEMA_CATALOG_VERSIONS namespace={child_id} count={min(16, version_count)} '
                    f'min={minimum} latest={latest} original={original} ret=0') in trace
            assert (f'NATIVE_SCHEMA_CATALOG_CURRENT_METADATA namespace={child_id} table={table_id} '
                    f'version={current_version} indexes={len(current_indexes)} ret=0') in trace, trace[-2000:]
            assert f'NATIVE_SCHEMA_CATALOG_NAMES namespace={child_id} ret=0' in trace, trace[-2000:]
            assert (f'NATIVE_SCHEMA_CATALOG_AUX namespace={child_id} count={len(auxiliary)} ret=0') in trace, trace[-2000:]
            for aux_id, aux_type, index_type in auxiliary:
                assert (f'NATIVE_SCHEMA_CATALOG_AUX_ROW namespace={child_id} id={aux_id} '
                        f'type={aux_type} index_type={index_type}') in trace, auxiliary
            if mock_case:
                assert (f'NATIVE_SCHEMA_CATALOG_MOCK_FULL namespace={child_id} id={mock_id} '
                        f'version={mock_version} columns={len(mock_columns)} ret=0') in trace, trace[-2000:]
                for column_id, column_name in mock_columns:
                    assert (f'NATIVE_SCHEMA_CATALOG_MOCK_COLUMN namespace={child_id} id={column_id} '
                            f'name={column_name}') in trace, mock_columns
                if label != 'old_mock_fk':
                    assert (f'NATIVE_SCHEMA_CATALOG_MOCK_PARENT namespace={child_id} id={mock_id} '
                            f'version={mock_version} ret=0') in trace, trace[-2000:]
            for index_id, index_name, version, index_type in current_indexes:
                assert (f'NATIVE_SCHEMA_CATALOG_INDEX_INFO namespace={child_id} id={index_id} name={index_name} '
                        f'version={version} type={index_type}') in trace, current_indexes
            if label == 'part':
                assert (f'NATIVE_SCHEMA_CATALOG_INDEX_METADATA_TX namespace={child_id} '
                        'stage=read_rollback ret=0') in trace, trace[-2000:]
            if label == 'mock_fk_multi':
                assert f'NATIVE_SCHEMA_CATALOG_FOREIGN_KEY namespace={child_id} stage=deleted ret=0' in trace
            if label == 'constraint':
                trace = '\n'.join(log.read_text(errors='replace') for log in (experiment.base / 'log').glob('seekdb.log*'))
                assert f'NATIVE_SCHEMA_CATALOG_CONSTRAINT namespace={child_id} stage=deleted ret=0' in trace
            if label == 'indexed':
                trace = '\n'.join(log.read_text(errors='replace') for log in (experiment.base / 'log').glob('seekdb.log*'))
                assert f'NATIVE_SCHEMA_CATALOG_INDEXED namespace={child_id} stage=erase_committed ret=0' in trace
                assert f'NATIVE_SCHEMA_CATALOG_PARTIAL namespace={child_id} stage=partial_missing ret=0' in trace
                assert f'NATIVE_SCHEMA_CATALOG_PREFIX_DELETE namespace={child_id} stage=prefix_deleted ret=0' in trace
            if label == 'lob':
                assert f'NATIVE_SCHEMA_CATALOG_LOB_RETAIN namespace={child_id} ret=0' in trace
                assert f'NATIVE_SCHEMA_CATALOG_PARTIAL_LATEST namespace={child_id} ret=0' in trace
                assert f'NATIVE_SCHEMA_CATALOG_LOB_DML namespace={child_id} stage=erase_committed ret=0' in trace
            if label == 'typed':
                trace = '\n'.join(log.read_text(errors='replace') for log in (experiment.base / 'log').glob('seekdb.log*'))
                assert f'NATIVE_SCHEMA_CATALOG_TYPED namespace={child_id} stage=committed ret=0' in trace
            if label == 'plain':
                assert (f'NATIVE_SCHEMA_CATALOG_CORE namespace={child_id} tables={core_tables} '
                        f'columns={core_columns} ret=0') in trace, trace[-2000:]
                assert (f'NATIVE_SCHEMA_CATALOG_CORE_SELF namespace={child_id} '
                        f'id={core_self[0][0]} version={core_self[0][1]} ret=0') in trace, trace[-2000:]
                assert (f'NATIVE_SCHEMA_CATALOG_NAMES_TX namespace={child_id} '
                        'stage=read_rollback ret=0') in trace, trace[-2000:]
                assert (f'NATIVE_SCHEMA_CATALOG_CORE_HISTORY namespace={child_id} '
                        'stage=read_rollback ret=0') in trace, trace[-2000:]
                assert f'NATIVE_SCHEMA_CATALOG_TOMBSTONE namespace={child_id} ret=0' in trace
                borrowed = [line for log in (experiment.base / 'log').glob('seekdb.log*')
                            for line in log.read_text(errors='replace').splitlines()
                            if f'NATIVE_SCHEMA_CATALOG_BORROWED namespace={child_id} ' in line]
                assert any('stage=read_rollback ret=0' in line for line in borrowed), borrowed
            experiment.record('PASS', case=f'native_schema_catalog_{label}', evidence=evidence,
                              inherited_history=True, runtime_slots_loaded=0)
    finally:
        control.unlink(missing_ok=True)
        experiment.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--case', action='append', choices=('typed', 'indexed', 'constraint', 'plain', 'versions', 'part', 'fk', 'mock_fk', 'mock_fk_multi', 'old_mock_fk', 'generated', 'lob', 'history', 'dropped', 'part2', 'drop_index', 'self_fk', 'drop_fk', 'old_fk'))
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    run(args.binary, args.case)
