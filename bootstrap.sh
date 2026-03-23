#!/bin/sh

set -eu

echo ">> Forging monbsd project structure..."

mkdir -p src include tests docs

touch README.md 
touch Makefile
touch .gitignore

echo ">> Structure forged successfully."

# :)
