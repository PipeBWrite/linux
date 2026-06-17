#!/bin/bash

git add .

git commit -m "$1"

old_sha=$(git rev-parse --short HEAD~1)
new_sha=$(git rev-parse --short HEAD)

echo "$old_sha -> $new_sha"
echo
git diff $old_sha..$new_sha
