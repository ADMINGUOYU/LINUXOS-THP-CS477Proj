#!/usr/bin/env python3
# file: helper_fileinspector.py
# inspects a text based file and export a JSON report on matched keywords
# run with `python -m scripts.helper_fileinspector`

from __future__ import annotations

# import libraries
import subprocess
import sys
from pathlib import Path

from scripts.commons import log, save_json

# [CONFIGURATION] =======================
report_path = "inspected.local.json"
# NOTE: current setting below inspects khugepaged.c
#       this source manages Transparent HugePage Support (THP)
search_file = "./linux/mm/khugepaged.c"  # to a text based file
search_terms = ("madvise_collapse", "hugepage_vma_check", "userfaultfd_armed", "VM_NOHUGEPAGE")
# =======================================


# main function
def main() -> None:

    global report_path, search_file, search_terms

    # make it a path
    search_file = Path(search_file)
    if not search_file.is_file():
        raise ValueError(f"No such file: {search_file}")

    # get lines of the source file
    source_lines = search_file.read_text().splitlines()

    # Record context and provenance; source matches do not prove runtime support.
    excerpts = []
    for index, line in enumerate(source_lines):
        if any(term in line for term in search_terms):
            start = max(0, index - 5)
            end = min(len(source_lines), index + 16)
            excerpts.append({"match_line": index + 1, "start_line": start + 1,
                             "text": "\n".join(source_lines[start:end])})
    save_json(Path(report_path), { "excerpts": excerpts })
    log("fileinspector", "OK", f"Saved {len(excerpts)} source excerpts to {report_path}")


# Entry point
if __name__ == "__main__":

    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        log("fileinspector", "ERROR", str(error))
        sys.exit(1)
