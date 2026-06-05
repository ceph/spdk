#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

from spdk.rpc.cmd_parser import print_json


def add_parser(subparsers):

    def kvdev_mem_create(args):
        print_json(args.client.kvdev_mem_create(name=args.name,
                                                 uuid=args.uuid,
                                                 max_value_len=args.max_value_len,
                                                 max_num_keys=args.max_num_keys))

    p = subparsers.add_parser('kvdev_mem_create', help='Create an in-memory kvdev')
    p.add_argument('name', help='Name of the in-memory kvdev to create')
    p.add_argument('-u', '--uuid', help='UUID of the kvdev (optional)')
    p.add_argument('-v', '--max-value-len', type=int,
                   help='Maximum value length in bytes (optional, 0 for default)')
    p.add_argument('-k', '--max-num-keys', type=int,
                   help='Maximum number of keys (optional, 0 for unlimited)')
    p.set_defaults(func=kvdev_mem_create)

    def kvdev_mem_delete(args):
        args.client.kvdev_mem_delete(name=args.name)

    p = subparsers.add_parser('kvdev_mem_delete', help='Delete an in-memory kvdev')
    p.add_argument('name', help='Name of the in-memory kvdev to delete')
    p.set_defaults(func=kvdev_mem_delete)

    def kvdev_mem_get_entry(args):
        print_json(args.client.kvdev_mem_get_entry(name=args.name, key=args.key))

    p = subparsers.add_parser('kvdev_mem_get_entry',
                              help='Inspect a stored entry (debug; exposes the store-only TTL)')
    p.add_argument('name', help='Name of the in-memory kvdev')
    p.add_argument('key', help='Key (1-16 byte ASCII string) of the entry to inspect')
    p.set_defaults(func=kvdev_mem_get_entry)

    # ---- librados-backed kvdev (ADR-0002/0004) ----

    def kvdev_rados_register_cluster(args):
        config_param = None
        if args.config_param:
            config_param = {}
            for entry in args.config_param:
                parts = entry.split('=', 1)
                if len(parts) != 2:
                    raise Exception('--config-param %s not in key=value form' % entry)
                config_param[parts[0]] = parts[1]
        print_json(args.client.kvdev_rados_register_cluster(
            name=args.name,
            user_id=args.user_id,
            config_param=config_param,
            config_file=args.config_file,
            key_file=args.key_file))

    p = subparsers.add_parser('kvdev_rados_register_cluster',
                              help='Register a named, shared rados cluster handle')
    p.add_argument('name', help='Name of the rados cluster handle')
    p.add_argument('--user', dest='user_id', help='Ceph client id (e.g. admin, not client.admin)')
    p.add_argument('--config-param', dest='config_param', action='append', metavar='key=value',
                   help='Explicit librados configuration')
    p.add_argument('--config-file', dest='config_file', help='Path of the rados (ceph.conf) configuration file')
    p.add_argument('--key-file', dest='key_file', help='Path of the rados keyring file')
    p.set_defaults(func=kvdev_rados_register_cluster)

    def kvdev_rados_unregister_cluster(args):
        args.client.kvdev_rados_unregister_cluster(name=args.name)

    p = subparsers.add_parser('kvdev_rados_unregister_cluster',
                              help='Unregister a named rados cluster handle')
    p.add_argument('name', help='Name of the rados cluster handle')
    p.set_defaults(func=kvdev_rados_unregister_cluster)

    def kvdev_rados_get_clusters(args):
        print_json(args.client.kvdev_rados_get_clusters(name=args.name))

    p = subparsers.add_parser('kvdev_rados_get_clusters',
                              help='Show registered rados cluster handles')
    p.add_argument('--name', dest='name', help='Rados cluster object name (omit for all)')
    p.set_defaults(func=kvdev_rados_get_clusters)

    def kvdev_rados_create(args):
        print_json(args.client.kvdev_rados_create(
            name=args.name,
            cluster_name=args.cluster_name,
            pool_name=args.pool_name,
            namespace=args.namespace,
            uuid=args.uuid,
            max_value_len=args.max_value_len))

    p = subparsers.add_parser('kvdev_rados_create', help='Create a librados-backed kvdev')
    p.add_argument('name', help='Name of the rados kvdev')
    p.add_argument('cluster_name', help='Name of a registered rados cluster')
    p.add_argument('pool_name', help='Rados pool (pre-provisioned; maps to the subsystem)')
    p.add_argument('--namespace', dest='namespace', help='Rados namespace (maps to the KV namespace)')
    p.add_argument('-u', '--uuid', dest='uuid', help='UUID of the kvdev (optional)')
    p.add_argument('-v', '--max-value-len', dest='max_value_len', type=int,
                   help='Maximum value length in bytes (0 for the 64 MB default)')
    p.set_defaults(func=kvdev_rados_create)

    def kvdev_rados_delete(args):
        args.client.kvdev_rados_delete(name=args.name)

    p = subparsers.add_parser('kvdev_rados_delete', help='Delete a librados-backed kvdev')
    p.add_argument('name', help='Name of the rados kvdev')
    p.set_defaults(func=kvdev_rados_delete)
