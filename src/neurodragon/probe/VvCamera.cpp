#include "neurodragon/probe/VvCamera.h"

#include <cstring>

#ifdef TEST
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"

#include <glm/glm.hpp>

namespace neurodragon::probe::camera {

namespace {

void copyMatrix(float* out, glm::mat4x4 const& matrix) {
    std::memcpy(out, &matrix, sizeof(float) * 16);
}

void copyVector(float* out, glm::vec3 const& vector) {
    out[0] = vector.x;
    out[1] = vector.y;
    out[2] = vector.z;
}

// EXP-0021 read an empty projection stack through back(), which is undefined and
// produced the garbage. Only a non-empty stack is a matrix.
bool copyStackTop(float* out, MatrixStack& stack) {
    auto const& matrices = *stack.stack;
    if (matrices.empty()) {
        return false;
    }
    copyMatrix(out, *matrices.back()._m);
    return true;
}

} // namespace

Sample read() {
    Sample sample{};
    auto   client = ll::service::getClientInstance();
    if (!client) {
        return sample;
    }
    auto& camera = client->getCamera();
    copyVector(sample.position, *camera.mPosition);
    copyVector(sample.right, *camera.mRight);
    copyVector(sample.up, *camera.mUp);
    copyVector(sample.forward, *camera.mForward);
    // Scalars are stored as the value itself in this LeviLamina version.
    sample.aspect = camera.mAspectRatio;
    sample.fov    = camera.mFov;
    sample.zNear  = camera.mZNear;
    sample.zFar   = camera.mZFar;
    sample.viewOk        = copyStackTop(sample.view, *camera.viewMatrixStack);
    sample.worldOk       = copyStackTop(sample.world, *camera.worldMatrixStack);
    sample.projectionOk  = copyStackTop(sample.projection, *camera.projectionMatrixStack);
    copyMatrix(sample.inverseView, *camera.mInverseViewMatrix);
    std::memcpy(sample.raw, &camera, sizeof(sample.raw));
    sample.valid = true;
    return sample;
}

} // namespace neurodragon::probe::camera

#else

namespace neurodragon::probe::camera {

Sample read() { return Sample{}; }

} // namespace neurodragon::probe::camera

#endif
