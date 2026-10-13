#!/usr/bin/env python3
"""A schema refresh must use this request's deadline, even on a reused worker.

Run after namespace_setup_template_local. The native test binary injects an
expired worker deadline for the marked SELECT; production runs the same query.
The first read after the readable SCN catches up must succeed without retries.
"""
import json
import time
import pymysql


def connect(port, namespace='sys'):
    return pymysql.connect(host='127.0.0.1', port=port, user='root@' + namespace,
                           database='oceanbase', autocommit=True, connect_timeout=2,
                           read_timeout=20, write_timeout=20)


def sql(connection, statement):
    with connection.cursor() as cursor:
        cursor.execute(statement)
        return cursor.fetchall()


with connect(42036) as standby, \
        connect(42035, 'replica_seed') as writer, connect(42036, 'replica_seed') as reader:
    sql(reader, 'SET ob_query_timeout=10000000')
    sql(reader, 'SELECT COUNT(*) FROM replica_ns.t')
    name = 'request_deadline_' + str(time.time_ns())
    sql(writer, f'CREATE TABLE replica_ns.{name}(id INT PRIMARY KEY,v INT)')
    # The primary's general decided SCN can lag this just-committed DDL.
    # Wait for the actual catalog row's commit SCN on the standby instead.
    target = int(sql(writer, 'SELECT ORA_ROWSCN FROM oceanbase.__all_table_history '
                     f"WHERE table_name='{name}' AND is_deleted=0")[0][0])
    deadline = time.monotonic() + 30
    while int(sql(standby, 'SELECT readable_scn FROM __all_virtual_server_stat')[0][0]) < target:
        assert time.monotonic() < deadline, 'standby readable SCN did not reach target'
        time.sleep(.05)
    started = time.monotonic()
    statement = ('SELECT /* native_request_deadline */ COUNT(*) FROM oceanbase.__all_table_history '
                 f"WHERE table_name='{name}' AND is_deleted=0")
    try:
        result = sql(reader, statement)
    except pymysql.MySQLError as error:
        print(json.dumps(dict(case='request_deadline_before_schema_refresh',
                              elapsed_s=time.monotonic()-started, error=error.args)), flush=True)
        raise
    assert result == ((1,),), result
    query_seconds = time.monotonic() - started
    sql(reader, 'SET ob_query_timeout=1')
    try:
        sql(reader, statement)
    except pymysql.MySQLError as error:
        assert error.args[0] == 4012, error.args
    else:
        raise AssertionError('an expired current request was allowed to run')
    sql(writer, f'DROP TABLE replica_ns.{name}')
    print(json.dumps(dict(case='request_deadline_before_schema_refresh',
                          query_s=query_seconds, result=result,
                          catalog_commit_scn=target,
                          expired_current_request_rejected=True)), flush=True)
