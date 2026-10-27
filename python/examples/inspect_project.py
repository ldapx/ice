"""Inspect an ICE project through the optional nanobind surface.

The binding module is named ``icepy`` when the optional Python target is built.
"""

import icepy


project = icepy.load_project("demo/project.icepro")
print(project.name)
print(project.schema_version)
