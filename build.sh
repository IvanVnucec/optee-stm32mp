#!/bin/bash

set -x -e

rm -rf build
make -j8 CROSS_COMPILE=arm-linux-gnueabihf- PLATFORM=stm32mp1-157_SOMIC O=build all
