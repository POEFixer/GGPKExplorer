#include "MeshCamera.h"
#include <algorithm>

namespace GGPKExp {

// -----------------------------------------------------------------------
// Reset / FitToBounds
// -----------------------------------------------------------------------

void MeshCamera::Reset() {
    // Z-down orbital defaults (POE2 mesh convention — +Z toward floor,
    // world-up = -Z).
    // yaw=pi  -> camera on -Y side, looking toward +Y (character's front).
    // pitch=0 -> eye-level (no tilt); screen-up corresponds to world -Z.
    m_yaw       = 3.1415927f;
    m_pitch     = 0.0f;
    m_distance  = 200.0f;
    m_targetX   = 0.0f;
    m_targetY   = 0.0f;
    m_targetZ   = 0.0f;
    m_rotating  = false;
    m_panning   = false;
    m_lastMouse = {0, 0};
}

void MeshCamera::FitToBounds(const float bboxMin[3], const float bboxMax[3]) {
    m_targetX = 0.5f * (bboxMin[0] + bboxMax[0]);
    m_targetY = 0.5f * (bboxMin[1] + bboxMax[1]);
    m_targetZ = 0.5f * (bboxMin[2] + bboxMax[2]);

    const float dx = bboxMax[0] - bboxMin[0];
    const float dy = bboxMax[1] - bboxMin[1];
    const float dz = bboxMax[2] - bboxMin[2];
    float maxDim = dx;
    if (dy > maxDim) maxDim = dy;
    if (dz > maxDim) maxDim = dz;
    if (maxDim < 1.0f) maxDim = 1.0f;

    // Distance = 1.8 * maxDim keeps the model at ~70% of viewport.
    m_distance = maxDim * 1.8f;

    // Z-down world (POE2 mesh convention — +Z toward floor, world-up = -Z).
    // Default eye-level front view:
    //   yaw pi  -> camera on -Y axis, looking toward +Y
    //             (visible face is the model's +Y face — the character's front)
    //   pitch 0 -> horizontal, no tilt; character stands upright on screen.
    m_yaw   = 3.1415927f;
    m_pitch = 0.0f;

    m_rotating = false;
    m_panning  = false;
}

// -----------------------------------------------------------------------
// Input handling
// -----------------------------------------------------------------------

void MeshCamera::ProcessInput(ImVec2 canvasPos, ImVec2 canvasSize) {
    (void)canvasPos; // position not needed for input — hovered check is done externally
    (void)canvasSize;

    bool hovered = ImGui::IsItemHovered();
    ImVec2 mousePos = ImGui::GetIO().MousePos;

    // Left drag = orbit rotate
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_rotating = true;
        m_lastMouse = mousePos;
    }
    if (m_rotating) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float dx = mousePos.x - m_lastMouse.x;
            float dy = mousePos.y - m_lastMouse.y;
            m_yaw   -= dx * 0.005f;
            m_pitch += dy * 0.005f;
            // No clamp, no pole-flip. Pitch is unbounded; BuildViewMatrix
            // picks the sign of the up vector from cos(pitch) so the view
            // stays stable when the eye crosses above/below the target.
            m_lastMouse = mousePos;
        } else {
            m_rotating = false;
        }
    }

    // Right drag or Middle drag = pan
    bool startPan = hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                                ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
    if (startPan) {
        m_panning = true;
        m_lastMouse = mousePos;
    }
    if (m_panning) {
        bool stillDown = ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        if (stillDown) {
            const float dx = mousePos.x - m_lastMouse.x;
            const float dy = mousePos.y - m_lastMouse.y;
            const float panScale = m_distance * 0.0015f;

            const float cp = std::cos(m_pitch);
            const float sp = std::sin(m_pitch);
            const float cy = std::cos(m_yaw);
            const float sy = std::sin(m_yaw);

            // Z-down world (+Z toward floor, world-up = -Z). forward
            // (eye->target) = (-sin(yaw)*cos(pitch), -cos(yaw)*cos(pitch),
            //                   sin(pitch)). Screen-right lies in the XY
            // plane perpendicular to forward.
            //   right = normalize(forward x world_up_{-Z}) = (cos(yaw), -sin(yaw), 0)
            const float rx = cy, ry = -sy, rz = 0.0f;

            // Screen-up in world = cross(right, forward).
            //   = (-sin(yaw)*sin(pitch), -cos(yaw)*sin(pitch), -cos(pitch))
            const float ux = -sy * sp, uy = -cy * sp, uz = -cp;

            m_targetX -= (dx * rx + dy * ux) * panScale;
            m_targetY -= (dx * ry + dy * uy) * panScale;
            m_targetZ -= (            dy * uz) * panScale;

            m_lastMouse = mousePos;
        } else {
            m_panning = false;
        }
    }

    // Scroll = zoom
    if (hovered) {
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f) {
            float factor = 1.0f - scroll * 0.08f;
            if (factor < 0.5f) factor = 0.5f;
            if (factor > 2.0f) factor = 2.0f;
            m_distance *= factor;
            if (m_distance < 0.01f) m_distance = 0.01f;
        }
    }
}

// -----------------------------------------------------------------------
// Position accessors
// -----------------------------------------------------------------------

