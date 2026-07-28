#!/bin/bash

set -euo pipefail

# iPlug2, Eigen, and AudioDSPTools are regular tracked directories in this
# repository. This helper downloads only iPlug2's optional external SDKs and
# prebuilt libraries; it does not initialize repository submodules.
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

test -d "$repo_root/iPlug2/Dependencies/IPlug"
test -x "$repo_root/iPlug2/Dependencies/IPlug/download-iplug-sdks.sh"
test -x "$repo_root/iPlug2/Dependencies/download-prebuilt-libs.sh"

echo "Downloading iPlug2 SDKs..."
(
  cd "$repo_root/iPlug2/Dependencies/IPlug"
  ./download-iplug-sdks.sh
)

echo "Downloading iPlug2 prebuilt libs..."
(
  cd "$repo_root/iPlug2/Dependencies"
  ./download-prebuilt-libs.sh
)
