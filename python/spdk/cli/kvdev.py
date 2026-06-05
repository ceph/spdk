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
