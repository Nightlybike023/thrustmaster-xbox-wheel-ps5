#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
payload_dir=payload
if [ ! -d "$payload_dir" ]; then payload_dir=.; fi
python3 "$payload_dir/embed_web.py"
test_dir=$(mktemp -d /tmp/ps5-wheel-tests.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
config_define="-DWHEEL_CFG_DIR=\"$test_dir/config\""
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
   -I"$payload_dir" tests/test_instance.c "$payload_dir/instance_guard.c" -o "$test_dir/test-instance"
"$test_dir/test-instance" "$test_dir"
cc -std=gnu11 -Wall -Wextra -Werror -g -pthread -fsanitize=address,undefined \
   -Itests/compat -I"$payload_dir" "$config_define" \
   tests/test_wheel.c "$payload_dir/controller_wheel.c" "$payload_dir/web_server.c" "$payload_dir/shifter.c" -lm -o "$test_dir/test-wheel"
"$test_dir/test-wheel"
cc -std=gnu11 -Wall -Wextra -Werror -g -pthread -fsanitize=address,undefined \
   -I"$payload_dir" tests/test_shifter.c "$payload_dir/shifter.c" -lm -o "$test_dir/test-shifter"
"$test_dir/test-shifter" "$test_dir"
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
   -Itests/compat -I"$payload_dir" tests/test_usb_helpers.c "$payload_dir/usb_helpers.c" -o "$test_dir/test-usb"
"$test_dir/test-usb"
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
   -Itests/compat -I"$payload_dir" tests/test_legacy.c "$payload_dir/controller_nintendo.c" -o "$test_dir/test-legacy"
"$test_dir/test-legacy"
python3 tests/test_http.py "$test_dir/test-wheel" "$test_dir/config"
