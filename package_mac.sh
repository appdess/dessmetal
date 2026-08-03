#!/usr/bin/env bash
set -Eeuo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$repo_root/NeuralAmpModeler"
project_file="$project_root/projects/DessMetal-macOS.xcodeproj"
release_root="$project_root/build-mac/release"
products_root="$release_root/products"
packages_root="$release_root/packages"
installer_root="$release_root/installer"
out_root="$project_root/build-mac/out"
output_archive_root="$(cd "$repo_root/.." && pwd -P)/dessmetal-release-output-archive"

promoted_backup_path=""
archived_output_batch=""
rotation_out_root=""
rotation_backup=""
rotation_archive_batch=""
rotation_candidate_root=""
rotation_build_root=""
retention_self_test_root=""

rollback_output_rotation()
{
  trap - HUP INT TERM
  local archived_output rollback_failed
  rollback_failed=0

  if [[ -n "$rotation_candidate_root" && ! -e "$rotation_candidate_root" && -d "$rotation_out_root" ]]; then
    mv "$rotation_out_root" "$rotation_candidate_root" || {
      echo "CRITICAL: could not recover the new release candidate: $rotation_out_root" >&2
      rollback_failed=1
    }
  fi
  if [[ -n "$rotation_backup" && ! -e "$rotation_out_root" && -d "$rotation_backup" ]]; then
    mv "$rotation_backup" "$rotation_out_root" || {
      echo "CRITICAL: could not restore the previous release output: $rotation_backup" >&2
      rollback_failed=1
    }
  fi
  if [[ -n "$rotation_archive_batch" && -d "$rotation_archive_batch" ]]; then
    while IFS= read -r -d '' archived_output; do
      mv "$archived_output" "$rotation_build_root/" || {
        echo "CRITICAL: could not restore archived rollback evidence: $archived_output" >&2
        rollback_failed=1
      }
    done < <(find "$rotation_archive_batch" -mindepth 1 -maxdepth 1 -print0)
    rmdir "$rotation_archive_batch" 2>/dev/null || rollback_failed=1
  fi
  return "$rollback_failed"
}

restore_interrupted_output_rotation()
{
  rollback_output_rotation || true
  exit 130
}

cleanup_output_retention_self_test()
{
  if [[ -n "$retention_self_test_root" ]]; then
    /bin/rm -rf -- "$retention_self_test_root"
    retention_self_test_root=""
  fi
}

verify_release_path_ancestors()
{
  local checkout_root="$1"
  local expected_project_root="$2"
  local build_root="$3"
  local target_release_root="$4"
  local checkout_real project_real

  [[ "$expected_project_root" == "$checkout_root/NeuralAmpModeler"
     && "$build_root" == "$expected_project_root/build-mac"
     && "$target_release_root" == "$build_root/release" ]] || {
    echo "Refusing to clean an unexpected release directory" >&2
    return 1
  }
  [[ -d "$checkout_root" && ! -L "$checkout_root" ]] || {
    echo "Checkout root is missing, not a directory, or symlinked: $checkout_root" >&2
    return 1
  }
  [[ -d "$expected_project_root" && ! -L "$expected_project_root" ]] || {
    echo "Project root is missing, not a directory, or symlinked: $expected_project_root" >&2
    return 1
  }
  checkout_real="$(cd "$checkout_root" && pwd -P)"
  project_real="$(cd "$expected_project_root" && pwd -P)"
  [[ "$project_real" == "$checkout_real/NeuralAmpModeler" ]] || {
    echo "Resolved project root escapes the checkout" >&2
    return 1
  }
}

detach_disk_image_mapping()
{
  local image_path="$1"
  local device

  # DiskImages can briefly leave a just-created UDZO image mapped without a
  # mount point. Signing or verifying during that window fails with
  # "Resource temporarily unavailable". Eject only the whole-disk mappings
  # that hdiutil reports for this exact build output; simulator and unrelated
  # user images are never touched.
  while IFS= read -r device; do
    [[ "$device" =~ ^/dev/disk[0-9]+$ ]] || continue
    hdiutil detach "$device" >/dev/null
  done < <(
    hdiutil info | awk -v target="$image_path" '
      $1 == "image-path" {
        current = substr($0, index($0, ":") + 2)
        selected = (current == target)
        next
      }
      selected && /^=+$/ { exit }
      selected && $1 ~ /^\/dev\/disk[0-9]+$/ { print $1 }
    '
  )
}

verify_release_cleanup_path()
{
  local checkout_root="$1"
  local expected_project_root="$2"
  local build_root="$3"
  local target_release_root="$4"
  local build_real

  verify_release_path_ancestors \
    "$checkout_root" "$expected_project_root" "$build_root" "$target_release_root" || return 1
  [[ -d "$build_root" && ! -L "$build_root" ]] || {
    echo "Release build root is missing, not a directory, or symlinked: $build_root" >&2
    return 1
  }
  [[ ! -L "$target_release_root" ]] || {
    echo "Refusing to clean through a symlinked release path: $target_release_root" >&2
    return 1
  }
  if [[ -e "$target_release_root" && ! -d "$target_release_root" ]]; then
    echo "Release path exists but is not a directory: $target_release_root" >&2
    return 1
  fi
  build_real="$(cd "$build_root" && pwd -P)"
  [[ "$build_real" == "$(cd "$expected_project_root" && pwd -P)/build-mac" ]] || {
    echo "Resolved release build path escapes the checkout" >&2
    return 1
  }
}

prepare_release_build_root()
{
  local checkout_root="$1"
  local expected_project_root="$2"
  local build_root="$3"
  local target_release_root="$4"

  verify_release_path_ancestors \
    "$checkout_root" "$expected_project_root" "$build_root" "$target_release_root" || return 1
  if [[ -e "$build_root" || -L "$build_root" ]]; then
    [[ -d "$build_root" && ! -L "$build_root" ]] || {
      echo "Refusing unexpected release build root: $build_root" >&2
      return 1
    }
  else
    mkdir "$build_root" || return 1
  fi
  verify_release_cleanup_path \
    "$checkout_root" "$expected_project_root" "$build_root" "$target_release_root"
}

source_archive_sha256()
{
  local checkout_root="$1"
  git -C "$checkout_root" archive --format=tar HEAD | shasum -a 256 | awk '{print $1}'
}

verify_source_identity()
{
  local checkout_root="$1"
  local expected_commit="$2"
  local expected_archive_sha256="$3"
  local status

  [[ "$(git -C "$checkout_root" rev-parse HEAD)" == "$expected_commit" ]] || {
    echo "Source commit changed during signed packaging" >&2
    return 1
  }
  status="$(git -C "$checkout_root" status --porcelain --untracked-files=normal)"
  [[ -z "$status" ]] || {
    echo "Signed packaging requires a clean tracked and untracked source tree" >&2
    printf '%s\n' "$status" >&2
    return 1
  }
  [[ "$(source_archive_sha256 "$checkout_root")" == "$expected_archive_sha256" ]] || {
    echo "Source archive changed during signed packaging" >&2
    return 1
  }
}

