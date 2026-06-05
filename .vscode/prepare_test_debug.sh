#!/usr/bin/env bash

set -euo pipefail

workspace_dir="${1:?workspace dir is required}"
relative_file="${2:?relative file is required}"
build_dir="${workspace_dir}/build"
link_dir="${build_dir}/.vscode"
link_path="${link_dir}/current-test"

if [[ ! "${relative_file}" =~ ^tests/(unit|contract|integration)/(.*)/([^/]+)\.cpp$ ]]; then
    echo "active file must be a test source under tests/<layer>/<module>/.../*.cpp" >&2
    echo "current file: ${relative_file}" >&2
    exit 1
fi

layer="${BASH_REMATCH[1]}"
module_path="${BASH_REMATCH[2]}"
test_name="${BASH_REMATCH[3]}"
module_id="${module_path//\//_}"
target="${layer}_${module_id}_${test_name}"

cmake --build "${build_dir}" --target "${target}" --config Debug

candidate_paths=(
    "${build_dir}/tests/${target}"
    "${build_dir}/tests/Debug/${target}"
    "${build_dir}/Debug/tests/${target}"
    "${build_dir}/${target}"
)

binary_path=""
for candidate in "${candidate_paths[@]}"; do
    if [[ -x "${candidate}" ]]; then
        binary_path="${candidate}"
        break
    fi
done

if [[ -z "${binary_path}" ]]; then
    echo "built target '${target}', but could not locate its executable." >&2
    exit 1
fi

mkdir -p "${link_dir}"
ln -sfn "${binary_path}" "${link_path}"

echo "prepared debug target: ${target}"
echo "debug binary: ${binary_path}"
