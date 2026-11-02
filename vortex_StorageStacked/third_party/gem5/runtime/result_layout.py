"""Platform data routes combined with the storage manifest routes."""
from pathlib import Path
from paths import STORAGE_RESULT_ROUTES
from result_core import Layout, load_routes

layout = Layout.merged(load_routes(STORAGE_RESULT_ROUTES),
                       load_routes(Path(__file__).with_name('result_routes.json')))
artifact = layout.artifact
grouped = layout.grouped
location = layout.location
organize = layout.organize
unpack = layout.unpack
working_result = layout.working_result
