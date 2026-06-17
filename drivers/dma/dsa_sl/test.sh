#!/bin/bash
set -x
set -e

make

rmmod idxd
insmod ./dsa.ko
