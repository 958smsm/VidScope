#!/usr/bin/env python3
"""VidScope environment and runtime-deployment diagnostics."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Optional, Sequence

MIN_PYTHON = (3, 8)
FFMPEG_COMPONENTS = ("avformat", "avcodec", "avutil", "swscale", "swresample")


def get_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def use_color() -> bool:
    return sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def colorize(text: str, code: str) -> str:
    return f"\033[{code}m{text}\033[0m" if use_color() else text


def header(text: str) -> None:
    print("\n" + colorize(f"=== {text} ===", "36;1"))


class DoctorReport:
    def __init__(self) -> None:
        self.ok_count = 0
        self.warn_count = 0
        self.fail_count = 0

    def ok(self, message: str) -> None:
        print(colorize(f"[OK]   {message}", "32"))
        self.ok_count += 1

    def warn(self, message: str) -> None:
        print(colorize(f"[WARN] {message}", "33"))
        self.warn_count += 1

    def fail(self, message: str) -> None:
        print(colorize(f"[FAIL] {message}", "31"))
        self.fail_count += 1


def load_presets(repo_root: Path) -> Dict[str, object]:
    path = repo_root / "CMakePresets.json"
    try:
        return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    except (OSError, json.JSONDecodeError):
        return {}


def preset_map(data: Mapping[str, object], key: str) -> Dict[str, dict]:
    values = data.get(key, [])
    if not isinstance(values, list):
        return {}
    return {str(item["name"]): item for item in values if isinstance(item, dict) and item.get("name")}


def required_qt_version(repo_root: Path) -> Optional[str]:
    try:
        text = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    except OSError:
        return None
    match = re.search(r"find_package\(Qt6\s+([0-9]+(?:\.[0-9]+){1,2})\s+EXACT", text)
    return match.group(1) if match else None


def split_cmake_list(value: str) -> Iterable[str]:
    for part in value.split(";"):
        if part:
            yield part


def qt_root_from_candidate(candidate: Path) -> Optional[Path]:
    if candidate.name.casefold() == "qt6" and candidate.parent.name.casefold() == "cmake":
        candidate = candidate.parents[2]
    if (candidate / "bin").is_dir() and (candidate / "lib" / "cmake" / "Qt6").is_dir():
        return candidate
    return None


def preset_dependency_values(presets: Mapping[str, object]) -> tuple[List[Path], List[Path], List[Path]]:
    qt: List[Path] = []
    ffmpeg: List[Path] = []
    ninja: List[Path] = []
    for preset in preset_map(presets, "configurePresets").values():
        cache = preset.get("cacheVariables", {})
        if not isinstance(cache, dict):
            continue
        prefix = cache.get("CMAKE_PREFIX_PATH")
        if isinstance(prefix, str):
            qt.extend(Path(part) for part in split_cmake_list(prefix))
        root = cache.get("FFMPEG_ROOT")
        if isinstance(root, str):
            ffmpeg.append(Path(root))
        make_program = cache.get("CMAKE_MAKE_PROGRAM")
        if isinstance(make_program, str):
            ninja.append(Path(make_program))
    return qt, ffmpeg, ninja


def discover_qt(repo_root: Path, presets: Mapping[str, object]) -> Optional[Path]:
    candidates: List[Path] = []
    # Prefer the project's declared preset over ambient shell state.
    preset_qt, _, _ = preset_dependency_values(presets)
    candidates.extend(preset_qt)
    for name in ("QT_DIR", "Qt6_DIR", "CMAKE_PREFIX_PATH"):
        value = os.environ.get(name)
        if value:
            candidates.extend(Path(part) for part in split_cmake_list(value))

    qmake = shutil.which("qmake6") or shutil.which("qmake")
    if qmake:
        candidates.append(Path(qmake).resolve().parent.parent)

    version = required_qt_version(repo_root)
    if sys.platform == "win32" and version:
        candidates += [
            Path(f"D:/dev/Qt/{version}/msvc2022_64"),
            Path(f"C:/Qt/{version}/msvc2022_64"),
            Path(f"D:/Qt/{version}/msvc2022_64"),
        ]

    seen = set()
    for candidate in candidates:
        key = str(candidate).casefold()
        if key in seen:
            continue
        seen.add(key)
        root = qt_root_from_candidate(candidate)
        if root:
            return root
    return None


def ffmpeg_library_exists(lib_dir: Path, component: str) -> bool:
    if sys.platform == "win32":
        patterns = (f"{component}.lib", f"lib{component}.lib", f"*{component}*.lib")
    else:
        patterns = (f"lib{component}.so*", f"lib{component}.dylib", f"lib{component}.a")
    return any(any(lib_dir.glob(pattern)) for pattern in patterns)


def ffmpeg_valid(root: Path) -> bool:
    if not (root / "include" / "libavformat" / "avformat.h").exists():
        return False
    lib_dir = root / "lib"
    return lib_dir.is_dir() and all(ffmpeg_library_exists(lib_dir, name) for name in FFMPEG_COMPONENTS)


def discover_ffmpeg(presets: Mapping[str, object]) -> Optional[Path]:
    candidates: List[Path] = []
    _, preset_ffmpeg, _ = preset_dependency_values(presets)
    candidates.extend(preset_ffmpeg)
    if os.environ.get("FFMPEG_ROOT"):
        candidates.append(Path(os.environ["FFMPEG_ROOT"]))
    if sys.platform == "win32":
        candidates += [
            Path("D:/dev/ffmpeg-master-latest-win64-gpl-shared"),
            Path("C:/ffmpeg-shared"),
            Path("D:/ffmpeg-shared"),
        ]
    else:
        candidates += [Path("/usr/local"), Path("/usr")]

    seen = set()
    for candidate in candidates:
        key = str(candidate).casefold()
        if key in seen:
            continue
        seen.add(key)
        if ffmpeg_valid(candidate):
            return candidate
    return None


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def same_file(a: Path, b: Path) -> bool:
    try:
        return a.stat().st_size == b.stat().st_size and sha256(a) == sha256(b)
    except OSError:
        return False


def qmake_version(qt_root: Path) -> Optional[str]:
    exe = qt_root / "bin" / ("qmake.exe" if sys.platform == "win32" else "qmake")
    if not exe.exists():
        alt = qt_root / "bin" / "qmake6"
        exe = alt if alt.exists() else exe
    if not exe.exists():
        return None
    try:
        result = subprocess.run([str(exe), "-query", "QT_VERSION"], capture_output=True, text=True, timeout=10)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def check_python(report: DoctorReport) -> None:
    header("Python")
    version = sys.version_info
    text = f"{version.major}.{version.minor}.{version.micro} ({sys.executable})"
    if version >= MIN_PYTHON:
        report.ok(f"Python {text}")
    else:
        report.fail(f"Python {text}; {MIN_PYTHON[0]}.{MIN_PYTHON[1]}+ is required")
    report.ok(f"Platform: {sys.platform} / {os.name}")


def check_build_tools(report: DoctorReport, presets: Mapping[str, object]) -> None:
    header("Build tools")
    cmake = shutil.which("cmake")
    if not cmake:
        report.fail("CMake not found in PATH")
    else:
        try:
            out = subprocess.run([cmake, "--version"], capture_output=True, text=True, timeout=10).stdout.splitlines()
            version_line = out[0] if out else "unknown version"
            match = re.search(r"(\d+)\.(\d+)\.(\d+)", version_line)
            if match and tuple(map(int, match.groups())) < (3, 25, 0):
                report.fail(f"{version_line}; VidScope requires CMake 3.25+")
            else:
                report.ok(f"{version_line} ({cmake})")
        except (OSError, subprocess.SubprocessError) as exc:
            report.fail(f"CMake could not be executed: {exc}")

    ctest = shutil.which("ctest")
    (report.ok if ctest else report.fail)(f"CTest: {ctest}" if ctest else "CTest not found in PATH")

    ninja = shutil.which("ninja")
    _, _, preset_ninja = preset_dependency_values(presets)
    preset_ninja = [path for path in preset_ninja if path.exists()]
    if ninja:
        report.ok(f"Ninja: {ninja}")
    elif preset_ninja:
        report.ok(f"Ninja from preset: {preset_ninja[0]}")
    else:
        report.warn("Ninja not found; raw-mode builds may use another generator")

    git = shutil.which("git")
    (report.ok if git else report.warn)(f"Git: {git}" if git else "Git not found in PATH")


def find_vs() -> Optional[str]:
    if sys.platform != "win32":
        return None
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    try:
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        return result.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def check_compiler(report: DoctorReport) -> None:
    header("C++ compiler")
    if sys.platform == "win32":
        cl = shutil.which("cl.exe") or shutil.which("cl")
        if cl:
            report.ok(f"MSVC: {cl}")
            return
        vs = find_vs()
        if vs:
            report.ok(f"Visual Studio C++ workload: {vs}")
            report.warn("cl.exe is not in this shell; build.py will auto-load the VS developer environment")
        else:
            report.fail("MSVC C++ tools not found")
        return

    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    (report.ok if compiler else report.fail)(f"C++ compiler: {compiler}" if compiler else "No C++ compiler found")


def check_qt(report: DoctorReport, repo_root: Path, presets: Mapping[str, object]) -> Optional[Path]:
    header("Qt 6 SDK")
    qt_root = discover_qt(repo_root, presets)
    required = required_qt_version(repo_root)
    if not qt_root:
        report.fail("Qt 6 SDK not found from environment, presets, qmake, or known Windows paths")
        return None

    actual = qmake_version(qt_root)
    report.ok(f"Qt SDK: {qt_root}")
    if required and actual == required:
        report.ok(f"Qt version {actual} matches CMakeLists.txt EXACT requirement")
    elif required and actual:
        report.fail(f"Qt version mismatch: project requires {required}, SDK reports {actual}")
    elif required:
        report.warn(f"Could not query Qt version; project requires exactly {required}")

    if sys.platform == "win32":
        deploy = qt_root / "bin" / "windeployqt.exe"
        (report.ok if deploy.exists() else report.fail)(
            f"windeployqt: {deploy}" if deploy.exists() else f"windeployqt missing: {deploy}"
        )
    return qt_root


def check_ffmpeg(report: DoctorReport, presets: Mapping[str, object]) -> Optional[Path]:
    header("FFmpeg SDK")
    root = discover_ffmpeg(presets)
    if not root:
        report.fail("FFmpeg shared development SDK not found; set FFMPEG_ROOT or correct the CMake preset")
        return None

    report.ok(f"FFmpeg SDK: {root}")
    report.ok("FFmpeg headers and required link libraries found: " + ", ".join(FFMPEG_COMPONENTS))
    cli_names = ["ffmpeg.exe", "ffmpeg"]
    cli = next((root / "bin" / name for name in cli_names if (root / "bin" / name).exists()), None) or shutil.which("ffmpeg")
    (report.ok if cli else report.warn)(f"FFmpeg CLI: {cli}" if cli else "FFmpeg CLI not found; media fixture tests may be unavailable")

    if sys.platform == "win32":
        missing_runtime = [name for name in FFMPEG_COMPONENTS if not list((root / "bin").glob(f"{name}-*.dll"))]
        if missing_runtime:
            report.fail("Missing FFmpeg runtime DLL families: " + ", ".join(missing_runtime))
        else:
            report.ok("FFmpeg runtime DLLs are available for deployment")
    return root


def check_presets(report: DoctorReport, repo_root: Path, presets: Mapping[str, object]) -> None:
    header("CMake presets / default workflow")
    path = repo_root / "CMakePresets.json"
    if not path.exists():
        report.warn("CMakePresets.json not present; build.py will use raw CMake mode")
        return
    if not presets:
        report.fail("CMakePresets.json exists but could not be parsed")
        return

    configure = preset_map(presets, "configurePresets")
    builds = preset_map(presets, "buildPresets")
    tests = preset_map(presets, "testPresets")
    report.ok(f"Configure presets: {', '.join(configure) or '(none)'}")
    report.ok(f"Build presets: {', '.join(builds) or '(none)'}")
    report.ok(f"Test presets: {', '.join(tests) or '(none)'}")

    if sys.platform == "win32":
        if "windows-release" in builds:
            report.ok("No-argument Windows workflow resolves to windows-release")
        else:
            report.warn("No windows-release build preset; build.py will fall back to raw Release mode")


def expand_binary_dir(value: str, repo_root: Path, preset_name: str) -> Path:
    return Path(
        value.replace("${sourceDir}", str(repo_root))
        .replace("${sourceParentDir}", str(repo_root.parent))
        .replace("${sourceDirName}", repo_root.name)
        .replace("${presetName}", preset_name)
    )


def check_windows_runtime_deployment(
    report: DoctorReport,
    repo_root: Path,
    presets: Mapping[str, object],
    qt_root: Optional[Path],
    ffmpeg_root: Optional[Path],
) -> None:
    if sys.platform != "win32" or not qt_root:
        return
    header("Built application runtime")

    configure = preset_map(presets, "configurePresets")
    builds = preset_map(presets, "buildPresets")
    found_any = False

    for build_name, build in builds.items():
        config = str(build.get("configuration") or "Release")
        configure_name = build.get("configurePreset")
        cp = configure.get(str(configure_name), {})
        binary_value = cp.get("binaryDir")
        if not isinstance(binary_value, str):
            continue
        binary_dir = expand_binary_dir(binary_value, repo_root, str(configure_name))
        exe = binary_dir / config / "VidScope.exe"
        if not exe.exists():
            continue
        found_any = True
        report.ok(f"{build_name}: {exe}")

        suffix = "d" if config.casefold() == "debug" else ""
        for base in ("Qt6Core", "Qt6Gui", "Qt6Widgets"):
            name = f"{base}{suffix}.dll"
            sdk = qt_root / "bin" / name
            deployed = exe.parent / name
            if not deployed.exists():
                report.fail(f"{build_name}: missing deployed {name}")
            elif not sdk.exists():
                report.fail(f"{build_name}: {name} missing from selected Qt SDK")
            elif same_file(sdk, deployed):
                report.ok(f"{build_name}: {name} matches selected Qt SDK")
            else:
                report.fail(
                    f"{build_name}: {name} differs from selected Qt SDK; this can cause 'Entry Point Not Found' before main()"
                )

        if ffmpeg_root:
            for component in FFMPEG_COMPONENTS:
                sources = sorted((ffmpeg_root / "bin").glob(f"{component}-*.dll"))
                for source in sources:
                    deployed = exe.parent / source.name
                    if not deployed.exists():
                        report.fail(f"{build_name}: missing deployed FFmpeg DLL {source.name}")
                    elif same_file(source, deployed):
                        report.ok(f"{build_name}: {source.name} matches selected FFmpeg SDK")
                    else:
                        report.fail(f"{build_name}: {source.name} differs from selected FFmpeg SDK")

    if not found_any:
        report.warn("VidScope.exe has not been built in the configured preset output directories yet")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Diagnose VidScope build dependencies and deployed runtimes.")
    parser.add_argument("--no-runtime", action="store_true", help="Skip comparison of deployed Windows Qt DLLs")
    return parser.parse_args(list(argv))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    repo_root = get_repo_root()
    presets = load_presets(repo_root)

    print(colorize("VidScope Environment Doctor", "35;1"))
    print(f"Repository: {repo_root}")

    report = DoctorReport()
    check_python(report)
    check_build_tools(report, presets)
    check_compiler(report)
    qt_root = check_qt(report, repo_root, presets)
    ffmpeg_root = check_ffmpeg(report, presets)
    check_presets(report, repo_root, presets)
    if not args.no_runtime:
        check_windows_runtime_deployment(report, repo_root, presets, qt_root, ffmpeg_root)

    header("Summary")
    print(f"Passed:   {report.ok_count}")
    print(f"Warnings: {report.warn_count}")
    print(f"Failures: {report.fail_count}")

    if report.fail_count:
        print(colorize("\nSystem status: ISSUES DETECTED", "31;1"))
        if sys.platform == "win32":
            print("Run tools/build_windows.ps1 (or .cmd) with no arguments to rebuild and repair runtime deployment.")
        else:
            print("Run tools/build_linux.sh with no arguments after resolving the failed dependencies.")
        return 1

    print(colorize("\nSystem status: READY", "32;1"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
