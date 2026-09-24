#pragma once
#include <cmath>
#include <cstring>
#include <imgui.h>

namespace GGPKExp {

/// Arcball camera for the mesh viewer.
/// Handles orbit rotation, pan, zoom, and matrix computation.
class MeshCamera {
public:
    /// Reset camera to default orientation
    void Reset();

    /// Auto-fit camera to a bounding box
    void FitToBounds(const float bboxMin[3], const float bboxMax[3]);

    /// Process mouse input for orbit/pan/zoom within a viewport region
    void ProcessInput(ImVec2 canvasPos, ImVec2 canvasSize);

    /// Compute the camera eye position in world space
    void GetEyePosition(float& x, float& y, float& z) const;

    /// Get the look-at target position
    void GetTargetPosition(float& x, float& y, float& z) const;

    /// Get the camera-to-target distance
    float GetDistance() const { return m_distance; }

    /// Get yaw/pitch angles (for info display)
    float GetYaw() const { return m_yaw; }
    float GetPitch() const { return m_pitch; }

    /// Override yaw/pitch (e.g. after FitToBounds, to pick a different starting angle)
    void SetYawPitch(float yaw, float pitch) { m_yaw = yaw; m_pitch = pitch; }

    /// Override the orbit pivot (look-at target) in world space.
    /// Pure setter: orbit yaw/pitch/distance are preserved. Use this from
    /// per-frame code (e.g. "camera follows model root bone") to shift the
    /// center of rotation without disturbing the current viewing angle.
    void SetTarget(float x, float y, float z) {
        m_targetX = x;
        m_targetY = y;
        m_targetZ = z;
    }

    // ----- Matrix math (float[16] row-major, no external dependency) -----

    static void MatIdentity(float* m);
    static void MatMul(float* out, const float* a, const float* b);
    static void MatPerspective(float* out, float fovY, float aspect, float zn, float zf);
    static void MatLookAt(float* out, float eyeX, float eyeY, float eyeZ,
                          float tgtX, float tgtY, float tgtZ,
                          float upX, float upY, float upZ);
    static void MatTranspose(float* out, const float* in);

    /// Build the view matrix from the current camera state
    void BuildViewMatrix(float* out) const;

    /// Build a perspective projection matrix from the current distance + aspect ratio
    void BuildProjectionMatrix(float* out, float aspect) const;

    /// Build combined VP matrix (view * projection)
    void BuildVPMatrix(float* out, float aspect) const;

private:
    float m_yaw = 0.3f, m_pitch = 0.3f;
    float m_distance = 200.0f;
    float m_targetX = 0, m_targetY = 0, m_targetZ = 60.0f;
    bool m_rotating = false, m_panning = false;
    ImVec2 m_lastMouse{0, 0};
};

} // namespace GGPKExp
