#pragma once

namespace neurodragon::probe::vv {

// True while the direct-factor hook is running the "native render, branch
// keepalive" mode: the camera/viewport is built at factor 1.0, then the factor is
// restored below 1 so frameImpl still declares TAAU/arg10. The actual colour/depth
// targets are display-sized, so the probe must report display size as the render
// size instead of display * factor.
[[nodiscard]] bool nativeRender();

} // namespace neurodragon::probe::vv
