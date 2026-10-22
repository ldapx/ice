# Experimental plugins

Plugins are discovered from a directory containing `plugin.toml`. The manifest records
the plugin name, version, targeted ICE build, capabilities, and explicit permission
scopes. The lifecycle is `discovered -> initialized -> enabled -> disabled -> unloaded`;
target mismatches fail initialization. The current manager is deliberately in-process
and experimental. A future worker boundary can use the same manifest and permission
contract without changing project-domain operations.

Plugins must call validated application operations rather than mutating serialized
project data directly. Permission defaults are read-only: project mutation, process
execution, and filesystem access must be granted explicitly.
