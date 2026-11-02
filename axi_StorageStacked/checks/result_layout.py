"""AXI result routes loaded from the public storage manifest."""
from result_core import Layout, load_routes
from storage_manifest import load

layout = Layout(load_routes(load()['result_routes']))
artifact = layout.artifact
grouped = layout.grouped
location = layout.location
organize = layout.organize
unpack = layout.unpack
working_result = layout.working_result
