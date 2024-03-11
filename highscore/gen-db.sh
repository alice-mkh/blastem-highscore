#!/bin/bash

sed $1 \
    -e 's/"/\\"/g' \
    -e 's/^\(.*\)$/"\1\\n"/' \
    -e '1s/^\(.*\)$/const char rom_db_data[] = \1/' \
    -e '$s/^\(.*\)$/\1;/'
