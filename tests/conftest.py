import os

# Tests must not write allocated IDs into ~/.dustx. Production leaves
# DUSTX_ID_STORE unset and persists under DUSTX_DATA_DIR or ~/.dustx.
os.environ["DUSTX_ID_STORE"] = "memory"
os.environ["DUSTX_DEVICE_DB"] = "memory"
os.environ["DUSTX_UPDATE_POLICY"] = "memory"
