"""Inspect an OpenVLSI project through the optional nanobind surface.

The binding module is named ``openvlsipy`` when the optional Python target is built.
"""

import openvlsipy


project = openvlsipy.load_project("demo/project.ovpro")
print(project.name)
print(project.schema_version)
