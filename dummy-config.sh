#!/bin/sh

debuglevel=${AHCP_DEBUG_LEVEL:-1}

if [ $debuglevel -ge 2 ]; then
  echo Dummy config: $1
  env | grep ^AHCP
fi
