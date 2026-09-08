#!/usr/bin/env bash
set -euo pipefail

repo_root() {
    local super_root
    super_root="$(git rev-parse --show-superproject-working-tree 2>/dev/null || true)"
    if [[ -n "${super_root}" ]]; then
        printf '%s\n' "${super_root%/}"
    else
        git rev-parse --show-toplevel
    fi
}

list_branched_submodules() {
    local gitmodules_path="$1"
    git config -f "${gitmodules_path}" --name-only --get-regexp '^submodule\..*\.branch$' 2>/dev/null \
        | sed -E 's/^submodule\.(.*)\.branch$/\1/'
}

ensure_local_branch() {
    local submodule_path="$1"
    local branch_name="$2"
    if git -C "${submodule_path}" show-ref --verify --quiet "refs/heads/${branch_name}"; then
        return 0
    fi
    if git -C "${submodule_path}" show-ref --verify --quiet "refs/remotes/origin/${branch_name}"; then
        git -C "${submodule_path}" branch "${branch_name}" "origin/${branch_name}" >/dev/null
        return 0
    fi
    return 1
}

sync_submodule_branch() {
    local root="$1"
    local submodule_rel="$2"
    local gitmodules_path="${root}/.gitmodules"
    local submodule_path="${root}/${submodule_rel}"
    local branch_name
    branch_name="$(git config -f "${gitmodules_path}" --get "submodule.${submodule_rel}.branch" 2>/dev/null || true)"
    if [[ -z "${branch_name}" || ! -d "${submodule_path}" ]]; then
        return 0
    fi
    if ! git -C "${submodule_path}" rev-parse --git-dir >/dev/null 2>&1; then
        return 0
    fi
    if ! ensure_local_branch "${submodule_path}" "${branch_name}"; then
        printf 'sync_submodule_branch: skipping %s, branch %s is unavailable.\n' "${submodule_rel}" "${branch_name}" >&2
        return 0
    fi

    local head_ref
    head_ref="$(git -C "${submodule_path}" symbolic-ref -q --short HEAD || true)"
    if [[ -n "${head_ref}" && "${head_ref}" != "${branch_name}" ]]; then
        return 0
    fi
    if [[ -n "$(git -C "${submodule_path}" status --porcelain --untracked-files=no)" ]]; then
        printf 'sync_submodule_branch: skipping %s, uncommitted changes are present.\n' "${submodule_rel}" >&2
        return 0
    fi
    if [[ "${head_ref}" == "${branch_name}" ]]; then
        return 0
    fi

    local detached_head
    detached_head="$(git -C "${submodule_path}" rev-parse HEAD)"

    if git -C "${submodule_path}" merge-base --is-ancestor "${branch_name}" "${detached_head}"; then
        git -C "${submodule_path}" branch -f "${branch_name}" "${detached_head}" >/dev/null
        git -C "${submodule_path}" checkout -q "${branch_name}"
        return 0
    fi

    local commits=()
    mapfile -t commits < <(git -C "${submodule_path}" rev-list --reverse --right-only --cherry-pick "${branch_name}...${detached_head}")

    git -C "${submodule_path}" checkout -q "${branch_name}"
    for commit in "${commits[@]}"; do
        git -C "${submodule_path}" cherry-pick --quiet "${commit}"
    done
}

main() {
    local root
    root="$(repo_root)"
    local gitmodules_path="${root}/.gitmodules"
    if [[ ! -f "${gitmodules_path}" ]]; then
        exit 0
    fi

    local submodules=()
    if (( $# > 0 )); then
        submodules=("$@")
    else
        mapfile -t submodules < <(list_branched_submodules "${gitmodules_path}")
    fi

    local submodule_rel
    for submodule_rel in "${submodules[@]}"; do
        sync_submodule_branch "${root}" "${submodule_rel}"
    done
}

main "$@"
