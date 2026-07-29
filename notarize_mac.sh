#!/usr/bin/env bash
set -Eeuo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$repo_root/NeuralAmpModeler"
build_root="$project_root/build-mac"
out_root="$build_root/out"
output_archive_root="$(cd "$repo_root/.." && pwd -P)/dessmetal-release-output-archive"
version="$(awk -F '"' '/^#define PLUG_VERSION_STR / { print $2; exit }' "$project_root/config.h")"

app_identity="${DESSMETAL_APP_IDENTITY:-Developer ID Application: Alexander Dess (6756YXU4J7)}"
installer_identity="${DESSMETAL_INSTALLER_IDENTITY:-Developer ID Installer: Alexander Dess (6756YXU4J7)}"
team_id="6756YXU4J7"

pkg_name="DessMetal-v${version}-mac.pkg"
dmg_name="DessMetal-v${version}-mac.dmg"
dsym_name="DessMetal-v${version}-mac-dSYMs.zip"
manifest_name="DessMetal-v${version}-mac-ARTIFACTS.txt"
pkg="$out_root/$pkg_name"
dmg="$out_root/$dmg_name"
dsym_zip="$out_root/$dsym_name"

transaction_root=""
candidate_root=""
active_mount=""
self_test_root=""
promoted_backup_path=""
archived_output_batch=""
rotation_out_root=""
rotation_backup=""
rotation_archive_batch=""
rotation_candidate_root=""
rotation_build_root=""

cleanup_mount()
{
  if [[ -n "$active_mount" && -d "$active_mount" ]]; then
    hdiutil detach "$active_mount" >/dev/null 2>&1 || true
  fi
  active_mount=""
}

cleanup_transaction()
{
  cleanup_mount
  if [[ -n "$transaction_root" && -d "$transaction_root" ]]; then
    case "$transaction_root" in
      "$build_root"/.DessMetal-notarization-work.*)
        /bin/rm -rf -- "$transaction_root"
        ;;
      *)
        echo "Refusing to clean unexpected notarization work path: $transaction_root" >&2
        ;;
    esac
  fi
  if [[ -n "$candidate_root" && -d "$candidate_root" ]]; then
    case "$candidate_root" in
      "$build_root"/.DessMetal-notarized-out-candidate.*)
        /bin/rm -rf -- "$candidate_root"
        ;;
      *)
        echo "Refusing to clean unexpected notarized candidate path: $candidate_root" >&2
        ;;
    esac
  fi
  if [[ -n "$self_test_root" && -d "$self_test_root" ]]; then
    case "$self_test_root" in
      "${TMPDIR:-/tmp}"/dessmetal-notary-transaction.*)
        /bin/rm -rf -- "$self_test_root"
        ;;
      *)
        echo "Refusing to clean unexpected notarization self-test path: $self_test_root" >&2
        ;;
    esac
  fi
}
trap cleanup_transaction EXIT

verify_notarization_paths()
{
  local checkout_root="$1"
  local expected_project_root="$2"
  local expected_build_root="$3"
  local expected_out_root="$4"
  local path checkout_real project_real build_real out_real

  [[ "$expected_project_root" == "$checkout_root/NeuralAmpModeler"
     && "$expected_build_root" == "$expected_project_root/build-mac"
     && "$expected_out_root" == "$expected_build_root/out" ]] || {
    echo "Refusing unexpected notarization paths" >&2
    return 1
  }
  for path in "$checkout_root" "$expected_project_root" "$expected_build_root" "$expected_out_root"; do
    [[ ! -L "$path" ]] || {
      echo "Refusing notarization through a symlinked path: $path" >&2
      return 1
    }
  done
  [[ -d "$checkout_root" && -d "$expected_project_root" && -d "$expected_build_root"
     && -d "$expected_out_root" ]] || {
    echo "Notarization path is missing or is not a directory" >&2
    return 1
  }
  checkout_real="$(cd "$checkout_root" && pwd -P)"
  project_real="$(cd "$expected_project_root" && pwd -P)"
  build_real="$(cd "$expected_build_root" && pwd -P)"
  out_real="$(cd "$expected_out_root" && pwd -P)"
  [[ "$project_real" == "$checkout_real/NeuralAmpModeler"
     && "$build_real" == "$project_real/build-mac"
     && "$out_real" == "$build_real/out" ]] || {
    echo "Resolved notarization path escapes the checkout" >&2
    return 1
  }
}

