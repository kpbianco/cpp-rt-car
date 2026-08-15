#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    printf 'usage: %s --engine-root PATH --sdk-root PATH --configuration Development|Shipping [--build-only]\n' "$0" >&2
}

ENGINE_ROOT=
SDK_ROOT=
CONFIGURATION=
BUILD_ONLY=0
while (($#)); do
    case "$1" in
        --engine-root)
            (($# >= 2)) || { usage; exit 2; }
            ENGINE_ROOT=$2
            shift 2
            ;;
        --sdk-root)
            (($# >= 2)) || { usage; exit 2; }
            SDK_ROOT=$2
            shift 2
            ;;
        --configuration)
            (($# >= 2)) || { usage; exit 2; }
            CONFIGURATION=$2
            shift 2
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

[[ -n "$ENGINE_ROOT" && -n "$SDK_ROOT" ]] || { usage; exit 2; }
[[ "$CONFIGURATION" == Development || "$CONFIGURATION" == Shipping ]] || {
    usage
    exit 2
}

REPOSITORY_ROOT=$(git rev-parse --show-toplevel)
ENGINE_ROOT=$(realpath "$ENGINE_ROOT")
SDK_ROOT=$(realpath "$SDK_ROOT")
EXPECTED_ENGINE_COMMIT=71fe36aac5a8df5ccd66c763ffc902b29b6a9c43
EXPECTED_ENGINE_TAG=5.8.1-release
TOOLCHAIN_NAME=v26_clang-20.1.8-rockylinux8
TOOLCHAIN_ROOT="$ENGINE_ROOT/Engine/Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64/$TOOLCHAIN_NAME/x86_64-unknown-linux-gnu"
BUILD_SCRIPT="$ENGINE_ROOT/Engine/Build/BatchFiles/Linux/Build.sh"
UAT_SCRIPT="$ENGINE_ROOT/Engine/Build/BatchFiles/RunUAT.sh"
EDITOR_COMMAND="$ENGINE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd"

[[ $(git -C "$ENGINE_ROOT" rev-parse HEAD) == "$EXPECTED_ENGINE_COMMIT" ]] || {
    printf 'engine commit is not the approved M19-02 commit\n' >&2
    exit 1
}
[[ $(git -C "$ENGINE_ROOT" describe --tags --exact-match HEAD) == "$EXPECTED_ENGINE_TAG" ]] || {
    printf 'engine tag is not the approved M19-02 tag\n' >&2
    exit 1
}
python3 - "$ENGINE_ROOT/Engine/Build/Build.version" <<'PY'
import json
import pathlib
import sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
actual = (data["MajorVersion"], data["MinorVersion"], data["PatchVersion"])
if actual != (5, 8, 1):
    raise SystemExit(f"unexpected Unreal version {actual}")
PY
[[ -x "$BUILD_SCRIPT" && -x "$UAT_SCRIPT" ]] || {
    printf 'approved UBT/UAT command path is unavailable\n' >&2
    exit 1
}
[[ -x "$TOOLCHAIN_ROOT/bin/clang++" && -x "$TOOLCHAIN_ROOT/bin/ld.lld" ]] || {
    printf 'approved bundled v26 Clang/lld toolchain is unavailable\n' >&2
    exit 1
}
"$TOOLCHAIN_ROOT/bin/clang++" --version | rg -q 'clang version 20\.1\.8'
"$TOOLCHAIN_ROOT/bin/ld.lld" --version | rg -q 'LLD 20\.1\.8'
[[ -f "$SDK_ROOT/include/rt/runtime.hpp" && -f "$SDK_ROOT/lib/librtfw_runtime.a" ]] || {
    printf 'relocated SDK is missing runtime header or archive\n' >&2
    exit 1
}
if ! readelf -p .comment "$SDK_ROOT/lib/librtfw_runtime.a" 2>/dev/null |
    rg -q 'clang version 20\.1\.8'; then
    printf 'relocated runtime archive was not built by the approved bundled Clang 20.1.8\n' >&2
    exit 1
fi

STAGE_ROOT="$REPOSITORY_ROOT/build/m19-02-unreal-${CONFIGURATION,,}"
PROJECT_ROOT="$STAGE_ROOT/project"
REPORT_ROOT="$STAGE_ROOT/reports"
rm -rf "$STAGE_ROOT"
mkdir -p "$PROJECT_ROOT/Plugins" "$REPORT_ROOT"
cp -R "$REPOSITORY_ROOT/tests/unreal/." "$PROJECT_ROOT/"
cp -R "$REPOSITORY_ROOT/integrations/unreal/RTFWUnreal" \
    "$PROJECT_ROOT/Plugins/RTFWUnreal"

export RTFW_UNREAL_SDK_ROOT="$SDK_ROOT"
PROJECT_FILE="$PROJECT_ROOT/RTFWUnrealTestHost.uproject"

if [[ "$CONFIGURATION" == Development ]]; then
    "$BUILD_SCRIPT" \
        RTFWUnrealTestHostEditor Linux Development \
        -Project="$PROJECT_FILE" -WaitMutex -NoHotReloadFromIDE \
        2>&1 | tee "$REPORT_ROOT/ubt-editor-development.log"
fi
"$BUILD_SCRIPT" \
    RTFWUnrealTestHost Linux "$CONFIGURATION" \
    -Project="$PROJECT_FILE" -WaitMutex -NoHotReloadFromIDE \
    2>&1 | tee "$REPORT_ROOT/ubt-game-${CONFIGURATION,,}.log"

if ((BUILD_ONLY == 0)); then
    [[ "$CONFIGURATION" == Development ]] || {
        printf 'automation is supported only by the Development editor target\n' >&2
        exit 2
    }
    [[ -x "$EDITOR_COMMAND" ]] || {
        printf 'editor commandlet binary is unavailable after the editor target build\n' >&2
        exit 1
    }
    "$EDITOR_COMMAND" "$PROJECT_FILE" \
        -unattended -nop4 -nullrhi -nosplash \
        -ExecCmds='Automation RunTests RTFW.M19_02; Quit' \
        -ReportExportPath="$REPORT_ROOT" \
        -TestExit='Automation Test Queue Empty' \
        -log="$REPORT_ROOT/automation.log" \
        2>&1 | tee "$REPORT_ROOT/automation-commandlet.log"
    [[ -f "$REPORT_ROOT/index.json" ]] || {
        printf 'Unreal automation did not export a machine-readable report\n' >&2
        exit 1
    }
    python3 - "$REPORT_ROOT/index.json" <<'PY'
import json
import pathlib
import sys

report = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
if not report.get("succeeded", False):
    raise SystemExit("RTFW.M19_02 automation report did not succeed")
PY
fi

sha256sum "$SDK_ROOT/lib/librtfw_runtime.a" > "$REPORT_ROOT/rtfw-runtime.sha256"
git -C "$ENGINE_ROOT" rev-parse HEAD > "$REPORT_ROOT/unreal-commit.txt"
"$TOOLCHAIN_ROOT/bin/clang++" --version > "$REPORT_ROOT/clang-version.txt"
cp "$ENGINE_ROOT/Engine/Build/Build.version" "$REPORT_ROOT/Build.version"
printf 'stage=%s\nreports=%s\n' "$PROJECT_ROOT" "$REPORT_ROOT"
