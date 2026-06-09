#!/usr/bin/env python3
"""
Release Consistency Checker for Angband

Checks that source files, headers, test files are registered across all build
systems (Makefile.src, CMakeLists.txt, Windows VS project) and that document
references are valid.
"""

import os
import re
import sys
import glob
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Dict, List, Set, Tuple

class Colors:
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    BOLD = "\033[1m"
    RESET = "\033[0m"


class ConsistencyChecker:
    def __init__(self, repo_root: str):
        self.repo_root = Path(repo_root).resolve()
        self.src_dir = self.repo_root / "src"
        self.errors: List[str] = []
        self.warnings: List[str] = []
        self.infos: List[str] = []

        self.EXCLUDED_SRC_DIRS = {
            "tests",
            "win/include",
            "win/dll",
            "win/lib",
            "doc",
            "cmake",
        }

        self.NOT_IN_MAKEFILE_SRC_MAIN = {
            "main-cocoa.m",
            "main-ibm.c",
            "main-nds.c",
            "main-nds-arm7.c",
            "main-xxx.c",
            "cocoa/AppDelegate.m",
            "cocoa/snd-cocoa.m",
        }

        self.NOT_IN_CMAKE = {
            "main-ibm.c",
            "main-nds.c",
            "main-nds-arm7.c",
            "main-xxx.c",
            "main-cocoa.m",
            "cocoa/AppDelegate.m",
            "cocoa/snd-cocoa.m",
        }

        self.NOT_IN_VS = {
            "main.c",
            "main-gcu.c",
            "main-sdl.c",
            "main-sdl2.c",
            "main-x11.c",
            "main-spoil.c",
            "main-stats.c",
            "main-test.c",
            "main-cocoa.m",
            "main-ibm.c",
            "main-nds.c",
            "main-nds-arm7.c",
            "main-xxx.c",
            "snd-sdl.c",
        }

        self.NDS_SOURCES = {
            "nds/dlmalloc.c",
            "nds/nds-buttons.c",
            "nds/nds-draw.c",
            "nds/nds-event.c",
            "nds/nds-font-3x8.c",
            "nds/nds-font-5x8.c",
            "nds/nds-keyboard.c",
            "nds/nds-screenkeys.c",
            "nds/nds-slot2-ram.c",
            "nds/nds-slot2-virt.c",
        }

        self.CORE_HEADERS_ONLY_IN_MAKEFILE = True

    def log_error(self, msg: str):
        self.errors.append(msg)
        print(f"{Colors.RED}ERROR: {msg}{Colors.RESET}")

    def log_warning(self, msg: str):
        self.warnings.append(msg)
        print(f"{Colors.YELLOW}WARNING: {msg}{Colors.RESET}")

    def log_info(self, msg: str):
        self.infos.append(msg)
        print(f"{Colors.BLUE}INFO: {msg}{Colors.RESET}")

    def _is_excluded_dir(self, rel_path: str) -> bool:
        norm = os.path.normpath(rel_path)
        for excl in self.EXCLUDED_SRC_DIRS:
            if norm == excl or norm.startswith(excl + os.sep):
                return True
        return False

    def scan_source_files(self) -> Set[str]:
        sources: Set[str] = set()
        for root, dirs, files in os.walk(self.src_dir):
            rel_root = os.path.relpath(root, self.src_dir)
            dirs[:] = [d for d in dirs if not self._is_excluded_dir(os.path.join(rel_root, d))]
            for f in files:
                if f.endswith((".c", ".m")) and not f.startswith("."):
                    rel = os.path.normpath(os.path.join(rel_root, f))
                    if rel_root == ".":
                        rel = f
                    sources.add(rel)
        return sources

    def parse_makefile_src(self) -> Tuple[Set[str], Set[str]]:
        makefile_path = self.src_dir / "Makefile.src"
        sources: Set[str] = set()
        headers: Set[str] = set()

        content = makefile_path.read_text()

        header_match = re.search(r"HEADERS\s*=\s*\\\n((?:[^\n]+\\\n)*[^\n]+)", content)
        if header_match:
            for line in header_match.group(1).strip().split("\\\n"):
                h = line.strip().rstrip("\\").strip()
                if h:
                    headers.add(h)

        for varname in ["ZFILES", "ANGFILES0"]:
            m = re.search(rf"{varname}\s*=\s*\\\n((?:[^\n]+\\\n)*[^\n]+)", content)
            if m:
                for line in m.group(1).strip().split("\\\n"):
                    obj = line.strip().rstrip("\\").strip()
                    if obj:
                        src = obj.replace(".o", ".c")
                        sources.add(src)

        sources.add("buildid.c")

        for varname in [
            "BASEMAINFILES",
            "GCUMAINFILES",
            "SDL2MAINFILES",
            "SDLMAINFILES",
            "SNDSDLFILES",
            "TESTMAINFILES",
            "WINMAINFILES",
            "X11MAINFILES",
            "STATSMAINFILES",
            "SPOILMAINFILES",
        ]:
            m = re.search(rf"{varname}\s*=\s*\\?\n?((?:[^\n]+\\\n)*[^\n]+)", content)
            if m:
                for line in m.group(1).strip().split("\\\n"):
                    parts = line.strip().rstrip("\\").strip().split()
                    for obj in parts:
                        if not obj or "$" in obj:
                            continue
                        src = obj.replace(".o", ".c").replace(".res", ".rc")
                        sources.add(src)

        return sources, headers

    def parse_cmakelists(self) -> Tuple[Set[str], Set[str]]:
        cmake_path = self.repo_root / "CMakeLists.txt"
        content = cmake_path.read_text()
        sources: Set[str] = set()
        tests: Set[str] = set()

        for obj_lib_match in re.finditer(
            r"add_library\(\w+Lib(?:rary)? OBJECT\s*(?:EXCLUDE_FROM_ALL\s*)?\n((?:\s*src/[^\s]+\s*\n)+)\)",
            content,
        ):
            for line in obj_lib_match.group(1).strip().split("\n"):
                s = line.strip()
                if s.startswith("src/"):
                    sources.add(s[4:])

        core_match = re.search(
            r"add_library\(OurCoreLib OBJECT\s*\n((?:\s*src/[^\s]+\s*\n)+)\)",
            content,
        )
        if core_match:
            for line in core_match.group(1).strip().split("\n"):
                s = line.strip()
                if s.startswith("src/"):
                    sources.add(s[4:])

        for m in re.finditer(r"src/([A-Za-z0-9_\-/.]+\.(?:c|m))\b", content):
            p = m.group(1)
            if "/tests/" not in p and "/cmake/" not in p:
                sources.add(p)

        test_match = re.search(
            r"set\(ANGBAND_TEST_CASE_SOURCES\s*\n((?:\s*[^\s]+\s*\n)+)\)",
            content,
        )
        if test_match:
            for line in test_match.group(1).strip().split("\n"):
                t = line.strip()
                if t and not t.startswith("#"):
                    tests.add(t)

        return sources, tests

    def parse_vcxproj(self) -> Tuple[Set[str], Set[str]]:
        vcx_path = self.src_dir / "win" / "vs2019" / "Angband.vcxproj"
        sources: Set[str] = set()
        headers: Set[str] = set()

        ns = {"msb": "http://schemas.microsoft.com/developer/msbuild/2003"}
        tree = ET.parse(str(vcx_path))
        root = tree.getroot()

        for cl in root.iter(f"{{{ns['msb']}}}ClCompile"):
            inc = cl.get("Include")
            if inc and inc.startswith("src\\"):
                rel = inc[4:].replace("\\", "/")
                sources.add(rel)

        for cl in root.iter(f"{{{ns['msb']}}}ClInclude"):
            inc = cl.get("Include")
            if inc and inc.startswith("src\\") and "win\\include\\" not in inc:
                rel = inc[4:].replace("\\", "/")
                headers.add(rel)

        return sources, headers

    def scan_header_files(self) -> Set[str]:
        headers: Set[str] = set()
        for root, dirs, files in os.walk(self.src_dir):
            rel_root = os.path.relpath(root, self.src_dir)
            dirs[:] = [d for d in dirs if not self._is_excluded_dir(os.path.join(rel_root, d))]
            for f in files:
                if f.endswith(".h") and not f.startswith("."):
                    rel = os.path.normpath(os.path.join(rel_root, f))
                    if rel_root == ".":
                        rel = f
                    headers.add(rel)
        return headers

    def parse_test_suites_mk(self) -> Set[str]:
        tests_makefile = self.src_dir / "tests" / "Makefile"
        content = tests_makefile.read_text()
        suite_paths: List[str] = []

        suites_match = re.search(r"SUITES\s*=\s*\\\n((?:[^\n]+\\\n)*[^\n]+)", content)
        if suites_match:
            for line in suites_match.group(1).strip().split("\\\n"):
                s = line.strip().rstrip("\\").strip()
                if s:
                    suite_paths.append(s)

        tests: Set[str] = set()
        for suite_rel in suite_paths:
            suite_path = self.src_dir / "tests" / suite_rel
            if suite_path.exists():
                suite_content = suite_path.read_text()
                for m in re.finditer(r"TESTPROGS\s*\+=\s*((?:[^\\\n]+\\?\n?)+)", suite_content):
                    raw = m.group(1)
                    parts = re.split(r"\s+|\\\n", raw)
                    for p in parts:
                        p = p.strip()
                        if p and not p.startswith("#"):
                            tests.add(p + ".c")
        return tests

    def scan_test_sources(self) -> Set[str]:
        tests: Set[str] = set()
        tests_dir = self.src_dir / "tests"
        for root, dirs, files in os.walk(tests_dir):
            rel_root = os.path.relpath(root, tests_dir)
            if rel_root == "win-stub":
                continue
            for f in files:
                if f.endswith(".c") and f not in ("test-utils.c", "unit-test.c"):
                    rel = os.path.normpath(os.path.join(rel_root, f))
                    if rel_root == ".":
                        rel = f
                    if not rel.startswith("win-stub"):
                        tests.add(rel)
        return tests

    def scan_documentation_files(self) -> List[Path]:
        docs: List[Path] = []
        for pat in ["README.md", "CONTRIBUTING.md", "docs/**/*.rst", "src/doc/**/*.rst", "src/doc/**/*.md", "src/doc/**/*.txt"]:
            docs.extend(self.repo_root.glob(pat))
        return docs

    def check_documentation_references(self, docs: List[Path]):
        for doc in docs:
            if doc.suffix not in (".md", ".rst", ".txt"):
                continue
            try:
                content = doc.read_text(errors="ignore")
            except Exception:
                continue
            doc_dir = doc.parent

            for m in re.finditer(r"[`(]([A-Za-z0-9_.\-/\\]+?\.[a-zA-Z0-9]{1,4})[`)']", content):
                ref = m.group(1)
                if ref.startswith("http://") or ref.startswith("https://") or ref.startswith("git://"):
                    continue
                if not ("/" in ref or "\\" in ref):
                    continue
                if re.match(r"^[A-Z]:", ref):
                    continue
                clean_ref = ref.lstrip("/")
                candidate = self.repo_root / clean_ref
                if not candidate.exists():
                    candidate2 = doc_dir / ref
                    if not candidate2.exists():
                        candidate3 = doc_dir / clean_ref
                        if not candidate3.exists():
                            self.log_warning(
                                f"Doc {doc.relative_to(self.repo_root)}: "
                                f"reference '{ref}' does not exist"
                            )

    def check_sources(self):
        self.log_info("Scanning actual source files in src/ ...")
        actual_sources = self.scan_source_files()
        self.log_info(f"Found {len(actual_sources)} source files")

        self.log_info("Parsing Makefile.src ...")
        mk_sources, mk_headers = self.parse_makefile_src()
        self.log_info(f"Makefile.src: {len(mk_sources)} sources, {len(mk_headers)} headers")

        self.log_info("Parsing CMakeLists.txt ...")
        cmake_sources, cmake_tests = self.parse_cmakelists()
        self.log_info(f"CMakeLists.txt: {len(cmake_sources)} core sources, {len(cmake_tests)} test cases")

        self.log_info("Parsing VS project file ...")
        vs_sources, vs_headers = self.parse_vcxproj()
        self.log_info(f"VS project: {len(vs_sources)} sources, {len(vs_headers)} headers")

        for src in sorted(actual_sources):
            if src in self.NDS_SOURCES:
                continue
            if src in self.NOT_IN_MAKEFILE_SRC_MAIN or src.startswith("cocoa/") or src.startswith("nds/"):
                pass
            elif src not in mk_sources:
                self.log_error(f"Makefile.src: source '{src}' is not registered")

            if src in self.NOT_IN_CMAKE or src.startswith("cocoa/") or src.startswith("nds/"):
                pass
            elif src not in cmake_sources:
                self.log_error(f"CMakeLists.txt: source '{src}' is not registered")

            if src in self.NOT_IN_VS or src.startswith("cocoa/") or src.startswith("nds/") or src.startswith("sdl2/") or src.startswith("stats/") or src == "snd-sdl.c":
                pass
            elif src not in vs_sources:
                self.log_error(f"Angband.vcxproj: source '{src}' is not registered")

        for src in sorted(mk_sources):
            if src.endswith(".rc"):
                continue
            if not (self.src_dir / src).exists():
                self.log_error(f"Makefile.src: registered source '{src}' does not exist on disk")

        for src in sorted(cmake_sources):
            if not (self.src_dir / src).exists():
                self.log_error(f"CMakeLists.txt: registered source '{src}' does not exist on disk")

        for src in sorted(vs_sources):
            if not (self.src_dir / src).exists():
                self.log_error(f"Angband.vcxproj: registered source '{src}' does not exist on disk")

    def check_headers(self):
        self.log_info("Scanning actual header files in src/ ...")
        actual_headers = self.scan_header_files()
        self.log_info(f"Found {len(actual_headers)} header files")

        mk_sources, mk_headers = self.parse_makefile_src()
        vs_sources, vs_headers = self.parse_vcxproj()

        for h in sorted(actual_headers):
            if h.startswith("win/include/"):
                continue
            if h.startswith("cocoa/") or h.startswith("nds/"):
                continue
            if h.startswith("sdl2/") or h.startswith("stats/"):
                continue
            if h not in vs_headers:
                self.log_warning(f"Angband.vcxproj: header '{h}' is not registered")

        for h in sorted(mk_headers):
            if not (self.src_dir / h).exists():
                self.log_error(f"Makefile.src: registered header '{h}' does not exist on disk")

        for h in sorted(vs_headers):
            if not (self.src_dir / h).exists():
                self.log_error(f"Angband.vcxproj: registered header '{h}' does not exist on disk")

        self._check_orphan_headers(actual_headers, mk_sources)

    def _check_orphan_headers(self, headers: Set[str], sources: Set[str]):
        for h in sorted(headers):
            base = h.replace(".h", "")
            if h.startswith("list-") or h.startswith("win/") or h.startswith("cocoa/") or h.startswith("nds/") or h.startswith("sdl2/") or h.startswith("stats/"):
                continue
            if base + ".c" not in sources and base + ".m" not in sources:
                basename = os.path.basename(h)
                has_include = False
                for root, _, files in os.walk(self.src_dir):
                    for f in files:
                        if f.endswith((".c", ".h", ".m")):
                            try:
                                content = (Path(root) / f).read_text(errors="ignore")
                                if re.search(rf'#include\s+[<"]{re.escape(basename)}[>"]', content):
                                    has_include = True
                                    break
                            except Exception:
                                pass
                    if has_include:
                        break
                if not has_include:
                    self.log_warning(f"Potentially orphan header: '{h}' (no matching source and no direct includes found)")

    def check_tests(self):
        self.log_info("Scanning actual test sources in src/tests/ ...")
        actual_tests = self.scan_test_sources()
        self.log_info(f"Found {len(actual_tests)} test source files")

        self.log_info("Parsing test suite Makefiles (suite.mk) ...")
        mk_tests = self.parse_test_suites_mk()
        self.log_info(f"suite.mk files: {len(mk_tests)} test cases")

        cmake_sources, cmake_tests = self.parse_cmakelists()

        for t in sorted(actual_tests):
            if t not in mk_tests:
                self.log_error(f"Test suite Makefiles: test '{t}' not registered in any suite.mk")
            if t not in cmake_tests:
                self.log_error(f"CMakeLists.txt: test '{t}' not registered in ANGBAND_TEST_CASE_SOURCES")

        for t in sorted(mk_tests):
            test_path = self.src_dir / "tests" / t
            if not test_path.exists():
                self.log_error(f"suite.mk: registered test '{t}' does not exist on disk")

        for t in sorted(cmake_tests):
            test_path = self.src_dir / "tests" / t
            if not test_path.exists():
                self.log_error(f"CMakeLists.txt: registered test '{t}' does not exist on disk")

    def check_docs(self):
        self.log_info("Scanning documentation files ...")
        docs = self.scan_documentation_files()
        self.log_info(f"Found {len(docs)} documentation files")
        self.check_documentation_references(docs)

    def run_all(self) -> bool:
        print(f"\n{Colors.BOLD}{'='*70}{Colors.RESET}")
        print(f"{Colors.BOLD}Angband Release Consistency Checker{Colors.RESET}")
        print(f"{Colors.BOLD}{'='*70}{Colors.RESET}\n")

        self.check_sources()
        print()
        self.check_headers()
        print()
        self.check_tests()
        print()
        self.check_docs()

        print(f"\n{Colors.BOLD}{'='*70}{Colors.RESET}")
        print(f"{Colors.BOLD}Summary:{Colors.RESET}")
        print(f"  {Colors.RED if self.errors else Colors.GREEN}Errors:   {len(self.errors)}{Colors.RESET}")
        print(f"  {Colors.YELLOW if self.warnings else Colors.GREEN}Warnings: {len(self.warnings)}{Colors.RESET}")
        print(f"  Infos:    {len(self.infos)}")
        print(f"{Colors.BOLD}{'='*70}{Colors.RESET}")

        if self.errors:
            print(f"\n{Colors.RED}{Colors.BOLD}CHECK FAILED: Found {len(self.errors)} error(s){Colors.RESET}")
            return False
        else:
            print(f"\n{Colors.GREEN}{Colors.BOLD}CHECK PASSED{Colors.RESET}", end="")
            if self.warnings:
                print(f" (with {len(self.warnings)} warning(s))")
            else:
                print()
            return True


def main():
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent

    checker = ConsistencyChecker(str(repo_root))
    ok = checker.run_all()

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