verify_identity_configuration()
{
  local app_team installer_team
  [[ "$app_identity" =~ ^Developer\ ID\ Application:\ .+\ \(([A-Z0-9]{10})\)$ ]] || {
    echo "DESSMETAL_APP_IDENTITY must name a Developer ID Application identity" >&2
    return 1
  }
  app_team="${BASH_REMATCH[1]}"
  [[ "$installer_identity" =~ ^Developer\ ID\ Installer:\ .+\ \(([A-Z0-9]{10})\)$ ]] || {
    echo "DESSMETAL_INSTALLER_IDENTITY must name a Developer ID Installer identity" >&2
    return 1
  }
  installer_team="${BASH_REMATCH[1]}"
  [[ "$app_team" == "$team_id" && "$installer_team" == "$team_id" ]] || {
    echo "Configured identities must both belong to Apple team $team_id" >&2
    return 1
  }
}

require_universal_binary()
{
  local binary="$1"
  local architectures
  [[ -f "$binary" && ! -L "$binary" ]] || {
    echo "Missing or symlinked release executable: $binary" >&2
    return 1
  }
  architectures="$(lipo -archs "$binary")" || return 1
  [[ " $architectures " == *" arm64 "* && " $architectures " == *" x86_64 "* ]] || {
    echo "Expected arm64+x86_64 binary, got '$architectures': $binary" >&2
    return 1
  }
}

verify_developer_application_code()
{
  local code="$1"
  local metadata authorities
  codesign --verify --strict --verbose=2 "$code" || return 1
  metadata="$(codesign -dvvv "$code" 2>&1)" || return 1
  authorities="$(sed -n 's/^Authority=//p' <<< "$metadata")"
  [[ "$authorities" == "$app_identity"$'\nDeveloper ID Certification Authority\nApple Root CA' ]] || {
    echo "Unexpected Developer ID Application authority: $code" >&2
    return 1
  }
  grep -Fqx "TeamIdentifier=$team_id" <<< "$metadata" || {
    echo "Unexpected signing team: $code" >&2
    return 1
  }
  grep -Eq '^CodeDirectory .*flags=.*\(runtime\)' <<< "$metadata" || {
    echo "Hardened runtime flag is missing: $code" >&2
    return 1
  }
  grep -Eq '^Timestamp=.+' <<< "$metadata" || {
    echo "Trusted code-signing timestamp is missing: $code" >&2
    return 1
  }
}

verify_developer_bundle()
{
  local bundle="$1"
  local binary="$bundle/Contents/MacOS/DessMetal"
  [[ -d "$bundle" && ! -L "$bundle" ]] || {
    echo "Missing or symlinked release bundle: $bundle" >&2
    return 1
  }
  require_universal_binary "$binary" || return 1
  verify_developer_application_code "$bundle" || return 1
  # Reassert the runtime flag and identity on the actual executable, rather
  # than relying only on the enclosing bundle's signature display.
  verify_developer_application_code "$binary" || return 1
}

verify_developer_disk_image_signature()
{
  local disk_image="$1"
  local metadata authorities
  codesign --verify --strict --verbose=2 "$disk_image" || return 1
  metadata="$(codesign -dvvv "$disk_image" 2>&1)" || return 1
  authorities="$(sed -n 's/^Authority=//p' <<< "$metadata")"
  [[ "$authorities" == "$app_identity"$'\nDeveloper ID Certification Authority\nApple Root CA' ]] || {
    echo "Unexpected Developer ID Application authority on DMG: $disk_image" >&2
    return 1
  }
  grep -Fqx "TeamIdentifier=$team_id" <<< "$metadata" || {
    echo "Unexpected signing team on DMG: $disk_image" >&2
    return 1
  }
  grep -Eq '^Timestamp=.+' <<< "$metadata" || {
    echo "Trusted code-signing timestamp is missing from DMG: $disk_image" >&2
    return 1
  }
}

verify_developer_installer_signature()
{
  local installer="$1"
  local signature authorities
  signature="$(pkgutil --check-signature "$installer" 2>&1)" || return 1
  grep -Fq 'Status: signed by a developer certificate issued by Apple for distribution' <<< "$signature" || {
    echo "Installer does not have a trusted Apple distribution signature: $installer" >&2
    printf '%s\n' "$signature" >&2
    return 1
  }
  grep -Fq 'Signed with a trusted timestamp on:' <<< "$signature" || {
    echo "Installer trusted timestamp is missing: $installer" >&2
    return 1
  }
  authorities="$(sed -n 's/^ *[123]\. //p' <<< "$signature")"
  [[ "$authorities" == "$installer_identity"$'\nDeveloper ID Certification Authority\nApple Root CA' ]] || {
    echo "Unexpected Developer ID Installer certificate chain: $installer" >&2
    return 1
  }
}

