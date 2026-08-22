#!/usr/bin/env python3
"""
VidScope System Diagnostic Tool (doctor.py)

Inspects system environment, build tools, compiler capabilities, Qt 6 SDK,
FFmpeg SDK/CLI, and CMake presets required to build and test VidScope.
"""

import sys
import os
import shutil
import subprocess
import json
import re
from pathlib import Path

# Color formatting helpers for stdout
IS_TTY = sys.stdout.isatty() and os.name != 'nt' or ('COLORTERM' in os.environ or 'TERM' in os.environ)

def colorize(text, code):
    if IS_TTY or True:  # Windows terminal supports standard ANSI escape sequences in PowerShell/CMD
        return f"\033[{code}m{text}\033[0m"
    return text

def ok_str(text):
    return colorize(f"[OK]   {text}", "32")  # Green

def warn_str(text):
    return colorize(f"[WARN] {text}", "33")  # Yellow

def fail_str(text):
    return colorize(f"[FAIL] {text}", "31")  # Red

def header_str(text):
    return colorize(f"=== {text} ===", "36;1")  # Cyan Bold


class DoctorReport:
    def __init__(self):
        self.ok_count = 0
        self.warn_count = 0
        self.fail_count = 0

    def ok(self, msg):
        print(ok_str(msg))
        self.ok_count += 1

    def warn(self, msg):
        print(warn_str(msg))
        self.warn_count += 1

    def fail(self, msg):
        print(fail_str(msg))
        self.fail_count += 1


def get_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def check_python(report: DoctorReport):
    print(header_str("Python Environment"))
    ver = sys.version_info
    ver_str = f"{ver.major}.{ver.minor}.{ver.micro}"
    if ver >= (3, 8):
        report.ok(f"Python version {ver_str} ({sys.executable})")
    else:
        report.fail(f"Python version {ver_str} is older than required 3.8+")
    print(f"       OS: {sys.platform} ({os.name})")


def check_build_tools(report: DoctorReport):
    print("\n" + header_str("Build Tools"))

    # CMake check
    cmake_bin = shutil.which("cmake")
    if cmake_bin:
        try:
            res = subprocess.run([cmake_bin, "--version"], capture_output=True, text=True, check=True)
            first_line = res.stdout.splitlines()[0] if res.stdout else ""
            match = re.search(r"version\s+(\d+\.\d+\.\d+)", first_line)
            if match:
                version_str = match.group(1)
                ver_tuple = tuple(map(int, version_str.split('.')))
                if ver_tuple >= (3, 25, 0):
                    report.ok(f"CMake {version_str} ({cmake_bin})")
                else:
                    report.fail(f"CMake {version_str} found, but 3.25.0+ is required ({cmake_bin})")
            else:
                report.ok(f"CMake found ({cmake_bin}): {first_line}")
        except Exception as e:
            report.fail(f"CMake found at {cmake_bin} but failed to run: {e}")
    else:
        report.fail("CMake executable not found in PATH")

    # CTest check
    ctest_bin = shutil.which("ctest")
    if ctest_bin:
        report.ok(f"CTest ({ctest_bin})")
    else:
        report.warn("CTest executable not found in PATH")

    # Ninja / Build tool check
    ninja_bin = shutil.which("ninja")
    if ninja_bin:
        report.ok(f"Ninja build system ({ninja_bin})")
    else:
        # Check standard Qt Ninja locations on Windows
        qt_ninja = Path("D:/dev/Qt/Tools/Ninja/ninja.exe")
        if qt_ninja.exists():
            report.ok(f"Ninja build system found at Qt path ({qt_ninja})")
        else:
            report.warn("Ninja build system not found in PATH or standard Qt location")

    # Git check
    git_bin = shutil.which("git")
    if git_bin:
        report.ok(f"Git CLI ({git_bin})")
    else:
        report.warn("Git CLI not found in PATH")


def check_compiler(report: DoctorReport):
    print("\n" + header_str("C++ Compiler Environment"))
    if sys.platform == "win32":
        cl_bin = shutil.which("cl")
        if cl_bin:
            report.ok(f"MSVC Compiler cl.exe ({cl_bin})")
        else:
            vswhere_exe = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            vs_path = None
            if vswhere_exe.exists():
                try:
                    res = subprocess.run([
                        str(vswhere_exe), "-latest", "-products", "*",
                        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-property", "installationPath"
                    ], capture_output=True, text=True, check=True)
                    vs_path = res.stdout.strip()
                except Exception:
                    pass
            
            if vs_path:
                report.ok(f"Visual Studio C++ Workload found at {vs_path}")
                report.warn("cl.exe not in current PATH; build.py will auto-load VsDevShell for this installation.")
            else:
                report.warn("MSVC compiler cl.exe not found in current PATH.")
                report.warn("Run from an MSVC Developer Shell or build script will auto-detect Launch-VsDevShell.ps1.")

        gpp_bin = shutil.which("g++")
        clang_bin = shutil.which("clang++")
        if gpp_bin:
            report.ok(f"GCC Compiler g++ ({gpp_bin})")
        if clang_bin:
            report.ok(f"Clang Compiler clang++ ({clang_bin})")
    else:
        cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        if cxx:
            report.ok(f"C++ Compiler ({cxx})")
        else:
            report.fail("No C++ compiler (g++ or clang++) found in PATH or CXX environment variable")


