/* applet-mitm - hand-rolled nvdrv/nvmap access probe (M4b). */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    /* One-shot: open nvdrv:s, /dev/nvmap, and try NVMAP_IOC_FROM_ID on the
     * game's buffer id. Breadcrumbed at every step (sdmc:/applet-mitm.last). */
    void TryNvmapProbe(u32 nvmap_id);

}
