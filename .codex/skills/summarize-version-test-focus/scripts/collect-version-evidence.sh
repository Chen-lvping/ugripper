#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: collect-version-evidence.sh OLD_REF NEW_REF OUTPUT_DIR

Collects read-only Git evidence for one version comparison. OUTPUT_DIR must be
inside the repository's ignored tmp/version-change-summary directory.
EOF
}

if [[ $# -ne 3 ]]; then
    usage >&2
    exit 2
fi

old_ref=$1
new_ref=$2
output_arg=$3

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "not inside a Git repository" >&2
    exit 2
}

resolve_ref() {
    local ref=$1
    git rev-parse --verify --end-of-options "${ref}^{commit}" 2>/dev/null
}

old_commit=$(resolve_ref "${old_ref}") || {
    echo "Git ref not found with exact spelling: ${old_ref}" >&2
    exit 2
}
new_commit=$(resolve_ref "${new_ref}") || {
    echo "Git ref not found with exact spelling: ${new_ref}" >&2
    exit 2
}

allowed_root=$(realpath -m "${repo_root}/tmp/version-change-summary")
if [[ ${output_arg} = /* ]]; then
    output_dir=$(realpath -m "${output_arg}")
else
    output_dir=$(realpath -m "${repo_root}/${output_arg}")
fi

case "${output_dir}" in
    "${allowed_root}"|"${allowed_root}"/*) ;;
    *)
        echo "OUTPUT_DIR must be inside ${allowed_root}: ${output_dir}" >&2
        exit 2
        ;;
esac

if ! git -C "${repo_root}" check-ignore -q tmp; then
    echo "refusing to write because ${repo_root}/tmp is not ignored by Git" >&2
    exit 2
fi

relationship=diverged
if [[ ${old_commit} == "${new_commit}" ]]; then
    relationship=same
elif git -C "${repo_root}" merge-base --is-ancestor "${old_commit}" "${new_commit}"; then
    relationship=forward
elif git -C "${repo_root}" merge-base --is-ancestor "${new_commit}" "${old_commit}"; then
    relationship=reversed
fi

if [[ ${relationship} == reversed ]]; then
    echo "version order appears reversed: ${new_ref} is an ancestor of ${old_ref}" >&2
    exit 3
fi

mkdir -p "${output_dir}"

{
    printf 'old_ref\t%s\n' "${old_ref}"
    printf 'old_commit\t%s\n' "${old_commit}"
    printf 'new_ref\t%s\n' "${new_ref}"
    printf 'new_commit\t%s\n' "${new_commit}"
    printf 'relationship\t%s\n' "${relationship}"
    printf 'merge_base\t%s\n' "$(git -C "${repo_root}" merge-base "${old_commit}" "${new_commit}")"
    printf 'generated_at\t%s\n' "$(date -Iseconds)"
} > "${output_dir}/refs.txt"

git -C "${repo_root}" log --reverse \
    --format='%H%x09%ad%x09%an%x09%s' --date=iso-strict \
    "${old_commit}..${new_commit}" -- > "${output_dir}/commits.txt"

git -C "${repo_root}" diff --find-renames --find-copies \
    --name-status "${old_commit}" "${new_commit}" -- > "${output_dir}/name-status.txt"

git -C "${repo_root}" diff --stat \
    "${old_commit}" "${new_commit}" -- > "${output_dir}/diff-stat.txt"

git -C "${repo_root}" diff --numstat \
    "${old_commit}" "${new_commit}" -- > "${output_dir}/numstat.txt"

git -C "${repo_root}" diff --find-renames --find-copies --unified=3 \
    "${old_commit}" "${new_commit}" -- > "${output_dir}/changes.patch"

printf 'evidence_dir=%s\n' "${output_dir}"
printf 'old_commit=%s\n' "${old_commit}"
printf 'new_commit=%s\n' "${new_commit}"
printf 'relationship=%s\n' "${relationship}"
