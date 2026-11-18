import os
import sys

project = "ICE"
author = "the Libre Design Automation Project"
copyright = "2026, the Libre Design Automation Project"
version = "0.1.0"
release = "0.1.0"
language = "en"

extensions = [
    "myst_parser",
    "sphinx.ext.autodoc",
    "sphinx_copybutton",
]

# The procedural layout runner is a standalone script under assets/. Put it on
# sys.path so autodoc can import the builder classes for the API reference.
_docs_dir = os.path.dirname(os.path.abspath(__file__))
_runner_dir = os.path.join(os.path.dirname(_docs_dir), "assets", "layout")
if _runner_dir not in sys.path:
    sys.path.insert(0, _runner_dir)

autodoc_member_order = "bysource"
autodoc_preserve_defaults = True

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}
master_doc = "index"

myst_enable_extensions = [
    "colon_fence",
    "deflist",
    "fieldlist",
]
myst_heading_anchors = 3

templates_path = ["_templates"]
exclude_patterns = [
    "_build",
    "Thumbs.db",
    ".DS_Store",
    # Work-in-progress proposals; keep them in the repository but out of the
    # published documentation until they are accepted.
    "developer/proposals/*.md",
]

html_theme = "furo"
html_static_path = ["_static"]
html_css_files = ["custom.css"]
html_title = "ICE"
html_logo = "logo.png"
html_favicon = "logo.png"
html_theme_options = {
    "light_css_variables": {
        "color-brand-primary": "#42b883",
        "color-brand-content": "#359268",
        "color-highlight-on-target": "#e6f6ee",
    },
    "dark_css_variables": {
        "color-brand-primary": "#4acf93",
        "color-brand-content": "#4acf93",
        "color-highlight-on-target": "#143325",
    },
    "sidebar_hide_name": False,
    "navigation_with_keys": True,
    "top_of_page_buttons": ["view", "edit"],
    "source_repository": "https://github.com/ldapx/ice",
    "source_branch": "main",
    "source_directory": "docs/",
}
