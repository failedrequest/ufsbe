#!/bin/sh
# mount_ufsbe — thin wrapper for mount(8) / fstab(5) integration.
#
# mount(8) calls helpers as:
#   mount_<type> [-o options] special mountpoint
#
# We translate that into a ufsbe(8) invocation.
# ufsbe accepts: ufsbe [-o options] special mountpoint
#
# SPDX-License-Identifier: BSD-2-Clause

UFSBE="${0%mount_ufsbe}ufsbe"

exec "${UFSBE}" "$@"