void MeshCamera::GetEyePosition(float& x, float& y, float& z) const {
    // Z-down spherical coordinates centered at (m_targetX, m_targetY, m_targetZ).
    // POE2 mesh convention: +Z points DOWN (toward floor), characters face +Y.
    // World-up is therefore -Z; camera elevation rotates around that axis.
    //   yaw 0     -> eye on +Y axis (behind character, sees back)
    //   yaw pi    -> eye on -Y axis (in front, sees face)  <- default
    //   yaw pi/2  -> eye on +X axis (right side)
    //   pitch 0   -> eye in horizontal plane through target
    //   pitch +   -> eye raised along -Z (above character, looking down)
    const float cp = std::cos(m_pitch);
    const float sp = std::sin(m_pitch);
    const float cy = std::cos(m_yaw);
    const float sy = std::sin(m_yaw);
    x = m_targetX + m_distance * sy * cp;
    y = m_targetY + m_distance * cy * cp;
    z = m_targetZ - m_distance * sp;   // -sp: pitch>0 moves eye toward -Z (world up)
}

// -----------------------------------------------------------------------
// Matrix builders
// -----------------------------------------------------------------------

void MeshCamera::BuildViewMatrix(float* out) const {
    float eyeX, eyeY, eyeZ;
    GetEyePosition(eyeX, eyeY, eyeZ);
    // POE2 is Z-down (+Z points toward floor), so world-up is -Z. Dynamic up
    // vector: flips to +Z when the eye crosses the top/bottom poles
    // (cos(pitch) changes sign). This lets pitch grow unbounded — user can
    // orbit the model vertically 360 degrees continuously without gimbal
    // degeneracy. A brief "upside-down world" pass happens exactly at the
    // pole, which is the standard trade-off for unlimited orbit.
    const float upZ = (std::cos(m_pitch) >= 0.0f) ? -1.0f : 1.0f;
    MatLookAt(out, eyeX, eyeY, eyeZ,
              m_targetX, m_targetY, m_targetZ,
              0.0f, 0.0f, upZ);
}

void MeshCamera::BuildProjectionMatrix(float* out, float aspect) const {
    float nearPlane = m_distance * 0.01f;
    if (nearPlane < 0.01f) nearPlane = 0.01f;
    float farPlane = m_distance * 100.0f;
    if (farPlane < 100.0f) farPlane = 100.0f;
    MatPerspective(out, 0.7854f, aspect, nearPlane, farPlane);
}

void MeshCamera::BuildVPMatrix(float* out, float aspect) const {
    float view[16], proj[16];
    BuildViewMatrix(view);
    BuildProjectionMatrix(proj, aspect);
    MatMul(out, view, proj);
}

// -----------------------------------------------------------------------
// Matrix math (row-major, no external dependency)
// -----------------------------------------------------------------------

void MeshCamera::MatIdentity(float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

void MeshCamera::MatMul(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            tmp[r * 4 + c] =
                a[r * 4 + 0] * b[0 * 4 + c] +
                a[r * 4 + 1] * b[1 * 4 + c] +
                a[r * 4 + 2] * b[2 * 4 + c] +
                a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
    std::memcpy(out, tmp, sizeof(float) * 16);
}

void MeshCamera::MatPerspective(float* out, float fovY, float aspect, float zn, float zf) {
    std::memset(out, 0, sizeof(float) * 16);
    float tanHalf = std::tan(fovY * 0.5f);
    if (tanHalf < 0.0001f) tanHalf = 0.0001f;
    out[0]  = 1.0f / (aspect * tanHalf);
    out[5]  = 1.0f / tanHalf;
    out[10] = zf / (zf - zn);
    out[11] = 1.0f;
    out[14] = -(zn * zf) / (zf - zn);
}

void MeshCamera::MatLookAt(float* out, float eyeX, float eyeY, float eyeZ,
                           float tgtX, float tgtY, float tgtZ,
                           float upX, float upY, float upZ) {
    float fx = tgtX - eyeX;
    float fy = tgtY - eyeY;
    float fz = tgtZ - eyeZ;
    float fLen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fLen < 0.0001f) fLen = 0.0001f;
    fx /= fLen; fy /= fLen; fz /= fLen;

    float rx = fy * upZ - fz * upY;
    float ry = fz * upX - fx * upZ;
    float rz = fx * upY - fy * upX;
    float rLen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rLen < 0.0001f) rLen = 0.0001f;
    rx /= rLen; ry /= rLen; rz /= rLen;

    float ux = ry * fz - rz * fy;
    float uy = rz * fx - rx * fz;
    float uz = rx * fy - ry * fx;

    std::memset(out, 0, sizeof(float) * 16);
    out[0]  = rx;   out[1]  = ux;   out[2]  = fx;   out[3]  = 0.0f;
    out[4]  = ry;   out[5]  = uy;   out[6]  = fy;   out[7]  = 0.0f;
    out[8]  = rz;   out[9]  = uz;   out[10] = fz;   out[11] = 0.0f;
    out[12] = -(rx * eyeX + ry * eyeY + rz * eyeZ);
    out[13] = -(ux * eyeX + uy * eyeY + uz * eyeZ);
    out[14] = -(fx * eyeX + fy * eyeY + fz * eyeZ);
    out[15] = 1.0f;
}

void MeshCamera::MatTranspose(float* out, const float* in) {
    float tmp[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            tmp[c * 4 + r] = in[r * 4 + c];
    std::memcpy(out, tmp, sizeof(float) * 16);
}

} // namespace GGPKExp
