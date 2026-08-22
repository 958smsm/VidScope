#!/usr/bin/env python3
"""VidScope centralized build runner.

The no-argument workflow is intentionally useful:

* Windows: auto-load MSVC, select the Release Windows preset, configure, build,
  repair/deploy the Qt + FFmpeg runtime, run a startup smoke probe, then CTest.
* Linux/macOS: configure a Release build, build, then CTest.

Command-line options can override any part of that workflow.
"""

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
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

MIN_PYTHON = (3, 8)
DEFAULT_CONFIG = "Release"
APP_NAME = "VidScope"
FFMPEG_COMPONENTS = ("avformat", "avcodec", "avutil", "swscale", "swresample")


def get_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def log(level: str, message: str) -> None:
    print(f"[{level}] {message}")


def _display_command(cmd: Sequence[str]) -> str:
    return subprocess.list2cmdline([str(part) for part in cmd])


def run_command(
    cmd: Sequence[str],
    cwd: Path,
    *,
    env: Optional[Mapping[str, str]] = None,
    timeout: Optional[int] = None,
    dry_run: bool = False,
) -> int:
    print(f"\n[EXEC] {_display_command(cmd)}")
    if dry_run:
        return 0
    try:
        result = subprocess.run([str(part) for part in cmd], cwd=cwd, env=env, timeout=timeout)
        return result.returncode
    except subprocess.TimeoutExpired:
        log("ERROR", f"Command timed out after {timeout}s")
        return 124
    except FileNotFoundError:
        log("ERROR", f"Executable not found: {cmd[0]}")
        return 127


def suppress_windows_error_dialogs() -> None:
    """Prevent loader/crash message boxes from turning CI/CTest failures into timeouts."""
    if sys.platform != "win32":
        return
    try:
        import ctypes

        sem_failcriticalerrors = 0x0001
        sem_nogpfaulterrorbox = 0x0002
        sem_noopenfileerrorbox = 0x8000
        ctypes.windll.kernel32.SetErrorMode(
            sem_failcriticalerrors | sem_nogpfaulterrorbox | sem_noopenfileerrorbox
        )
    except Exception as exc:  # pragma: no cover - best effort on Windows
        log("WARN", f"Could not disable Windows error dialogs: {exc}")


def _find_vs_installation() -> Optional[Path]:
    if sys.platform != "win32":
        return None

    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        try:
            result = subprocess.run(
                [
                    str(vswhere),
                    "-latest",
                    "-products",
                    "*",
                    "-requires",
                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-property",
                    "installationPath",
                ],
                capture_output=True,
                text=True,
                check=True,
            )
            if result.stdout.strip():
                return Path(result.stdout.strip())
        except (OSError, subprocess.SubprocessError):
            pass

    search_roots = [
        Path(r"E:\Program Files\Microsoft Visual Studio"),
        Path(r"D:\Program Files\Microsoft Visual Studio"),
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Microsoft Visual Studio",
        Path(program_files_x86) / "Microsoft Visual Studio",
    ]
    candidates: List[Path] = []
    for root in search_roots:
        if root.exists():
            candidates.extend(root.glob("*/*/Common7/Tools/Launch-VsDevShell.ps1"))
    if not candidates:
        return None
    script = sorted(candidates, reverse=True)[0]
    return script.parents[2]


