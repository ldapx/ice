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
    "sphinx_copybutton",
]

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
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]

html_theme = "furo"
html_static_path = ["_static"]
html_css_files = ["custom.css"]
html_title = "ICE"
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