verify_component_install_policy()
{
  local package_info="$1"
  local expected_location="$2"
  local expected_bundle_id="$3"
  local expected_bundle_path="$4"

  [[ -f "$package_info" ]] || {
    echo "Installer component is missing PackageInfo: $package_info" >&2
    return 1
  }
  [[ "$(xmllint --xpath 'string(/pkg-info/@install-location)' "$package_info")" == "$expected_location" ]] || {
    echo "Unexpected component install location: $package_info" >&2
    return 1
  }
  [[ "$(xmllint --xpath 'string(/pkg-info/@relocatable)' "$package_info")" == "false" ]] || {
    echo "Installer component does not explicitly disable relocation: $package_info" >&2
    return 1
  }
  [[ "$(xmllint --xpath 'count(/pkg-info/relocate/*)' "$package_info")" == "0" ]] || {
    echo "Installer component is relocatable and could install outside its fixed destination: $package_info" >&2
    return 1
  }
  [[ "$(xmllint --xpath 'count(/pkg-info/strict-identifier/bundle)' "$package_info")" == "1" ]] || {
    echo "Installer component must contain exactly one strict bundle identifier: $package_info" >&2
    return 1
  }
  [[ "$(xmllint --xpath "count(/pkg-info/strict-identifier/bundle[@id='$expected_bundle_id'])" "$package_info")" == "1" ]] || {
    echo "Installer component is missing strict bundle-identifier enforcement: $package_info" >&2
    return 1
  }
  [[ "$(xmllint --xpath "count(/pkg-info/bundle[@path='./$expected_bundle_path' and @id='$expected_bundle_id'])" "$package_info")" == "1" ]] || {
    echo "Installer component has an unexpected primary bundle path or identifier: $package_info" >&2
    return 1
  }
  [[ "$(xmllint --xpath 'count(/pkg-info/bundle)' "$package_info")" == "1" ]] || {
    echo "Installer component must describe exactly one primary bundle: $package_info" >&2
    return 1
  }
}

verify_installer_payloads()
{
  local installer="$1"
  local extraction_root="$2"
  local expanded="$extraction_root/expanded"
  local payloads="$extraction_root/payloads"
  local component payload_dir bundle expected_components actual_components actual_payload_top

  verify_developer_installer_signature "$installer" || return 1
  [[ ! -e "$extraction_root" ]] || {
    echo "Installer extraction path already exists: $extraction_root" >&2
    return 1
  }
  mkdir -p "$extraction_root" "$payloads" || return 1
  pkgutil --expand "$installer" "$expanded" || return 1
  [[ -f "$expanded/Distribution" ]] || {
    echo "Expanded installer is missing its Distribution file" >&2
    return 1
  }
  verify_component_install_policy \
    "$expanded/DessMetal_APP.pkg/PackageInfo" \
    /Applications com.AlexanderDess.app.DessMetal DessMetal.app || return 1
  verify_component_install_policy \
    "$expanded/DessMetal_AU.pkg/PackageInfo" \
    /Library/Audio/Plug-Ins/Components com.AlexanderDess.audiounit.DessMetal DessMetal.component || return 1
  verify_component_install_policy \
    "$expanded/DessMetal_VST3.pkg/PackageInfo" \
    /Library/Audio/Plug-Ins/VST3 com.AlexanderDess.vst3.DessMetal DessMetal.vst3 || return 1

  expected_components="$(printf '%s\n' DessMetal_APP.pkg DessMetal_AU.pkg DessMetal_VST3.pkg | LC_ALL=C sort)"
  actual_components="$(find "$expanded" -mindepth 1 -maxdepth 1 -type d -name '*.pkg' \
    -exec basename {} \; | LC_ALL=C sort)" || return 1
  [[ "$actual_components" == "$expected_components" ]] || {
    echo "Installer component package set differs from the expected APP/AU/VST3 set:" >&2
    printf '%s\n' "$actual_components" >&2
    return 1
  }

  for component in DessMetal_APP.pkg DessMetal_AU.pkg DessMetal_VST3.pkg; do
    [[ -f "$expanded/$component/Payload" ]] || {
      echo "Installer is missing component payload: $component" >&2
      return 1
    }
  done

  payload_dir="$payloads/app"
  mkdir -p "$payload_dir"
  gzip -dc "$expanded/DessMetal_APP.pkg/Payload" | (cd "$payload_dir" && cpio -idm --quiet) || return 1
  bundle="$payload_dir/DessMetal.app"
  actual_payload_top="$(find "$payload_dir" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort)" || return 1
  [[ "$actual_payload_top" == "DessMetal.app" ]] || {
    echo "Standalone component contains unexpected top-level payload entries" >&2
    return 1
  }
  verify_developer_bundle "$bundle" || return 1

  payload_dir="$payloads/au"
  mkdir -p "$payload_dir"
  gzip -dc "$expanded/DessMetal_AU.pkg/Payload" | (cd "$payload_dir" && cpio -idm --quiet) || return 1
  bundle="$payload_dir/DessMetal.component"
  actual_payload_top="$(find "$payload_dir" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort)" || return 1
  [[ "$actual_payload_top" == "DessMetal.component" ]] || {
    echo "Audio Unit component contains unexpected top-level payload entries" >&2
    return 1
  }
  verify_developer_bundle "$bundle" || return 1

  payload_dir="$payloads/vst3"
  mkdir -p "$payload_dir"
  gzip -dc "$expanded/DessMetal_VST3.pkg/Payload" | (cd "$payload_dir" && cpio -idm --quiet) || return 1
  bundle="$payload_dir/DessMetal.vst3"
  actual_payload_top="$(find "$payload_dir" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort)" || return 1
  [[ "$actual_payload_top" == "DessMetal.vst3" ]] || {
    echo "VST3 component contains unexpected top-level payload entries" >&2
    return 1
  }
  verify_developer_bundle "$bundle" || return 1
}

