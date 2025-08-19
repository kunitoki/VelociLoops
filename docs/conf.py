import sys
import os
from datetime import datetime

sys.path.insert(0, os.path.abspath("../src"))

project = "VelociLoops"
author = "kunitoki"
release = "0.1.0"
copyright = f"{datetime.now().year}, {author}"

extensions = [
    "myst_parser",
    "sphinx.ext.autodoc",
    "sphinx.ext.doctest",
    "sphinx.ext.extlinks",
    "sphinx.ext.intersphinx",
    "sphinx.ext.todo",
    "sphinx.ext.mathjax",
    "sphinx.ext.viewcode",
]

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

master_doc = "index"

html_theme = "sphinx_rtd_theme"
html_static_path = []

exclude_patterns = [
    "_build", "dist", "src", "tests", "Thumbs.db", ".DS_Store"]
