#!/usr/bin/env python3
"""
Release Consistency Checker for Angband

Checks that source files, headers, test files are registered across all build
systems (Makefile.src, CMakeLists.txt, Windows VS project, Makefile.nmake,
Makefile.osx) and that document references are valid.

Default mode is strict: any warning not in the allowlist will cause the check
to fail. Use --lenient to allow warnings.
"""

import argparse
import json
import os
import re
import sys
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
    VALID_CATEGORIES = {
        "vs_missing_headers",
        "orphan_headers",
        "missing_makefile_inc_deps",
        "doc_missing_references",
        "doc_missing_code_paths",
        "vs_filters_missing_sources",
        "vs_filters_missing_headers",
        "vs_project_missing_filters_sources",
        "vs_project_missing_filters_headers",
    }

    def __init__(self, repo_root: str, max_warnings: int = 0):
        self.repo_root = Path(repo_root).resolve()
        self.src_dir = self.repo_root / "src"
        self.max_warnings = max_warnings
        self.errors: List[str] = []
        self.warnings: List[str] = []
        self.infos: List[str] = []
        self.suppressed: List[str] = []

        self.allowlist_entries: Dict[Tuple[str, str], Dict] = {}
        self.allowlist_raw: Dict = {}
        self.used_allowlist: Dict[Tuple[str, str], int] = {}

        self._load_allowlist()

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

        self.OSX_EXTRA_SOURCES = {
            "cocoa/snd-cocoa.m",
            "main-cocoa.m",
        }

    def _load_allowlist(self):
        allowlist_path = self.repo_root / "scripts" / "check-consistency.allowlist.json"
        self.allowlist_entries = {}
        self.allowlist_raw = {}

        if not allowlist_path.exists():
            return

        try:
            with open(allowlist_path) as f:
                self.allowlist_raw = json.load(f)
        except Exception as e:
            self.log_error(f"Allowlist: failed to parse JSON: {e}")
            return

        entries = self.allowlist_raw.get("entries", [])
        if not isinstance(entries, list):
            self.log_error("Allowlist: 'entries' must be a list")
            return

        for idx, entry in enumerate(entries):
            if not isinstance(entry, dict):
                self.log_error(f"Allowlist entry #{idx}: must be an object")
                continue

            category = entry.get("category")
            item = entry.get("item")
            reason = entry.get("reason")
            owner = entry.get("owner")

            missing = []
            if not category:
                missing.append("category")
            if not item:
                missing.append("item")
            if not reason:
                missing.append("reason")
            if not owner:
                missing.append("owner")

            if missing:
                self.log_error(
                    f"Allowlist entry #{idx} (item='{item}'): missing required fields: {', '.join(missing)}"
                )
                continue

            if category not in self.VALID_CATEGORIES:
                self.log_error(
                    f"Allowlist entry #{idx}: invalid category '{category}'. "
                    f"Valid categories: {sorted(self.VALID_CATEGORIES)}"
                )
                continue

            key = (category, item)
            if key in self.allowlist_entries:
                self.log_error(
                    f"Allowlist: duplicate entry for category='{category}', item='{item}'"
                )
                continue

            if not re.search(r"\.[a-zA-Z0-9]{1,5}$", item):
                self.log_error(
                    f"Allowlist entry #{idx}: item '{item}' is overly broad (no file extension). "
                    f"Specify the exact file path with extension (e.g. 'build-capabilities.h', "
                    f"'subdir/file.h', 'lib/user/borg.txt')."
                )
                continue

            self.allowlist_entries[key] = entry

    def _is_allowed(self, category: str, item: str) -> bool:
        if (category, item) in self.allowlist_entries:
            self.used_allowlist[(category, item)] = self.used_allowlist.get((category, item), 0) + 1
            return True
        basename = os.path.basename(item)
        if (category, basename) in self.allowlist_entries:
            self.used_allowlist[(category, basename)] = self.used_allowlist.get((category, basename), 0) + 1
            return True
        return False

    def _check_allowlist_health(self):
        all_keys = set(self.allowlist_entries.keys())
        used_keys = set(self.used_allowlist.keys())
        unused = all_keys - used_keys

        if unused:
            for category, item in sorted(unused):
                entry = self.allowlist_entries.get((category, item), {})
                owner = entry.get("owner", "unknown")
                self.log_error(
                    f"Allowlist: unused entry category='{category}', item='{item}' "
                    f"(owner: {owner}). Remove stale entries to keep the allowlist clean."
                )

        for (category, item), count in self.used_allowlist.items():
            entry = self.allowlist_entries.get((category, item), {})
            allow_multiple = entry.get("allow_multiple", False)
            if count > 1 and not allow_multiple:
                self.log_error(
                    f"Allowlist: entry category='{category}', item='{item}' matched {count} "
                    f"warnings but does not have 'allow_multiple': true. Either set "
                    f"allow_multiple explicitly or split into more precise entries."
                )

    def log_error(self, msg: str):
        self.errors.append(msg)
        print(f"{Colors.RED}ERROR: {msg}{Colors.RESET}")

    def warn(self, category: str, item: str, msg: str):
        if self._is_allowed(category, item):
            self.suppressed.append(msg)
            return
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

    def parse_makefile_nmake(self) -> Set[str]:
        nmake_path = self.src_dir / "Makefile.nmake"
        sources: Set[str] = set()
        content = nmake_path.read_text()

        if "include Makefile.src" not in content:
            self.log_error("Makefile.nmake: does not include Makefile.src")

        objs_match = re.search(r"OBJS\s*=\s*\$\(([^)]+)\)\s*\$\(([^)]+)\)\s*\$\(([^)]+)\)", content)
        if objs_match:
            vars_used = [objs_match.group(1), objs_match.group(2), objs_match.group(3)]
            expected = {"ANGFILES", "ZFILES", "WINMAINFILES"}
            if set(vars_used) != expected:
                msg = (
                    f"Makefile.nmake: OBJS uses unexpected variables {vars_used}, "
                    f"expected {sorted(expected)}"
                )
                self.warnings.append(msg)
                print(f"{Colors.YELLOW}WARNING: {msg}{Colors.RESET}")
        else:
            msg = "Makefile.nmake: could not parse OBJS variable"
            self.warnings.append(msg)
            print(f"{Colors.YELLOW}WARNING: {msg}{Colors.RESET}")

        mk_sources, _ = self.parse_makefile_src()
        for s in mk_sources:
            if s.startswith("win/") or s in self.NOT_IN_VS:
                if s not in ("main-gcu.c", "main-sdl.c", "main-sdl2.c", "main-x11.c",
                             "main-spoil.c", "main-stats.c", "main-test.c", "main.c",
                             "main-cocoa.m", "main-nds.c", "main-nds-arm7.c", "main-ibm.c",
                             "main-xxx.c", "snd-sdl.c"):
                    sources.add(s)
            elif not s.startswith("cocoa/") and not s.startswith("nds/"):
                sources.add(s)

        return sources

    def parse_makefile_osx(self) -> Tuple[Set[str], Set[str]]:
        osx_path = self.src_dir / "Makefile.osx"
        content = osx_path.read_text()
        sources: Set[str] = set()
        extra_sources: Set[str] = set()

        if "include Makefile.inc" not in content:
            self.log_error("Makefile.osx: does not include Makefile.inc")

        mk_sources, _ = self.parse_makefile_src()
        for s in mk_sources:
            if not s.startswith("win/") and not s.startswith("nds/"):
                if s not in ("main-ibm.c", "main-test.c", "main-spoil.c", "main-stats.c"):
                    sources.add(s)

        objs_match = re.search(r"OBJS\s*=\s*\$\(BASEOBJS\)\s+(\S+)", content)
        if objs_match:
            extra = objs_match.group(1)
            extra_src = extra.replace(".o", ".m").replace(".o", ".c")
            extra_sources.add(extra_src)
            if extra_src not in ("cocoa/snd-cocoa.m",):
                msg = f"Makefile.osx: unexpected extra OBJS entry: {extra}"
                self.warnings.append(msg)
                print(f"{Colors.YELLOW}WARNING: {msg}{Colors.RESET}")

        osx_objs_match = re.search(r"OSX_OBJS\s*=\s*(\S+)", content)
        if osx_objs_match:
            extra = osx_objs_match.group(1)
            extra_src = extra.replace(".o", ".m").replace(".o", ".c")
            extra_sources.add(extra_src)
            if extra_src not in ("main-cocoa.m",):
                msg = f"Makefile.osx: unexpected OSX_OBJS entry: {extra}"
                self.warnings.append(msg)
                print(f"{Colors.YELLOW}WARNING: {msg}{Colors.RESET}")

        return sources, extra_sources

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

    def parse_vcxproj_filters(self) -> Tuple[Set[str], Set[str]]:
        filters_path = self.src_dir / "win" / "vs2019" / "Angband.vcxproj.filters"
        sources: Set[str] = set()
        headers: Set[str] = set()

        ns = {"msb": "http://schemas.microsoft.com/developer/msbuild/2003"}
        tree = ET.parse(str(filters_path))
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

    def parse_makefile_inc_deps(self) -> Dict[str, Set[str]]:
        inc_path = self.src_dir / "Makefile.inc"
        content = inc_path.read_text()
        deps: Dict[str, Set[str]] = {}

        if "include Makefile.src" not in content:
            self.log_error("Makefile.inc: does not include Makefile.src")

        baseobjs_match = re.search(r"BASEOBJS\s*:=\s*\$\(([^)]+)\)\s*\$\(([^)]+)\)", content)
        if baseobjs_match:
            vars_used = [baseobjs_match.group(1), baseobjs_match.group(2)]
            expected = {"ANGFILES", "ZFILES"}
            if set(vars_used) != expected:
                msg = (
                    f"Makefile.inc: BASEOBJS uses unexpected variables {vars_used}, "
                    f"expected {sorted(expected)}"
                )
                self.warnings.append(msg)
                print(f"{Colors.YELLOW}WARNING: {msg}{Colors.RESET}")

        dep_pattern = re.compile(r"^\./([A-Za-z0-9_\-/]+)\.o:\s*([A-Za-z0-9_\-/]+\.[cm])\s*(.*?)(?=\n\./|\Z)", re.MULTILINE | re.DOTALL)
        for m in dep_pattern.finditer(content):
            obj_base = m.group(1)
            src_file = m.group(2)
            dep_text = m.group(3)
            src_key = obj_base + ".c"

            if "/" not in src_file and not src_file.startswith("./"):
                src_key = src_file

            dep_files: Set[str] = set()
            all_dep_text = src_file + " " + dep_text.replace("\\\n", " ")
            for dep in re.findall(r"([A-Za-z0-9_\-/.]+\.h)", all_dep_text):
                dep_clean = dep.strip()
                if dep_clean.startswith("/"):
                    dep_clean = dep_clean[1:]
                while "../" in dep_clean:
                    dep_clean = re.sub(r"^[A-Za-z0-9_\-]+/\.\./", "", dep_clean)
                    if dep_clean.startswith("../"):
                        dep_clean = dep_clean[3:]
                if dep_clean:
                    dep_files.add(dep_clean)

            deps[src_key] = dep_files

        return deps

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
                            self.warn(
                                "doc_missing_references",
                                ref,
                                f"Doc {doc.relative_to(self.repo_root)}: "
                                f"reference '{ref}' does not exist"
                            )

    def check_documentation_code_examples(self, docs: List[Path]):
        for doc in docs:
            if doc.suffix not in (".md", ".rst", ".txt"):
                continue
            try:
                content = doc.read_text(errors="ignore")
            except Exception:
                continue
            doc_dir = doc.parent

            code_blocks: List[str] = []
            if doc.suffix == ".md":
                code_blocks = re.findall(r"```(?:bash|sh|shell)?\n(.*?)```", content, re.DOTALL)
                code_blocks += re.findall(r"`([^`\n]{3,})`", content)
            elif doc.suffix == ".rst":
                code_blocks = re.findall(r"\.\. code-block::\s*(?:bash|sh|shell)?\s*\n((?:\s+[^\n]+\n?)+)", content, re.IGNORECASE)
                code_blocks += re.findall(r"::\s*\n((?:\s+[^\n]+\n?)+)", content)

            for block in code_blocks:
                for line in block.split("\n"):
                    line = line.strip()
                    if not line or line.startswith("#") or line.startswith("//"):
                        continue

                    path_matches = re.findall(r"(?:^|\s)([A-Za-z0-9_.\-/]{3,}/[A-Za-z0-9_.\-/]+)", line)
                    for ref in path_matches:
                        ref = ref.rstrip(";)\"'")
                        if ref.startswith("http://") or ref.startswith("https://") or ref.startswith("git://"):
                            continue
                        if ref.startswith("/") or ref.startswith(".."):
                            continue
                        if "/" not in ref:
                            continue
                        if ref.endswith((".exe", ".app", ".o", ".obj", ".a", ".so", ".dylib", ".dll")):
                            continue
                        if "/bin/" in ref or "/build/" in ref or "/_doxygen" in ref:
                            continue
                        if ref.startswith("build/") or ref.startswith("bin/"):
                            continue
                        if ref.endswith("/bin") or ref.endswith("/bin."):
                            continue
                        if "tests/bin" in ref:
                            continue

                        clean_ref = ref.lstrip("/./")
                        candidate = self.repo_root / clean_ref
                        if not candidate.exists():
                            candidate2 = doc_dir / ref
                            if not candidate2.exists():
                                candidate3 = doc_dir / clean_ref
                                if not candidate3.exists():
                                    parts = clean_ref.split("/")
                                    if len(parts) > 1 and parts[0] in ("src", "scripts", "docs", "lib"):
                                        self.warn(
                                            "doc_missing_code_paths",
                                            ref,
                                            f"Doc {doc.relative_to(self.repo_root)}: "
                                            f"code example path '{ref}' may not exist"
                                        )

                    command_tokens = re.findall(r"(?:^|\s)(make|cmake|python3?|gcc|clang|nmake)\s+(\S+)", line)
                    for cmd, target in command_tokens:
                        target = target.rstrip(";)\"'")
                        if cmd in ("python3", "python") and target.startswith("scripts/"):
                            script_path = self.repo_root / target
                            if not script_path.exists():
                                self.warn(
                                    "doc_missing_code_paths",
                                    target,
                                    f"Doc {doc.relative_to(self.repo_root)}: "
                                    f"referenced script '{target}' does not exist"
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

        self.log_info("Parsing VS filters file ...")
        vsf_sources, vsf_headers = self.parse_vcxproj_filters()
        self.log_info(f"VS filters: {len(vsf_sources)} sources, {len(vsf_headers)} headers")

        self.log_info("Parsing Makefile.nmake ...")
        nmake_sources = self.parse_makefile_nmake()
        self.log_info(f"Makefile.nmake: {len(nmake_sources)} inferred sources")

        self.log_info("Parsing Makefile.osx ...")
        osx_sources, osx_extra = self.parse_makefile_osx()
        self.log_info(f"Makefile.osx: {len(osx_sources)} base sources, {len(osx_extra)} platform-specific")

        self.log_info("Parsing Makefile.inc dependencies ...")
        inc_deps = self.parse_makefile_inc_deps()
        self.log_info(f"Makefile.inc: {len(inc_deps)} source dependency entries")

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

            if not src.startswith("cocoa/") and not src.startswith("nds/") and not src.startswith("sdl2/") and not src.startswith("stats/") and src != "snd-sdl.c":
                if src not in vsf_sources and src not in self.NOT_IN_VS:
                    if not src.startswith("win/") and not src.endswith(".m"):
                        self.warn(
                            "vs_filters_missing_sources",
                            src,
                            f"Angband.vcxproj.filters: source '{src}' is not registered"
                        )

            if not src.startswith("cocoa/") and not src.startswith("nds/") and not src.startswith("sdl2/") and not src.startswith("stats/") and src != "snd-sdl.c":
                base = os.path.splitext(src)[0]
                if base + ".c" in inc_deps or base + ".m" in inc_deps or src in inc_deps:
                    pass
                elif src.startswith("win/") or src.endswith(".m") or src in ("buildid.c", "main.c") or src.startswith("main-"):
                    pass
                elif src.startswith("borg/") and src != "borg/borg.c":
                    pass
                else:
                    self.warn(
                        "missing_makefile_inc_deps",
                        src,
                        f"Makefile.inc: source '{src}' may have stale or missing dependency entry"
                    )

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

        vs_only_in_project = vs_sources - vsf_sources
        if vs_only_in_project:
            for s in sorted(vs_only_in_project):
                self.warn(
                    "vs_project_missing_filters_sources",
                    s,
                    f"Angband.vcxproj has '{s}' but filters file is missing it"
                )

        vs_only_in_filters = vsf_sources - vs_sources
        if vs_only_in_filters:
            for s in sorted(vs_only_in_filters):
                self.warn(
                    "vs_filters_missing_sources",
                    s,
                    f"Angband.vcxproj.filters has '{s}' but project file is missing it"
                )

        vs_h_only_in_project = vs_headers - vsf_headers
        if vs_h_only_in_project:
            for h in sorted(vs_h_only_in_project):
                self.warn(
                    "vs_project_missing_filters_headers",
                    h,
                    f"Angband.vcxproj has header '{h}' but filters file is missing it"
                )

        vs_h_only_in_filters = vsf_headers - vs_headers
        if vs_h_only_in_filters:
            for h in sorted(vs_h_only_in_filters):
                self.warn(
                    "vs_filters_missing_headers",
                    h,
                    f"Angband.vcxproj.filters has header '{h}' but project file is missing it"
                )

        self.mk_headers_ref = mk_headers
        self.vs_headers_ref = vs_headers

    def check_headers(self):
        self.log_info("Scanning actual header files in src/ ...")
        actual_headers = self.scan_header_files()
        self.log_info(f"Found {len(actual_headers)} header files")

        mk_sources, mk_headers = self.parse_makefile_src()
        vs_sources, vs_headers = self.parse_vcxproj()
        inc_deps = self.parse_makefile_inc_deps()

        self.log_info(f"Makefile.src HEADERS: {len(mk_headers)} registered core headers")
        self.log_info(f"VS project headers: {len(vs_headers)} registered")

        for h in sorted(actual_headers):
            if h.startswith("win/include/"):
                continue
            if h.startswith("cocoa/") or h.startswith("nds/"):
                continue
            if h.startswith("sdl2/") or h.startswith("stats/"):
                continue
            if h not in vs_headers:
                self.warn(
                    "vs_missing_headers",
                    h,
                    f"Angband.vcxproj: header '{h}' is not registered"
                )

        for h in sorted(mk_headers):
            if not (self.src_dir / h).exists():
                self.log_error(f"Makefile.src: registered header '{h}' does not exist on disk")

        for h in sorted(vs_headers):
            if not (self.src_dir / h).exists():
                self.log_error(f"Angband.vcxproj: registered header '{h}' does not exist on disk")

        for src, dep_headers in inc_deps.items():
            src_dir_name = os.path.dirname(src) if os.path.dirname(src) else "."
            for dh in dep_headers:
                candidates = []
                candidates.append(self.src_dir / dh)
                if src_dir_name != ".":
                    candidates.append(self.src_dir / src_dir_name / dh)
                dh_basename = os.path.basename(dh)
                candidates.append(self.src_dir / dh_basename)
                exists = any(c.exists() for c in candidates)
                if not exists:
                    self.warn(
                        "missing_makefile_inc_deps",
                        dh,
                        f"Makefile.inc: dependency header '{dh}' for '{src}' may not exist"
                    )

        self._check_orphan_headers(actual_headers, mk_sources, inc_deps)

    def _check_orphan_headers(self, headers: Set[str], sources: Set[str], deps: Dict[str, Set[str]]):
        all_included_headers: Set[str] = set()

        for dep_headers in deps.values():
            for dh in dep_headers:
                all_included_headers.add(dh)

        for root, _, files in os.walk(self.src_dir):
            for f in files:
                if f.endswith((".c", ".h", ".m")):
                    try:
                        content = (Path(root) / f).read_text(errors="ignore")
                        for inc_match in re.finditer(r'#include\s+[<"]([^>"]+)[>"]', content):
                            inc = inc_match.group(1)
                            all_included_headers.add(inc)
                            basename = os.path.basename(inc)
                            all_included_headers.add(basename)
                    except Exception:
                        pass

        for h in sorted(headers):
            base = h.replace(".h", "")
            h_basename = os.path.basename(h)
            if h.startswith("list-") or h.startswith("win/") or h.startswith("cocoa/") or h.startswith("nds/") or h.startswith("sdl2/") or h.startswith("stats/"):
                continue
            has_matching_source = (base + ".c" in sources) or (base + ".m" in sources)
            is_included = (h in all_included_headers) or (h_basename in all_included_headers)

            if not has_matching_source and not is_included:
                self.warn(
                    "orphan_headers",
                    h,
                    f"Potentially orphan header: '{h}' (no matching source and no includes found)"
                )

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

        self.log_info("Checking file references in documentation ...")
        self.check_documentation_references(docs)

        self.log_info("Checking command/path examples in documentation ...")
        self.check_documentation_code_examples(docs)

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

        print()
        self.log_info("Checking allowlist health (unused/stale entries) ...")
        self._check_allowlist_health()

        print(f"\n{Colors.BOLD}{'='*70}{Colors.RESET}")
        print(f"{Colors.BOLD}Summary:{Colors.RESET}")
        print(f"  {Colors.RED if self.errors else Colors.GREEN}Errors:   {len(self.errors)}{Colors.RESET}")
        print(f"  {Colors.YELLOW if self.warnings else Colors.GREEN}Warnings: {len(self.warnings)}{Colors.RESET}")
        if self.suppressed:
            print(f"  Suppressed (allowlist): {len(self.suppressed)}")
        if self.allowlist_entries:
            used_count = len(self.used_allowlist)
            total_count = len(self.allowlist_entries)
            print(f"  Allowlist: {used_count}/{total_count} entries used")
            for (category, item), count in sorted(self.used_allowlist.items()):
                entry = self.allowlist_entries.get((category, item), {})
                allow_mult = entry.get("allow_multiple", False)
                tag = " [allow_multiple]" if allow_mult and count > 1 else ""
                print(f"    - {category}:{item} -> {count} hit(s){tag}")
        if self.max_warnings >= 0:
            print(f"  Max allowed warnings: {self.max_warnings}")
        else:
            print(f"  Max allowed warnings: unlimited")
        print(f"  Infos:    {len(self.infos)}")
        print(f"{Colors.BOLD}{'='*70}{Colors.RESET}")

        failed = False
        if self.errors:
            print(f"\n{Colors.RED}{Colors.BOLD}CHECK FAILED: Found {len(self.errors)} error(s){Colors.RESET}")
            failed = True
        elif self.max_warnings >= 0 and len(self.warnings) > self.max_warnings:
            print(
                f"\n{Colors.RED}{Colors.BOLD}CHECK FAILED: {len(self.warnings)} warning(s) "
                f"exceeds threshold of {self.max_warnings}{Colors.RESET}"
            )
            failed = True
        else:
            print(f"\n{Colors.GREEN}{Colors.BOLD}CHECK PASSED{Colors.RESET}", end="")
            if self.warnings:
                print(f" (with {len(self.warnings)} warning(s))")
            else:
                print()

        if self.suppressed:
            print(f"{Colors.BLUE}NOTE: {len(self.suppressed)} warning(s) suppressed by allowlist "
                  f"(scripts/check-consistency.allowlist.json){Colors.RESET}")

        return not failed

    @staticmethod
    def run_self_checks(repo_root: str) -> bool:
        """Run 6 self-tests covering allowlist failure paths.
        Returns True if all tests pass (expected errors triggered correctly)."""

        import tempfile
        import shutil

        print(f"\n{Colors.BOLD}{'='*70}{Colors.RESET}")
        print(f"{Colors.BOLD}Allowlist Self-Checks (6 scenarios){Colors.RESET}")
        print(f"{Colors.BOLD}{'='*70}{Colors.RESET}\n")

        tmpdir = tempfile.mkdtemp(prefix="angband-cc-selfcheck-")

        def _make_checker(allowlist_data: dict) -> ConsistencyChecker:
            tmp_scripts = Path(tmpdir) / "scripts"
            tmp_scripts.mkdir(parents=True, exist_ok=True)
            tmp_allowlist = tmp_scripts / "check-consistency.allowlist.json"
            with open(tmp_allowlist, "w") as f:
                json.dump(allowlist_data, f, indent=2)

            checker = ConsistencyChecker.__new__(ConsistencyChecker)
            checker.repo_root = Path(tmpdir).resolve()
            checker.src_dir = checker.repo_root / "src"
            checker.max_warnings = 0
            checker.errors = []
            checker.warnings = []
            checker.infos = []
            checker.suppressed = []
            checker.allowlist_entries = {}
            checker.allowlist_raw = {}
            checker.used_allowlist = {}
            checker._load_allowlist()
            return checker

        def _run_test(name: str, description: str, setup_fn, expect_errors: int) -> bool:
            print(f"{Colors.BOLD}Test: {name}{Colors.RESET}")
            print(f"  {description}")
            checker = setup_fn()
            actual = len(checker.errors)
            ok = actual >= expect_errors
            if ok:
                print(f"  {Colors.GREEN}PASS{Colors.RESET}: got {actual} error(s) (expected >= {expect_errors})")
            else:
                print(f"  {Colors.RED}FAIL{Colors.RESET}: got {actual} error(s) (expected >= {expect_errors})")
                for e in checker.errors:
                    print(f"    ERROR: {e}")
            print()
            return ok

        all_pass = True

        # Test 1: Missing required fields
        def t1_setup():
            data = {
                "entries": [
                    {"category": "orphan_headers", "item": "test.h", "reason": "x"}  # missing owner
                ]
            }
            c = _make_checker(data)
            return c
        all_pass &= _run_test(
            "T1 - Missing fields",
            "Entry missing 'owner' field should trigger ERROR",
            t1_setup, 1
        )

        # Test 2: Invalid category
        def t2_setup():
            data = {
                "entries": [
                    {"category": "INVALID_CATEGORY", "item": "test.h", "reason": "x", "owner": "t"}
                ]
            }
            c = _make_checker(data)
            return c
        all_pass &= _run_test(
            "T2 - Invalid category",
            "Entry with invalid category name should trigger ERROR",
            t2_setup, 1
        )

        # Test 3: Duplicate entry
        def t3_setup():
            data = {
                "entries": [
                    {"category": "orphan_headers", "item": "dup.h", "reason": "first", "owner": "t"},
                    {"category": "orphan_headers", "item": "dup.h", "reason": "second", "owner": "t"},
                ]
            }
            c = _make_checker(data)
            return c
        all_pass &= _run_test(
            "T3 - Duplicate entry",
            "Two entries with same (category, item) should trigger ERROR",
            t3_setup, 1
        )

        # Test 4: Overly broad item (no file extension)
        def t4_setup():
            data = {
                "entries": [
                    {"category": "orphan_headers", "item": "no_extension", "reason": "x", "owner": "t"}
                ]
            }
            c = _make_checker(data)
            return c
        all_pass &= _run_test(
            "T4 - Overly broad item",
            "Item without file extension should trigger ERROR",
            t4_setup, 1
        )

        # Test 5: Unused entry
        def t5_setup():
            data = {
                "entries": [
                    {"category": "orphan_headers", "item": "unused.h", "reason": "x", "owner": "t"}
                ]
            }
            c = _make_checker(data)
            c._check_allowlist_health()
            return c
        all_pass &= _run_test(
            "T5 - Unused entry",
            "Entry never matched by any warning should trigger ERROR in health check",
            t5_setup, 1
        )

        # Test 6: Multi-hit without allow_multiple
        def t6_setup():
            data = {
                "entries": [
                    {"category": "orphan_headers", "item": "multi.h", "reason": "x", "owner": "t"}
                ]
            }
            c = _make_checker(data)
            c.warn("orphan_headers", "multi.h", "first match")
            c.warn("orphan_headers", "multi.h", "second match")
            c._check_allowlist_health()
            return c
        all_pass &= _run_test(
            "T6 - Multi-hit without allow_multiple",
            "Entry matching 2+ warnings without allow_multiple:true should trigger ERROR",
            t6_setup, 1
        )

        # Test 7 (bonus): Multi-hit WITH allow_multiple should NOT trigger
        def t7_setup():
            data = {
                "entries": [
                    {"category": "orphan_headers", "item": "ok.h", "reason": "x", "owner": "t", "allow_multiple": True}
                ]
            }
            c = _make_checker(data)
            c.warn("orphan_headers", "ok.h", "first match")
            c.warn("orphan_headers", "ok.h", "second match")
            c._check_allowlist_health()
            return c
        all_pass &= _run_test(
            "T7 - Multi-hit WITH allow_multiple",
            "Entry matching 2+ warnings WITH allow_multiple:true should NOT trigger ERROR",
            t7_setup, 0
        )

        print(f"{Colors.BOLD}{'='*70}{Colors.RESET}")
        if all_pass:
            print(f"{Colors.GREEN}{Colors.BOLD}ALL SELF-CHECK TESTS PASSED{Colors.RESET}")
        else:
            print(f"{Colors.RED}{Colors.BOLD}SOME SELF-CHECK TESTS FAILED{Colors.RESET}")
        print(f"{Colors.BOLD}{'='*70}{Colors.RESET}")

        shutil.rmtree(tmpdir, ignore_errors=True)
        return all_pass


def main():
    parser = argparse.ArgumentParser(
        description="Angband release consistency checker (strict by default)"
    )
    parser.add_argument(
        "--max-warnings",
        type=int,
        default=None,
        metavar="N",
        help="Maximum number of warnings allowed before check fails (default: 0, strict mode)",
    )
    parser.add_argument(
        "--lenient",
        action="store_true",
        help="Lenient mode: allow any number of warnings (equivalent to --max-warnings=-1)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Strict mode: treat any warnings as failures (default behavior)",
    )
    parser.add_argument(
        "--repo-root",
        type=str,
        default=None,
        help="Path to the repository root (default: inferred from script location)",
    )
    parser.add_argument(
        "--self-check",
        action="store_true",
        help="Run built-in self-tests on the allowlist validation logic (6 failure scenarios)",
    )

    args = parser.parse_args()

    if args.self_check:
        script_dir = Path(__file__).resolve().parent
        repo_root = Path(args.repo_root).resolve() if args.repo_root else script_dir.parent
        return 0 if ConsistencyChecker.run_self_checks(str(repo_root)) else 1

    if args.lenient:
        max_warnings = -1
    elif args.max_warnings is not None:
        max_warnings = args.max_warnings
    else:
        max_warnings = 0

    if args.repo_root:
        repo_root = args.repo_root
    else:
        script_dir = Path(__file__).resolve().parent
        repo_root = script_dir.parent

    checker = ConsistencyChecker(str(repo_root), max_warnings=max_warnings)
    ok = checker.run_all()

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
