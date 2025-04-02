#!/bin/bash

set -x -e

PLATFORM=stm32mp1-157_SOMIC

rm -rf build
make -j8 DEBUG=1 CFG_TEE_CORE_LOG_LEVEL=4 CROSS_COMPILE=arm-linux-gnueabihf- PLATFORM=$PLATFORM O=build all