verify_checksum_record()
{
  local root="$1"
  local checksum_file="$2"
  local expected_name="$3"
  local line recorded_name
  [[ "$(wc -l < "$root/$checksum_file" | tr -d ' ')" == "1" ]] || {
    echo "Checksum file must contain exactly one record: $root/$checksum_file" >&2
    return 1
  }
  line="$(cat "$root/$checksum_file")"
  [[ "$line" =~ ^[[:xdigit:]]{64}[[:space:]]+\*?(.+)$ ]] || {
    echo "Malformed checksum record: $root/$checksum_file" >&2
    return 1
  }
  recorded_name="${BASH_REMATCH[1]}"
  [[ "$recorded_name" == "$expected_name" ]] || {
    echo "Checksum record names '$recorded_name', expected '$expected_name'" >&2
    return 1
  }
}

verify_six_file_set()
{
  local root="$1"
  local expected_files actual_files expected_manifest_names actual_manifest_names entry
  [[ -d "$root" && ! -L "$root" ]] || {
    echo "Release output is missing, not a directory, or symlinked: $root" >&2
    return 1
  }
  expected_files="$(printf '%s\n' \
    "$pkg_name" "$pkg_name.sha256" \
    "$dmg_name" "$dmg_name.sha256" \
    "$dsym_name" "$manifest_name" | LC_ALL=C sort)"
  actual_files="$(find "$root" -mindepth 1 -maxdepth 1 -exec basename {} \; | LC_ALL=C sort)" || return 1
  [[ "$actual_files" == "$expected_files" ]] || {
    echo "Release output must be the exact six-file set:" >&2
    printf '%s\n' "$actual_files" >&2
    return 1
  }
  while IFS= read -r entry; do
    [[ -f "$root/$entry" && ! -L "$root/$entry" ]] || {
      echo "Release entry must be a regular non-symlink file: $root/$entry" >&2
      return 1
    }
  done <<< "$expected_files"

  verify_checksum_record "$root" "$pkg_name.sha256" "$pkg_name" || return 1
  verify_checksum_record "$root" "$dmg_name.sha256" "$dmg_name" || return 1
  expected_manifest_names="$(printf '%s\n' "$pkg_name" "$dmg_name" "$dsym_name" | LC_ALL=C sort)"
  [[ "$(wc -l < "$root/$manifest_name" | tr -d ' ')" == "3" ]] || {
    echo "Artifact manifest must contain exactly three records" >&2
    return 1
  }
  actual_manifest_names="$(sed -E 's/^[[:xdigit:]]{64}[[:space:]]+[*]?//' "$root/$manifest_name" | LC_ALL=C sort)"
  [[ "$actual_manifest_names" == "$expected_manifest_names" ]] || {
    echo "Artifact manifest must contain exactly the pkg, DMG, and dSYM records" >&2
    return 1
  }
  (cd "$root" && shasum -a 256 -c "$pkg_name.sha256" "$dmg_name.sha256" "$manifest_name") || return 1
  unzip -tq "$root/$dsym_name" || return 1
}

