#!/usr/bin/env bash
# 只跑构建好的可移植测试（不重新编译），汇总断言数。
# 用法：./scripts/run_portable_tests.sh [build_dir]
#   build_dir 默认 build_portable（bash 脚本的产物）；
#   在 Linux + CMake 下传 build-linux 也能用 —— 那里是可执行文件同名。
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

build_dir="${1:-build_portable}"
tests=(portable_tests device_tests simulator_tests serial_tests config_tests storage_tests fault_tests recovery_tests native_transport_tests)

if [ ! -d "$build_dir" ]; then
    echo "构建目录不存在：$build_dir（先跑 ./scripts/build_portable_tests.sh）" >&2
    exit 1
fi

total_assertions=0
total_failures=0
failed_suites=()

for test in "${tests[@]}"; do
    binary="$build_dir/$test"
    [ -x "$binary" ] || binary="$build_dir/$test.exe"
    if [ ! -x "$binary" ]; then
        echo "跳过（未找到可执行文件）：$test" >&2
        failed_suites+=("$test(缺失)")
        continue
    fi

    # 传仓库根目录：需要读仓库真实文件的用例靠它拼绝对路径（与 ctest 一致）。
    output="$(cd "$build_dir" && "./${binary##*/}" "$root" 2>&1)"
    code=$?
    # 各套件输出末尾都有「断言 N 项，失败 M 项」。
    line="$(printf '%s\n' "$output" | grep -o '断言 [0-9]* 项，失败 [0-9]* 项' | tail -1)"
    assertions="$(printf '%s' "$line" | sed -n 's/断言 \([0-9]*\) 项.*/\1/p')"
    failures="$(printf '%s' "$line" | sed -n 's/.*失败 \([0-9]*\) 项/\1/p')"
    assertions="${assertions:-0}"
    failures="${failures:-0}"

    total_assertions=$((total_assertions + assertions))
    total_failures=$((total_failures + failures))

    if [ "$code" -eq 0 ]; then
        printf '  ✓ %-18s %4s 断言\n' "$test" "$assertions"
    else
        printf '  ✗ %-18s %4s 断言，%s 失败\n' "$test" "$assertions" "$failures"
        failed_suites+=("$test")
    fi
done

echo
echo "合计断言 $total_assertions 项，失败 $total_failures 项"
if [ "${#failed_suites[@]}" -gt 0 ]; then
    echo "失败套件：${failed_suites[*]}" >&2
    exit 1
fi
echo "全部通过"
