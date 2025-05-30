#!/usr/bin/env python3

import os
from multiprocessing.dummy import Pool

import pytest

import helpers.keeper_utils as ku
from helpers.cluster import ClickHouseCluster

CURRENT_TEST_DIR = os.path.dirname(os.path.abspath(__file__))
cluster = ClickHouseCluster(__file__)
nodes = [
    cluster.add_instance(
        "node1",
        main_configs=[
            "configs/enable_secure_keeper1.xml",
            "configs/ssl_conf.yml",
            "configs/WithoutPassPhrase.crt",
            "configs/WithoutPassPhrase.key",
            "configs/WithPassPhrase.crt",
            "configs/WithPassPhrase.key",
            "configs/rootCA.pem",
            "configs/logger.xml",
        ],
        stay_alive=True,
    ),
    cluster.add_instance(
        "node2",
        main_configs=[
            "configs/enable_secure_keeper2.xml",
            "configs/ssl_conf.yml",
            "configs/WithoutPassPhrase.crt",
            "configs/WithoutPassPhrase.key",
            "configs/WithPassPhrase.crt",
            "configs/WithPassPhrase.key",
            "configs/rootCA.pem",
            "configs/logger.xml",
        ],
        stay_alive=True,
    ),
    cluster.add_instance(
        "node3",
        main_configs=[
            "configs/enable_secure_keeper3.xml",
            "configs/ssl_conf.yml",
            "configs/WithoutPassPhrase.crt",
            "configs/WithoutPassPhrase.key",
            "configs/WithPassPhrase.crt",
            "configs/WithPassPhrase.key",
            "configs/rootCA.pem",
            "configs/logger.xml",
        ],
        stay_alive=True,
    ),
]


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()

        yield cluster

    finally:
        cluster.shutdown()


def get_fake_zk(nodename, timeout=30.0):
    return ku.get_fake_zk(cluster, nodename, timeout=timeout)


def run_test():
    node_zks = []
    try:
        for node in nodes:
            node_zks.append(get_fake_zk(node.name))

        node_zks[0].create("/test_node", b"somedata1")
        node_zks[1].sync("/test_node")
        node_zks[2].sync("/test_node")

        for node_zk in node_zks:
            assert node_zk.exists("/test_node") is not None
    finally:
        try:
            for zk_conn in node_zks:
                if zk_conn is None:
                    continue
                zk_conn.stop()
                zk_conn.close()
        except:
            pass


def setupSsl(node, filename, password):
    if password is None:
        node.copy_file_to_container(
            os.path.join(CURRENT_TEST_DIR, "configs/ssl_conf.yml"),
            "/etc/clickhouse-server/config.d/ssl_conf.yml",
        )

        node.replace_in_config(
            "/etc/clickhouse-server/config.d/ssl_conf.yml",
            "WithoutPassPhrase",
            filename,
        )
        return

    node.copy_file_to_container(
        os.path.join(CURRENT_TEST_DIR, "configs/ssl_conf_password.yml"),
        "/etc/clickhouse-server/config.d/ssl_conf.yml",
    )

    node.replace_in_config(
        "/etc/clickhouse-server/config.d/ssl_conf.yml",
        "WithoutPassPhrase",
        filename,
    )

    node.replace_in_config(
        "/etc/clickhouse-server/config.d/ssl_conf.yml",
        "PASSWORD",
        password,
    )


def stop_all_clickhouse():
    for node in nodes:
        node.stop_clickhouse()

    for node in nodes:
        node.exec_in_container(["rm", "-rf", "/var/lib/clickhouse/coordination"])


def start_clickhouse(node):
    node.start_clickhouse()


def start_all_clickhouse():
    p = Pool(3)
    waiters = []

    for node in nodes:
        waiters.append(p.apply_async(start_clickhouse, args=(node,)))

    for waiter in waiters:
        waiter.wait()

    for node in nodes:
        ku.wait_until_connected(cluster, node)


def check_valid_configuration(filename, password):
    stop_all_clickhouse()
    for node in nodes:
        setupSsl(node, filename, password)
    start_all_clickhouse()
    nodes[0].wait_for_log_line(
        "Raft ASIO listener initiated on :::9234, SSL enabled", look_behind_lines=5000
    )
    run_test()


def check_invalid_configuration(filename, password):
    stop_all_clickhouse()
    for node in nodes:
        setupSsl(node, filename, password)

    nodes[0].start_clickhouse()
    nodes[0].wait_for_log_line(
        "Raft ASIO listener initiated on :::9234, SSL enabled", look_behind_lines=5000
    )
    nodes[0].wait_for_log_line("failed to connect to peer.*Connection refused")


def test_secure_raft_works(started_cluster):
    check_valid_configuration("WithoutPassPhrase", None)


def test_secure_raft_works_with_password(started_cluster):
    check_valid_configuration("WithoutPassPhrase", "unusedpassword")
    check_invalid_configuration("WithPassPhrase", "wrongpassword")
    check_invalid_configuration("WithPassPhrase", "")
    check_valid_configuration("WithPassPhrase", "test")
    check_invalid_configuration("WithPassPhrase", None)