verify_dmg_contents()
{
  local disk_image="$1"
  local installer="$2"
  local mountpoint="$3"
  local assess="${4:-no}"
  local expected_top actual_top nested_installer

  verify_developer_disk_image_signature "$disk_image" || return 1
  hdiutil verify "$disk_image" || return 1
  [[ ! -e "$mountpoint" ]] || {
    echo "DMG mount path already exists: $mountpoint" >&2
    return 1
  }
  mkdir -p "$mountpoint" || return 1
  if ! hdiutil attach -readonly -nobrowse -mountpoint "$mountpoint" "$disk_image" >/dev/null; then
    return 1
  fi
  active_mount="$mountpoint"
  nested_installer="$mountpoint/$pkg_name"
  if [[ ! -f "$nested_installer" || -L "$nested_installer" ]]; then
    echo "DMG is missing its exact nested installer name: $pkg_name" >&2
    cleanup_mount
    return 1
  fi
  if ! cmp "$installer" "$nested_installer"; then
    echo "DMG nested PKG bytes differ from the verified installer" >&2
    cleanup_mount
    return 1
  fi
  expected_top="$(printf '%s\n' \
    BUILD-INFO.txt "$pkg_name" LICENSE.txt RELEASE.md \
    THIRD_PARTY_LICENSES THIRD_PARTY_NOTICES.md | LC_ALL=C sort)"
  actual_top="$(cd "$mountpoint" && find . -mindepth 1 -maxdepth 1 -print | sed 's|^./||' | LC_ALL=C sort)" || {
    cleanup_mount
    return 1
  }
  if [[ "$actual_top" != "$expected_top" ]]; then
    echo "Unexpected DMG top-level contents:" >&2
    printf '%s\n' "$actual_top" >&2
    cleanup_mount
    return 1
  fi
  if [[ "$assess" == "yes" ]]; then
    if ! spctl --assess --type install --verbose=2 "$nested_installer"; then
      echo "Gatekeeper rejected the exact nested installer" >&2
      cleanup_mount
      return 1
    fi
  fi
  cleanup_mount
  if [[ "$assess" == "yes" ]]; then
    spctl --assess --type open --context context:primary-signature --verbose=2 "$disk_image" || return 1
  fi
}

reset_rotation_state()
{
  rotation_out_root=""
  rotation_backup=""
  rotation_archive_batch=""
  rotation_candidate_root=""
  rotation_build_root=""
}

