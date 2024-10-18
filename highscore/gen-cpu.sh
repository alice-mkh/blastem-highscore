#!/bin/bash

ROOT=$1
INPUT=$2
DISPATCH=$3
OUT=$4

"$ROOT/../cpu_dsl.py" -d $DISPATCH $INPUT > $OUT