def setup_msvc_env_if_needed() -> bool:
    """Import a 64-bit Visual Studio developer environment when cl.exe is absent."""
    if sys.platform != "win32" or shutil.which("cl.exe"):
        return True

    log("INFO", "MSVC compiler not in PATH; locating a Visual Studio C++ installation...")
    installation = _find_vs_installation()
    if not installation:
        log("ERROR", "Visual Studio C++ tools were not found. Install the Desktop development with C++ workload.")
        return False

    devshell = installation / "Common7" / "Tools" / "Launch-VsDevShell.ps1"
    if not devshell.exists():
        log("ERROR", f"Visual Studio developer shell not found: {devshell}")
        return False

    powershell = shutil.which("pwsh.exe") or shutil.which("powershell.exe")
    if not powershell:
        log("ERROR", "PowerShell was not found; cannot initialize the MSVC environment.")
        return False

    log("INFO", f"Loading MSVC x64 environment from {installation}")
    escaped_devshell = str(devshell).replace("'", "''")
    ps_script = (
        f"& '{escaped_devshell}' -Arch amd64 -SkipAutomaticLocation | Out-Null; "
        "Get-ChildItem Env: | ForEach-Object { Write-Output ($_.Name + '=' + $_.Value) }"
    )
    try:
        result = subprocess.run(
            [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps_script],
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        log("ERROR", f"Failed to initialize the Visual Studio developer environment: {exc}")
        return False

    for line in result.stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key:
            os.environ[key] = value

    if not shutil.which("cl.exe"):
        log("ERROR", "Visual Studio developer environment loaded, but cl.exe is still unavailable.")
        return False

    log("OK", f"MSVC ready: {shutil.which('cl.exe')}")
    return True


def load_presets(repo_root: Path) -> Dict[str, object]:
    presets_file = repo_root / "CMakePresets.json"
    if not presets_file.exists():
        return {}
    try:
        return json.loads(presets_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Failed to read {presets_file}: {exc}") from exc


def _preset_map(data: Mapping[str, object], key: str) -> Dict[str, dict]:
    result: Dict[str, dict] = {}
    for item in data.get(key, []) if isinstance(data.get(key, []), list) else []:
        if isinstance(item, dict) and item.get("name"):
            result[str(item["name"])] = item
    return result


def _expand_preset_path(value: str, repo_root: Path, preset_name: str = "") -> Path:
    replacements = {
        "${sourceDir}": str(repo_root),
        "${sourceParentDir}": str(repo_root.parent),
        "${sourceDirName}": repo_root.name,
        "${presetName}": preset_name,
    }
    expanded = value
    for token, replacement in replacements.items():
        expanded = expanded.replace(token, replacement)
    return Path(expanded)


def get_configure_preset(data: Mapping[str, object], name: str) -> Optional[dict]:
    return _preset_map(data, "configurePresets").get(name)


def get_build_preset(data: Mapping[str, object], name: str) -> Optional[dict]:
    return _preset_map(data, "buildPresets").get(name)


def get_test_preset(data: Mapping[str, object], name: str) -> Optional[dict]:
    return _preset_map(data, "testPresets").get(name)


def find_matching_build_preset(data: Mapping[str, object], configure_name: str, config: str) -> Optional[str]:
    wanted = config.casefold()
    for name, preset in _preset_map(data, "buildPresets").items():
        if preset.get("configurePreset") != configure_name:
            continue
        preset_config = str(preset.get("configuration", "")).casefold()
        if preset_config == wanted:
            return name
    return None


def find_matching_test_preset(data: Mapping[str, object], configure_name: str, config: str) -> Optional[str]:
    wanted = config.casefold()
    for name, preset in _preset_map(data, "testPresets").items():
        if preset.get("configurePreset") != configure_name:
            continue
        preset_config = str(preset.get("configuration", "")).casefold()
        if preset_config == wanted:
            return name
    return None


def choose_default_preset(data: Mapping[str, object], config: str) -> Optional[str]:
    build_presets = _preset_map(data, "buildPresets")
    platform_prefix = "windows" if sys.platform == "win32" else "macos" if sys.platform == "darwin" else "linux"
    preferred = f"{platform_prefix}-{config.lower()}"
    if preferred in build_presets:
        return preferred

    for name, preset in build_presets.items():
        if not name.casefold().startswith(platform_prefix):
            continue
        if str(preset.get("configuration", "")).casefold() == config.casefold():
            return name
    return None


def resolve_preset_selection(
    data: Mapping[str, object], explicit: Optional[str], config: str, no_preset: bool
) -> Tuple[Optional[str], Optional[str], Optional[str], str]:
    """Return configure preset, build preset, test preset, effective config."""
    if no_preset or not data:
        return None, None, None, config

    selected = explicit or choose_default_preset(data, config)
    if not selected:
        return None, None, None, config

    configure_presets = _preset_map(data, "configurePresets")
    build_presets = _preset_map(data, "buildPresets")
    test_presets = _preset_map(data, "testPresets")

    configure_name: Optional[str] = None
    build_name: Optional[str] = None
    effective_config = config

    if selected in build_presets:
        build_name = selected
        build = build_presets[selected]
        configure_name = build.get("configurePreset")
        effective_config = str(build.get("configuration") or config)
    elif selected in configure_presets:
        configure_name = selected
        build_name = find_matching_build_preset(data, selected, config)
    else:
        known = sorted(set(configure_presets) | set(build_presets))
        raise RuntimeError(f"Unknown CMake preset '{selected}'. Known presets: {', '.join(known) or '(none)'}")

    test_name: Optional[str] = None
    if build_name and build_name in test_presets:
        test_name = build_name
    elif configure_name:
        test_name = find_matching_test_preset(data, configure_name, effective_config)

    return configure_name, build_name, test_name, effective_config


def get_binary_dir(
    repo_root: Path,
    data: Mapping[str, object],
    configure_preset: Optional[str],
    raw_build_dir: Optional[str],
    config: str,
) -> Path:
    if configure_preset:
        preset = get_configure_preset(data, configure_preset) or {}
        binary_dir = preset.get("binaryDir")
        if binary_dir:
            return _expand_preset_path(str(binary_dir), repo_root, configure_preset)

    if raw_build_dir:
        path = Path(raw_build_dir)
        return path if path.is_absolute() else repo_root / path

    platform_name = "windows" if sys.platform == "win32" else "macos" if sys.platform == "darwin" else "linux"
    return repo_root / "build" / f"{platform_name}-{config.lower()}"


def _split_cmake_path_list(value: str) -> Iterable[str]:
    # CMake lists use ';' on all platforms. Environment CMAKE_PREFIX_PATH may use
    # the native path separator, but splitting ';' first preserves Windows drive colons.
    chunks: List[str] = []
    for part in value.split(";"):
        if not part:
            continue
        if os.pathsep != ";" and os.pathsep in part:
            chunks.extend(p for p in part.split(os.pathsep) if p)
        else:
            chunks.append(part)
    return chunks


def _cache_variables(data: Mapping[str, object], configure_preset: Optional[str]) -> Mapping[str, object]:
    if not configure_preset:
        return {}
    preset = get_configure_preset(data, configure_preset) or {}
    cache = preset.get("cacheVariables", {})
    return cache if isinstance(cache, dict) else {}


def required_qt_version(repo_root: Path) -> Optional[str]:
    try:
        text = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    except OSError:
        return None
    match = re.search(r"find_package\(Qt6\s+([0-9]+(?:\.[0-9]+){1,2})\s+EXACT", text)
    return match.group(1) if match else None


def _qt_root_from_candidate(candidate: Path) -> Optional[Path]:
    candidate = candidate.expanduser()
    # Qt6_DIR commonly points at <prefix>/lib/cmake/Qt6.
    if candidate.name.casefold() == "qt6" and candidate.parent.name.casefold() == "cmake":
        candidate = candidate.parents[2]
    if (candidate / "bin").is_dir() and (candidate / "lib" / "cmake" / "Qt6").is_dir():
        return candidate
    return None


def discover_qt_root(
    repo_root: Path,
    data: Mapping[str, object],
    configure_preset: Optional[str],
    explicit: Optional[str],
) -> Optional[Path]:
    candidates: List[Path] = []
    if explicit:
        candidates.append(Path(explicit))

    # Project preset wins over ambient shell variables for reproducible no-argument builds.
    cache = _cache_variables(data, configure_preset)
    preset_prefix = cache.get("CMAKE_PREFIX_PATH")
    if isinstance(preset_prefix, str):
        candidates.extend(Path(item) for item in _split_cmake_path_list(preset_prefix))

    for env_name in ("QT_DIR", "Qt6_DIR", "CMAKE_PREFIX_PATH"):
        value = os.environ.get(env_name)
        if value:
            candidates.extend(Path(item) for item in _split_cmake_path_list(value))

    qmake = shutil.which("qmake6") or shutil.which("qmake")
    if qmake:
        candidates.append(Path(qmake).resolve().parent.parent)

    version = required_qt_version(repo_root)
    if sys.platform == "win32" and version:
        candidates.extend(
            [
                Path(f"D:/dev/Qt/{version}/msvc2022_64"),
                Path(f"C:/Qt/{version}/msvc2022_64"),
                Path(f"D:/Qt/{version}/msvc2022_64"),
            ]
        )

    seen = set()
    for candidate in candidates:
        key = str(candidate).casefold()
        if key in seen:
            continue
        seen.add(key)
        root = _qt_root_from_candidate(candidate)
        if root:
            return root
    return None


def _ffmpeg_library_exists(lib_dir: Path, component: str) -> bool:
    if sys.platform == "win32":
        patterns = (f"{component}.lib", f"lib{component}.lib", f"*{component}*.lib")
    else:
        patterns = (f"lib{component}.so*", f"lib{component}.dylib", f"lib{component}.a")
    return any(any(lib_dir.glob(pattern)) for pattern in patterns)


def _ffmpeg_sdk_valid(root: Path) -> bool:
    if not (root / "include" / "libavformat" / "avformat.h").exists():
        return False
    lib_dir = root / "lib"
    if not lib_dir.is_dir():
        return False
    return all(_ffmpeg_library_exists(lib_dir, component) for component in FFMPEG_COMPONENTS)


def discover_ffmpeg_root(
    data: Mapping[str, object], configure_preset: Optional[str], explicit: Optional[str]
) -> Optional[Path]:
    candidates: List[Path] = []
    if explicit:
        candidates.append(Path(explicit))

    cache = _cache_variables(data, configure_preset)
    preset_root = cache.get("FFMPEG_ROOT")
    if isinstance(preset_root, str):
        candidates.append(Path(preset_root))

    if os.environ.get("FFMPEG_ROOT"):
        candidates.append(Path(os.environ["FFMPEG_ROOT"]))

    if sys.platform == "win32":
        candidates.extend(
            [
                Path("D:/dev/ffmpeg-master-latest-win64-gpl-shared"),
                Path("C:/ffmpeg-shared"),
                Path("D:/ffmpeg-shared"),
            ]
        )
        for parent in (Path("D:/dev"), Path("C:/dev"), Path("D:/"), Path("C:/")):
            if parent.exists():
                candidates.extend(parent.glob("ffmpeg*shared*"))
    else:
        candidates.extend([Path("/usr/local"), Path("/usr")])

    seen = set()
    for candidate in candidates:
        key = str(candidate).casefold()
        if key in seen:
            continue
        seen.add(key)
        if _ffmpeg_sdk_valid(candidate):
            return candidate
    return None


def discover_ninja(data: Mapping[str, object], configure_preset: Optional[str], qt_root: Optional[Path]) -> Optional[Path]:
    cache = _cache_variables(data, configure_preset)
    preset_ninja = cache.get("CMAKE_MAKE_PROGRAM")
    if isinstance(preset_ninja, str) and Path(preset_ninja).is_file():
        return Path(preset_ninja)

    ninja = shutil.which("ninja")
    if ninja:
        return Path(ninja)

    candidates: List[Path] = []
    if qt_root:
        candidates.append(qt_root.parent.parent / "Tools" / "Ninja" / ("ninja.exe" if sys.platform == "win32" else "ninja"))
    if sys.platform == "win32":
        candidates.extend([Path("D:/dev/Qt/Tools/Ninja/ninja.exe"), Path("C:/Qt/Tools/Ninja/ninja.exe")])
    return next((path for path in candidates if path.is_file()), None)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _same_file_contents(a: Path, b: Path) -> bool:
    try:
        return a.stat().st_size == b.stat().st_size and file_sha256(a) == file_sha256(b)
    except OSError:
        return False


def locate_app_executable(binary_dir: Path, config: str) -> Optional[Path]:
    if sys.platform == "win32":
        direct = binary_dir / config / f"{APP_NAME}.exe"
        if direct.exists():
            return direct
        candidates = list(binary_dir.glob(f"**/{APP_NAME}.exe"))
    else:
        direct = binary_dir / APP_NAME
        if direct.exists():
            return direct
        candidates = [path for path in binary_dir.glob(f"**/{APP_NAME}") if path.is_file()]

    if not candidates:
        return None
    config_fold = config.casefold()
    preferred = [p for p in candidates if any(part.casefold() == config_fold for part in p.parts)]
    return (preferred or candidates)[0]


def deploy_ffmpeg_runtime(ffmpeg_root: Optional[Path], app_dir: Path, dry_run: bool = False) -> bool:
    if sys.platform != "win32" or not ffmpeg_root:
        return True
    bin_dir = ffmpeg_root / "bin"
    if not bin_dir.is_dir():
        log("WARN", f"FFmpeg runtime directory not found: {bin_dir}")
        return False

    copied = 0
    for component in FFMPEG_COMPONENTS:
        sources = sorted(bin_dir.glob(f"{component}-*.dll"))
        if not sources:
            log("ERROR", f"FFmpeg runtime DLL missing for {component} in {bin_dir}")
            return False
        source_names = {src.name.casefold() for src in sources}
        for stale in app_dir.glob(f"{component}-*.dll"):
            if stale.name.casefold() not in source_names:
                log("INFO", f"Removing stale FFmpeg runtime: {stale.name}")
                if not dry_run:
                    try:
                        stale.unlink(missing_ok=True)
                    except OSError as exc:
                        log("ERROR", f"Could not remove stale FFmpeg runtime {stale}: {exc}")
                        return False
        for source in sources:
            dest = app_dir / source.name
            if dry_run or not dest.exists() or not _same_file_contents(source, dest):
                log("INFO", f"Deploy FFmpeg: {source.name}")
                if not dry_run:
                    try:
                        shutil.copy2(source, dest)
                    except OSError as exc:
                        log("ERROR", f"Could not deploy FFmpeg runtime {source.name}: {exc}")
                        return False
                copied += 1
    log("OK", f"FFmpeg runtime synchronized ({copied} file(s) updated)")
    return True


def deploy_qt_runtime(qt_root: Optional[Path], executable: Path, config: str, repo_root: Path, dry_run: bool = False) -> bool:
    if sys.platform != "win32":
        return True
    if not qt_root:
        log("ERROR", "Cannot deploy Qt runtime because the Qt SDK root could not be determined.")
        return False

    windeployqt = qt_root / "bin" / "windeployqt.exe"
    if not windeployqt.exists():
        log("ERROR", f"windeployqt.exe not found in the configured Qt SDK: {windeployqt}")
        return False

    # A stale DLL beside the EXE wins over PATH and can fail before main().
    # Remove top-level Qt runtime DLLs first, then deploy from the exact SDK CMake used.
    for stale in executable.parent.glob("Qt6*.dll"):
        log("INFO", f"Removing previously deployed Qt runtime: {stale.name}")
        if not dry_run:
            try:
                stale.unlink(missing_ok=True)
            except OSError as exc:
                log("ERROR", f"Could not remove stale Qt runtime {stale}: {exc}")
                log("ERROR", "Close any running VidScope instance and retry.")
                return False

    mode = "--debug" if config.casefold() == "debug" else "--release"
    cmd = [str(windeployqt), mode, "--force", "--no-translations", str(executable)]
    env = os.environ.copy()
    env["PATH"] = str(qt_root / "bin") + os.pathsep + env.get("PATH", "")
    code = run_command(cmd, cwd=repo_root, env=env, dry_run=dry_run)
    if code != 0:
        log("ERROR", f"windeployqt failed with exit code {code}")
        return False

    suffix = "d" if config.casefold() == "debug" else ""
    required = [f"Qt6Core{suffix}.dll", f"Qt6Gui{suffix}.dll", f"Qt6Widgets{suffix}.dll"]
    for name in required:
        source = qt_root / "bin" / name
        dest = executable.parent / name
        if dry_run:
            continue
        if not source.exists() or not dest.exists():
            log("ERROR", f"Required deployed Qt DLL is missing: {name}")
            return False
        if not _same_file_contents(source, dest):
            log("ERROR", f"Deployed {name} does not match the Qt SDK used for this build.")
            return False

    log("OK", f"Qt runtime synchronized from {qt_root}")
    return True


def build_runtime_env(qt_root: Optional[Path], ffmpeg_root: Optional[Path]) -> Dict[str, str]:
    env = os.environ.copy()
    prepend: List[str] = []
    if qt_root:
        prepend.append(str(qt_root / "bin"))
        plugins = qt_root / "plugins"
        if plugins.is_dir():
            env["QT_PLUGIN_PATH"] = str(plugins)
    if ffmpeg_root and (ffmpeg_root / "bin").is_dir():
        prepend.append(str(ffmpeg_root / "bin"))
    if prepend:
        env["PATH"] = os.pathsep.join(prepend + [env.get("PATH", "")])
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    return env


def run_startup_probe(
    executable: Path,
    repo_root: Path,
    qt_root: Optional[Path],
    ffmpeg_root: Optional[Path],
    dry_run: bool = False,
) -> bool:
    log("INFO", "Running VidScope startup smoke probe (offscreen)...")
    code = run_command(
        [str(executable), "--smoke-test"],
        cwd=repo_root,
        env=build_runtime_env(qt_root, ffmpeg_root),
        timeout=20,
        dry_run=dry_run,
    )
    if code != 0:
        log("ERROR", f"VidScope startup smoke probe failed with exit code {code}.")
        if code == 124:
            log("ERROR", "The application did not reach/finish its smoke path. Check runtime DLL compatibility.")
        return False
    log("OK", "VidScope startup smoke probe passed")
    return True


def parse_args(argv: Sequence[str]) -> Tuple[argparse.Namespace, List[str]]:
    parser = argparse.ArgumentParser(
        description="Configure, build, repair runtime deployment, and test VidScope.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--preset", help="CMake configure/build preset to use")
    parser.add_argument("--no-preset", action="store_true", help="Ignore CMakePresets.json and use a raw build directory")
    parser.add_argument("-c", "--config", default=DEFAULT_CONFIG, choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"])
    parser.add_argument("-b", "--build-dir", help="Raw-mode build directory")
    parser.add_argument("--clean", action="store_true", help="Remove the selected build directory before configuring")
    parser.add_argument("-t", "--target", help="Specific build target")
    parser.add_argument("-T", "--test", action="store_true", help="Run CTest after building")
    parser.add_argument("--skip-test", action="store_true", help="Skip CTest (no-argument mode runs tests by default)")
    parser.add_argument("--test-preset", help="Explicit CTest preset")
    parser.add_argument("--install", action="store_true", help="Install after a successful build/test")
    parser.add_argument("--prefix", help="Installation prefix")
    parser.add_argument("--qt-dir", help="Qt 6 installation prefix; overrides preset discovery")
    parser.add_argument("--ffmpeg-dir", help="FFmpeg shared development SDK root; overrides preset discovery")
    parser.add_argument("-G", "--generator", help="Generator for raw mode")
    parser.add_argument("-j", "--jobs", type=int, help="Parallel build jobs")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose build/test output")
    parser.add_argument("--no-deploy", action="store_true", help="Disable Windows runtime deployment/repair")
    parser.add_argument("--no-tests", action="store_true", help="Configure without VidScope tests and skip CTest")
    parser.add_argument("--skip-smoke", action="store_true", help="Skip the post-deploy startup smoke probe")
    parser.add_argument("--configure-only", action="store_true", help="Stop after CMake configure")
    parser.add_argument("--dry-run", action="store_true", help="Print commands/actions without executing them")
    parser.add_argument("--doctor", action="store_true", help="Run tools/doctor.py and exit")

    args, extra = parser.parse_known_args(list(argv))
    if args.test and args.skip_test:
        parser.error("--test and --skip-test cannot be used together")
    if args.no_tests and args.test:
        parser.error("--no-tests cannot be combined with --test")
    return args, extra


def main(argv: Optional[Sequence[str]] = None) -> int:
    if sys.version_info < MIN_PYTHON:
        log("ERROR", f"Python {MIN_PYTHON[0]}.{MIN_PYTHON[1]}+ is required.")
        return 2

    repo_root = get_repo_root()
    argv = list(sys.argv[1:] if argv is None else argv)

    # Backward-compatible `build.py doctor` / `build.py --doctor` delegation.
    if argv and argv[0] == "doctor":
        return subprocess.call([sys.executable, str(repo_root / "tools" / "doctor.py"), *argv[1:]], cwd=repo_root)

    args, extra_args = parse_args(argv)
    if args.doctor:
        return subprocess.call([sys.executable, str(repo_root / "tools" / "doctor.py")], cwd=repo_root)

    suppress_windows_error_dialogs()
    if not setup_msvc_env_if_needed():
        return 2

    for tool in ("cmake", "ctest"):
        if not shutil.which(tool):
            log("ERROR", f"{tool} was not found in PATH.")
            return 2

    try:
        presets = load_presets(repo_root)
        configure_preset, build_preset, auto_test_preset, config = resolve_preset_selection(
            presets, args.preset, args.config, args.no_preset
        )
    except RuntimeError as exc:
        log("ERROR", str(exc))
        return 2

    no_argument_mode = len(argv) == 0
    run_tests = not args.no_tests and not args.skip_test and (args.test or no_argument_mode)
    test_preset = args.test_preset or auto_test_preset
    binary_dir = get_binary_dir(repo_root, presets, configure_preset, args.build_dir, config)

    qt_root = discover_qt_root(repo_root, presets, configure_preset, args.qt_dir)
    ffmpeg_root = discover_ffmpeg_root(presets, configure_preset, args.ffmpeg_dir)
    ninja = discover_ninja(presets, configure_preset, qt_root)

    if no_argument_mode:
        default_steps = f"{config} configure + build"
        if sys.platform == "win32":
            default_steps += " + runtime repair + startup smoke"
        default_steps += " + tests"
        log("INFO", f"Default workflow: {default_steps}")
    if configure_preset:
        log("INFO", f"Configure preset: {configure_preset}")
    else:
        log("INFO", f"Raw CMake build directory: {binary_dir}")
    if build_preset:
        log("INFO", f"Build preset: {build_preset}")
    if qt_root:
        log("INFO", f"Qt SDK: {qt_root}")
    elif sys.platform == "win32":
        log("WARN", "Qt SDK was not auto-detected; CMake may still find it, but runtime repair cannot be guaranteed.")
    if ffmpeg_root:
        log("INFO", f"FFmpeg SDK: {ffmpeg_root}")
    elif sys.platform == "win32":
        log("WARN", "FFmpeg SDK was not auto-detected; configure may fail unless CMake can find it independently.")

    if args.clean and binary_dir.exists():
        log("INFO", f"Cleaning build directory: {binary_dir}")
        if not args.dry_run:
            shutil.rmtree(binary_dir)

    configure_cmd: List[str]
    if configure_preset:
        configure_cmd = ["cmake", "--preset", configure_preset]
    else:
        configure_cmd = ["cmake", "-S", ".", "-B", str(binary_dir)]
        generator = args.generator or ("Ninja" if ninja else None)
        if generator:
            configure_cmd += ["-G", generator]
        configure_cmd.append(f"-DCMAKE_BUILD_TYPE={config}")

    # Explicit/auto-discovered dependency paths override stale machine-specific preset values.
    if qt_root:
        configure_cmd.append(f"-DCMAKE_PREFIX_PATH={qt_root}")
    if ffmpeg_root:
        configure_cmd.append(f"-DFFMPEG_ROOT={ffmpeg_root}")
    if ninja and configure_preset:
        cache_ninja = _cache_variables(presets, configure_preset).get("CMAKE_MAKE_PROGRAM")
        if isinstance(cache_ninja, str) and not Path(cache_ninja).is_file():
            configure_cmd.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")
    if args.no_deploy:
        configure_cmd.append("-DVIDSCOPE_DEPLOY_RUNTIME=OFF")
    if args.no_tests:
        configure_cmd += ["-DVIDSCOPE_BUILD_TESTS=OFF", "-DBUILD_TESTING=OFF"]
    else:
        configure_cmd += ["-DVIDSCOPE_BUILD_TESTS=ON", "-DBUILD_TESTING=ON"]
    configure_cmd.extend(extra_args)

    code = run_command(configure_cmd, cwd=repo_root, dry_run=args.dry_run)
    if code != 0:
        log("ERROR", f"CMake configuration failed with exit code {code}")
        return code
    if args.configure_only:
        log("SUCCESS", "VidScope configuration completed successfully.")
        return 0

    if build_preset:
        build_cmd = ["cmake", "--build", "--preset", build_preset]
    else:
        build_cmd = ["cmake", "--build", str(binary_dir), "--config", config]
    if args.target:
        build_cmd += ["--target", args.target]
    if args.jobs:
        build_cmd += ["--parallel", str(args.jobs)]
    if args.verbose:
        build_cmd.append("--verbose")

    code = run_command(build_cmd, cwd=repo_root, dry_run=args.dry_run)
    if code != 0:
        log("ERROR", f"CMake build failed with exit code {code}")
        return code

    # Always repair runtime deployment after a Windows build. This runs even when
    # Ninja reports "no work to do", which is exactly when stale DLLs otherwise survive.
    executable = locate_app_executable(binary_dir, config)
    app_target_requested = not args.target or args.target.casefold() in {"vidscope", "all", "install"}
    if sys.platform == "win32" and not args.no_deploy and app_target_requested:
        if not executable and not args.dry_run:
            log("ERROR", f"{APP_NAME}.exe was not found under {binary_dir} after a successful build.")
            return 3
        if executable:
            if not deploy_ffmpeg_runtime(ffmpeg_root, executable.parent, args.dry_run):
                return 3
            if not deploy_qt_runtime(qt_root, executable, config, repo_root, args.dry_run):
                return 3
            if not args.skip_smoke and not run_startup_probe(
                executable, repo_root, qt_root, ffmpeg_root, args.dry_run
            ):
                return 4

    if run_tests:
        if test_preset:
            test_cmd = ["ctest", "--preset", test_preset]
        else:
            test_cmd = ["ctest", "--test-dir", str(binary_dir), "-C", config, "--output-on-failure"]
        if args.verbose:
            test_cmd.append("--verbose")
        code = run_command(test_cmd, cwd=repo_root, dry_run=args.dry_run)
        if code != 0:
            log("ERROR", f"CTest failed with exit code {code}")
            return code

    if args.install:
        install_cmd = ["cmake", "--install", str(binary_dir), "--config", config]
        if args.prefix:
            install_cmd += ["--prefix", args.prefix]
        code = run_command(install_cmd, cwd=repo_root, dry_run=args.dry_run)
        if code != 0:
            log("ERROR", f"CMake install failed with exit code {code}")
            return code

    log("SUCCESS", "VidScope build workflow completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
