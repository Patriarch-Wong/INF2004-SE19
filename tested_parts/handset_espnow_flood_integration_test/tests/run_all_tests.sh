#!/bin/sh
# Everything that runs without a board: the build tests, then the host
# tests. Carries on after a failure and says at the end which part failed.
cd "$(dirname "$0")/.." || exit 1
failed=""
for part in run_build_tests run_host_tests run_handset_tests; do
    echo "== tests/$part.sh"
    sh "tests/$part.sh" || failed="$failed $part"
done
if [ -n "$failed" ]; then
    echo "FAILED:$failed"
    exit 1
fi
echo "All build and host tests passed"
