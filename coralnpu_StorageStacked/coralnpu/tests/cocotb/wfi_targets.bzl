"""Macro to generate wfi_slot targets for template_rule."""

def get_wfi_slot_targets():
    """Returns a dictionary of wfi_slot targets for template_rule."""
    targets = {}
    for slot in range(4):
        # Standard
        targets["wfi_slot_%d" % slot] = {
            "srcs": ["wfi_slot_%d.c" % slot],
        }

        # Highmem
        targets["wfi_slot_%d_highmem" % slot] = {
            "srcs": ["wfi_slot_%d.c" % slot],
            "itcm_size_kbytes": 1024,
            "dtcm_size_kbytes": 1024,
        }

        # 512KB
        targets["wfi_slot_%d_itcm512kb_dtcm512kb" % slot] = {
            "srcs": ["wfi_slot_%d.c" % slot],
            "itcm_size_kbytes": 512,
            "dtcm_size_kbytes": 512,
        }
    return targets