def check_qt6(report: DoctorReport, repo_root: Path):
    print("\n" + header_str("Qt 6 SDK"))
    candidates = []

    # Check environment variables
    for env_var in ["CMAKE_PREFIX_PATH", "Qt6_DIR", "QT_DIR"]:
        val = os.environ.get(env_var)
        if val:
            for p in val.split(os.pathsep):
                if p:
                    candidates.append(Path(p))

    # Known standard paths
    candidates.extend([
        Path("D:/dev/Qt/6.11.2/msvc2022_64"),
        Path("C:/Qt/6.11.2/msvc2022_64"),
        Path("D:/Qt/6.11.2/msvc2022_64"),
        Path("/usr/lib/qt6"),
        Path("/usr/include/qt6"),
    ])

    found_qt = None
    for cand in candidates:
        if cand.exists():
            # Verify Qt6 headers or cmake config
            cmake_qt6 = cand / "lib" / "cmake" / "Qt6"
            include_qt = cand / "include"
            if cmake_qt6.exists() or include_qt.exists() or (cand / "bin").exists():
                found_qt = cand
                break

    if found_qt:
        report.ok(f"Qt 6 SDK found at {found_qt}")
        # Check required components
        windeployqt = shutil.which("windeployqt") or (found_qt / "bin" / "windeployqt.exe")
        if isinstance(windeployqt, Path) and windeployqt.exists():
            report.ok(f"Qt Deployment Tool windeployqt ({windeployqt})")
        elif windeployqt and not isinstance(windeployqt, Path):
            report.ok(f"Qt Deployment Tool windeployqt ({windeployqt})")
        else:
            report.warn("windeployqt not found (automatic runtime deployment may fail)")
    else:
        report.fail("Qt 6 SDK not found in CMAKE_PREFIX_PATH, Qt6_DIR, or default paths (D:/dev/Qt/6.11.2/msvc2022_64)")


def check_ffmpeg(report: DoctorReport):
    print("\n" + header_str("FFmpeg SDK & CLI"))

    ffmpeg_root_env = os.environ.get("FFMPEG_ROOT")
    candidates = []
    if ffmpeg_root_env:
        candidates.append(Path(ffmpeg_root_env))

    candidates.extend([
        Path("D:/dev/ffmpeg-master-latest-win64-gpl-shared"),
        Path("C:/ffmpeg-shared"),
        Path("D:/ffmpeg-shared"),
        Path("/usr/local"),
        Path("/usr"),
    ])

    found_sdk = None
    for cand in candidates:
        inc = cand / "include"
        if (inc / "libavformat" / "avformat.h").exists():
            found_sdk = cand
            break

    if found_sdk:
        report.ok(f"FFmpeg SDK headers found at {found_sdk / 'include'}")
        # Check required libraries
        lib_dir = found_sdk / "lib"
        required_libs = ["avformat", "avcodec", "avutil", "swscale", "swresample"]
        missing_libs = []
        if lib_dir.exists():
            for lib_name in required_libs:
                # look for .lib or .a or .so or .dylib
                matches = list(lib_dir.glob(f"*{lib_name}*"))
                if not matches:
                    missing_libs.append(lib_name)
        if not missing_libs:
            report.ok(f"FFmpeg SDK libraries ({', '.join(required_libs)}) verified")
        else:
            report.warn(f"Some FFmpeg libraries were not found in {lib_dir}: {', '.join(missing_libs)}")
    else:
        report.fail("FFmpeg SDK not found (libavformat/avformat.h). Set FFMPEG_ROOT environment variable.")

    # FFmpeg CLI check (needed for test fixture generation)
    ffmpeg_cli = shutil.which("ffmpeg")
    if not ffmpeg_cli and found_sdk:
        cli_in_sdk = found_sdk / "bin" / "ffmpeg.exe"
        if cli_in_sdk.exists():
            ffmpeg_cli = str(cli_in_sdk)

    if ffmpeg_cli:
        report.ok(f"FFmpeg CLI fixture generator ({ffmpeg_cli})")
    else:
        report.warn("FFmpeg CLI executable not found (synthetic media integration tests will be skipped)")


def check_presets(report: DoctorReport, repo_root: Path):
    print("\n" + header_str("CMake Presets"))
    presets_file = repo_root / "CMakePresets.json"
    if not presets_file.exists():
        report.fail(f"CMakePresets.json not found at {presets_file}")
        return

    try:
        with open(presets_file, "r", encoding="utf-8") as f:
            data = json.load(f)

        config_presets = [p.get("name") for p in data.get("configurePresets", []) if "name" in p]
        build_presets = [p.get("name") for p in data.get("buildPresets", []) if "name" in p]
        test_presets = [p.get("name") for p in data.get("testPresets", []) if "name" in p]

        report.ok(f"CMakePresets.json valid (Configure: {config_presets}, Build: {build_presets}, Test: {test_presets})")
    except Exception as e:
        report.fail(f"Failed to parse CMakePresets.json: {e}")


def main():
    repo_root = get_repo_root()
    print(colorize(f"VidScope Environment Doctor", "35;1"))
    print(f"Repository Root: {repo_root}\n")

    report = DoctorReport()
    check_python(report)
    check_build_tools(report)
    check_compiler(report)
    check_qt6(report, repo_root)
    check_ffmpeg(report)
    check_presets(report, repo_root)

    print("\n" + header_str("Diagnostic Summary"))
    print(f"Passed Checks:   {report.ok_count}")
    print(f"Warnings:        {report.warn_count}")
    print(f"Failures:        {report.fail_count}")

    if report.fail_count == 0:
        print("\n" + colorize("System status: READY to build VidScope!", "32;1"))
        return 0
    else:
        print("\n" + colorize("System status: ISSUES DETECTED. Please resolve failed items before building.", "31;1"))
        return 1


if __name__ == "__main__":
    sys.exit(main())
