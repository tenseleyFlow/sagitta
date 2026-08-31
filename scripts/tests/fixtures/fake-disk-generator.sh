#!/bin/sh

set -eu
[ "$#" -eq 1 ]
printf x >"$1"