rotate_release_output()
{
  local candidate_root="$1"
  local output_root="$2"
  local build_root="$3"
  local checkout_root="$4"
  local archive_root="$5"
  local failpoint="${6:-}"
  local checkout_real archive_real previous_output
  local -a previous_outputs=()

  promoted_backup_path=""
  archived_output_batch=""

  [[ "$output_root" == "$build_root/out" && ! -L "$output_root" ]] || {
    echo "Refusing to replace an unexpected or symlinked output directory" >&2
    return 1
  }
  [[ -d "$candidate_root" && ! -L "$candidate_root" ]] || {
    echo "Release candidate is missing, not a directory, or symlinked: $candidate_root" >&2
    return 1
  }
  if [[ -e "$output_root" && ! -d "$output_root" ]]; then
    echo "Output path exists but is not a directory: $output_root" >&2
    return 1
  fi

  while IFS= read -r -d '' previous_output; do
    [[ -d "$previous_output" && ! -L "$previous_output" ]] || {
      echo "Refusing unexpected superseded-output path: $previous_output" >&2
      return 1
    }
    previous_outputs+=("$previous_output")
  done < <(find "$build_root" -mindepth 1 -maxdepth 1 -name 'superseded-out-*' -print0)

  # Prepare (but do not populate) a recoverable archive outside the checkout.
  # Existing rollback evidence remains untouched until the new output is live.
  if (( ${#previous_outputs[@]} > 0 )); then
    [[ "$archive_root" == /* && ! -L "$archive_root" ]] || {
      echo "Output archive must be an absolute, non-symlink path: $archive_root" >&2
      return 1
    }
    mkdir -p "$archive_root"
    [[ -d "$archive_root" && ! -L "$archive_root" ]] || {
      echo "Could not prepare output archive: $archive_root" >&2
      return 1
    }
    checkout_real="$(cd "$checkout_root" && pwd -P)"
    archive_real="$(cd "$archive_root" && pwd -P)"
    case "$archive_real/" in
      "$checkout_real/"*)
        echo "Output archive must be outside the checkout: $archive_real" >&2
        return 1
        ;;
    esac
    archived_output_batch="$(mktemp -d \
      "$archive_root/$(basename "$checkout_real")-$(date -u '+%Y%m%d-%H%M%S')-XXXXXX")"
  fi

  if [[ -d "$output_root" ]]; then
    promoted_backup_path="$build_root/superseded-out-$(date -u '+%Y%m%d-%H%M%S')-$$"
    [[ ! -e "$promoted_backup_path" && ! -L "$promoted_backup_path" ]] || {
      echo "Output backup path already exists: $promoted_backup_path" >&2
      [[ -z "$archived_output_batch" ]] || rmdir "$archived_output_batch"
      return 1
    }
  fi

  rotation_out_root="$output_root"
  rotation_backup="$promoted_backup_path"
  rotation_archive_batch="$archived_output_batch"
  rotation_candidate_root="$candidate_root"
  rotation_build_root="$build_root"
  trap restore_interrupted_output_rotation HUP INT TERM

  if [[ -n "$promoted_backup_path" ]]; then
    if ! mv "$output_root" "$promoted_backup_path"; then
      trap - HUP INT TERM
      [[ -z "$archived_output_batch" ]] || rmdir "$archived_output_batch"
      return 1
    fi
  fi

  if [[ "$failpoint" == "after-backup" ]]; then
    echo "Injected output-promotion failure after backup" >&2
    rollback_output_rotation || {
      echo "CRITICAL: output rollback failed after injected promotion failure" >&2
      return 1
    }
    promoted_backup_path=""
    archived_output_batch=""
    return 97
  fi

  if ! mv "$candidate_root" "$output_root"; then
    rollback_output_rotation || {
      echo "CRITICAL: output rollback failed after candidate promotion error" >&2
      return 1
    }
    promoted_backup_path=""
    archived_output_batch=""
    return 1
  fi

  # Promotion succeeded. Keep its immediate predecessor in the checkout and
  # move every older rollback set to the external archive without deleting it.
  local archived_count
  archived_count=0
  if (( ${#previous_outputs[@]} > 0 )); then
    for previous_output in "${previous_outputs[@]}"; do
      if ! mv "$previous_output" "$archived_output_batch/"; then
        echo "Older rollback archival failed; restoring the prior output: $previous_output" >&2
        rollback_output_rotation || {
          echo "CRITICAL: output rollback failed after archival error" >&2
          return 1
        }
        return 1
      fi
      archived_count=$((archived_count + 1))
      if [[ "$failpoint" == "during-archive" && "$archived_count" == "1" ]]; then
        echo "Injected output-retention failure during archival" >&2
        rollback_output_rotation || {
          echo "CRITICAL: output rollback failed after injected archival failure" >&2
          return 1
        }
        promoted_backup_path=""
        archived_output_batch=""
        return 98
      fi
    done
  fi

  local remaining_count
  local retention_verification_failed
  retention_verification_failed=false
  if [[ "$failpoint" == "verify-error" ]]; then
    echo "Injected output-retention verification failure" >&2
    retention_verification_failed=true
  elif ! remaining_count="$(find "$build_root" -mindepth 1 -maxdepth 1 \
    -type d -name 'superseded-out-*' | wc -l | tr -d ' ')"; then
    retention_verification_failed=true
  fi
  if [[ "$retention_verification_failed" == "true" ]]; then
    echo "Could not verify in-checkout rollback retention; restoring the prior output" >&2
    rollback_output_rotation || {
      echo "CRITICAL: output rollback failed after retention verification error" >&2
      return 1
    }
    promoted_backup_path=""
    archived_output_batch=""
    [[ "$failpoint" == "verify-error" ]] && return 99
    return 1
  fi
  if [[ -n "$promoted_backup_path" ]]; then
    [[ "$remaining_count" == "1" && -d "$promoted_backup_path" ]] || {
      echo "Expected exactly one in-checkout rollback after promotion" >&2
      rollback_output_rotation || true
      return 1
    }
  else
    [[ "$remaining_count" == "0" ]] || {
      echo "Unexpected in-checkout rollback remains after first promotion" >&2
      rollback_output_rotation || true
      return 1
    }
  fi
  trap - HUP INT TERM
}

run_output_retention_self_test()
{
  local fixture_root fixture_checkout fixture_build fixture_archive failure_status
  local first_checkout first_build first_archive
  fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/dessmetal-output-retention.XXXXXX")"
  fixture_checkout="$fixture_root/dessmetal"
  fixture_build="$fixture_checkout/build-mac"
  fixture_archive="$fixture_root/archive"
  retention_self_test_root="$fixture_root"
  trap cleanup_output_retention_self_test EXIT

  first_checkout="$fixture_root/first-run-dessmetal"
  first_build="$first_checkout/build-mac"
  first_archive="$fixture_root/first-run-archive"
  mkdir -p "$first_build/candidate/candidate-marker"

  rotate_release_output \
    "$first_build/candidate" "$first_build/out" "$first_build" \
    "$first_checkout" "$first_archive"
  [[ -d "$first_build/out/candidate-marker" ]]
  [[ ! -e "$first_build/candidate" ]]
  [[ -z "$promoted_backup_path" ]]
  [[ -z "$archived_output_batch" ]]
  [[ ! -e "$first_archive" ]]
  [[ "$(find "$first_build" -mindepth 1 -maxdepth 1 \
    -type d -name 'superseded-out-*' | wc -l | tr -d ' ')" == "0" ]]

  local clean_checkout clean_project clean_build clean_release cleanup_sentinel
  clean_checkout="$fixture_root/clean-checkout"
  clean_project="$clean_checkout/NeuralAmpModeler"
  clean_build="$clean_project/build-mac"
  clean_release="$clean_build/release"
  cleanup_sentinel="$fixture_root/cleanup-sentinel"
  mkdir -p "$clean_project"
  printf 'outside-cleanup-sentinel\n' > "$cleanup_sentinel"
  [[ ! -e "$clean_build" && ! -L "$clean_build" ]]
  prepare_release_build_root "$clean_checkout" "$clean_project" "$clean_build" "$clean_release"
  [[ -d "$clean_build" && ! -L "$clean_build" ]]
  [[ ! -e "$clean_release" ]]
  mkdir -p "$clean_release/generated-marker"
  verify_release_cleanup_path "$clean_checkout" "$clean_project" "$clean_build" "$clean_release"
  /bin/rm -rf -- "$clean_release"
  [[ ! -e "$clean_release" ]]
  [[ "$(cat "$cleanup_sentinel")" == "outside-cleanup-sentinel" ]]

  local cleanup_checkout cleanup_project cleanup_build outside_build
  cleanup_checkout="$fixture_root/cleanup-checkout"
  cleanup_project="$cleanup_checkout/NeuralAmpModeler"
  cleanup_build="$cleanup_project/build-mac"
  mkdir -p "$cleanup_build/release"
  verify_release_cleanup_path "$cleanup_checkout" "$cleanup_project" "$cleanup_build" "$cleanup_build/release"
  outside_build="$fixture_root/outside-build"
  mkdir -p "$outside_build/release"
  printf 'outside-build-sentinel\n' > "$outside_build/release/do-not-delete.txt"
  find "$cleanup_build" -depth -delete
  ln -s "$outside_build" "$cleanup_build"
  if prepare_release_build_root \
    "$cleanup_checkout" "$cleanup_project" "$cleanup_build" "$cleanup_build/release"; then
    echo "Symlinked release build path unexpectedly passed validation" >&2
    return 1
  fi
  [[ -d "$outside_build/release" ]]
  [[ "$(cat "$outside_build/release/do-not-delete.txt")" == "outside-build-sentinel" ]]
  [[ "$(cat "$cleanup_sentinel")" == "outside-cleanup-sentinel" ]]

  local source_checkout source_commit_fixture source_hash_fixture
  source_checkout="$fixture_root/source-checkout"
  mkdir -p "$source_checkout/out/current-marker"
  git -C "$source_checkout" init -q
  printf '/out/\n' > "$source_checkout/.gitignore"
  printf 'stable\n' > "$source_checkout/tracked.txt"
  git -C "$source_checkout" add .gitignore tracked.txt
  git -C "$source_checkout" -c user.name=DessMetal -c user.email=ci.invalid commit -qm initial
  source_commit_fixture="$(git -C "$source_checkout" rev-parse HEAD)"
  source_hash_fixture="$(source_archive_sha256 "$source_checkout")"
  verify_source_identity "$source_checkout" "$source_commit_fixture" "$source_hash_fixture"
  printf 'dirty\n' >> "$source_checkout/tracked.txt"
  if verify_source_identity "$source_checkout" "$source_commit_fixture" "$source_hash_fixture"; then
    echo "Dirty signed source unexpectedly passed identity validation" >&2
    return 1
  fi
  [[ -d "$source_checkout/out/current-marker" ]]
  git -C "$source_checkout" add tracked.txt
  git -C "$source_checkout" -c user.name=DessMetal -c user.email=ci.invalid commit -qm changed
  if verify_source_identity "$source_checkout" "$source_commit_fixture" "$source_hash_fixture"; then
    echo "Changed signed source commit unexpectedly passed identity validation" >&2
    return 1
  fi
  [[ -d "$source_checkout/out/current-marker" ]]

  mkdir -p \
    "$fixture_build/out/current-marker" \
    "$fixture_build/candidate/candidate-marker" \
    "$fixture_build/superseded-out-old-a/old-a-marker" \
    "$fixture_build/superseded-out-old-b/old-b-marker"

  failure_status=0
  rotate_release_output \
    "$fixture_build/candidate" "$fixture_build/out" "$fixture_build" \
    "$fixture_checkout" "$fixture_archive" after-backup || failure_status=$?
  [[ "$failure_status" == "97" ]]
  [[ -d "$fixture_build/out/current-marker" ]]
  [[ -d "$fixture_build/candidate/candidate-marker" ]]
  [[ -d "$fixture_build/superseded-out-old-a" && -d "$fixture_build/superseded-out-old-b" ]]

  failure_status=0
  rotate_release_output \
    "$fixture_build/candidate" "$fixture_build/out" "$fixture_build" \
    "$fixture_checkout" "$fixture_archive" during-archive || failure_status=$?
  [[ "$failure_status" == "98" ]]
  [[ -d "$fixture_build/out/current-marker" ]]
  [[ -d "$fixture_build/candidate/candidate-marker" ]]
  [[ -d "$fixture_build/superseded-out-old-a/old-a-marker" ]]
  [[ -d "$fixture_build/superseded-out-old-b/old-b-marker" ]]

  failure_status=0
  rotate_release_output \
    "$fixture_build/candidate" "$fixture_build/out" "$fixture_build" \
    "$fixture_checkout" "$fixture_archive" verify-error || failure_status=$?
  [[ "$failure_status" == "99" ]]
  [[ -d "$fixture_build/out/current-marker" ]]
  [[ -d "$fixture_build/candidate/candidate-marker" ]]
  [[ -d "$fixture_build/superseded-out-old-a/old-a-marker" ]]
  [[ -d "$fixture_build/superseded-out-old-b/old-b-marker" ]]

  rotate_release_output \
    "$fixture_build/candidate" "$fixture_build/out" "$fixture_build" \
    "$fixture_checkout" "$fixture_archive"
  [[ -d "$fixture_build/out/candidate-marker" ]]
  [[ -d "$promoted_backup_path/current-marker" ]]
  [[ "$(find "$fixture_build" -mindepth 1 -maxdepth 1 \
    -type d -name 'superseded-out-*' | wc -l | tr -d ' ')" == "1" ]]
  [[ -d "$archived_output_batch/superseded-out-old-a/old-a-marker" ]]
  [[ -d "$archived_output_batch/superseded-out-old-b/old-b-marker" ]]

  echo "Output retention self-test passed"
  cleanup_output_retention_self_test
  trap - EXIT
}

app_identity="${DESSMETAL_APP_IDENTITY:-Developer ID Application: Alexander Dess (6756YXU4J7)}"
installer_identity="${DESSMETAL_INSTALLER_IDENTITY:-Developer ID Installer: Alexander Dess (6756YXU4J7)}"
team_id="6756YXU4J7"
mode="signed"
case "${1:-}" in
  "") ;;
  --unsigned) mode="unsigned" ;;
  --self-test-output-retention) run_output_retention_self_test; exit 0 ;;
  *) echo "Usage: $0 [--unsigned|--self-test-output-retention]" >&2; exit 2 ;;
esac

source_commit="$(git -C "$repo_root" rev-parse HEAD)"
source_hash="$(source_archive_sha256 "$repo_root")"
if [[ "$mode" == "signed" ]]; then
  verify_source_identity "$repo_root" "$source_commit" "$source_hash"
fi

if [[ -n "${DESSMETAL_NOTARIZE:-}" ]]; then
  echo "DESSMETAL_NOTARIZE is no longer accepted here. Build first, review its SHA-256, then use notarize_mac.sh." >&2
  exit 2
fi

version="$(awk -F '"' '/^#define PLUG_VERSION_STR / { print $2; exit }' "$project_root/config.h")"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Could not determine a semantic version from config.h" >&2
  exit 1
fi

echo "Running versioned state-layout migration regression tests"
bash "$project_root/tests/run_unserialization_layout_tests.sh"

app="$products_root/Applications/DessMetal.app"
au="$products_root/Components/DessMetal.component"
vst3="$products_root/VST3/DessMetal.vst3"
suffix=""
[[ "$mode" == "unsigned" ]] && suffix="-UNSIGNED-VALIDATION-ONLY"
pkg="$installer_root/DessMetal-v${version}-mac${suffix}.pkg"
dmg_work="$release_root/DessMetal-v${version}-mac${suffix}.dmg"
pkg_out="$out_root/$(basename "$pkg")"
dmg="$out_root/$(basename "$dmg_work")"
dsym_work="$release_root/DessMetal-v${version}-mac${suffix}-dSYMs.zip"
dsym_zip="$out_root/$(basename "$dsym_work")"
set_manifest="$out_root/DessMetal-v${version}-mac${suffix}-ARTIFACTS.txt"

require_file()
{
  [[ -e "$1" ]] || { echo "Missing required artifact: $1" >&2; exit 1; }
}

prepare_nonrelocatable_component_plist()
{
  local component_root="$1"
  local expected_bundle="$2"
  local component_plist="$3"
  local actual_root_entries

  actual_root_entries="$(find "$component_root" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort)"
  [[ "$actual_root_entries" == "$expected_bundle" ]] || {
    echo "Component root must contain only $expected_bundle: $component_root" >&2
    printf '%s\n' "$actual_root_entries" >&2
    exit 1
  }

  pkgbuild --analyze --root "$component_root" "$component_plist"
  [[ "$(plutil -extract '0.RootRelativeBundlePath' raw -o - "$component_plist")" == "$expected_bundle" ]] || {
    echo "pkgbuild analyzed an unexpected bundle in $component_root" >&2
    exit 1
  }
  plutil -replace '0.BundleIsRelocatable' -bool NO "$component_plist"
  plutil -replace '0.BundleHasStrictIdentifier' -bool YES "$component_plist"
  plutil -replace '0.BundleIsVersionChecked' -bool YES "$component_plist"
  plutil -replace '0.BundleOverwriteAction' -string upgrade "$component_plist"
  [[ "$(plutil -extract '0.BundleIsRelocatable' raw -o - "$component_plist")" == "false" ]] || {
    echo "Could not disable package relocation for $expected_bundle" >&2
    exit 1
  }
}

verify_component_install_policy()
{
  local package_info="$1"
  local expected_location="$2"
  local expected_bundle_id="$3"
  local expected_bundle_path="$4"

  require_file "$package_info"
  [[ "$(xmllint --xpath 'string(/pkg-info/@install-location)' "$package_info")" == "$expected_location" ]] || {
    echo "Unexpected component install location: $package_info" >&2
    exit 1
  }
  [[ "$(xmllint --xpath 'string(/pkg-info/@relocatable)' "$package_info")" == "false" ]] || {
    echo "Release component does not explicitly disable relocation: $package_info" >&2
    exit 1
  }
  [[ "$(xmllint --xpath 'count(/pkg-info/relocate/*)' "$package_info")" == "0" ]] || {
    echo "Release component is relocatable and could install outside its fixed destination: $package_info" >&2
    exit 1
  }
  [[ "$(xmllint --xpath 'count(/pkg-info/strict-identifier/bundle)' "$package_info")" == "1" ]] || {
    echo "Release component must contain exactly one strict bundle identifier: $package_info" >&2
    exit 1
  }
  [[ "$(xmllint --xpath "count(/pkg-info/strict-identifier/bundle[@id='$expected_bundle_id'])" "$package_info")" == "1" ]] || {
    echo "Release component is missing strict bundle-identifier enforcement: $package_info" >&2
    exit 1
  }
  [[ "$(xmllint --xpath "count(/pkg-info/bundle[@path='./$expected_bundle_path' and @id='$expected_bundle_id'])" "$package_info")" == "1" ]] || {
    echo "Release component has an unexpected primary bundle path or identifier: $package_info" >&2
    exit 1
  }
  [[ "$(xmllint --xpath 'count(/pkg-info/bundle)' "$package_info")" == "1" ]] || {
    echo "Release component must describe exactly one primary bundle: $package_info" >&2
    exit 1
  }
}

require_universal_binary()
{
  local binary="$1"
  local architectures
  architectures="$(lipo -archs "$binary")"
  [[ " $architectures " == *" arm64 "* && " $architectures " == *" x86_64 "* ]] || {
    echo "Expected arm64+x86_64 binary, got '$architectures': $binary" >&2
    exit 1
  }
}

verify_resource_manifest()
{
  local bundle="$1"
  local resources="$bundle/Contents/Resources"
  local expected_models actual_models expected_irs actual_irs expected_images actual_images expected_fonts actual_fonts
  expected_models=$'DessBlock-green/model.nam\nDessDrive/OD808.nam\nDessDrive/SD1.nam\nDessDrive/TS9.nam\nDessDrive/aesahaettr.nam\nDessTortion-blue/DessTortion-blue.nam\nDessTortion-red/DessTortion-red.nam\nSickDess/SickDess.nam'
  actual_models="$(cd "$resources/models" && find . -type f ! -path './IRs/*' -print | sed 's|^./||' | LC_ALL=C sort)"
  [[ "$actual_models" == "$expected_models" ]] || {
    echo "Unexpected release model manifest in $bundle:" >&2
    printf '%s\n' "$actual_models" >&2
    exit 1
  }
  expected_irs=$'ENGL-5150-V30-Mix.wav\nENGL-5153-V30-Sheffield-Mix.wav\nENGL-MIX.wav\nENGL-ORG.1.wav\nENGL-V30-1.wav\nENGL-V30-2.wav\nENGL-V30-3.wav\nENGL-V30-4.wav\nENGL-V30-Sheffield-Mix.wav\nENGL-V30-UK-SM57+E609.1.wav\nENGL-V30.wav\nbeasty-trio.wav\ncutout-switch.wav\nerretic-bloodbath.wav\nesele-dying.wav\npolestar.wav'
  local source_irs
  source_irs="$(cd "$project_root/resources/models/IRs" && find . ! -type d \
    ! -name 'ENGL-Mix-5153-Rhythm.wav' -print | sed 's|^./||' | LC_ALL=C sort)"
  [[ "$source_irs" == "$expected_irs" ]] || {
    echo "Release source IR manifest differs from the locked 16-file allowlist:" >&2
    printf '%s\n' "$source_irs" >&2
    exit 1
  }
  actual_irs="$(cd "$resources/IRs" && find . ! -type d -print | sed 's|^./||' | LC_ALL=C sort)"
  [[ "$actual_irs" == "$expected_irs" ]] || {
    echo "Bundled IR manifest does not match the release source in $bundle" >&2
    exit 1
  }
  expected_images=$'ArrowLeft.svg\nArrowRight.svg\nBackground.jpg\nBackground@2x.jpg\nBackground@3x.jpg\nCross.svg\nDessMetal/Background.png\nDessMetal/DessBlock-green.jpg\nDessMetal/DessTortion-blue.jpg\nDessMetal/DessTortion-red.jpg\nDessMetal/SickDess.jpg\nFile.svg\nFileBackground.png\nFileBackground@2x.png\nFileBackground@3x.png\nGear.svg\nGlobe.svg\nIRIconOff.svg\nIRIconOn.svg\nInputLevelBackground.png\nInputLevelBackground@2x.png\nInputLevelBackground@3x.png\nKnobBackground.png\nKnobBackground@2x.png\nKnobBackground@3x.png\nLines.png\nLines@2x.png\nLines@3x.png\nMeterBackground.png\nMeterBackground@2x.png\nMeterBackground@3x.png\nModelIcon.svg\nSlideSwitchHandle.png\nSlideSwitchHandle@2x.png\nSlideSwitchHandle@3x.png'
  actual_images="$(cd "$resources" && find . -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.svg' \) -print | sed 's|^./||' | LC_ALL=C sort)"
  [[ "$actual_images" == "$expected_images" ]] || {
    echo "Unexpected release image manifest in $bundle:" >&2
    printf '%s\n' "$actual_images" >&2
    exit 1
  }
  expected_fonts=$'Michroma-Regular.ttf\nRoboto-Regular.ttf'
  actual_fonts="$(cd "$resources" && find . -type f -iname '*.ttf' -print | sed 's|^./||' | LC_ALL=C sort)"
  [[ "$actual_fonts" == "$expected_fonts" ]] || {
    echo "Unexpected release font manifest in $bundle:" >&2
    printf '%s\n' "$actual_fonts" >&2
    exit 1
  }
}

verify_unique_objc_namespace()
{
  local bundle="$1"
  local binary="$bundle/Contents/MacOS/DessMetal"
  local api_suffix
  case "$bundle" in
    *.app) api_suffix="app" ;;
    *.component) api_suffix="au" ;;
    *.vst3) api_suffix="vst3" ;;
    *) echo "Unknown Apple bundle type: $bundle" >&2; exit 1 ;;
  esac

  local architecture legacy_symbols symbols expected_symbol
  for architecture in arm64 x86_64; do
    symbols="$(nm -arch "$architecture" -j "$binary")"
    legacy_symbols="$(grep -E 'vNeuralAmpModeler|NeuralAmpModeler_View' <<< "$symbols" || true)"
    [[ -z "$legacy_symbols" ]] || {
      echo "Legacy NeuralAmpModeler Objective-C symbols remain in $bundle ($architecture):" >&2
      printf '%s\n' "$legacy_symbols" >&2
      exit 1
    }
    for expected_symbol in \
      "_OBJC_CLASS_\$_IGraphicsView_vDessMetal_${api_suffix}" \
      "_OBJC_CLASS_\$_MNVGcontext_vDessMetal_${api_suffix}"; do
      grep -Fqx "$expected_symbol" <<< "$symbols" || {
        echo "Expected DessMetal Objective-C symbol is missing in $bundle ($architecture): $expected_symbol" >&2
        exit 1
      }
    done
    if [[ "$api_suffix" == "au" ]]; then
      for expected_symbol in \
        '_DessMetal_Entry' \
        '_DessMetal_Factory' \
        '_OBJC_CLASS_$_DessMetal_View'; do
        grep -Fqx "$expected_symbol" <<< "$symbols" || {
          echo "Expected DessMetal AU symbol is missing in $bundle ($architecture): $expected_symbol" >&2
          exit 1
        }
      done
    fi
  done

  if [[ "$api_suffix" == "au" ]]; then
    [[ "$(plutil -extract AudioComponents.0.factoryFunction raw "$bundle/Contents/Info.plist")" == \
      "DessMetal_Factory" ]] || { echo "Unexpected AU factory function in $bundle" >&2; exit 1; }
    [[ "$(plutil -extract NSPrincipalClass raw "$bundle/Contents/Info.plist")" == "DessMetal_View" ]] || {
      echo "Unexpected AU principal class in $bundle" >&2
      exit 1
    }
  fi
}

verify_developer_application_signature()
{
  local bundle="$1"
  local metadata
  codesign --verify --strict --verbose=2 "$bundle"
  metadata="$(codesign -dvvv "$bundle" 2>&1)"
  local authorities
  authorities="$(sed -n 's/^Authority=//p' <<< "$metadata")"
  [[ "$authorities" == "$app_identity"$'\nDeveloper ID Certification Authority\nApple Root CA' ]] || {
    echo "Unexpected Developer ID Application authority: $bundle" >&2
    exit 1
  }
  grep -Fqx "TeamIdentifier=$team_id" <<< "$metadata" || {
    echo "Unexpected signing team: $bundle" >&2
    exit 1
  }
  grep -Eq '^CodeDirectory .*flags=.*\(runtime\)' <<< "$metadata" || {
    echo "Hardened runtime flag is missing: $bundle" >&2
    exit 1
  }
  grep -Eq '^Timestamp=.+' <<< "$metadata" || {
    echo "Trusted code-signing timestamp is missing: $bundle" >&2
    exit 1
  }
}

verify_developer_disk_image_signature()
{
  local disk_image="$1"
  local metadata
  codesign --verify --strict --verbose=2 "$disk_image"
  metadata="$(codesign -dvvv "$disk_image" 2>&1)"
  local authorities
  authorities="$(sed -n 's/^Authority=//p' <<< "$metadata")"
  [[ "$authorities" == "$app_identity"$'\nDeveloper ID Certification Authority\nApple Root CA' ]] || {
    echo "Unexpected Developer ID Application authority on DMG" >&2
    exit 1
  }
  grep -Fqx "TeamIdentifier=$team_id" <<< "$metadata" || {
    echo "Unexpected signing team on DMG" >&2
    exit 1
  }
  grep -Eq '^Timestamp=.+' <<< "$metadata" || {
    echo "Trusted code-signing timestamp is missing from DMG" >&2
    exit 1
  }
}

verify_developer_installer_signature()
{
  local installer="$1"
  local signature
  signature="$(pkgutil --check-signature "$installer" 2>&1)"
  grep -Fq 'Status: signed by a developer certificate issued by Apple for distribution' <<< "$signature" || {
    echo "Installer does not have a trusted Apple distribution signature" >&2
    printf '%s\n' "$signature" >&2
    exit 1
  }
  grep -Fq 'Signed with a trusted timestamp on:' <<< "$signature" || {
    echo "Installer trusted timestamp is missing" >&2
    exit 1
  }
  local authorities
  authorities="$(sed -n 's/^ *[123]\. //p' <<< "$signature")"
  [[ "$authorities" == "$installer_identity"$'\nDeveloper ID Certification Authority\nApple Root CA' ]] || {
    echo "Unexpected Developer ID Installer certificate chain" >&2
    exit 1
  }
}

uuid_set()
{
  local binary="$1"
  local uuids
  uuids="$(dwarfdump --uuid "$binary" | awk '/^UUID: / { print toupper($2) " " $3 }' | LC_ALL=C sort)"
  [[ "$(wc -l <<< "$uuids" | tr -d ' ')" == "2" ]] || {
    echo "Expected exactly two UUIDs: $binary" >&2
    exit 1
  }
  grep -Eq '^[0-9A-F-]{36} \(arm64\)$' <<< "$uuids" || {
    echo "arm64 UUID is missing: $binary" >&2
    exit 1
  }
  grep -Eq '^[0-9A-F-]{36} \(x86_64\)$' <<< "$uuids" || {
    echo "x86_64 UUID is missing: $binary" >&2
    exit 1
  }
  printf '%s\n' "$uuids"
}

verify_dsym_matches_binary()
{
  local binary="$1"
  local dsym="$2"
  local debug_binary="$dsym/Contents/Resources/DWARF/DessMetal"
  require_file "$debug_binary"
  [[ "$(uuid_set "$binary")" == "$(uuid_set "$debug_binary")" ]] || {
    echo "dSYM UUIDs do not match the release binary: $binary" >&2
    exit 1
  }
}

compare_payload_bundle()
{
  local label="$1"
  local staged="$2"
  local payload="$3"
  local staged_manifest="$release_root/${label}-staged-files.txt"
  local payload_manifest="$release_root/${label}-payload-files.txt"
  (cd "$staged" && find . \( -type f -o -type l \) ! -name '._*' -print | LC_ALL=C sort) \
    > "$staged_manifest"
  (cd "$payload" && find . \( -type f -o -type l \) ! -name '._*' -print | LC_ALL=C sort) \
    > "$payload_manifest"
  cmp "$staged_manifest" "$payload_manifest" || {
    echo "Installer payload file list differs from staged $label bundle" >&2
    exit 1
  }
  while IFS= read -r entry; do
    if [[ -L "$staged/$entry" ]]; then
      [[ -L "$payload/$entry" && "$(readlink "$staged/$entry")" == "$(readlink "$payload/$entry")" ]] || {
        echo "Installer payload symlink differs from staged $label bundle: $entry" >&2
        exit 1
      }
    else
      [[ ! -L "$payload/$entry" ]] && cmp "$staged/$entry" "$payload/$entry" || {
        echo "Installer payload bytes differ from staged $label bundle: $entry" >&2
        exit 1
      }
    fi
  done < "$staged_manifest"
}

prepare_release_build_root "$repo_root" "$project_root" "$project_root/build-mac" "$release_root"
verify_release_cleanup_path "$repo_root" "$project_root" "$project_root/build-mac" "$release_root"
/bin/rm -rf -- "$release_root"
mkdir -p "$products_root/Applications" "$products_root/Components" "$products_root/VST3" \
  "$packages_root" "$installer_root"

if [[ "$mode" == "signed" ]]; then
  [[ "$app_identity" =~ ^Developer\ ID\ Application:\ .+\ \(([A-Z0-9]{10})\)$ ]] || {
    echo "DESSMETAL_APP_IDENTITY must name a Developer ID Application identity" >&2
    exit 1
  }
  app_team="${BASH_REMATCH[1]}"
  [[ "$installer_identity" =~ ^Developer\ ID\ Installer:\ .+\ \(([A-Z0-9]{10})\)$ ]] || {
    echo "DESSMETAL_INSTALLER_IDENTITY must name a Developer ID Installer identity" >&2
    exit 1
  }
  installer_team="${BASH_REMATCH[1]}"
  [[ "$app_team" == "$team_id" && "$installer_team" == "$team_id" ]] || {
    echo "Configured identities must both belong to Apple team $team_id" >&2
    exit 1
  }
  security find-identity -v -p codesigning | grep -Fq "\"$app_identity\"" || {
    echo "Developer ID Application identity is unavailable: $app_identity" >&2
    exit 1
  }
  security find-identity -v | grep -Fq "\"$installer_identity\"" || {
    echo "Developer ID Installer identity is unavailable: $installer_identity" >&2
    exit 1
  }
fi

echo "Building DessMetal $version (APP, AU, VST3; arm64 + x86_64)"
xcodebuild \
  -project "$project_file" \
  -target APP -target AU -target VST3 \
  -configuration Release \
  ARCHS="arm64 x86_64" \
  ONLY_ACTIVE_ARCH=NO \
  CODE_SIGNING_ALLOWED=NO \
  APP_PATH="$products_root/Applications" \
  AU_PATH="$products_root/Components" \
  VST3_PATH="$products_root/VST3" \
  SYMROOT="$release_root/xcode" \
  OBJROOT="$release_root/objects"

require_file "$app/Contents/MacOS/DessMetal"
require_file "$au/Contents/MacOS/DessMetal"
require_file "$vst3/Contents/MacOS/DessMetal"
require_universal_binary "$app/Contents/MacOS/DessMetal"
require_universal_binary "$au/Contents/MacOS/DessMetal"
require_universal_binary "$vst3/Contents/MacOS/DessMetal"
[[ "$(plutil -extract CFBundlePackageType raw "$app/Contents/Info.plist")" == "APPL" ]] || {
  echo "Standalone bundle must declare CFBundlePackageType=APPL" >&2
  exit 1
}
[[ "$(cat "$app/Contents/PkgInfo")" == "APPL1YEo" ]] || {
  echo "Standalone bundle must contain PkgInfo APPL1YEo" >&2
  exit 1
}
verify_resource_manifest "$app"
verify_resource_manifest "$au"
verify_resource_manifest "$vst3"
verify_unique_objc_namespace "$app"
verify_unique_objc_namespace "$au"
verify_unique_objc_namespace "$vst3"

if [[ "$mode" == "signed" ]]; then
  echo "Signing release bundles with Developer ID"
  codesign --force --options runtime --timestamp --sign "$app_identity" "$au"
  codesign --force --options runtime --timestamp --sign "$app_identity" "$vst3"
  codesign --force --options runtime --timestamp \
    --entitlements "$project_root/resources/DessMetal-macOS-release.entitlements" \
    --sign "$app_identity" "$app"
else
  echo "Applying ad-hoc signatures for packaging validation only"
  codesign --force --deep --sign - "$au"
  codesign --force --deep --sign - "$vst3"
  codesign --force --deep --sign - "$app"
fi

for bundle in "$app" "$au" "$vst3"; do
  if [[ "$mode" == "signed" ]]; then
    verify_developer_application_signature "$bundle"
  else
    codesign --verify --strict --verbose=2 "$bundle"
  fi
done

app_component_plist="$release_root/DessMetal_APP-components.plist"
au_component_plist="$release_root/DessMetal_AU-components.plist"
vst3_component_plist="$release_root/DessMetal_VST3-components.plist"
prepare_nonrelocatable_component_plist \
  "$products_root/Applications" DessMetal.app "$app_component_plist"
prepare_nonrelocatable_component_plist \
  "$products_root/Components" DessMetal.component "$au_component_plist"
prepare_nonrelocatable_component_plist \
  "$products_root/VST3" DessMetal.vst3 "$vst3_component_plist"

pkgbuild --root "$products_root/Applications" --install-location /Applications \
  --component-plist "$app_component_plist" \
  --identifier com.AlexanderDess.pkg.DessMetal.app --version "$version" \
  "$packages_root/DessMetal_APP.pkg"
pkgbuild --root "$products_root/Components" --install-location /Library/Audio/Plug-Ins/Components \
  --component-plist "$au_component_plist" \
  --identifier com.AlexanderDess.pkg.DessMetal.au --version "$version" \
  "$packages_root/DessMetal_AU.pkg"
pkgbuild --root "$products_root/VST3" --install-location /Library/Audio/Plug-Ins/VST3 \
  --component-plist "$vst3_component_plist" \
  --identifier com.AlexanderDess.pkg.DessMetal.vst3 --version "$version" \
  "$packages_root/DessMetal_VST3.pkg"

distribution="$release_root/distribution.xml"
sed "s/@VERSION@/$version/g" "$project_root/installer/distribution.xml.in" > "$distribution"
productbuild_args=(
  --distribution "$distribution"
  --resources "$project_root/installer"
  --package-path "$packages_root"
)
if [[ "$mode" == "signed" ]]; then
  productbuild_args+=(--sign "$installer_identity" --timestamp)
fi
productbuild "${productbuild_args[@]}" "$pkg"

if [[ "$mode" == "signed" ]]; then
  verify_developer_installer_signature "$pkg"
fi

expanded_installer="$release_root/expanded-installer"
payload_root="$release_root/verified-payloads"
pkgutil --expand "$pkg" "$expanded_installer"
require_file "$expanded_installer/Distribution"
verify_component_install_policy \
  "$expanded_installer/DessMetal_APP.pkg/PackageInfo" \
  /Applications com.AlexanderDess.app.DessMetal DessMetal.app
verify_component_install_policy \
  "$expanded_installer/DessMetal_AU.pkg/PackageInfo" \
  /Library/Audio/Plug-Ins/Components com.AlexanderDess.audiounit.DessMetal DessMetal.component
verify_component_install_policy \
  "$expanded_installer/DessMetal_VST3.pkg/PackageInfo" \
  /Library/Audio/Plug-Ins/VST3 com.AlexanderDess.vst3.DessMetal DessMetal.vst3
mkdir -p "$payload_root/app" "$payload_root/au" "$payload_root/vst3"
gzip -dc "$expanded_installer/DessMetal_APP.pkg/Payload" | \
  (cd "$payload_root/app" && cpio -idm --quiet)
gzip -dc "$expanded_installer/DessMetal_AU.pkg/Payload" | \
  (cd "$payload_root/au" && cpio -idm --quiet)
gzip -dc "$expanded_installer/DessMetal_VST3.pkg/Payload" | \
  (cd "$payload_root/vst3" && cpio -idm --quiet)
compare_payload_bundle app "$app" "$payload_root/app/DessMetal.app"
compare_payload_bundle au "$au" "$payload_root/au/DessMetal.component"
compare_payload_bundle vst3 "$vst3" "$payload_root/vst3/DessMetal.vst3"

dmg_source="$release_root/dmg"
mkdir -p "$dmg_source"
ditto "$pkg" "$dmg_source/$(basename "$pkg")"
ditto "$repo_root/LICENSE" "$dmg_source/LICENSE.txt"
ditto "$repo_root/THIRD_PARTY_NOTICES.md" "$dmg_source/THIRD_PARTY_NOTICES.md"
ditto "$repo_root/RELEASE.md" "$dmg_source/RELEASE.md"
licenses_dir="$dmg_source/THIRD_PARTY_LICENSES"
mkdir -p "$licenses_dir"
ditto "$repo_root/NeuralAmpModelerCore/LICENSE" "$licenses_dir/NeuralAmpModelerCore-MIT.txt"
ditto "$repo_root/AudioDSPTools/LICENSE" "$licenses_dir/AudioDSPTools-MIT.txt"
ditto "$repo_root/iPlug2/LICENSE.txt" "$licenses_dir/iPlug2.txt"
ditto "$repo_root/NeuralAmpModelerCore/Dependencies/eigen/COPYING.MPL2" "$licenses_dir/Eigen-MPL-2.0.txt"
ditto "$repo_root/NeuralAmpModelerCore/Dependencies/eigen/COPYING.BSD" "$licenses_dir/Eigen-BSD.txt"
ditto "$repo_root/NeuralAmpModelerCore/Dependencies/eigen/COPYING.APACHE" "$licenses_dir/Eigen-and-Roboto-Apache-2.0.txt"
ditto "$repo_root/NeuralAmpModelerCore/Dependencies/eigen/COPYING.MINPACK" "$licenses_dir/Eigen-Minpack.txt"
ditto "$repo_root/iPlug2/Dependencies/IPlug/VST3_SDK/LICENSE.txt" "$licenses_dir/VST3-SDK-MIT.txt"
ditto "$repo_root/iPlug2/Dependencies/IGraphics/NanoVG/LICENSE.txt" "$licenses_dir/NanoVG-zlib.txt"
ditto "$repo_root/iPlug2/Dependencies/IGraphics/NanoSVG/LICENSE.txt" "$licenses_dir/NanoSVG-zlib.txt"
ditto "$repo_root/iPlug2/Dependencies/IGraphics/MetalNanoVG/LICENSE" "$licenses_dir/MetalNanoVG-MIT.txt"
ditto "$repo_root/iPlug2/Dependencies/IGraphics/yoga/LICENSE" "$licenses_dir/Yoga-MIT.txt"
ditto "$repo_root/iPlug2/Dependencies/IGraphics/NanoVG/example/LICENSE_OFL.txt" "$licenses_dir/Michroma-OFL-1.1.txt"
ditto "$repo_root/iPlug2/Dependencies/IPlug/RTAudio/doc/doxygen/license.txt" "$licenses_dir/RtAudio-MIT.txt"
ditto "$repo_root/licenses/RtMidi-MIT.txt" "$licenses_dir/RtMidi-MIT.txt"
ditto "$repo_root/licenses/nlohmann-json-MIT.txt" "$licenses_dir/nlohmann-json-MIT.txt"
ditto "$repo_root/licenses/stb-MIT.txt" "$licenses_dir/stb-MIT.txt"
ditto "$repo_root/licenses/terrarium-poly-octave-MIT.txt" "$licenses_dir/terrarium-poly-octave-MIT.txt"

git_status="clean"
if [[ -n "$(git -C "$repo_root" status --porcelain --untracked-files=normal)" ]]; then
  git_status="dirty"
fi
{
  echo "DessMetal $version"
  echo "Git commit: $source_commit"
  echo "Source archive SHA-256: $source_hash"
  echo "Working tree: $git_status"
  echo "Xcode: $(xcodebuild -version | paste -sd ' ' -)"
  echo "Built UTC: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "Architectures: arm64 x86_64"
  echo "Signing mode: $mode"
  echo "Notarization status at build time: not submitted"
} > "$dmg_source/BUILD-INFO.txt"

hdiutil create -quiet -format UDZO -srcfolder "$dmg_source" -volname "DessMetal $version" "$dmg_work"
detach_disk_image_mapping "$dmg_work"
if [[ "$mode" == "signed" ]]; then
  codesign --force --timestamp --sign "$app_identity" "$dmg_work"
else
  codesign --force --sign - "$dmg_work"
fi
if [[ "$mode" == "signed" ]]; then
  verify_developer_disk_image_signature "$dmg_work"
else
  codesign --verify --strict --verbose=2 "$dmg_work"
fi
hdiutil verify "$dmg_work"

dmg_mount="$release_root/verified-dmg"
mkdir -p "$dmg_mount"
hdiutil attach -readonly -nobrowse -mountpoint "$dmg_mount" "$dmg_work" >/dev/null
cleanup_dmg_mount()
{
  hdiutil detach "$dmg_mount" >/dev/null 2>&1 || true
}
trap cleanup_dmg_mount EXIT
cmp "$pkg" "$dmg_mount/$(basename "$pkg")" || {
  echo "DMG does not contain the exact verified installer bytes" >&2
  exit 1
}
expected_dmg_top="$(printf '%s\n' \
  BUILD-INFO.txt \
  "$(basename "$pkg")" \
  LICENSE.txt \
  RELEASE.md \
  THIRD_PARTY_LICENSES \
  THIRD_PARTY_NOTICES.md | LC_ALL=C sort)"
actual_dmg_top="$(cd "$dmg_mount" && find . -mindepth 1 -maxdepth 1 -print | sed 's|^./||' | LC_ALL=C sort)"
[[ "$actual_dmg_top" == "$expected_dmg_top" ]] || {
  echo "Unexpected DMG top-level contents:" >&2
  printf '%s\n' "$actual_dmg_top" >&2
  exit 1
}
cleanup_dmg_mount
trap - EXIT

dsym_source="$release_root/dSYMs"
mkdir -p "$dsym_source"
app_dsym="$release_root/xcode/DessMetal.app.dSYM"
au_dsym="$release_root/xcode/DessMetal.component.dSYM"
vst3_dsym="$release_root/xcode/DessMetal.vst3.dSYM"
verify_dsym_matches_binary "$app/Contents/MacOS/DessMetal" "$app_dsym"
verify_dsym_matches_binary "$au/Contents/MacOS/DessMetal" "$au_dsym"
verify_dsym_matches_binary "$vst3/Contents/MacOS/DessMetal" "$vst3_dsym"
ditto "$app_dsym" "$dsym_source/$(basename "$app_dsym")"
ditto "$au_dsym" "$dsym_source/$(basename "$au_dsym")"
ditto "$vst3_dsym" "$dsym_source/$(basename "$vst3_dsym")"
(cd "$release_root" && /usr/bin/zip -qry -X "$(basename "$dsym_work")" "$(basename "$dsym_source")")

# Build and verify the complete six-file output as a sibling candidate before
# replacing the visible output directory. Existing artifacts are retained in a
# timestamped, recoverable directory so signed and unsigned generations cannot mix.
candidate_root="$release_root/out-candidate"
candidate_pkg="$candidate_root/$(basename "$pkg_out")"
candidate_dmg="$candidate_root/$(basename "$dmg")"
candidate_dsym="$candidate_root/$(basename "$dsym_zip")"
candidate_manifest="$candidate_root/$(basename "$set_manifest")"
mkdir -p "$candidate_root"
ditto "$pkg" "$candidate_pkg"
ditto "$dmg_work" "$candidate_dmg"
ditto "$dsym_work" "$candidate_dsym"
(cd "$candidate_root" && shasum -a 256 "$(basename "$candidate_pkg")") > "$candidate_pkg.sha256"
(cd "$candidate_root" && shasum -a 256 "$(basename "$candidate_dmg")") > "$candidate_dmg.sha256"
(cd "$candidate_root" && shasum -a 256 \
  "$(basename "$candidate_pkg")" "$(basename "$candidate_dmg")" "$(basename "$candidate_dsym")") \
  > "$candidate_manifest"
expected_output="$(printf '%s\n' \
  "$(basename "$candidate_pkg")" \
  "$(basename "$candidate_pkg").sha256" \
  "$(basename "$candidate_dmg")" \
  "$(basename "$candidate_dmg").sha256" \
  "$(basename "$candidate_dsym")" \
  "$(basename "$candidate_manifest")" | LC_ALL=C sort)"
actual_output="$(find "$candidate_root" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort)"
[[ "$actual_output" == "$expected_output" ]] || {
  echo "Candidate output is not the exact six-file release set" >&2
  printf '%s\n' "$actual_output" >&2
  exit 1
}
(cd "$candidate_root" && shasum -a 256 -c \
  "$(basename "$candidate_pkg").sha256" \
  "$(basename "$candidate_dmg").sha256" \
  "$(basename "$candidate_manifest")")

if [[ "$mode" == "signed" ]]; then
  verify_source_identity "$repo_root" "$source_commit" "$source_hash"
fi

rotate_release_output \
  "$candidate_root" "$out_root" "$project_root/build-mac" \
  "$repo_root" "$output_archive_root"

if [[ -n "$promoted_backup_path" ]]; then
  echo "Previous output preserved at: $promoted_backup_path"
fi
if [[ -n "$archived_output_batch" ]]; then
  echo "Older output sets archived outside the checkout at: $archived_output_batch"
fi
echo "Release artifacts:"
echo "  $pkg_out"
echo "  $pkg_out.sha256"
echo "  $dmg"
echo "  $dmg.sha256"
echo "  $dsym_zip"
echo "  $set_manifest"
if [[ "$mode" == "signed" ]]; then
  echo "Notarization was intentionally skipped. Gatekeeper distribution is not final until Apple notarization is approved."
else
  echo "Unsigned validation mode: these artifacts are not shareable release files."
fi