rollback_output_rotation()
{
  trap - HUP INT TERM
  local archived_output rollback_failed
  rollback_failed=0
  if [[ -n "$rotation_candidate_root" && ! -e "$rotation_candidate_root" && -d "$rotation_out_root" ]]; then
    mv "$rotation_out_root" "$rotation_candidate_root" || {
      echo "CRITICAL: could not recover the notarized candidate: $rotation_out_root" >&2
      rollback_failed=1
    }
  fi
  if [[ -n "$rotation_backup" && ! -e "$rotation_out_root" && -d "$rotation_backup" ]]; then
    mv "$rotation_backup" "$rotation_out_root" || {
      echo "CRITICAL: could not restore the pre-notary output: $rotation_backup" >&2
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
  reset_rotation_state
  return "$rollback_failed"
}

restore_interrupted_output_rotation()
{
  rollback_output_rotation || true
  exit 130
}

finalize_output_rotation()
{
  trap - HUP INT TERM
  reset_rotation_state
}

rotate_release_output()
{
  local new_candidate="$1"
  local output_root="$2"
  local target_build_root="$3"
  local checkout_root="$4"
  local archive_root="$5"
  local failpoint="${6:-}"
  local checkout_real archive_real previous_output remaining_count archived_count
  local -a previous_outputs=()

  promoted_backup_path=""
  archived_output_batch=""
  [[ "$output_root" == "$target_build_root/out" && ! -L "$output_root" ]] || {
    echo "Refusing to replace an unexpected or symlinked output directory" >&2
    return 1
  }
  [[ -d "$new_candidate" && ! -L "$new_candidate" ]] || {
    echo "Notarized release candidate is missing or symlinked: $new_candidate" >&2
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
  done < <(find "$target_build_root" -mindepth 1 -maxdepth 1 -name 'superseded-out-*' -print0)

  if (( ${#previous_outputs[@]} > 0 )); then
    [[ "$archive_root" == /* && ! -L "$archive_root" ]] || {
      echo "Output archive must be an absolute, non-symlink path: $archive_root" >&2
      return 1
    }
    mkdir -p "$archive_root" || return 1
    checkout_real="$(cd "$checkout_root" && pwd -P)" || return 1
    archive_real="$(cd "$archive_root" && pwd -P)" || return 1
    case "$archive_real/" in
      "$checkout_real/"*)
        echo "Output archive must be outside the checkout: $archive_real" >&2
        return 1
        ;;
    esac
    archived_output_batch="$(mktemp -d \
      "$archive_root/$(basename "$checkout_real")-notarized-$(date -u '+%Y%m%d-%H%M%S')-XXXXXX")"
  fi

  if [[ -d "$output_root" ]]; then
    promoted_backup_path="$target_build_root/superseded-out-$(date -u '+%Y%m%d-%H%M%S')-$$"
    [[ ! -e "$promoted_backup_path" && ! -L "$promoted_backup_path" ]] || {
      echo "Output backup path already exists: $promoted_backup_path" >&2
      [[ -z "$archived_output_batch" ]] || rmdir "$archived_output_batch"
      return 1
    }
  fi

  rotation_out_root="$output_root"
  rotation_backup="$promoted_backup_path"
  rotation_archive_batch="$archived_output_batch"
  rotation_candidate_root="$new_candidate"
  rotation_build_root="$target_build_root"
  trap restore_interrupted_output_rotation HUP INT TERM

  if [[ -n "$promoted_backup_path" ]]; then
    if ! mv "$output_root" "$promoted_backup_path"; then
      trap - HUP INT TERM
      [[ -z "$archived_output_batch" ]] || rmdir "$archived_output_batch"
      reset_rotation_state
      return 1
    fi
  fi
  if [[ "$failpoint" == "after-backup" ]]; then
    echo "Injected notarization promotion failure after backup" >&2
    rollback_output_rotation || return 1
    promoted_backup_path=""
    archived_output_batch=""
    return 97
  fi
  if ! mv "$new_candidate" "$output_root"; then
    rollback_output_rotation || {
      echo "CRITICAL: output rollback failed after candidate promotion error" >&2
      return 1
    }
    promoted_backup_path=""
    archived_output_batch=""
    return 1
  fi
  if [[ "$failpoint" == "after-promotion" ]]; then
    echo "Injected notarization failure after candidate promotion" >&2
    rollback_output_rotation || return 1
    promoted_backup_path=""
    archived_output_batch=""
    return 98
  fi

  archived_count=0
  if (( ${#previous_outputs[@]} > 0 )); then
    for previous_output in "${previous_outputs[@]}"; do
      if ! mv "$previous_output" "$archived_output_batch/"; then
        echo "Older rollback archival failed; restoring the pre-notary output" >&2
        rollback_output_rotation || return 1
        return 1
      fi
      archived_count=$((archived_count + 1))
      if [[ "$failpoint" == "during-archive" && "$archived_count" == "1" ]]; then
        echo "Injected notarization retention failure during archival" >&2
        rollback_output_rotation || return 1
        promoted_backup_path=""
        archived_output_batch=""
        return 99
      fi
    done
  fi

  remaining_count="$(find "$target_build_root" -mindepth 1 -maxdepth 1 \
    -type d -name 'superseded-out-*' | wc -l | tr -d ' ')" || {
    rollback_output_rotation || true
    return 1
  }
  if [[ -n "$promoted_backup_path" ]]; then
    [[ "$remaining_count" == "1" && -d "$promoted_backup_path" ]] || {
      echo "Expected exactly one immediate pre-notary rollback directory" >&2
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
  # Keep the signal rollback armed until the caller verifies the renamed set
  # and explicitly finalizes the transaction.
}

make_self_test_set()
{
  local root="$1"
  local marker="$2"
  local zip_source="$root/.zip-source"
  mkdir -p "$root" "$zip_source"
  printf 'pkg-%s\n' "$marker" > "$root/$pkg_name"
  printf 'dmg-%s\n' "$marker" > "$root/$dmg_name"
  printf 'dsym-%s\n' "$marker" > "$zip_source/marker.txt"
  (cd "$zip_source" && /usr/bin/zip -q -X "$root/$dsym_name" marker.txt)
  /bin/rm -rf -- "$zip_source"
  (cd "$root" && shasum -a 256 "$pkg_name") > "$root/$pkg_name.sha256"
  (cd "$root" && shasum -a 256 "$dmg_name") > "$root/$dmg_name.sha256"
  (cd "$root" && shasum -a 256 "$pkg_name" "$dmg_name" "$dsym_name") > "$root/$manifest_name"
}

run_transaction_self_test()
{
  local fixture checkout fixture_build archive old_hash failure_status corrupt
  local path_checkout path_project path_build outside_build
  self_test_root="$(mktemp -d "${TMPDIR:-/tmp}/dessmetal-notary-transaction.XXXXXX")"
  fixture="$self_test_root/fixture"
  checkout="$fixture/dessmetal"
  fixture_build="$checkout/build-mac"
  archive="$fixture/archive"
  mkdir -p "$fixture_build"

  path_checkout="$fixture/path-checkout"
  path_project="$path_checkout/NeuralAmpModeler"
  path_build="$path_project/build-mac"
  outside_build="$fixture/outside-build"
  mkdir -p "$path_build/out" "$outside_build/out/sentinel"
  verify_notarization_paths "$path_checkout" "$path_project" "$path_build" "$path_build/out"
  find "$path_build" -depth -delete
  ln -s "$outside_build" "$path_build"
  if verify_notarization_paths "$path_checkout" "$path_project" "$path_build" "$path_build/out"; then
    echo "Symlinked notarization path unexpectedly passed validation" >&2
    return 1
  fi
  [[ -d "$outside_build/out/sentinel" ]]

  make_self_test_set "$fixture_build/out" old
  make_self_test_set "$fixture_build/candidate" new
  mkdir -p "$fixture_build/superseded-out-old-a/marker" "$fixture_build/superseded-out-old-b/marker"
  verify_six_file_set "$fixture_build/out"
  verify_six_file_set "$fixture_build/candidate"
  old_hash="$(shasum -a 256 "$fixture_build/out/$dmg_name" | awk '{print $1}')"

  corrupt="$fixture_build/corrupt"
  make_self_test_set "$corrupt" corrupt
  printf 'changed\n' >> "$corrupt/$dmg_name"
  if verify_six_file_set "$corrupt" >/dev/null 2>&1; then
    echo "Corrupt six-file fixture unexpectedly passed checksum validation" >&2
    return 1
  fi
  /bin/rm -rf -- "$corrupt"

  failure_status=0
  rotate_release_output "$fixture_build/candidate" "$fixture_build/out" "$fixture_build" \
    "$checkout" "$archive" after-backup || failure_status=$?
  [[ "$failure_status" == "97" ]]
  [[ "$(shasum -a 256 "$fixture_build/out/$dmg_name" | awk '{print $1}')" == "$old_hash" ]]
  verify_six_file_set "$fixture_build/candidate"

  failure_status=0
  rotate_release_output "$fixture_build/candidate" "$fixture_build/out" "$fixture_build" \
    "$checkout" "$archive" after-promotion || failure_status=$?
  [[ "$failure_status" == "98" ]]
  [[ "$(shasum -a 256 "$fixture_build/out/$dmg_name" | awk '{print $1}')" == "$old_hash" ]]
  verify_six_file_set "$fixture_build/candidate"

  failure_status=0
  rotate_release_output "$fixture_build/candidate" "$fixture_build/out" "$fixture_build" \
    "$checkout" "$archive" during-archive || failure_status=$?
  [[ "$failure_status" == "99" ]]
  [[ "$(shasum -a 256 "$fixture_build/out/$dmg_name" | awk '{print $1}')" == "$old_hash" ]]
  verify_six_file_set "$fixture_build/candidate"

  rotate_release_output "$fixture_build/candidate" "$fixture_build/out" "$fixture_build" \
    "$checkout" "$archive"
  verify_six_file_set "$fixture_build/out" || {
    rollback_output_rotation || true
    return 1
  }
  [[ "$(find "$fixture_build" -mindepth 1 -maxdepth 1 -type d \
    -name 'superseded-out-*' | wc -l | tr -d ' ')" == "1" ]]
  [[ -n "$archived_output_batch" && -d "$archived_output_batch/superseded-out-old-a/marker" ]]
  [[ -d "$archived_output_batch/superseded-out-old-b/marker" ]]
  finalize_output_rotation
  echo "Notarization transaction self-test passed"
}

if [[ "${DESSMETAL_NOTARY_TRANSACTION_SELF_TEST:-}" == "1" ]]; then
  run_transaction_self_test
  exit 0
fi

if [[ $# -ne 1 || ! "$1" =~ ^[[:xdigit:]]{64}$ ]]; then
  echo "Usage: DESSMETAL_NOTARY_PROFILE=<existing-profile> $0 <approved-dmg-sha256>" >&2
  echo "This command uploads an exact working copy of the approved signed DMG. It never rebuilds it." >&2
  exit 2
fi

: "${DESSMETAL_NOTARY_PROFILE:?Set DESSMETAL_NOTARY_PROFILE to an existing Keychain profile}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "Could not determine a semantic version from config.h" >&2
  exit 1
}
verify_identity_configuration
verify_notarization_paths "$repo_root" "$project_root" "$build_root" "$out_root"

# Fail before contacting Apple unless the visible output is one complete,
# internally consistent, exact six-file signed set.
verify_six_file_set "$out_root"
expected="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
actual="$(shasum -a 256 "$dmg" | awk '{print $1}')"
if [[ "$actual" != "$expected" ]]; then
  echo "Refusing upload: approved SHA-256 does not match the existing DMG." >&2
  echo "Expected: $expected" >&2
  echo "Actual:   $actual" >&2
  exit 1
fi

transaction_root="$(mktemp -d "$build_root/.DessMetal-notarization-work.XXXXXX")"
candidate_root="$(mktemp -d "$build_root/.DessMetal-notarized-out-candidate.XXXXXX")"
verify_installer_payloads "$pkg" "$transaction_root/preflight-pkg"
verify_dmg_contents "$dmg" "$pkg" "$transaction_root/preflight-mount" no

work_pkg="$transaction_root/$pkg_name"
work_dmg="$transaction_root/$dmg_name"
work_dsym="$transaction_root/$dsym_name"
ditto "$pkg" "$work_pkg"
ditto "$dmg" "$work_dmg"
ditto "$dsym_zip" "$work_dsym"
cmp "$pkg" "$work_pkg"
cmp "$dsym_zip" "$work_dsym"
work_hash="$(shasum -a 256 "$work_dmg" | awk '{print $1}')"
[[ "$work_hash" == "$expected" ]] || { echo "Working-copy hash mismatch" >&2; exit 1; }
verify_installer_payloads "$work_pkg" "$transaction_root/working-pkg"
verify_dmg_contents "$work_dmg" "$work_pkg" "$transaction_root/working-mount" no

if [[ "${DESSMETAL_NOTARY_PREFLIGHT_ONLY:-}" == "1" ]]; then
  echo "Notarization preflight passed without contacting Apple."
  exit 0
fi

echo "Uploading an exact working copy of $dmg"
echo "Approved SHA-256: $actual"
xcrun notarytool submit "$work_dmg" --keychain-profile "$DESSMETAL_NOTARY_PROFILE" --wait
xcrun stapler staple "$work_dmg"
xcrun stapler validate "$work_dmg"
verify_dmg_contents "$work_dmg" "$work_pkg" "$transaction_root/post-staple-mount" yes

# Assemble every post-staple release artifact as one sibling candidate. Nothing
# under out changes until this complete set and its nested contents pass again.
ditto "$work_pkg" "$candidate_root/$pkg_name"
ditto "$work_dmg" "$candidate_root/$dmg_name"
ditto "$work_dsym" "$candidate_root/$dsym_name"
(cd "$candidate_root" && shasum -a 256 "$pkg_name") > "$candidate_root/$pkg_name.sha256"
(cd "$candidate_root" && shasum -a 256 "$dmg_name") > "$candidate_root/$dmg_name.sha256"
(cd "$candidate_root" && shasum -a 256 "$pkg_name" "$dmg_name" "$dsym_name") \
  > "$candidate_root/$manifest_name"
verify_six_file_set "$candidate_root"
verify_installer_payloads "$candidate_root/$pkg_name" "$transaction_root/candidate-pkg"
xcrun stapler validate "$candidate_root/$dmg_name"
verify_dmg_contents "$candidate_root/$dmg_name" "$candidate_root/$pkg_name" \
  "$transaction_root/candidate-mount" yes

rotation_failpoint="${DESSMETAL_NOTARY_PROMOTION_FAILPOINT:-}"
rotate_release_output "$candidate_root" "$out_root" "$build_root" \
  "$repo_root" "$output_archive_root" "$rotation_failpoint"
if ! verify_six_file_set "$out_root" || ! xcrun stapler validate "$out_root/$dmg_name"; then
  echo "Promoted notarized set failed final verification; restoring pre-notary output" >&2
  rollback_output_rotation || {
    echo "CRITICAL: could not restore pre-notary output" >&2
    exit 1
  }
  exit 1
fi
finalize_output_rotation

echo "Notarization and stapling complete. Complete post-staple set promoted atomically."
echo "Post-staple checksums:"
cat "$out_root/$pkg_name.sha256"
cat "$out_root/$dmg_name.sha256"
if [[ -n "$promoted_backup_path" ]]; then
  echo "Pre-notary output preserved at: $promoted_backup_path"
fi
if [[ -n "$archived_output_batch" ]]; then
  echo "Older rollback sets archived outside the checkout at: $archived_output_batch"
fi
